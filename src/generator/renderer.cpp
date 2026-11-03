#include "generator/renderer.h"

#include "common/sha256.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ocr {

namespace {

inline float srgbToLinear( float c )
{
	return ( c <= 0.04045f ) ? ( c / 12.92f ) : std::pow( ( c + 0.055f ) / 1.055f, 2.4f );
}

// Decodes UTF-8 into code points; the generator's charset is ASCII by default
// but labels may contain any UTF-8 the fonts support.
std::vector<uint32_t> decodeUtf8( const std::string& text )
{
	std::vector<uint32_t> out;
	size_t i = 0;
	while( i < text.size() ) {
		const unsigned char c = static_cast<unsigned char>( text[i] );
		uint32_t cp = 0;
		int extra = 0;
		if( c < 0x80 ) {
			cp = c;
		} else if( ( c & 0xE0 ) == 0xC0 ) {
			cp = c & 0x1F;
			extra = 1;
		} else if( ( c & 0xF0 ) == 0xE0 ) {
			cp = c & 0x0F;
			extra = 2;
		} else if( ( c & 0xF8 ) == 0xF0 ) {
			cp = c & 0x07;
			extra = 3;
		} else {
			++i; // invalid lead byte, skip
			continue;
		}
		if( i + extra >= text.size() ) {
			break;
		}
		bool ok = true;
		for( int k = 1; k <= extra; ++k ) {
			const unsigned char cc = static_cast<unsigned char>( text[i + k] );
			if( ( cc & 0xC0 ) != 0x80 ) {
				ok = false;
				break;
			}
			cp = ( cp << 6 ) | ( cc & 0x3F );
		}
		if( ok ) {
			out.push_back( cp );
		}
		i += extra + 1;
	}
	return out;
}

// Rasterizes the line into an 8-bit alpha mask, cropping to the ink bounding box.
bool rasterizeLine( FT_Face face, const std::vector<uint32_t>& codePoints, int letterSpacing,
	float italicShear, Image& maskOut )
{
	const int ascender = static_cast<int>( face->size->metrics.ascender >> 6 );
	const int descender = static_cast<int>( face->size->metrics.descender >> 6 );
	const int lineHeight = std::max( 4, ascender - descender );
	const int padding = 4;

	// First pass: measure the advance width so the canvas is big enough.
	int advanceSum = 0;
	for( uint32_t cp : codePoints ) {
		if( FT_Load_Char( face, cp, FT_LOAD_DEFAULT ) != 0 ) {
			return false;
		}
		advanceSum += static_cast<int>( face->glyph->advance.x >> 6 );
	}
	advanceSum += letterSpacing * std::max( 0, static_cast<int>( codePoints.size() ) - 1 );
	advanceSum += static_cast<int>( std::fabs( italicShear ) * lineHeight );
	advanceSum = std::max( advanceSum, 4 );

	const int canvasW = advanceSum + 2 * padding + 2;
	const int canvasH = lineHeight + 2 * padding + 2;

	Image mask( canvasW, canvasH, 1 );
	const int baseline = padding + ascender;

	int penX = padding;
	for( uint32_t cp : codePoints ) {
		if( FT_Load_Char( face, cp, FT_LOAD_RENDER ) != 0 ) {
			return false;
		}
		const FT_GlyphSlot glyph = face->glyph;
		const int bitmapW = static_cast<int>( glyph->bitmap.width );
		const int bitmapH = static_cast<int>( glyph->bitmap.rows );
		const int left = static_cast<int>( glyph->bitmap_left );
		const int top = static_cast<int>( glyph->bitmap_top );

		for( int y = 0; y < bitmapH; ++y ) {
			const int dy = baseline - top + y;
			if( dy < 0 || dy >= canvasH ) {
				continue;
			}
			uint8_t* dstRow = mask.row( dy );
			const uint8_t* srcRow = glyph->bitmap.buffer + static_cast<size_t>( y ) * glyph->bitmap.pitch;
			for( int x = 0; x < bitmapW; ++x ) {
				const int dx = penX + left + x;
				if( dx < 0 || dx >= canvasW ) {
					continue;
				}
				// Take the maximum so overlapping glyphs do not darken each other.
				dstRow[dx] = std::max( dstRow[dx], srcRow[x] );
			}
		}

		penX += static_cast<int>( glyph->advance.x >> 6 ) + letterSpacing;
	}

	// Synthetic italic: shift each row horizontally, more at the top.
	if( std::fabs( italicShear ) > 1e-4f ) {
		const int extra = static_cast<int>( std::floor( std::fabs( italicShear ) * canvasH ) ) + 2;
		Image sheared( canvasW + extra, canvasH, 1 );
		for( int y = 0; y < canvasH; ++y ) {
			const int shift = static_cast<int>( italicShear * ( canvasH - 1 - y ) );
			const int base = std::max( 0, -shift );
			const uint8_t* srcRow = mask.row( y );
			uint8_t* dstRow = sheared.row( y );
			for( int x = 0; x < canvasW; ++x ) {
				const int dx = x + base + shift;
				if( dx >= 0 && dx < sheared.width ) {
					dstRow[dx] = std::max( dstRow[dx], srcRow[x] );
				}
			}
		}
		mask = std::move( sheared );
	}

	// Crop to the ink bounding box.
	int minX = mask.width, minY = mask.height, maxX = -1, maxY = -1;
	for( int y = 0; y < mask.height; ++y ) {
		const uint8_t* row = mask.row( y );
		for( int x = 0; x < mask.width; ++x ) {
			if( row[x] > 0 ) {
				minX = std::min( minX, x );
				maxX = std::max( maxX, x );
				minY = std::min( minY, y );
				maxY = std::max( maxY, y );
			}
		}
	}
	if( maxX < minX || maxY < minY ) {
		return false; // nothing drawn (e.g. only spaces)
	}

	Image cropped( maxX - minX + 1, maxY - minY + 1, 1 );
	for( int y = 0; y < cropped.height; ++y ) {
		std::memcpy( cropped.row( y ), mask.row( minY + y ) + minX, static_cast<size_t>( cropped.width ) );
	}
	maskOut = std::move( cropped );
	return true;
}

} // namespace

float relativeLuminance( const uint8_t* rgb )
{
	const float r = srgbToLinear( rgb[0] / 255.f );
	const float g = srgbToLinear( rgb[1] / 255.f );
	const float b = srgbToLinear( rgb[2] / 255.f );
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float contrastRatio( float l1, float l2 )
{
	const float lighter = std::max( l1, l2 );
	const float darker = std::min( l1, l2 );
	return ( lighter + 0.05f ) / ( darker + 0.05f );
}

const char* describe( RenderFailure failure )
{
	switch( failure ) {
	case RenderFailure::None: return "ok";
	case RenderFailure::NoFace: return "font could not be loaded";
	case RenderFailure::EmptyText: return "empty text";
	case RenderFailure::MissingGlyph: return "font lacks a glyph for the label";
	case RenderFailure::RasterizeFailed: return "rasterization produced no ink";
	}
	return "unknown";
}

bool renderSample( FontCache& fonts, const BackgroundPool& backgrounds, Rng& rng,
	int fontIndex, const std::string& text, const RenderSettings& settings, Image& out,
	RenderFailure* failure )
{
	auto fail = [&]( RenderFailure reason ) {
		if( failure != nullptr ) {
			*failure = reason;
		}
		return false;
	};
	if( failure != nullptr ) {
		*failure = RenderFailure::None;
	}

	const int pixelHeight = rng.uniformInt( settings.fontSizeMin, settings.fontSizeMax );
	FT_Face face = fonts.faceAt( fontIndex, pixelHeight );
	if( face == nullptr ) {
		return fail( RenderFailure::NoFace );
	}

	const std::vector<uint32_t> codePoints = decodeUtf8( text );
	if( codePoints.empty() ) {
		return fail( RenderFailure::EmptyText );
	}

	// Every character must exist in this font, otherwise the label would not
	// describe what is drawn.
	for( uint32_t cp : codePoints ) {
		if( FT_Get_Char_Index( face, cp ) == 0 ) {
			return fail( RenderFailure::MissingGlyph );
		}
	}

	const int letterSpacing = rng.uniformInt( settings.charSpacingMin, settings.charSpacingMax );
	const bool wantItalic = rng.chance( settings.italicProbability );
	const bool faceIsItalic = ( face->style_flags & FT_STYLE_FLAG_ITALIC ) != 0;
	const float shear = ( wantItalic && !faceIsItalic ) ? rng.uniform( 0.16f, 0.34f ) : 0.f;

	Image mask;
	if( !rasterizeLine( face, codePoints, letterSpacing, shear, mask ) ) {
		return fail( RenderFailure::RasterizeFailed );
	}

	// Background patch with a small random margin around the text.
	const int margin = rng.uniformInt( 1, 4 );
	Image canvas = backgrounds.sample( mask.width + 2 * margin, mask.height + 2 * margin, rng );

	// Mean background luminance, used to pick a readable foreground colour.
	float bgLumaSum = 0.f;
	for( int y = 0; y < canvas.height; ++y ) {
		const uint8_t* row = canvas.row( y );
		for( int x = 0; x < canvas.width; ++x ) {
			bgLumaSum += relativeLuminance( row + x * 3 );
		}
	}
	const float bgLuma = bgLumaSum / ( canvas.width * canvas.height );

	float fg[3] = { 0.f, 0.f, 0.f };
	bool foundColor = false;
	for( int attempt = 0; attempt < 32 && !foundColor; ++attempt ) {
		const float candidate[3] = {
			static_cast<float>( rng.uniformInt( 0, 255 ) ),
			static_cast<float>( rng.uniformInt( 0, 255 ) ),
			static_cast<float>( rng.uniformInt( 0, 255 ) )
		};
		const uint8_t rgb[3] = {
			static_cast<uint8_t>( candidate[0] ),
			static_cast<uint8_t>( candidate[1] ),
			static_cast<uint8_t>( candidate[2] )
		};
		if( contrastRatio( relativeLuminance( rgb ), bgLuma ) >= settings.minContrast ) {
			fg[0] = candidate[0];
			fg[1] = candidate[1];
			fg[2] = candidate[2];
			foundColor = true;
		}
	}
	if( !foundColor ) {
		// Fall back to whichever extreme contrasts best with the background.
		const float blackContrast = contrastRatio( 0.f, bgLuma );
		const float whiteContrast = contrastRatio( 1.f, bgLuma );
		const float v = ( whiteContrast > blackContrast ) ? 255.f : 0.f;
		fg[0] = fg[1] = fg[2] = v;
	}

	// Alpha-composite the glyph mask over the background.
	for( int y = 0; y < mask.height; ++y ) {
		const uint8_t* maskRow = mask.row( y );
		uint8_t* canvasRow = canvas.row( y + margin ) + static_cast<size_t>( margin ) * 3;
		for( int x = 0; x < mask.width; ++x ) {
			const float a = maskRow[x] / 255.f;
			if( a <= 0.f ) {
				continue;
			}
			for( int c = 0; c < 3; ++c ) {
				const float bg = canvasRow[x * 3 + c];
				canvasRow[x * 3 + c] = static_cast<uint8_t>( fg[c] * a + bg * ( 1.f - a ) + 0.5f );
			}
		}
	}

	// Fit into the network input geometry, preserving aspect ratio.
	const float targetW = static_cast<float>( settings.targetWidth );
	const float targetH = static_cast<float>( settings.targetHeight );
	const float usableW = targetW - 4.f;
	const float usableH = targetH - 4.f;
	const float scale = std::min( usableW / canvas.width, usableH / canvas.height );

	const float drawnW = canvas.width * scale;
	const float drawnH = canvas.height * scale;
	const float slackX = targetW - drawnW;
	const float slackY = targetH - drawnH;
	const float offsetX = ( slackX > 0.f ) ? rng.uniform( 0.f, slackX ) : 0.f;
	const float offsetY = ( slackY > 0.f ) ? rng.uniform( 0.f, slackY ) : 0.f;

	// dst = scale * src + offset  =>  dstToSrc = (dst - offset) / scale
	Mat3 dstToSrc = identity3();
	dstToSrc.m[0] = 1.f / scale;
	dstToSrc.m[4] = 1.f / scale;
	dstToSrc.m[2] = -offsetX / scale;
	dstToSrc.m[5] = -offsetY / scale;

	warp( canvas, out, settings.targetWidth, settings.targetHeight, dstToSrc );
	return true;
}

} // namespace ocr
