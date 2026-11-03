#include "common/image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ocr {

void Image::allocate( int w, int h, int ch )
{
	width = w;
	height = h;
	channels = ch;
	pixels.assign( static_cast<size_t>( w > 0 ? w : 0 ) * static_cast<size_t>( h > 0 ? h : 0 )
		* static_cast<size_t>( ch > 0 ? ch : 0 ), 0 );
}

void Image::fill( uint8_t v )
{
	std::memset( pixels.data(), v, pixels.size() );
}

namespace {

inline uint8_t clampByte( float v )
{
	if( v <= 0.f ) {
		return 0;
	}
	if( v >= 255.f ) {
		return 255;
	}
	return static_cast<uint8_t>( v + 0.5f );
}

inline float lerp( float a, float b, float t ) { return a + ( b - a ) * t; }

} // namespace

Image resizeBilinear( const Image& src, int dstW, int dstH )
{
	Image dst( dstW, dstH, src.channels );
	if( src.empty() || dst.empty() ) {
		return dst;
	}

	const float scaleX = static_cast<float>( src.width ) / dstW;
	const float scaleY = static_cast<float>( src.height ) / dstH;

	for( int y = 0; y < dstH; ++y ) {
		const float sy = ( y + 0.5f ) * scaleY - 0.5f;
		int y0 = static_cast<int>( std::floor( sy ) );
		float fy = sy - y0;
		int y1 = y0 + 1;
		y0 = std::max( 0, std::min( src.height - 1, y0 ) );
		y1 = std::max( 0, std::min( src.height - 1, y1 ) );

		uint8_t* out = dst.row( y );
		const uint8_t* r0 = src.row( y0 );
		const uint8_t* r1 = src.row( y1 );

		for( int x = 0; x < dstW; ++x ) {
			const float sx = ( x + 0.5f ) * scaleX - 0.5f;
			int x0 = static_cast<int>( std::floor( sx ) );
			const float fx = sx - x0;
			int x1 = x0 + 1;
			x0 = std::max( 0, std::min( src.width - 1, x0 ) );
			x1 = std::max( 0, std::min( src.width - 1, x1 ) );

			for( int c = 0; c < src.channels; ++c ) {
				const float top = lerp( r0[x0 * src.channels + c], r0[x1 * src.channels + c], fx );
				const float bottom = lerp( r1[x0 * src.channels + c], r1[x1 * src.channels + c], fx );
				out[x * dst.channels + c] = clampByte( lerp( top, bottom, fy ) );
			}
		}
	}
	return dst;
}

Image toGrayscale( const Image& src )
{
	if( src.empty() ) {
		return Image();
	}
	if( src.channels == 1 ) {
		return src;
	}
	Image dst( src.width, src.height, 1 );
	for( int y = 0; y < src.height; ++y ) {
		const uint8_t* in = src.row( y );
		uint8_t* out = dst.row( y );
		for( int x = 0; x < src.width; ++x ) {
			const float r = in[x * 3 + 0];
			const float g = in[x * 3 + 1];
			const float b = in[x * 3 + 2];
			out[x] = clampByte( 0.299f * r + 0.587f * g + 0.114f * b );
		}
	}
	return dst;
}

Mat3 identity3()
{
	Mat3 r{};
	r.m[0] = 1.f;
	r.m[4] = 1.f;
	r.m[8] = 1.f;
	return r;
}

Mat3 multiply3( const Mat3& a, const Mat3& b )
{
	Mat3 r{};
	for( int i = 0; i < 3; ++i ) {
		for( int j = 0; j < 3; ++j ) {
			float sum = 0.f;
			for( int k = 0; k < 3; ++k ) {
				sum += a.m[i * 3 + k] * b.m[k * 3 + j];
			}
			r.m[i * 3 + j] = sum;
		}
	}
	return r;
}

Mat3 invert3( const Mat3& a, bool* ok )
{
	const float det = a.m[0] * ( a.m[4] * a.m[8] - a.m[5] * a.m[7] )
		- a.m[1] * ( a.m[3] * a.m[8] - a.m[5] * a.m[6] )
		+ a.m[2] * ( a.m[3] * a.m[7] - a.m[4] * a.m[6] );

	if( std::fabs( det ) < 1e-12f ) {
		if( ok != nullptr ) {
			*ok = false;
		}
		return identity3();
	}
	if( ok != nullptr ) {
		*ok = true;
	}

	const float inv = 1.f / det;
	Mat3 r{};
	r.m[0] = ( a.m[4] * a.m[8] - a.m[5] * a.m[7] ) * inv;
	r.m[1] = ( a.m[2] * a.m[7] - a.m[1] * a.m[8] ) * inv;
	r.m[2] = ( a.m[1] * a.m[5] - a.m[2] * a.m[4] ) * inv;
	r.m[3] = ( a.m[5] * a.m[6] - a.m[3] * a.m[8] ) * inv;
	r.m[4] = ( a.m[0] * a.m[8] - a.m[2] * a.m[6] ) * inv;
	r.m[5] = ( a.m[2] * a.m[3] - a.m[0] * a.m[5] ) * inv;
	r.m[6] = ( a.m[3] * a.m[7] - a.m[4] * a.m[6] ) * inv;
	r.m[7] = ( a.m[1] * a.m[6] - a.m[0] * a.m[7] ) * inv;
	r.m[8] = ( a.m[0] * a.m[4] - a.m[1] * a.m[3] ) * inv;
	return r;
}

Mat3 homographyFromPoints( const float src[8], const float dst[8], bool* ok )
{
	// Solves the 8x8 linear system for the homography h (h33 = 1).
	double a[8][9];
	for( int i = 0; i < 4; ++i ) {
		const double sx = src[i * 2 + 0], sy = src[i * 2 + 1];
		const double dx = dst[i * 2 + 0], dy = dst[i * 2 + 1];

		a[i * 2][0] = sx; a[i * 2][1] = sy; a[i * 2][2] = 1;
		a[i * 2][3] = 0;  a[i * 2][4] = 0;  a[i * 2][5] = 0;
		a[i * 2][6] = -dx * sx; a[i * 2][7] = -dx * sy; a[i * 2][8] = dx;

		a[i * 2 + 1][0] = 0; a[i * 2 + 1][1] = 0; a[i * 2 + 1][2] = 0;
		a[i * 2 + 1][3] = sx; a[i * 2 + 1][4] = sy; a[i * 2 + 1][5] = 1;
		a[i * 2 + 1][6] = -dy * sx; a[i * 2 + 1][7] = -dy * sy; a[i * 2 + 1][8] = dy;
	}

	for( int col = 0; col < 8; ++col ) {
		int pivot = col;
		for( int r = col + 1; r < 8; ++r ) {
			if( std::fabs( a[r][col] ) > std::fabs( a[pivot][col] ) ) {
				pivot = r;
			}
		}
		if( std::fabs( a[pivot][col] ) < 1e-12 ) {
			if( ok != nullptr ) {
				*ok = false;
			}
			return identity3();
		}
		if( pivot != col ) {
			for( int k = 0; k < 9; ++k ) {
				std::swap( a[col][k], a[pivot][k] );
			}
		}
		const double d = a[col][col];
		for( int k = col; k < 9; ++k ) {
			a[col][k] /= d;
		}
		for( int r = 0; r < 8; ++r ) {
			if( r == col ) {
				continue;
			}
			const double f = a[r][col];
			if( f == 0.0 ) {
				continue;
			}
			for( int k = col; k < 9; ++k ) {
				a[r][k] -= f * a[col][k];
			}
		}
	}

	Mat3 h{};
	for( int i = 0; i < 8; ++i ) {
		h.m[i] = static_cast<float>( a[i][8] );
	}
	h.m[8] = 1.f;
	if( ok != nullptr ) {
		*ok = true;
	}
	return h;
}

void warp( const Image& src, Image& dst, int dstW, int dstH, const Mat3& dstToSrc )
{
	dst.allocate( dstW, dstH, src.channels );
	if( src.empty() || dst.empty() ) {
		return;
	}

	for( int y = 0; y < dstH; ++y ) {
		uint8_t* out = dst.row( y );
		const float dy = y + 0.5f;
		for( int x = 0; x < dstW; ++x ) {
			const float dx = x + 0.5f;
			const float w0 = dstToSrc.m[6] * dx + dstToSrc.m[7] * dy + dstToSrc.m[8];
			float sx, sy;
			if( std::fabs( w0 ) < 1e-8f ) {
				sx = 0.f;
				sy = 0.f;
			} else {
				const float invW = 1.f / w0;
				sx = ( dstToSrc.m[0] * dx + dstToSrc.m[1] * dy + dstToSrc.m[2] ) * invW - 0.5f;
				sy = ( dstToSrc.m[3] * dx + dstToSrc.m[4] * dy + dstToSrc.m[5] ) * invW - 0.5f;
			}

			int x0 = static_cast<int>( std::floor( sx ) );
			int y0 = static_cast<int>( std::floor( sy ) );
			const float fx = sx - x0;
			const float fy = sy - y0;
			int x1 = x0 + 1;
			int y1 = y0 + 1;

			// Clamp to the border so that warped-in regions show the edge
			// content (background) rather than black.
			x0 = std::max( 0, std::min( src.width - 1, x0 ) );
			x1 = std::max( 0, std::min( src.width - 1, x1 ) );
			y0 = std::max( 0, std::min( src.height - 1, y0 ) );
			y1 = std::max( 0, std::min( src.height - 1, y1 ) );

			const uint8_t* r0 = src.row( y0 );
			const uint8_t* r1 = src.row( y1 );
			for( int c = 0; c < src.channels; ++c ) {
				const float top = lerp( r0[x0 * src.channels + c], r0[x1 * src.channels + c], fx );
				const float bottom = lerp( r1[x0 * src.channels + c], r1[x1 * src.channels + c], fx );
				out[x * dst.channels + c] = clampByte( lerp( top, bottom, fy ) );
			}
		}
	}
}

void blendOver( Image& dst, const Image& src, int offsetX, int offsetY )
{
	if( dst.channels != src.channels ) {
		return;
	}
	for( int y = 0; y < src.height; ++y ) {
		const int dy = y + offsetY;
		if( dy < 0 || dy >= dst.height ) {
			continue;
		}
		const uint8_t* in = src.row( y );
		uint8_t* out = dst.row( dy );
		for( int x = 0; x < src.width; ++x ) {
			const int dx = x + offsetX;
			if( dx < 0 || dx >= dst.width ) {
				continue;
			}
			for( int c = 0; c < dst.channels; ++c ) {
				out[dx * dst.channels + c] = in[x * src.channels + c];
			}
		}
	}
}

} // namespace ocr
