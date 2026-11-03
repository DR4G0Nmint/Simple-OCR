// --verify support: structurally validates generated shards, checks the
// manifest checksums, and exports sample images for manual inspection.
#pragma once

#include <string>

namespace ocr {

struct VerifyOptions {
	std::string shardDir;
	std::string dumpDir;   // empty => no images written
	int sampleCount = 50;
};

bool verifyShards( const VerifyOptions& options, std::string* error );

} // namespace ocr
