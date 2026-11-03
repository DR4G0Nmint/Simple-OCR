#include "generator/augment.h"

#include "generator/jpeg_artifacts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ocr {

namespace {

inline uint8_t clampToByte( float v )
{
	return static_cast<uint8_t>( v <= 0.f ? 0 : ( v >= 255.f ? 255 : v + 0.5f ) );
}

Mat3 translation( float tx, float ty )
{
	Mat3 m = identity3();
	m.m[2] = tx;
	m.m[5] = ty;
	return m;
}

Mat3 rotation( float radians, float cx, float cy )
{
	const float c = std::cos( radians );
	const float s = std::sin( radians );
	Mat3 m = identity3();
	m.m[0] = c;  m.m[1] = -s; m.m[2] = cx - c * cx + s * cy;
	m.m[3] = s;  m.m[4] = c;  m.m[5] = cy - s * cx - c * cy;
	return m;
}

Mat3 shearX( float k, float cy )
{
	Mat3 m = identity3();
	m.m[1] = k;
	m.m[5] = -k * cy;
	return m;
}

void applyGeometric( Image& img, Rng& rng, const AugmentSettings& s, float severity )
{
	const float w = static_cast<float>( img.width );
	const float h = static_cast<float>( img.height );
	const float cx = w * 0.5f;
	const float cy = h * 0.5f;

	Mat3 transform = identity3();
	bool required = false;

	if( rng.chance( s.rotationP ) ) {
		const float maxDeg = s.rotationMaxDegrees * std::min( 1.f, severity );
		const float deg = rng.uniform( -maxDeg, maxDeg );
		transform = rotation( deg * 3.14159265358979f / 180.f, cx, cy );
		required = true;
	}

	if( rng.chance( s.shearP ) ) {
		const float k = rng.uniform( -s.shearMax, s.shearMax ) * std::min( 1.f, severity );
		transform = multiply3( shearX( k, cy ), transform );
		required = true;
	}

	if( rng.chance( s.perspectiveP ) ) {
		const float jitter = s.perspectiveMaxJitterFraction * std::min( w, h ) * std::min( 1.f, severity );
		const float src[8] = { 0.f, 0.f, w, 0.f, w, h, 0.f, h };
		const float dst[8] = {
			rng.uniform( 0.f, jitter ), rng.uniform( 0.f, jitter ),
			w - rng.uniform( 0.f, jitter ), rng.uniform( 0.f, jitter ),
			w - rng.uniform( 0.f, jitter ), h - rng.uniform( 0.f, jitter ),
			rng.uniform( 0.f, jitter ), h - rng.uniform( 0.f, jitter )
		};
		bool ok = false;
		const Mat3 perspectiveMatrix = homographyFromPoints( src, dst, &ok );
		if( ok ) {
			transform = multiply3( perspectiveMatrix, transform );
			required = true;
		}
	}

	if( !required ) {
		return;
	}

	bool invertible = false;
	const Mat3 dstToSrc = invert3( transform, &invertible );
	if( !invertible ) {
		return;
	}

	Image warped;
	warp( img, warped, img.width, img.height, dstToSrc );
	img = std::move( warped );
}

void gaussianBlur( Image& img, float sigma )
{
	if( sigma <= 0.05f ) {
		return;
	}
	const int radius = std::max( 1, static_cast<int>( std::ceil( sigma * 2.5f ) ) );
	std::vector<float> kernel( static_cast<size_t>( 2 * radius + 1 ) );
	float sum = 0.f;
	for( int i = -radius; i <= radius; ++i ) {
		const float v = std::exp( -( i * i ) / ( 2.f * sigma * sigma ) );
		kernel[static_cast<size_t>( i + radius )] = v;
		sum += v;
	}
	for( float& v : kernel ) {
		v /= sum;
	}

	Image tmp( img.width, img.height, img.channels );

	// Horizontal pass.
	for( int y = 0; y < img.height; ++y ) {
		const uint8_t* srcRow = img.row( y );
		uint8_t* dstRow = tmp.row( y );
		for( int x = 0; x < img.width; ++x ) {
			float acc[3] = { 0.f, 0.f, 0.f };
			for( int k = -radius; k <= radius; ++k ) {
				const int sx = std::max( 0, std::min( img.width - 1, x + k ) );
				const float wgt = kernel[static_cast<size_t>( k + radius )];
				for( int c = 0; c < img.channels; ++c ) {
					acc[c] += srcRow[sx * img.channels + c] * wgt;
				}
			}
			for( int c = 0; c < img.channels; ++c ) {
				dstRow[x * img.channels + c] = clampToByte( acc[c] );
			}
		}
	}

	// Vertical pass.
	for( int y = 0; y < img.height; ++y ) {
		uint8_t* dstRow = img.row( y );
		for( int x = 0; x < img.width; ++x ) {
			float acc[3] = { 0.f, 0.f, 0.f };
			for( int k = -radius; k <= radius; ++k ) {
				const int sy = std::max( 0, std::min( img.height - 1, y + k ) );
				const float wgt = kernel[static_cast<size_t>( k + radius )];
				const uint8_t* srcRow = tmp.row( sy );
				for( int c = 0; c < img.channels; ++c ) {
					acc[c] += srcRow[x * img.channels + c] * wgt;
				}
			}
			for( int c = 0; c < img.channels; ++c ) {
				dstRow[x * img.channels + c] = clampToByte( acc[c] );
			}
		}
	}
}

void motionBlur( Image& img, float angle, int length )
{
	if( length < 2 ) {
		return;
	}
	const float dx = std::cos( angle );
	const float dy = std::sin( angle );

	Image src = img;
	const float half = ( length - 1 ) * 0.5f;

	for( int y = 0; y < img.height; ++y ) {
		uint8_t* dstRow = img.row( y );
		for( int x = 0; x < img.width; ++x ) {
			float acc[3] = { 0.f, 0.f, 0.f };
			for( int i = 0; i < length; ++i ) {
				const float t = i - half;
				const int sx = std::max( 0, std::min( img.width - 1, static_cast<int>( x + dx * t + 0.5f ) ) );
				const int sy = std::max( 0, std::min( img.height - 1, static_cast<int>( y + dy * t + 0.5f ) ) );
				const uint8_t* srcPix = src.row( sy ) + static_cast<size_t>( sx ) * src.channels;
				for( int c = 0; c < img.channels; ++c ) {
					acc[c] += srcPix[c];
				}
			}
			for( int c = 0; c < img.channels; ++c ) {
				dstRow[x * img.channels + c] = clampToByte( acc[c] / length );
			}
		}
	}
}

void applyPhotometric( Image& img, Rng& rng, const AugmentSettings& s, float severity )
{
	const float scale = std::min( 1.f, severity );
	float brightness = 0.f;
	float contrast = 1.f;
	float gamma = 1.f;

	if( rng.chance( s.brightnessP ) ) {
		brightness = rng.uniform( -s.brightnessAmount, s.brightnessAmount ) * scale * 255.f;
	}
	if( rng.chance( s.contrastP ) ) {
		contrast = 1.f + rng.uniform( -s.contrastAmount, s.contrastAmount ) * scale;
	}
	if( rng.chance( s.gammaP ) ) {
		gamma = rng.uniform( s.gammaMin, s.gammaMax );
		// Pull gamma back toward 1 with severity so weak samples stay weak.
		gamma = 1.f + ( gamma - 1.f ) * scale;
	}

	if( brightness == 0.f && contrast == 1.f && std::fabs( gamma - 1.f ) < 1e-4f ) {
		return;
	}

	uint8_t lut[256];
	for( int i = 0; i < 256; ++i ) {
		float v = i / 255.f;
		v = std::pow( v, gamma );
		v = ( v - 0.5f ) * contrast + 0.5f;
		v = v * 255.f + brightness;
		lut[i] = clampToByte( v );
	}

	for( size_t i = 0; i < img.pixels.size(); ++i ) {
		img.pixels[i] = lut[img.pixels[i]];
	}
}

void applyNoise( Image& img, Rng& rng, const AugmentSettings& s, float severity )
{
	const float scale = std::min( 1.f, severity );

	if( rng.chance( s.gaussianNoiseP ) ) {
		const float sigma = rng.uniform( 2.f, s.gaussianNoiseMaxSigma ) * scale;
		if( sigma > 0.1f ) {
			for( size_t i = 0; i < img.pixels.size(); ++i ) {
				const float v = img.pixels[i] + rng.normal() * sigma;
				img.pixels[i] = clampToByte( v );
			}
		}
	}

	if( rng.chance( s.saltPepperP ) ) {
		const float fraction = rng.uniform( 0.0005f, s.saltPepperMaxFraction ) * scale;
		const size_t total = static_cast<size_t>( img.width ) * img.height;
		const size_t count = static_cast<size_t>( fraction * total );
		for( size_t i = 0; i < count; ++i ) {
			const int x = rng.uniformInt( 0, img.width - 1 );
			const int y = rng.uniformInt( 0, img.height - 1 );
			const uint8_t v = rng.chance( 0.5f ) ? 0 : 255;
			uint8_t* pix = img.pixel( x, y );
			for( int c = 0; c < img.channels; ++c ) {
				pix[c] = v;
			}
		}
	}
}

void applyScanlines( Image& img, Rng& rng, float severity )
{
	const float darken = 1.f - rng.uniform( 0.08f, 0.25f ) * std::min( 1.f, severity );
	const int period = rng.uniformInt( 2, 4 );
	const int phase = rng.uniformInt( 0, period - 1 );

	for( int y = 0; y < img.height; ++y ) {
		if( ( ( y + phase ) % period ) != 0 ) {
			continue;
		}
		uint8_t* row = img.row( y );
		for( int i = 0; i < img.width * img.channels; ++i ) {
			row[i] = clampToByte( row[i] * darken );
		}
	}

	// Occasional bright scanline, as produced by interlaced capture.
	if( rng.chance( 0.5f ) ) {
		const int y = rng.uniformInt( 0, img.height - 1 );
		uint8_t* row = img.row( y );
		for( int i = 0; i < img.width * img.channels; ++i ) {
			row[i] = clampToByte( row[i] * 1.15f + 12.f );
		}
	}
}

void applyChromaticAberration( Image& img, Rng& rng, float severity )
{
	if( img.channels < 3 ) {
		return;
	}
	const float shift = rng.uniform( 0.4f, 1.8f ) * std::min( 1.f, severity );

	Image src = img;

	for( int y = 0; y < img.height; ++y ) {
		const uint8_t* srcRow = src.row( y );
		uint8_t* dstRow = img.row( y );
		for( int x = 0; x < img.width; ++x ) {
			// Sample red shifted right and blue shifted left with linear
			// interpolation, then keep green in place.
			auto sampleChannel = [&]( float sx, int channel ) -> float {
				const int x0 = static_cast<int>( std::floor( sx ) );
				const int x1 = x0 + 1;
				const float t = sx - x0;
				const int cx0 = std::max( 0, std::min( src.width - 1, x0 ) );
				const int cx1 = std::max( 0, std::min( src.width - 1, x1 ) );
				const float a = srcRow[cx0 * 3 + channel];
				const float b = srcRow[cx1 * 3 + channel];
				return a + ( b - a ) * t;
			};

			dstRow[x * 3 + 0] = clampToByte( sampleChannel( x + shift, 0 ) );
			dstRow[x * 3 + 1] = srcRow[x * 3 + 1];
			dstRow[x * 3 + 2] = clampToByte( sampleChannel( x - shift, 2 ) );
		}
	}
}

} // namespace

void applyAugmentations( Image& img, Rng& rng, const AugmentSettings& settings, float severity )
{
	applyGeometric( img, rng, settings, severity );

	// Blur is capped harder than the other degradations: beyond ~2.5 px of sigma a
	// 32 px tall line loses the stroke structure entirely, and the sample teaches
	// the network nothing.
	if( rng.chance( settings.gaussianBlurP ) ) {
		const float sigma = rng.uniform( 0.5f, settings.gaussianSigmaMax ) * std::min( 1.25f, severity );
		gaussianBlur( img, sigma );
	}

	if( rng.chance( settings.motionBlurP ) ) {
		const float angle = rng.uniform( 0.f, 6.2831853f );
		const int length = rng.uniformInt( 2, std::max( 2, static_cast<int>( 9 * std::min( 1.f, severity ) ) ) );
		motionBlur( img, angle, length );
	}

	applyPhotometric( img, rng, settings, severity );
	applyNoise( img, rng, settings, severity );

	// Post effects are the point of the generator, so they are never skipped
	// entirely; only their strength follows the severity.
	if( rng.chance( settings.chromaticAberrationP ) ) {
		applyChromaticAberration( img, rng, severity );
	}
	if( rng.chance( settings.jpegP ) ) {
		int quality = rng.uniformInt( 25, 70 );
		quality = static_cast<int>( 100 - ( 100 - quality ) * std::min( 1.2f, severity ) );
		applyJpegArtifacts( img, std::max( 5, quality ) );
	}
	if( rng.chance( settings.scanlineP ) ) {
		applyScanlines( img, rng, severity );
	}
}

float estimateCharacterErrorRate( const Image& clean, const Image& augmented )
{
	const Image a = toGrayscale( clean );
	const Image b = toGrayscale( augmented );
	if( a.empty() || b.empty() || a.width != b.width || a.height != b.height ) {
		return 1.f;
	}

	const int total = a.width * a.height;
	double meanA = 0.0;
	double meanB = 0.0;
	for( int i = 0; i < total; ++i ) {
		meanA += a.pixels[i];
		meanB += b.pixels[i];
	}
	meanA /= total;
	meanB /= total;

	double varA = 0.0;
	for( int i = 0; i < total; ++i ) {
		const double d = a.pixels[i] - meanA;
		varA += d * d;
	}
	varA /= total;
	const double stdA = std::sqrt( varA );

	// Only the pixels that carry ink in the clean render are informative: the
	// background is shared by both images and would inflate the correlation.
	const double threshold = 0.35 * stdA;

	double sumAB = 0.0;
	double sumBB = 0.0;
	int considered = 0;
	for( int i = 0; i < total; ++i ) {
		if( std::fabs( a.pixels[i] - meanA ) < threshold ) {
			continue;
		}
		const double da = a.pixels[i] - meanA;
		const double db = b.pixels[i] - meanB;
		sumAB += da * db;
		sumBB += db * db;
		++considered;
	}

	if( considered < 8 || varA <= 1e-6 ) {
		return 1.f; // no ink, or nothing left to compare
	}

	// Correlation restricted to ink pixels, normalised by the clean energy.
	double energyA = 0.0;
	for( int i = 0; i < total; ++i ) {
		if( std::fabs( a.pixels[i] - meanA ) < threshold ) {
			continue;
		}
		const double da = a.pixels[i] - meanA;
		energyA += da * da;
	}

	const double correlation = sumAB / std::sqrt( std::max( 1e-9, energyA * sumBB ) );
	const double cer = 1.0 - std::max( 0.0, std::min( 1.0, correlation ) );
	return static_cast<float>( cer );
}

} // namespace ocr
