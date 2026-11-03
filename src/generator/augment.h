// Probabilistic augmentations plus the legibility check that keeps generated
// samples inside the intended difficulty band.
#pragma once

#include "common/image.h"
#include "common/rng.h"

namespace ocr {

struct AugmentSettings {
	// Geometric
	float rotationP = 0.5f;
	float rotationMaxDegrees = 15.f;
	float perspectiveP = 0.4f;
	float perspectiveMaxJitterFraction = 0.06f; // of the smaller side
	float shearP = 0.3f;
	float shearMax = 0.12f;

	// Photometric
	float brightnessP = 0.5f;
	float brightnessAmount = 0.25f;
	float contrastP = 0.5f;
	float contrastAmount = 0.20f;
	float gammaP = 0.3f;
	float gammaMin = 0.75f;
	float gammaMax = 1.35f;

	// Blur
	float gaussianBlurP = 0.3f;
	float gaussianSigmaMax = 2.0f;
	float motionBlurP = 0.2f;

	// Noise
	float gaussianNoiseP = 0.5f;
	float gaussianNoiseMaxSigma = 12.f;
	float saltPepperP = 0.3f;
	float saltPepperMaxFraction = 0.02f;

	// Real-world post effects
	float jpegP = 0.4f;
	float scanlineP = 0.15f;
	float chromaticAberrationP = 0.2f;
};

// Applies the stack to `img` in place. `severity` scales every degradation
// strength (1.0 = nominal); the pipeline uses it to steer the difficulty.
void applyAugmentations( Image& img, Rng& rng, const AugmentSettings& settings, float severity );

// Cheap legibility proxy: normalised correlation between the clean render and
// the augmented one, measured on the pixels that carry ink in the clean image.
// Returns an estimated character error rate in [0, 1].
float estimateCharacterErrorRate( const Image& clean, const Image& augmented );

} // namespace ocr
