// End-to-end sample generation: worker pool -> bounded queue -> shard writer.
#pragma once

#include "generator/augment.h"
#include "generator/renderer.h"
#include "generator/shard_writer.h"
#include "generator/text.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ocr {

struct GeneratorConfig {
	std::string fontsDir;
	std::string backgroundsDir;
	std::string wordlist;
	std::string outputDir;

	uint64_t count = 10000;
	uint64_t shardSize = 10000;
	uint64_t seed = 1234;
	int width = 128;
	int height = 32;
	int channels = 1;
	int threads = 0; // 0 => hardware_concurrency

	RenderSettings render;
	TextConfig text;
	AugmentSettings augment;

	// Accepted difficulty band (see README): augmentation strength is steered
	// so the estimated character error rate stays inside this range.
	float minCer = 0.05f;
	float maxCer = 0.20f;
	int legibilityAttempts = 8;

	bool verbose = true;

	// "full" (default), "light" or "none": used for the throughput sweep in
	// BENCHMARKS.md. Post-effects stay enabled in "light" because they are the
	// point of the generator; only their probability is halved.
	std::string augmentProfile = "full";
};

struct GeneratorStats {
	uint64_t records = 0;
	uint64_t shards = 0;
	double seconds = 0.0;
	double imagesPerSecond = 0.0;

	// Difficulty control outcome: how many samples landed inside the accepted
	// CER band, and the mean estimated CER actually produced.
	uint64_t inBand = 0;
	double meanCer = 0.0;
};

bool runGenerator( const GeneratorConfig& config, GeneratorStats& stats, std::string* error );

// Applies an augmentation preset ("full", "light", "none") to `config`.
void applyAugmentProfile( GeneratorConfig& config, const std::string& profile );

} // namespace ocr
