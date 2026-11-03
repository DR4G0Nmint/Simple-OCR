#include "generator/shard_writer.h"

#include "common/sha256.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace ocr {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t ShardVersion = 1;
constexpr char ShardMagic[4] = { 'O', 'C', 'R', 'B' };

void appendU32( std::vector<uint8_t>& out, uint32_t value )
{
	out.push_back( static_cast<uint8_t>( value & 0xFF ) );
	out.push_back( static_cast<uint8_t>( ( value >> 8 ) & 0xFF ) );
	out.push_back( static_cast<uint8_t>( ( value >> 16 ) & 0xFF ) );
	out.push_back( static_cast<uint8_t>( ( value >> 24 ) & 0xFF ) );
}

std::string jsonEscape( const std::string& s )
{
	std::string out;
	out.reserve( s.size() + 8 );
	for( char c : s ) {
		switch( c ) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if( static_cast<unsigned char>( c ) < 0x20 ) {
				char buf[8];
				std::snprintf( buf, sizeof( buf ), "\\u%04x", static_cast<unsigned char>( c ) );
				out += buf;
			} else {
				out.push_back( c );
			}
		}
	}
	return out;
}

std::string hexOf( const uint8_t* digest, int size )
{
	static const char* hex = "0123456789abcdef";
	std::string result;
	result.reserve( static_cast<size_t>( size ) * 2 );
	for( int i = 0; i < size; ++i ) {
		result.push_back( hex[digest[i] >> 4] );
		result.push_back( hex[digest[i] & 0xF] );
	}
	return result;
}

// Streaming file digest; avoids loading a whole shard into memory.
bool hashFile( const std::string& path, std::string& digestOut )
{
	std::FILE* f = std::fopen( path.c_str(), "rb" );
	if( f == nullptr ) {
		return false;
	}
	Sha256 hash;
	std::vector<uint8_t> buffer( 1 << 20 );
	size_t read = 0;
	while( ( read = std::fread( buffer.data(), 1, buffer.size(), f ) ) > 0 ) {
		hash.update( buffer.data(), read );
	}
	const bool failed = std::ferror( f ) != 0;
	std::fclose( f );
	if( failed ) {
		return false;
	}
	uint8_t digest[Sha256::DigestSize];
	hash.finish( digest );
	digestOut = hexOf( digest, Sha256::DigestSize );
	return true;
}

} // namespace

bool BoundedQueue::push( Record&& record )
{
	std::unique_lock<std::mutex> lock( mutex );
	notFull.wait( lock, [this] { return closed || items.size() < capacity; } );
	if( closed ) {
		return false;
	}
	items.push_back( std::move( record ) );
	notEmpty.notify_one();
	return true;
}

bool BoundedQueue::pop( Record& out )
{
	std::unique_lock<std::mutex> lock( mutex );
	notEmpty.wait( lock, [this] { return closed || !items.empty(); } );
	if( items.empty() ) {
		return false;
	}
	out = std::move( items.front() );
	items.pop_front();
	notFull.notify_one();
	return true;
}

void BoundedQueue::close()
{
	std::lock_guard<std::mutex> lock( mutex );
	closed = true;
	notEmpty.notify_all();
	notFull.notify_all();
}

ShardWriter::ShardWriter( std::string outputDir, uint64_t shardSize, uint64_t seed, int targetWidth,
	int targetHeight, int channels, std::string charset )
	: outputDir( std::move( outputDir ) )
	, shardSize( shardSize == 0 ? 10000 : shardSize )
	, seed( seed )
	, targetWidth( targetWidth )
	, targetHeight( targetHeight )
	, channels( channels )
	, charset( std::move( charset ) )
	, recordQueue( 64 )
{
}

ShardWriter::~ShardWriter()
{
	if( file != nullptr ) {
		std::fclose( file );
		file = nullptr;
	}
	if( thread.joinable() ) {
		recordQueue.close();
		thread.join();
	}
}

bool ShardWriter::start( std::string* error )
{
	std::error_code ec;
	fs::create_directories( outputDir, ec );
	if( ec ) {
		if( error != nullptr ) {
			*error = "cannot create output directory " + outputDir + ": " + ec.message();
		}
		return false;
	}

	thread = std::thread( [this] { run(); } );
	return true;
}

bool ShardWriter::openShard( std::string* error )
{
	char name[64];
	std::snprintf( name, sizeof( name ), "shard_%05llu.ocrb",
		static_cast<unsigned long long>( shardInfos.size() ) );
	currentShardPath = ( fs::path( outputDir ) / name ).string();

	file = std::fopen( currentShardPath.c_str(), "wb" );
	if( file == nullptr ) {
		if( error != nullptr ) {
			*error = "cannot create " + currentShardPath;
		}
		return false;
	}

	// Header placeholder; num_records is patched on close.
	std::fwrite( ShardMagic, 1, 4, file );
	const uint32_t version = ShardVersion;
	const uint32_t zero = 0;
	std::fwrite( &version, 4, 1, file );
	std::fwrite( &zero, 4, 1, file );

	recordsInCurrentShard = 0;
	return true;
}

bool ShardWriter::closeShard( std::string* error )
{
	if( file == nullptr ) {
		return true;
	}

	const long countOffset = 8; // magic(4) + version(4)
	if( std::fseek( file, countOffset, SEEK_SET ) != 0 ) {
		if( error != nullptr ) {
			*error = "cannot seek in " + currentShardPath;
		}
		std::fclose( file );
		file = nullptr;
		return false;
	}
	const uint32_t numRecords = static_cast<uint32_t>( recordsInCurrentShard );
	std::fwrite( &numRecords, 4, 1, file );

	if( std::fflush( file ) != 0 ) {
		if( error != nullptr ) {
			*error = "cannot flush " + currentShardPath;
		}
		std::fclose( file );
		file = nullptr;
		return false;
	}

	// ftell would report the header position; ask for the real end of file.
	if( std::fseek( file, 0, SEEK_END ) != 0 ) {
		if( error != nullptr ) {
			*error = "cannot seek to the end of " + currentShardPath;
		}
		std::fclose( file );
		file = nullptr;
		return false;
	}
	const long fileSize = std::ftell( file );
	std::fclose( file );
	file = nullptr;

	ShardInfo info;
	info.file = fs::path( currentShardPath ).filename().string();
	info.records = recordsInCurrentShard;
	info.bytes = static_cast<uint64_t>( fileSize );
	if( !hashFile( currentShardPath, info.sha256 ) ) {
		if( error != nullptr ) {
			*error = "cannot checksum " + currentShardPath;
		}
		return false;
	}

	shardFiles.push_back( currentShardPath );
	shardInfos.push_back( info );
	recordsInCurrentShard = 0;
	return true;
}

bool ShardWriter::writeRecord( const Record& record, std::string* error )
{
	if( file == nullptr && !openShard( error ) ) {
		return false;
	}

	const Image& img = record.image;
	const uint32_t labelLen = static_cast<uint32_t>( record.label.size() );
	const uint32_t h = static_cast<uint32_t>( img.height );
	const uint32_t w = static_cast<uint32_t>( img.width );
	const uint32_t ch = static_cast<uint32_t>( img.channels );

	std::vector<uint8_t> header;
	header.reserve( 16 + record.label.size() );
	appendU32( header, labelLen );
	header.insert( header.end(), record.label.begin(), record.label.end() );
	appendU32( header, h );
	appendU32( header, w );
	appendU32( header, ch );

	if( std::fwrite( header.data(), 1, header.size(), file ) != header.size() ) {
		if( error != nullptr ) {
			*error = "short write to " + currentShardPath;
		}
		return false;
	}
	if( !img.pixels.empty()
		&& std::fwrite( img.pixels.data(), 1, img.pixels.size(), file ) != img.pixels.size() )
	{
		if( error != nullptr ) {
			*error = "short pixel write to " + currentShardPath;
		}
		return false;
	}

	++recordsInCurrentShard;
	++writtenRecords;

	if( recordsInCurrentShard >= shardSize ) {
		return closeShard( error );
	}
	return true;
}

void ShardWriter::run()
{
	Record record;
	std::string error;
	while( recordQueue.pop( record ) ) {
		pending.emplace( record.index, std::move( record ) );

		// Drain everything that is now contiguous. Waiting for the missing
		// index is safe: it was claimed before this one and its worker will
		// eventually push it.
		while( true ) {
			auto it = pending.find( nextIndex );
			if( it == pending.end() ) {
				break;
			}
			if( !writeRecord( it->second, &error ) ) {
				std::lock_guard<std::mutex> lock( errorMutex );
				if( firstError.empty() ) {
					firstError = error;
				}
				recordQueue.close();
				return;
			}
			pending.erase( it );
			++nextIndex;
		}
	}

	if( !closeShard( &error ) ) {
		std::lock_guard<std::mutex> lock( errorMutex );
		if( firstError.empty() ) {
			firstError = error;
		}
	}
}

bool ShardWriter::writeManifest( std::string* error )
{
	const fs::path path = fs::path( outputDir ) / "manifest.json";
	std::FILE* f = std::fopen( path.string().c_str(), "wb" );
	if( f == nullptr ) {
		if( error != nullptr ) {
			*error = "cannot create " + path.string();
		}
		return false;
	}

	std::ostringstream json;
	json << "{\n";
	json << "  \"format\": \"OCRB-shard\",\n";
	json << "  \"version\": " << ShardVersion << ",\n";
	json << "  \"seed\": " << seed << ",\n";
	json << "  \"image_width\": " << targetWidth << ",\n";
	json << "  \"image_height\": " << targetHeight << ",\n";
	json << "  \"image_channels\": " << channels << ",\n";
	json << "  \"charset\": \"" << jsonEscape( charset ) << "\",\n";
	json << "  \"total_records\": " << writtenRecords << ",\n";
	json << "  \"shards\": [\n";
	for( size_t i = 0; i < shardInfos.size(); ++i ) {
		const ShardInfo& info = shardInfos[i];
		json << "    { \"file\": \"" << jsonEscape( info.file ) << "\", \"records\": " << info.records
			<< ", \"bytes\": " << info.bytes << ", \"sha256\": \"" << info.sha256 << "\" }";
		json << ( i + 1 == shardInfos.size() ? "\n" : ",\n" );
	}
	json << "  ]\n";
	json << "}\n";

	const std::string text = json.str();
	const bool ok = std::fwrite( text.data(), 1, text.size(), f ) == text.size();
	std::fclose( f );
	if( !ok && error != nullptr ) {
		*error = "short write to " + path.string();
	}
	return ok;
}

bool ShardWriter::finish( std::string* error )
{
	recordQueue.close();
	if( thread.joinable() ) {
		thread.join();
	}

	std::string storedError;
	{
		std::lock_guard<std::mutex> lock( errorMutex );
		storedError = firstError;
	}
	if( !storedError.empty() ) {
		if( error != nullptr ) {
			*error = storedError;
		}
		return false;
	}

	if( !writeManifest( error ) ) {
		return false;
	}
	return true;
}

} // namespace ocr
