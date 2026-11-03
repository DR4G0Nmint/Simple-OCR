#include "generator/verify.h"

#include "common/image.h"
#include "common/sha256.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace ocr {

namespace fs = std::filesystem;

namespace {

struct ShardSummary {
	uint32_t version = 0;
	uint32_t declaredRecords = 0;
	uint32_t scannedRecords = 0;
	std::string sha256;
	bool structureOk = false;
	bool checksumOk = true; // no manifest entry => nothing to compare against
};

class Cursor {
public:
	Cursor( const std::vector<uint8_t>& data ) : data( data ) {}

	bool readU32( uint32_t& value )
	{
		if( pos + 4 > data.size() ) {
			return false;
		}
		value = static_cast<uint32_t>( data[pos] ) | ( static_cast<uint32_t>( data[pos + 1] ) << 8 )
			| ( static_cast<uint32_t>( data[pos + 2] ) << 16 ) | ( static_cast<uint32_t>( data[pos + 3] ) << 24 );
		pos += 4;
		return true;
	}

	bool readBytes( size_t count, const uint8_t*& out )
	{
		if( pos + count > data.size() ) {
			return false;
		}
		out = data.data() + pos;
		pos += count;
		return true;
	}

	size_t offset() const { return pos; }
	size_t remaining() const { return data.size() - pos; }

private:
	const std::vector<uint8_t>& data;
	size_t pos = 0;
};

std::string sanitizeForFileName( const std::string& label )
{
	std::string out;
	for( char c : label ) {
		const unsigned char uc = static_cast<unsigned char>( c );
		if( ( uc >= '0' && uc <= '9' ) || ( uc >= 'a' && uc <= 'z' ) || ( uc >= 'A' && uc <= 'Z' ) ) {
			out.push_back( c );
		} else {
			out.push_back( '_' );
		}
	}
	if( out.size() > 40 ) {
		out.resize( 40 );
	}
	return out;
}

bool writePnm( const std::string& path, const Image& img )
{
	std::FILE* f = std::fopen( path.c_str(), "wb" );
	if( f == nullptr ) {
		return false;
	}
	const bool isGray = img.channels == 1;
	std::fprintf( f, "P%c\n%d %d\n255\n", isGray ? '5' : '6', img.width, img.height );
	const bool ok = std::fwrite( img.pixels.data(), 1, img.pixels.size(), f ) == img.pixels.size();
	std::fclose( f );
	return ok;
}

// The manifest is machine-written with a fixed layout, so scanning for the
// "file"/"sha256" pairs avoids pulling in a JSON parser.
void loadManifestChecksums( const std::string& manifestPath, std::vector<std::pair<std::string, std::string>>& out )
{
	std::ifstream in( manifestPath );
	if( !in ) {
		return;
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	const std::string text = buffer.str();

	size_t pos = 0;
	while( ( pos = text.find( "\"file\"", pos ) ) != std::string::npos ) {
		const size_t colon = text.find( ':', pos );
		if( colon == std::string::npos ) {
			break;
		}
		const size_t nameStart = text.find( '"', colon );
		if( nameStart == std::string::npos ) {
			break;
		}
		const size_t nameEnd = text.find( '"', nameStart + 1 );
		if( nameEnd == std::string::npos ) {
			break;
		}
		const size_t hashKey = text.find( "\"sha256\"", nameEnd );
		if( hashKey == std::string::npos ) {
			break;
		}
		const size_t hashColon = text.find( ':', hashKey );
		const size_t hashStart = text.find( '"', hashColon );
		if( hashStart == std::string::npos ) {
			break;
		}
		const size_t hashEnd = text.find( '"', hashStart + 1 );
		if( hashEnd == std::string::npos ) {
			break;
		}
		out.emplace_back( text.substr( nameStart + 1, nameEnd - nameStart - 1 ),
			text.substr( hashStart + 1, hashEnd - hashStart - 1 ) );
		pos = hashEnd;
	}
}

std::string hexOf( const uint8_t* digest, int size )
{
	static const char* hex = "0123456789abcdef";
	std::string result;
	for( int i = 0; i < size; ++i ) {
		result.push_back( hex[digest[i] >> 4] );
		result.push_back( hex[digest[i] & 0xF] );
	}
	return result;
}

bool readWholeFile( const std::string& path, std::vector<uint8_t>& out )
{
	std::ifstream in( path, std::ios::binary );
	if( !in ) {
		return false;
	}
	in.seekg( 0, std::ios::end );
	const std::streamoff size = in.tellg();
	in.seekg( 0, std::ios::beg );
	if( size < 0 ) {
		return false;
	}
	out.resize( static_cast<size_t>( size ) );
	if( size > 0 ) {
		in.read( reinterpret_cast<char*>( out.data() ), size );
	}
	return static_cast<bool>( in ) || in.eof();
}

// Validates one shard and, when dumpEvery > 0, exports every dumpEvery-th
// record as a PNM image for visual inspection.
bool scanShard( const std::string& path, ShardSummary& summary, const std::string& dumpDir,
	uint32_t dumpEvery, uint32_t& dumpedSoFar, std::vector<std::string>& labelLines, uint64_t& totalLabelChars )
{
	std::vector<uint8_t> data;
	if( !readWholeFile( path, data ) ) {
		return false;
	}

	uint8_t digest[Sha256::DigestSize];
	Sha256 hash;
	hash.update( data.data(), data.size() );
	hash.finish( digest );
	summary.sha256 = hexOf( digest, Sha256::DigestSize );

	Cursor cursor( data );
	const uint8_t* magic = nullptr;
	if( !cursor.readBytes( 4, magic ) ) {
		return false;
	}
	const bool magicOk = std::memcmp( magic, "OCRB", 4 ) == 0;

	if( !cursor.readU32( summary.version ) || !cursor.readU32( summary.declaredRecords ) ) {
		return false;
	}

	for( uint32_t i = 0; i < summary.declaredRecords; ++i ) {
		uint32_t labelLen = 0;
		const uint8_t* labelBytes = nullptr;
		uint32_t h = 0, w = 0, ch = 0;
		if( !cursor.readU32( labelLen ) || labelLen > 4096 ) {
			break;
		}
		if( !cursor.readBytes( labelLen, labelBytes ) ) {
			break;
		}
		if( !cursor.readU32( h ) || !cursor.readU32( w ) || !cursor.readU32( ch ) ) {
			break;
		}
		if( h == 0 || w == 0 || ( ch != 1 && ch != 3 ) || h > 4096 || w > 4096 ) {
			break;
		}
		const size_t pixelBytes = static_cast<size_t>( h ) * w * ch;
		const uint8_t* pixels = nullptr;
		if( !cursor.readBytes( pixelBytes, pixels ) ) {
			break;
		}

		++summary.scannedRecords;
		totalLabelChars += labelLen;

		if( dumpDir.empty() || dumpEvery == 0 || ( summary.scannedRecords % dumpEvery ) != 0 ) {
			continue;
		}

		Image img( static_cast<int>( w ), static_cast<int>( h ), static_cast<int>( ch ) );
		std::memcpy( img.pixels.data(), pixels, pixelBytes );

		const std::string label( reinterpret_cast<const char*>( labelBytes ), labelLen );
		char name[160];
		std::snprintf( name, sizeof( name ), "%04u_%s.pnm", dumpedSoFar, sanitizeForFileName( label ).c_str() );
		if( writePnm( ( fs::path( dumpDir ) / name ).string(), img ) ) {
			labelLines.push_back( std::string( name ) + "\t" + label );
		}
		++dumpedSoFar;
	}

	// No trailing bytes: the declared record count must account for the file.
	summary.structureOk = magicOk && summary.scannedRecords == summary.declaredRecords
		&& cursor.remaining() == 0;
	return true;
}

} // namespace

bool verifyShards( const VerifyOptions& options, std::string* error )
{
	std::error_code ec;
	if( !fs::exists( options.shardDir, ec ) ) {
		if( error != nullptr ) {
			*error = "shard directory does not exist: " + options.shardDir;
		}
		return false;
	}

	std::vector<std::string> shardPaths;
	for( fs::directory_iterator it( options.shardDir, ec ), end; it != end; it.increment( ec ) ) {
		if( ec ) {
			break;
		}
		if( it->is_regular_file() && it->path().extension() == ".ocrb" ) {
			shardPaths.push_back( it->path().string() );
		}
	}
	std::sort( shardPaths.begin(), shardPaths.end() );

	if( shardPaths.empty() ) {
		if( error != nullptr ) {
			*error = "no .ocrb shards found in " + options.shardDir;
		}
		return false;
	}

	std::vector<std::pair<std::string, std::string>> manifestHashes;
	loadManifestChecksums( ( fs::path( options.shardDir ) / "manifest.json" ).string(), manifestHashes );

	if( !options.dumpDir.empty() ) {
		fs::create_directories( options.dumpDir, ec );
	}

	// First pass: count records so sample dumps spread over the dataset.
	uint64_t totalRecords = 0;
	uint64_t totalChars = 0;
	std::vector<std::string> ignored;
	for( const std::string& path : shardPaths ) {
		ShardSummary summary;
		uint32_t dumped = 0;
		if( !scanShard( path, summary, std::string(), 0, dumped, ignored, totalChars ) ) {
			if( error != nullptr ) {
				*error = "cannot read shard " + path;
			}
			return false;
		}
		totalRecords += summary.scannedRecords;
	}

	const uint32_t dumpEvery = ( options.sampleCount > 0 && totalRecords > 0 )
		? static_cast<uint32_t>( std::max<uint64_t>( 1, totalRecords / options.sampleCount ) )
		: 0;

	uint64_t records = 0;
	uint64_t labelChars = 0;
	bool allStructureOk = true;
	bool allChecksumsOk = true;
	uint32_t dumped = 0;
	std::vector<std::string> labelLines;

	std::printf( "%-22s %8s %8s %-10s %s\n", "shard", "records", "version", "structure", "sha256" );
	for( const std::string& path : shardPaths ) {
		ShardSummary summary;
		if( !scanShard( path, summary, options.dumpDir, dumpEvery, dumped, labelLines, labelChars ) ) {
			if( error != nullptr ) {
				*error = "cannot read shard " + path;
			}
			return false;
		}

		const std::string fileName = fs::path( path ).filename().string();
		for( const auto& entry : manifestHashes ) {
			if( entry.first == fileName && entry.second != summary.sha256 ) {
				summary.checksumOk = false;
			}
		}

		records += summary.scannedRecords;
		allStructureOk = allStructureOk && summary.structureOk;
		allChecksumsOk = allChecksumsOk && summary.checksumOk;

		std::printf( "%-22s %8u %8u %-10s %s%s\n", fileName.c_str(), summary.scannedRecords,
			summary.version, summary.structureOk ? "ok" : "BAD", summary.sha256.c_str(),
			summary.checksumOk ? "" : "  (MISMATCH)" );
	}

	if( !options.dumpDir.empty() ) {
		std::ofstream labels( ( fs::path( options.dumpDir ) / "labels.txt" ).string() );
		for( const std::string& line : labelLines ) {
			labels << line << "\n";
		}
	}

	const double avgLabel = ( records > 0 ) ? static_cast<double>( labelChars ) / records : 0.0;
	std::printf( "\nshards=%zu records=%llu avg_label_len=%.2f\n", shardPaths.size(),
		static_cast<unsigned long long>( records ), avgLabel );
	std::printf( "structure=%s checksums=%s\n", allStructureOk ? "ok" : "FAILED",
		allChecksumsOk ? "ok" : "MISMATCH" );
	if( !options.dumpDir.empty() ) {
		std::printf( "dumped %u sample images to %s (see labels.txt)\n", dumped, options.dumpDir.c_str() );
	}

	if( !allStructureOk || !allChecksumsOk ) {
		if( error != nullptr ) {
			*error = "shard verification failed";
		}
		return false;
	}
	return true;
}

} // namespace ocr
