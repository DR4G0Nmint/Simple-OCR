// SHA-256 (FIPS 180-4), used for shard checksums in the manifest and for the
// footer of the .ocrbin model format. Self-contained so that neither the
// generator nor the inference engine needs a crypto dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ocr {

class Sha256 {
public:
	static const int DigestSize = 32;
	static const int BlockSize = 64;

	Sha256() { reset(); }

	void reset();
	void update( const void* data, size_t size );
	void finish( uint8_t digest[DigestSize] );

	// Convenience: hex digest of the whole buffer.
	static std::string hexDigest( const void* data, size_t size );

private:
	void transform( const uint8_t block[BlockSize] );

	uint32_t state[8];
	uint64_t bitCount;
	uint8_t buffer[BlockSize];
	size_t bufferSize;
};

} // namespace ocr
