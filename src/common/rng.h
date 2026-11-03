// Small deterministic PRNG. Every record is generated from a seed derived from
// (master seed, record index), so any single record can be reproduced without
// replaying the whole run.
#pragma once

#include <cmath>
#include <cstdint>

namespace ocr {

inline uint64_t splitmix64( uint64_t& state )
{
	uint64_t z = ( state += 0x9E3779B97F4A7C15ull );
	z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
	return z ^ ( z >> 31 );
}

// Mixes the master seed with a record index; two different (seed, index) pairs
// give unrelated streams.
inline uint64_t deriveSeed( uint64_t masterSeed, uint64_t index )
{
	uint64_t s = masterSeed ^ 0xA0761D6478BD642Full;
	s = splitmix64( s );
	s ^= index * 0xE7037ED1A0B428DBull;
	s = splitmix64( s );
	return s;
}

class Rng {
public:
	explicit Rng( uint64_t seed ) : state( seed ) {}

	uint64_t nextU64() { return splitmix64( state ); }

	// Uniform in [0, 1)
	float uniform()
	{
		// 24 bits of mantissa keeps this exact in float.
		return static_cast<float>( nextU64() >> 40 ) * ( 1.f / 16777216.f );
	}

	float uniform( float lo, float hi ) { return lo + ( hi - lo ) * uniform(); }

	int uniformInt( int lo, int hi ) // inclusive
	{
		if( hi <= lo ) {
			return lo;
		}
		const uint32_t span = static_cast<uint32_t>( hi - lo ) + 1u;
		return lo + static_cast<int>( nextU64() % span );
	}

	bool chance( float p ) { return uniform() < p; }

	// Box-Muller; returns a standard normal sample.
	float normal()
	{
		if( hasSpare ) {
			hasSpare = false;
			return spare;
		}
		float u1, u2;
		do {
			u1 = uniform();
		} while( u1 <= 1e-7f );
		u2 = uniform();
		const float r = std::sqrt( -2.f * std::log( u1 ) );
		const float theta = 6.28318530718f * u2;
		spare = r * std::sin( theta );
		hasSpare = true;
		return r * std::cos( theta );
	}

private:
	uint64_t state;
	float spare = 0.f;
	bool hasSpare = false;
};

} // namespace ocr
