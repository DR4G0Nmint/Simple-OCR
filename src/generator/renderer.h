// Renders a text line onto a background and warps it into the network's fixed
// input geometry.
#pragma once

#include "common/image.h"
#include "common/rng.h"
#include "generator/backgrounds.h"
#include "generator/fonts.h"

#include <string>

namespace ocr {

struct RenderSettings {
	int targetWidth = 128;
	int targetHeight = 32;
	int fontSizeMin = 20;
	int fontSizeMax = 48;
	int charSpacingMin = -2;
	int charSpacingMax = 6;
	float italicProbability = 0.25f;
	// Minimum WCAG contrast ratio between foreground and background.
	float minContrast = 4.5f;
};

// Why a render attempt failed; used for diagnostics when a record cannot be
// produced at all.
enum class RenderFailure {
	None = 0,
	NoFace,
	EmptyText,
	MissingGlyph,
	RasterizeFailed
};

const char* describe( RenderFailure failure );

// Renders `text` with `fontIndex` into `out` (targetWidth x targetHeight RGB).
// Returns false when the font lacks a glyph for some character or the line
// cannot be rasterized; callers should retry with another font/text.
bool renderSample( FontCache& fonts, const BackgroundPool& backgrounds, Rng& rng,
	int fontIndex, const std::string& text, const RenderSettings& settings, Image& out,
	RenderFailure* failure = nullptr );

// WCAG 2.1 relative luminance of one sRGB pixel.
float relativeLuminance( const uint8_t* rgb );

// WCAG 2.1 contrast ratio between two relative luminances, in [1, 21].
float contrastRatio( float l1, float l2 );

} // namespace ocr
