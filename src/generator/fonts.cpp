#include "generator/fonts.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace ocr {

namespace fs = std::filesystem;

namespace {

bool hasFontExtension( const fs::path& p )
{
	std::string ext = p.extension().string();
	std::transform( ext.begin(), ext.end(), ext.begin(),
		[]( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
	return ext == ".ttf" || ext == ".otf";
}

} // namespace

std::vector<std::string> findFontFiles( const std::string& root, std::string* error )
{
	std::vector<std::string> files;
	std::error_code ec;
	if( !fs::exists( root, ec ) ) {
		if( error != nullptr ) {
			*error = "font directory does not exist: " + root;
		}
		return files;
	}

	for( fs::recursive_directory_iterator it( root, fs::directory_options::skip_permission_denied, ec ), end;
		it != end; it.increment( ec ) )
	{
		if( ec ) {
			ec.clear();
			continue;
		}
		if( !it->is_regular_file( ec ) ) {
			continue;
		}
		if( hasFontExtension( it->path() ) ) {
			files.push_back( it->path().string() );
		}
	}

	std::sort( files.begin(), files.end() );
	return files;
}

bool FontCoverage::covers( int font, uint32_t codepoint ) const
{
	if( font < 0 || font >= static_cast<int>( coverage.size() ) ) {
		return false;
	}
	for( size_t i = 0; i < codepoints.size(); ++i ) {
		if( codepoints[i] != codepoint ) {
			continue;
		}
		return ( coverage[static_cast<size_t>( font )][i / 8] & ( 1u << ( i % 8 ) ) ) != 0;
	}
	return false;
}

bool computeFontCoverage( const std::vector<std::string>& fontFiles,
	const std::vector<uint32_t>& requiredCodepoints, FontCoverage& out, std::string* error )
{
	FT_Library library = nullptr;
	if( FT_Init_FreeType( &library ) != 0 ) {
		if( error != nullptr ) {
			*error = "FT_Init_FreeType failed while measuring font coverage";
		}
		return false;
	}

	out.codepoints = requiredCodepoints;
	out.coverage.assign( fontFiles.size(), std::vector<uint8_t>( ( requiredCodepoints.size() + 7 ) / 8, 0 ) );

	for( size_t f = 0; f < fontFiles.size(); ++f ) {
		FT_Face face = nullptr;
		if( FT_New_Face( library, fontFiles[f].c_str(), 0, &face ) != 0 || face == nullptr ) {
			continue; // unreadable font: no coverage at all
		}
		std::vector<uint8_t>& bits = out.coverage[f];
		for( size_t i = 0; i < requiredCodepoints.size(); ++i ) {
			if( FT_Get_Char_Index( face, requiredCodepoints[i] ) != 0 ) {
				bits[i / 8] |= static_cast<uint8_t>( 1u << ( i % 8 ) );
			}
		}
		FT_Done_Face( face );
	}

	FT_Done_FreeType( library );
	return true;
}

FontCache::FontCache( FT_Library library, const std::vector<std::string>& fontPaths, size_t maxCachedFaces )
	: library( library )
	, paths( fontPaths )
	, maxCachedFaces( maxCachedFaces < 8 ? 8 : maxCachedFaces )
{
}

FontCache::~FontCache()
{
	for( auto& entry : cache ) {
		if( entry.second.face != nullptr ) {
			FT_Done_Face( entry.second.face );
		}
	}
}

FT_Face FontCache::load( int index )
{
	if( index < 0 || index >= static_cast<int>( paths.size() ) ) {
		return nullptr;
	}

	FT_Face face = nullptr;
	const FT_Error err = FT_New_Face( library, paths[index].c_str(), 0, &face );
	if( err != 0 || face == nullptr ) {
		return nullptr;
	}

	// Fixed-width rendering is not what we want; sizes are set per request.
	if( !FT_IS_SCALABLE( face ) ) {
		FT_Done_Face( face );
		return nullptr;
	}

	return face;
}

FT_Face FontCache::faceAt( int index, int pixelHeight )
{
	if( pixelHeight < 6 ) {
		pixelHeight = 6;
	}
	if( pixelHeight > 512 ) {
		pixelHeight = 512;
	}

	auto it = cache.find( index );
	if( it != cache.end() ) {
		// Touch LRU.
		auto pos = lruPos.find( index );
		if( pos != lruPos.end() ) {
			lru.splice( lru.begin(), lru, pos->second );
		}
		Entry& entry = it->second;
		if( entry.lastPixelHeight != static_cast<uint32_t>( pixelHeight ) ) {
			if( FT_Set_Pixel_Sizes( entry.face, 0, static_cast<FT_UInt>( pixelHeight ) ) != 0 ) {
				return nullptr;
			}
			entry.lastPixelHeight = static_cast<uint32_t>( pixelHeight );
		}
		return entry.face;
	}

	FT_Face face = load( index );
	if( face == nullptr ) {
		return nullptr;
	}
	if( FT_Set_Pixel_Sizes( face, 0, static_cast<FT_UInt>( pixelHeight ) ) != 0 ) {
		FT_Done_Face( face );
		return nullptr;
	}

	// Evict the least recently used face if the cache is full.
	if( cache.size() >= maxCachedFaces && !lru.empty() ) {
		const int victim = lru.back();
		lru.pop_back();
		lruPos.erase( victim );
		auto victimIt = cache.find( victim );
		if( victimIt != cache.end() ) {
			if( victimIt->second.face != nullptr ) {
				FT_Done_Face( victimIt->second.face );
			}
			cache.erase( victimIt );
		}
	}

	Entry entry;
	entry.face = face;
	entry.lastPixelHeight = static_cast<uint32_t>( pixelHeight );
	cache.emplace( index, entry );
	lru.push_front( index );
	lruPos[index] = lru.begin();
	return face;
}

} // namespace ocr
