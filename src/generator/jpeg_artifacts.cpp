#include "generator/jpeg_artifacts.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ocr {

namespace {

// Standard JPEG luminance quantization table (ITU T.81, Annex K).
const int StdLumaQuant[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99
};

// cos( (2x+1) * u * pi / 16 ) * 0.5 * (u == 0 ? 1/sqrt(2) : 1)
struct DctTables {
	float cosTable[8][8];
	float scale[8];
	DctTables()
	{
		for( int u = 0; u < 8; ++u ) {
			for( int x = 0; x < 8; ++x ) {
				cosTable[u][x] = std::cos( ( 2 * x + 1 ) * u * 3.14159265358979f / 16.f );
			}
			scale[u] = ( u == 0 ) ? 0.35355339f : 0.5f;
		}
	}
};

const DctTables& tables()
{
	static const DctTables t;
	return t;
}

void forwardDct( const float in[64], float out[64] )
{
	const DctTables& t = tables();
	for( int v = 0; v < 8; ++v ) {
		for( int u = 0; u < 8; ++u ) {
			float sum = 0.f;
			for( int y = 0; y < 8; ++y ) {
				for( int x = 0; x < 8; ++x ) {
					sum += in[y * 8 + x] * t.cosTable[u][x] * t.cosTable[v][y];
				}
			}
			out[v * 8 + u] = sum * t.scale[u] * t.scale[v];
		}
	}
}

void inverseDct( const float in[64], float out[64] )
{
	const DctTables& t = tables();
	for( int y = 0; y < 8; ++y ) {
		for( int x = 0; x < 8; ++x ) {
			float sum = 0.f;
			for( int v = 0; v < 8; ++v ) {
				for( int u = 0; u < 8; ++u ) {
					sum += t.scale[u] * t.scale[v] * in[v * 8 + u]
						* t.cosTable[u][x] * t.cosTable[v][y];
				}
			}
			out[y * 8 + x] = sum;
		}
	}
}

float quantScaleFromQuality( int quality )
{
	quality = std::max( 1, std::min( 100, quality ) );
	if( quality < 50 ) {
		return 5000.f / quality;
	}
	return 200.f - 2.f * quality;
}

} // namespace

void applyJpegArtifacts( Image& image, int quality )
{
	if( image.empty() ) {
		return;
	}

	float quant[64];
	const float scale = quantScaleFromQuality( quality ) / 100.f;
	for( int i = 0; i < 64; ++i ) {
		quant[i] = std::max( 1.f, std::floor( StdLumaQuant[i] * scale + 0.5f ) );
	}

	float block[64];
	float coeffs[64];
	float restored[64];

	const int blocksX = ( image.width + 7 ) / 8;
	const int blocksY = ( image.height + 7 ) / 8;

	for( int by = 0; by < blocksY; ++by ) {
		for( int bx = 0; bx < blocksX; ++bx ) {
			// Gather one block, replicating edge pixels for partial blocks.
			for( int y = 0; y < 8; ++y ) {
				const int sy = std::min( image.height - 1, by * 8 + y );
				const uint8_t* src = image.row( sy );
				for( int x = 0; x < 8; ++x ) {
					const int sx = std::min( image.width - 1, bx * 8 + x );
					block[y * 8 + x] = static_cast<float>( src[sx * image.channels] );
				}
			}

			forwardDct( block, coeffs );
			for( int i = 0; i < 64; ++i ) {
				coeffs[i] = std::round( coeffs[i] / quant[i] ) * quant[i];
			}
			inverseDct( coeffs, restored );

			// Write back into every channel (chroma is not modelled separately:
			// the strongest, most relevant artefacts are luminance ringing).
			for( int y = 0; y < 8; ++y ) {
				const int dy = by * 8 + y;
				if( dy >= image.height ) {
					break;
				}
				uint8_t* dst = image.row( dy );
				for( int x = 0; x < 8; ++x ) {
					const int dx = bx * 8 + x;
					if( dx >= image.width ) {
						break;
					}
					const float v = restored[y * 8 + x];
					const uint8_t q = static_cast<uint8_t>( std::max( 0.f, std::min( 255.f, v + 0.5f ) ) );
					for( int c = 0; c < image.channels; ++c ) {
						dst[dx * image.channels + c] = q;
					}
				}
			}
		}
	}
}

} // namespace ocr
