// Background sources: a pool of images loaded from disk (cached as decoded RGB
// pixels) plus fully procedural backgrounds when no directory is given.
#pragma once

#include "common/image.h"
#include "common/rng.h"

#include <string>
#include <vector>

namespace ocr {

class BackgroundPool {
public:
	// Loads every image below `directory` (png/jpg/bmp/tga/ppm supported by
	// stb_image). Large images are downscaled to `maxDimension` while caching to
	// bound memory. Returns false with `error` set on failure.
	bool loadDirectory( const std::string& directory, int maxDimension, std::string* error );

	size_t imageCount() const { return images.size(); }

	// Produces a w x h RGB background: a random crop of a pool image if any are
	// loaded, otherwise a procedural one.
	Image sample( int w, int h, Rng& rng ) const;

private:
	Image sampleFromPool( int w, int h, Rng& rng ) const;
	Image makeProcedural( int w, int h, Rng& rng ) const;

	std::vector<Image> images; // decoded, RGB
};

} // namespace ocr
