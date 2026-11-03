// Font discovery and per-thread FreeType face caching.
//
// FreeType's FT_Face is not thread-safe, so every worker thread owns its own
// FT_Library plus a cache of faces. Face creation is lazy and the number of
// cached faces per thread is bounded, which keeps RSS flat on long runs while
// still allowing a pool of hundreds of fonts.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace ocr {

// Recursively collects .ttf/.otf files below `root`. Results are sorted so the
// font order (and therefore a seeded run) is stable.
std::vector<std::string> findFontFiles( const std::string& root, std::string* error );

// Charset coverage of every font, measured once at startup.
//
// Script-specific faces (a large part of a typical font tree: Noto Arabic,
// Devanagari, CJK...) do not contain Latin letters or ASCII punctuation. Drawing
// a random label with a random font would therefore fail most of the time, so
// the generator picks the label characters from what the chosen font actually
// covers. `coverage[font]` is a bitset over `codepoints`.
struct FontCoverage {
	std::vector<uint32_t> codepoints;
	std::vector<std::vector<uint8_t>> coverage; // [font][bit i] => has codepoints[i]

	bool covers( int font, uint32_t codepoint ) const;
	size_t fontCount() const { return coverage.size(); }
};

bool computeFontCoverage( const std::vector<std::string>& fontFiles,
	const std::vector<uint32_t>& requiredCodepoints, FontCoverage& out, std::string* error );

class FontCache {
public:
	FontCache( FT_Library library, const std::vector<std::string>& fontPaths, size_t maxCachedFaces = 256 );
	~FontCache();

	FontCache( const FontCache& ) = delete;
	FontCache& operator=( const FontCache& ) = delete;

	size_t fontCount() const { return paths.size(); }

	// Returns a face for `index` scaled to `pixelHeight`, or nullptr if the
	// file cannot be loaded (a broken font file must not kill the run).
	FT_Face faceAt( int index, int pixelHeight );

private:
	FT_Face load( int index );

	struct Entry {
		FT_Face face = nullptr;
		uint32_t lastPixelHeight = 0;
	};

	FT_Library library;
	std::vector<std::string> paths;
	size_t maxCachedFaces;
	std::unordered_map<int, Entry> cache;
	std::list<int> lru; // front = most recently used
	std::unordered_map<int, std::list<int>::iterator> lruPos;
};

} // namespace ocr
