// Minimal image container and resampling/warping primitives.
// Deliberately dependency-free: the generator renders and augments in this
// format, and the inference engine reads PNG via stb_image into it.
#pragma once

#include <cstdint>
#include <vector>

namespace ocr {

struct Image {
	int width = 0;
	int height = 0;
	int channels = 0; // 1 = gray, 3 = RGB
	std::vector<uint8_t> pixels;

	Image() = default;
	Image( int w, int h, int ch ) { allocate( w, h, ch ); }

	void allocate( int w, int h, int ch );
	bool empty() const { return width <= 0 || height <= 0 || channels <= 0; }
	size_t sizeBytes() const { return pixels.size(); }

	uint8_t* row( int y ) { return pixels.data() + static_cast<size_t>( y ) * width * channels; }
	const uint8_t* row( int y ) const { return pixels.data() + static_cast<size_t>( y ) * width * channels; }

	uint8_t* pixel( int x, int y ) { return row( y ) + static_cast<size_t>( x ) * channels; }
	const uint8_t* pixel( int x, int y ) const { return row( y ) + static_cast<size_t>( x ) * channels; }

	void fill( uint8_t v );
};

// Bilinear resize into a new image.
Image resizeBilinear( const Image& src, int dstW, int dstH );

// Converts RGB to single-channel luminance (Rec. 601).
Image toGrayscale( const Image& src );

// A 3x3 projective transform stored row-major; maps destination -> source when
// used by warp(), which keeps the sampling loop cache friendly and needs no
// inversion at run time.
struct Mat3 {
	float m[9];
};

Mat3 identity3();
Mat3 multiply3( const Mat3& a, const Mat3& b );
Mat3 invert3( const Mat3& a, bool* ok = nullptr );

// Builds the homography that maps the four given source points onto the four
// destination points (both in order: TL, TR, BR, BL).
Mat3 homographyFromPoints( const float src[8], const float dst[8], bool* ok = nullptr );

// Warps src into dst (which is resized to dstW x dstH) using `dstToSrc`.
// Sampling outside the source clamps to the border, so rotated edges pick up
// the neighbouring background instead of black.
void warp( const Image& src, Image& dst, int dstW, int dstH, const Mat3& dstToSrc );

// Composite helper: draws `src` onto `dst` at full opacity (used for glyph
// rendering onto a background); both must have the same channel count.
void blendOver( Image& dst, const Image& src, int offsetX, int offsetY );

} // namespace ocr
