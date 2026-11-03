// ocr_gen: synthetic OCR training data generator.
//
// Example:
//   ocr_gen --fonts /usr/share/fonts --out data/train --count 10000 --seed 1
//   ocr_gen --verify data/train --dump data/samples --verify-count 50

#include "generator/generator.h"
#include "generator/verify.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void printUsage()
{
	std::printf(
		"ocr_gen - synthetic OCR training data generator\n"
		"\n"
		"Generation:\n"
		"  --fonts DIR            directory tree scanned for .ttf/.otf (required)\n"
		"  --backgrounds DIR       image directory for backgrounds (optional)\n"
		"  --out DIR               output directory for shards + manifest.json\n"
		"  --count N               number of images to generate (default 10000)\n"
		"  --shard-size N          records per shard (default 10000)\n"
		"  --seed N                master seed (default 1234)\n"
		"  --width N --height N    image geometry (default 128x32)\n"
		"  --channels N            1 (grayscale, default) or 3 (RGB)\n"
		"  --charset STR           character set for random-char mode\n"
		"  --mode MODE             random-char | words | numeric\n"
		"  --wordlist FILE         word list for --mode words\n"
		"  --len-min N --len-max N label length range (default 3..20)\n"
		"  --size-min N --size-max N font pixel size range (default 20..48)\n"
		"  --threads N             worker threads (default: hardware_concurrency)\n"
		"  --augment PROFILE       full (default) | light | none (for benchmarking)\n"
		"  --quiet                 no progress output\n"
		"\n"
		"Verification:\n"
		"  --verify DIR            validate shards in DIR (structure, checksums)\n"
		"  --dump DIR              write sample images as .pnm next to labels.txt\n"
		"  --verify-count N        number of samples to dump (default 50)\n" );
}

bool parseU64( const char* text, uint64_t& out )
{
	char* end = nullptr;
	const unsigned long long value = std::strtoull( text, &end, 10 );
	if( end == text || *end != '\0' ) {
		return false;
	}
	out = value;
	return true;
}

bool parseInt( const char* text, int& out )
{
	char* end = nullptr;
	const long value = std::strtol( text, &end, 10 );
	if( end == text || *end != '\0' ) {
		return false;
	}
	out = static_cast<int>( value );
	return true;
}

} // namespace

int main( int argc, char** argv )
{
	ocr::GeneratorConfig config;
	config.outputDir = "data";

	std::string verifyDir;
	std::string dumpDir;
	int verifyCount = 50;

	for( int i = 1; i < argc; ++i ) {
		const std::string arg = argv[i];
		auto next = [&]( const char* name ) -> const char* {
			if( i + 1 >= argc ) {
				std::fprintf( stderr, "error: %s requires a value\n", name );
				std::exit( 2 );
			}
			return argv[++i];
		};

		if( arg == "--fonts" ) {
			config.fontsDir = next( "--fonts" );
		} else if( arg == "--backgrounds" ) {
			config.backgroundsDir = next( "--backgrounds" );
		} else if( arg == "--out" ) {
			config.outputDir = next( "--out" );
		} else if( arg == "--wordlist" ) {
			config.wordlist = next( "--wordlist" );
		} else if( arg == "--count" ) {
			if( !parseU64( next( "--count" ), config.count ) ) {
				std::fprintf( stderr, "error: bad --count\n" );
				return 2;
			}
		} else if( arg == "--shard-size" ) {
			if( !parseU64( next( "--shard-size" ), config.shardSize ) ) {
				std::fprintf( stderr, "error: bad --shard-size\n" );
				return 2;
			}
		} else if( arg == "--seed" ) {
			if( !parseU64( next( "--seed" ), config.seed ) ) {
				std::fprintf( stderr, "error: bad --seed\n" );
				return 2;
			}
		} else if( arg == "--width" ) {
			if( !parseInt( next( "--width" ), config.width ) ) {
				return 2;
			}
		} else if( arg == "--height" ) {
			if( !parseInt( next( "--height" ), config.height ) ) {
				return 2;
			}
		} else if( arg == "--channels" ) {
			if( !parseInt( next( "--channels" ), config.channels ) || ( config.channels != 1 && config.channels != 3 ) ) {
				std::fprintf( stderr, "error: --channels must be 1 or 3\n" );
				return 2;
			}
		} else if( arg == "--charset" ) {
			config.text.charset = next( "--charset" );
		} else if( arg == "--mode" ) {
			const std::string mode = next( "--mode" );
			if( mode == "random-char" ) {
				config.text.mode = ocr::TextMode::RandomChar;
			} else if( mode == "words" ) {
				config.text.mode = ocr::TextMode::Words;
			} else if( mode == "numeric" ) {
				config.text.mode = ocr::TextMode::Numeric;
			} else {
				std::fprintf( stderr, "error: unknown --mode %s\n", mode.c_str() );
				return 2;
			}
		} else if( arg == "--len-min" ) {
			if( !parseInt( next( "--len-min" ), config.text.minLength ) ) {
				return 2;
			}
		} else if( arg == "--len-max" ) {
			if( !parseInt( next( "--len-max" ), config.text.maxLength ) ) {
				return 2;
			}
		} else if( arg == "--size-min" ) {
			if( !parseInt( next( "--size-min" ), config.render.fontSizeMin ) ) {
				return 2;
			}
		} else if( arg == "--size-max" ) {
			if( !parseInt( next( "--size-max" ), config.render.fontSizeMax ) ) {
				return 2;
			}
		} else if( arg == "--augment" ) {
			config.augmentProfile = next( "--augment" );
		} else if( arg == "--threads" ) {
			if( !parseInt( next( "--threads" ), config.threads ) ) {
				return 2;
			}
		} else if( arg == "--verify" ) {
			verifyDir = next( "--verify" );
		} else if( arg == "--dump" ) {
			dumpDir = next( "--dump" );
		} else if( arg == "--verify-count" ) {
			if( !parseInt( next( "--verify-count" ), verifyCount ) ) {
				return 2;
			}
		} else if( arg == "--quiet" ) {
			config.verbose = false;
		} else if( arg == "--help" || arg == "-h" ) {
			printUsage();
			return 0;
		} else {
			std::fprintf( stderr, "error: unknown argument %s\n", arg.c_str() );
			printUsage();
			return 2;
		}
	}

	if( !verifyDir.empty() ) {
		ocr::VerifyOptions options;
		options.shardDir = verifyDir;
		options.dumpDir = dumpDir;
		options.sampleCount = verifyCount;
		std::string error;
		if( !ocr::verifyShards( options, &error ) ) {
			std::fprintf( stderr, "verify failed: %s\n", error.c_str() );
			return 1;
		}
		std::printf( "\nVisual inspection checklist (per dumped image):\n"
			"  1. The image shows exactly the characters in labels.txt, in order.\n"
			"  2. Text is legible to you at normal viewing distance.\n"
			"  3. Background varies between images (not one constant colour).\n"
			"  4. Font face, weight and size visibly differ between images.\n"
			"  5. Degradations are present (blur/noise/compression/rotation) but do not\n"
			"     make the string unreadable.\n" );
		return 0;
	}

	if( config.fontsDir.empty() ) {
		std::fprintf( stderr, "error: --fonts is required (see --help)\n" );
		return 2;
	}

	if( config.text.mode == ocr::TextMode::Words ) {
		if( config.wordlist.empty() ) {
			std::fprintf( stderr, "error: --mode words requires --wordlist\n" );
			return 2;
		}
		std::string error;
		if( !ocr::loadWordList( config.wordlist, config.text.words, &error ) ) {
			std::fprintf( stderr, "error: %s\n", error.c_str() );
			return 1;
		}
	}

	config.width = config.width > 0 ? config.width : 128;
	config.height = config.height > 0 ? config.height : 32;
	config.render.targetWidth = config.width;
	config.render.targetHeight = config.height;

	ocr::applyAugmentProfile( config, config.augmentProfile );

	ocr::GeneratorStats stats;
	std::string error;
	if( !ocr::runGenerator( config, stats, &error ) ) {
		std::fprintf( stderr, "generation failed: %s\n", error.c_str() );
		return 1;
	}

	std::printf( "generated %llu images in %llu shard(s) -> %s\n",
		static_cast<unsigned long long>( stats.records ),
		static_cast<unsigned long long>( stats.shards ), config.outputDir.c_str() );
	std::printf( "elapsed %.2fs, %.1f img/s\n", stats.seconds, stats.imagesPerSecond );
	const double inBandPct = ( stats.records > 0 ) ? 100.0 * stats.inBand / stats.records : 0.0;
	std::printf( "difficulty: mean_cer=%.3f in_band=%.1f%% (target band %.2f..%.2f)\n",
		stats.meanCer, inBandPct, config.minCer, config.maxCer );
	return 0;
}
