#include "generator/backgrounds.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "stb_image.h"

namespace ocr {

namespace fs = std::filesystem;

bool BackgroundPool::loadDirectory( const std::string& directory, int maxDimension, std::string* error )
{
	std::error_code ec;
	if( !fs::exists( directory, ec ) || !fs::is_directory( directory, ec ) ) {
		if( error != nullptr ) {
			*error = "background directory does not exist: " + directory;
		}
		return false;
	}

	// Deterministic order.
	std::vector<std::string> files;
	for( fs::recursive_directory_iterator it( directory, fs::directory_options::skip_permission_denied, ec ), end;
		it != end; it.increment( ec ) )
	{
		if( ec ) {
			ec.clear();
			continue;
		}
		if( it->is_regular_file( ec ) ) {
			files.push_back( it->path().string() );
		}
	}
	std::sort( files.begin(), files.end() );

	int failed = 0;
	for( const std::string& path : files ) {
		int w = 0, h = 0, channels = 0;
		stbi_uc* data = stbi_load( path.c_str(), &w, &h, &channels, 3 );
		if( data == nullptr ) {
			++failed; // not an image (or unsupported); keep going
			continue;
		}

		Image img( w, h, 3 );
		std::copy( data, data + static_cast<size_t>( w ) * h * 3, img.pixels.begin() );
		stbi_image_free( data );

		if( maxDimension > 0 && ( w > maxDimension || h > maxDimension ) ) {
			const float scale = static_cast<float>( maxDimension ) / std::max( w, h );
			img = resizeBilinear( img,
				std::max( 1, static_cast<int>( w * scale ) ),
				std::max( 1, static_cast<int>( h * scale ) ) );
		}
		images.push_back( std::move( img ) );
	}

	if( images.empty() ) {
		if( error != nullptr ) {
			*error = "no decodable images found in " + directory;
		}
		return false;
	}
	return true;
}

Image BackgroundPool::sampleFromPool( int w, int h, Rng& rng ) const
{
	const Image& src = images[rng.uniformInt( 0, static_cast<int>( images.size() ) - 1 )];

	// Random crop with the target aspect ratio, then resize. Cropping (rather
	// than stretching) keeps textures looking natural.
	const float targetAspect = static_cast<float>( w ) / h;
	float cropW = static_cast<float>( src.width );
	float cropH = cropW / targetAspect;
	if( cropH > src.height ) {
		cropH = static_cast<float>( src.height );
		cropW = cropH * targetAspect;
	}
	// Random zoom-out so we do not always take a minimal crop.
	const float zoom = rng.uniform( 1.f, 2.2f );
	cropW = std::min( static_cast<float>( src.width ), cropW * zoom );
	cropH = std::min( static_cast<float>( src.height ), cropW / targetAspect );
	cropW = std::min( static_cast<float>( src.width ), cropH * targetAspect );

	const int maxX = std::max( 0, src.width - static_cast<int>( cropW ) );
	const int maxY = std::max( 0, src.height - static_cast<int>( cropH ) );
	const int offX = ( maxX > 0 ) ? rng.uniformInt( 0, maxX ) : 0;
	const int offY = ( maxY > 0 ) ? rng.uniformInt( 0, maxY ) : 0;

	Image cropped( std::max( 1, static_cast<int>( cropW ) ), std::max( 1, static_cast<int>( cropH ) ), 3 );
	for( int y = 0; y < cropped.height; ++y ) {
		const int sy = std::min( src.height - 1, offY + y );
		const uint8_t* srcRow = src.row( sy ) + static_cast<size_t>( offX ) * 3;
		std::copy( srcRow, srcRow + static_cast<size_t>( cropped.width ) * 3, cropped.row( y ) );
	}

	return resizeBilinear( cropped, w, h );
}

Image BackgroundPool::makeProcedural( int w, int h, Rng& rng ) const
{
	Image img( w, h, 3 );

	switch( rng.uniformInt( 0, 4 ) ) {
	case 0: { // solid colour
		const uint8_t r = static_cast<uint8_t>( rng.uniformInt( 0, 255 ) );
		const uint8_t g = static_cast<uint8_t>( rng.uniformInt( 0, 255 ) );
		const uint8_t b = static_cast<uint8_t>( rng.uniformInt( 0, 255 ) );
		for( int y = 0; y < h; ++y ) {
			uint8_t* row = img.row( y );
			for( int x = 0; x < w; ++x ) {
				row[x * 3 + 0] = r;
				row[x * 3 + 1] = g;
				row[x * 3 + 2] = b;
			}
		}
		break;
	}
	case 1: { // linear gradient at a random angle
		const float angle = rng.uniform( 0.f, 6.2831853f );
		const float dx = std::cos( angle );
		const float dy = std::sin( angle );
		float c0[3], c1[3];
		for( int c = 0; c < 3; ++c ) {
			c0[c] = rng.uniform( 0.f, 255.f );
			c1[c] = rng.uniform( 0.f, 255.f );
		}
		const float maxProj = std::fabs( dx ) * w + std::fabs( dy ) * h;
		for( int y = 0; y < h; ++y ) {
			uint8_t* row = img.row( y );
			for( int x = 0; x < w; ++x ) {
				float t = ( dx * x + dy * y ) / maxProj;
				t = std::max( 0.f, std::min( 1.f, t ) );
				for( int c = 0; c < 3; ++c ) {
					row[x * 3 + c] = static_cast<uint8_t>( c0[c] + ( c1[c] - c0[c] ) * t );
				}
			}
		}
		break;
	}
	case 2: { // radial gradient
		const float cx = rng.uniform( 0.f, static_cast<float>( w ) );
		const float cy = rng.uniform( 0.f, static_cast<float>( h ) );
		const float radius = std::max( 1.f, rng.uniform( 0.4f, 1.2f ) * std::max( w, h ) );
		float c0[3], c1[3];
		for( int c = 0; c < 3; ++c ) {
			c0[c] = rng.uniform( 0.f, 255.f );
			c1[c] = rng.uniform( 0.f, 255.f );
		}
		for( int y = 0; y < h; ++y ) {
			uint8_t* row = img.row( y );
			for( int x = 0; x < w; ++x ) {
				const float d = std::sqrt( ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy ) ) / radius;
				const float t = std::max( 0.f, std::min( 1.f, d ) );
				for( int c = 0; c < 3; ++c ) {
					row[x * 3 + c] = static_cast<uint8_t>( c0[c] + ( c1[c] - c0[c] ) * t );
				}
			}
		}
		break;
	}
	default: { // value-noise texture over a base colour
		const float base[3] = { rng.uniform( 60.f, 200.f ), rng.uniform( 60.f, 200.f ), rng.uniform( 60.f, 200.f ) };
		const float amplitude = rng.uniform( 10.f, 45.f );
		const int cell = std::max( 2, rng.uniformInt( 4, 16 ) );
		const int gw = w / cell + 2;
		const int gh = h / cell + 2;
		std::vector<float> grid( static_cast<size_t>( gw ) * gh );
		for( float& v : grid ) {
			v = rng.uniform( -1.f, 1.f );
		}
		for( int y = 0; y < h; ++y ) {
			uint8_t* row = img.row( y );
			const float gy = static_cast<float>( y ) / cell;
			const int y0 = static_cast<int>( gy );
			const float fy = gy - y0;
			for( int x = 0; x < w; ++x ) {
				const float gx = static_cast<float>( x ) / cell;
				const int x0 = static_cast<int>( gx );
				const float fx = gx - x0;
				const float v00 = grid[static_cast<size_t>( y0 ) * gw + x0];
				const float v10 = grid[static_cast<size_t>( y0 ) * gw + x0 + 1];
				const float v01 = grid[static_cast<size_t>( y0 + 1 ) * gw + x0];
				const float v11 = grid[static_cast<size_t>( y0 + 1 ) * gw + x0 + 1];
				const float v = ( v00 * ( 1 - fx ) + v10 * fx ) * ( 1 - fy )
					+ ( v01 * ( 1 - fx ) + v11 * fx ) * fy;
				const float noise = v * amplitude + rng.uniform( -4.f, 4.f );
				for( int c = 0; c < 3; ++c ) {
					const float value = base[c] + noise;
					row[x * 3 + c] = static_cast<uint8_t>( std::max( 0.f, std::min( 255.f, value ) ) );
				}
			}
		}
		break;
	}
	}

	return img;
}

Image BackgroundPool::sample( int w, int h, Rng& rng ) const
{
	if( !images.empty() && rng.chance( 0.75f ) ) {
		return sampleFromPool( w, h, rng );
	}
	return makeProcedural( w, h, rng );
}

} // namespace ocr
