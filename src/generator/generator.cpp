#include "generator/generator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace ocr {

namespace {

struct SharedState {
	std::atomic<uint64_t> nextIndex{ 0 };
	std::atomic<bool> failed{ false };
	std::atomic<uint64_t> cerMicroSum{ 0 }; // sum of cer * 1e6, for the mean
	std::atomic<uint64_t> inBandCount{ 0 };
	std::atomic<uint64_t> contrastRejects{ 0 }; // renders thrown away for lack of contrast
	std::string error;
	std::mutex errorMutex;

	void fail( const std::string& message )
	{
		std::lock_guard<std::mutex> lock( errorMutex );
		if( !failed.exchange( true ) ) {
			error = message;
		}
	}
};

// Resolves characters against the precomputed font coverage, so that a label is
// only ever built from characters the chosen font actually contains.
class CharsetPlan {
public:
	CharsetPlan( const GeneratorConfig& config, const FontCoverage& coverage )
		: coverage( coverage )
		, minLength( std::max( 1, config.text.minLength ) )
		, maxLength( std::max( std::max( 1, config.text.minLength ), config.text.maxLength ) )
		, mode( config.text.mode )
	{
		for( size_t i = 0; i < coverage.codepoints.size(); ++i ) {
			bitOf[coverage.codepoints[i]] = static_cast<int>( i );
		}
		for( char c : config.text.charset ) {
			auto it = bitOf.find( static_cast<uint32_t>( static_cast<unsigned char>( c ) ) );
			if( it != bitOf.end() ) {
				charsetBits.emplace_back( c, it->second );
			}
		}
	}

	bool fontHas( int font, int bit ) const
	{
		const std::vector<uint8_t>& bits = coverage.coverage[static_cast<size_t>( font )];
		return ( bits[static_cast<size_t>( bit ) / 8] & ( 1u << ( bit % 8 ) ) ) != 0;
	}

	// Alphabet of `config.text.charset` characters supported by this font.
	std::string alphabetFor( int font ) const
	{
		std::string alphabet;
		for( const auto& entry : charsetBits ) {
			if( fontHas( font, entry.second ) ) {
				alphabet.push_back( entry.first );
			}
		}
		return alphabet;
	}

	bool fontCoversWord( int font, const std::string& word ) const
	{
		for( char c : word ) {
			auto it = bitOf.find( static_cast<uint32_t>( static_cast<unsigned char>( c ) ) );
			if( it == bitOf.end() || !fontHas( font, it->second ) ) {
				return false;
			}
		}
		return true;
	}

	int minLength;
	int maxLength;
	TextMode mode;

private:
	const FontCoverage& coverage;
	std::unordered_map<uint32_t, int> bitOf;
	std::vector<std::pair<char, int>> charsetBits;
};

// Builds one record. Returns false only on unrecoverable problems.
bool produceRecord( SharedState& state, const GeneratorConfig& config, FontCache& fonts,
	const BackgroundPool& backgrounds, const CharsetPlan& plan, uint64_t index, Record& out,
	float& chosenCer )
{
	Rng rng( deriveSeed( config.seed, index ) );

	std::string lastLabel;
	int lastFont = -1;
	RenderFailure lastFailure = RenderFailure::None;
	int failureCounts[5] = { 0, 0, 0, 0, 0 };

	for( int attempt = 0; attempt < 64; ++attempt ) {
		const int fontIndex = rng.uniformInt( 0, static_cast<int>( fonts.fontCount() ) - 1 );
		const std::string alphabet = plan.alphabetFor( fontIndex );
		if( alphabet.empty() ) {
			continue; // this font supports none of the configured characters
		}

		// Length is biased toward the short end: long random strings are squashed
		// into 128 px and become ambiguous, which would teach the network noise
		// (a 20-character line leaves ~6 px per glyph).
		const int span = plan.maxLength - plan.minLength + 1;
		const float u = std::min( rng.uniform(), rng.uniform() );
		const int length = plan.minLength + std::min( span - 1, static_cast<int>( u * span ) );
		std::string label;
		if( plan.mode == TextMode::Words && !config.text.words.empty() ) {
			for( int wordAttempt = 0; wordAttempt < 8; ++wordAttempt ) {
				const std::string word = pickWord( config.text.words, rng );
				const int wordLength = static_cast<int>( word.size() );
				if( wordLength >= plan.minLength && wordLength <= plan.maxLength
					&& plan.fontCoversWord( fontIndex, word ) )
				{
					label = word;
					break;
				}
			}
		}
		if( label.empty() ) {
			std::string drawAlphabet = alphabet;
			if( plan.mode == TextMode::Numeric ) {
				std::string digits;
				for( char c : alphabet ) {
					if( std::isdigit( static_cast<unsigned char>( c ) ) ) {
						digits.push_back( c );
					}
				}
				if( !digits.empty() ) {
					drawAlphabet = digits;
				}
			}
			label = randomStringFrom( drawAlphabet, length, rng );
		}

		Image clean;
		RenderFailure failure = RenderFailure::None;
		if( !renderSample( fonts, backgrounds, rng, fontIndex, label, config.render, clean, &failure ) ) {
			lastLabel = label;
			lastFont = fontIndex;
			lastFailure = failure;
			++failureCounts[static_cast<int>( failure )];
			continue; // font lacks a glyph or the line collapsed; try again
		}

		Image best;
		float bestDistance = 1e9f;
		float bestCer = 0.f;
		float severity = 1.0f;
		bool accepted = false;

		for( int legibilityAttempt = 0; legibilityAttempt < config.legibilityAttempts; ++legibilityAttempt ) {
			Image candidate = clean;
			applyAugmentations( candidate, rng, config.augment, severity );

			Image candidateGray = ( config.channels == 1 ) ? toGrayscale( candidate ) : candidate;
			const float cer = estimateCharacterErrorRate( clean, candidateGray );

			if( cer >= config.minCer && cer <= config.maxCer ) {
				best = std::move( candidateGray );
				bestCer = cer;
				accepted = true;
				break;
			}

			// If the band was missed, steer severity: too legible means we need
			// more degradation, too damaged means less.
			const float distance = ( cer < config.minCer ) ? ( config.minCer - cer ) : ( cer - config.maxCer );
			if( distance < bestDistance ) {
				bestDistance = distance;
				best = candidateGray;
				bestCer = cer;
			}
			severity *= ( cer < config.minCer ) ? 1.45f : 0.70f;
			severity = std::max( 0.2f, std::min( 1.8f, severity ) );
		}

		if( best.empty() ) {
			continue;
		}

		// Far outside the band (e.g. black text on a near-black background) means the
		// chosen background/foreground pair has no contrast to lose; no severity can
		// fix that, so throw this render away and draw a different one. Such a sample
		// would only teach the network noise.
		if( !accepted && bestCer > config.maxCer * 1.8f ) {
			state.contrastRejects.fetch_add( 1, std::memory_order_relaxed );
			continue;
		}

		state.cerMicroSum.fetch_add( static_cast<uint64_t>( bestCer * 1e6f ), std::memory_order_relaxed );
		if( accepted ) {
			state.inBandCount.fetch_add( 1, std::memory_order_relaxed );
		}

		chosenCer = bestCer;
		out.index = index;
		out.label = label;
		out.image = std::move( best );
		return true;
	}

	char message[512];
	std::snprintf( message, sizeof( message ),
		"record %llu: 64 render attempts failed (last: font #%d, label \"%s\", reason: %s; "
		"counts: noFace=%d empty=%d missingGlyph=%d raster=%d)",
		static_cast<unsigned long long>( index ), lastFont, lastLabel.c_str(), describe( lastFailure ),
		failureCounts[1], failureCounts[2], failureCounts[3], failureCounts[4] );
	state.fail( message );
	return false;
}

} // namespace

bool runGenerator( const GeneratorConfig& config, GeneratorStats& stats, std::string* error )
{
	std::string fontError;
	std::vector<std::string> fontFiles = findFontFiles( config.fontsDir, &fontError );
	if( fontFiles.empty() ) {
		if( error != nullptr ) {
			*error = fontError.empty() ? ( "no .ttf files found under " + config.fontsDir ) : fontError;
		}
		return false;
	}

	// Characters any label may use: the configured charset plus wordlist letters.
	std::vector<uint32_t> requiredCodepoints;
	{
		auto addChar = [&requiredCodepoints]( char c ) {
			const uint32_t cp = static_cast<uint32_t>( static_cast<unsigned char>( c ) );
			if( std::find( requiredCodepoints.begin(), requiredCodepoints.end(), cp ) == requiredCodepoints.end() ) {
				requiredCodepoints.push_back( cp );
			}
		};
		for( char c : config.text.charset ) {
			addChar( c );
		}
		for( const std::string& word : config.text.words ) {
			for( char c : word ) {
				addChar( c );
			}
		}
		for( char c : std::string( "0123456789" ) ) {
			addChar( c );
		}
	}

	FontCoverage coverage;
	{
		const auto coverageStart = std::chrono::steady_clock::now();
		std::string coverageError;
		if( !computeFontCoverage( fontFiles, requiredCodepoints, coverage, &coverageError ) ) {
			if( error != nullptr ) {
				*error = coverageError;
			}
			return false;
		}
		if( config.verbose ) {
			const double seconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - coverageStart ).count();
			std::fprintf( stderr, "measured charset coverage of %zu fonts in %.2fs\n",
				fontFiles.size(), seconds );
		}
	}

	// Keep only fonts that cover a meaningful share of the charset. Script-
	// specific faces (Arabic, CJK, symbol fonts) may contain a handful of ASCII
	// characters; drawing from those produces degenerate labels such as "?????".
	{
		std::vector<uint32_t> charsetCodepoints;
		for( char c : config.text.charset ) {
			charsetCodepoints.push_back( static_cast<uint32_t>( static_cast<unsigned char>( c ) ) );
		}
		std::unordered_map<uint32_t, size_t> bitOf;
		for( size_t i = 0; i < coverage.codepoints.size(); ++i ) {
			bitOf[coverage.codepoints[i]] = i;
		}

		const size_t required = std::max<size_t>( 8, charsetCodepoints.size() * 3 / 5 );
		std::vector<size_t> keep;
		for( size_t f = 0; f < coverage.fontCount(); ++f ) {
			size_t coveredCount = 0;
			for( uint32_t cp : charsetCodepoints ) {
				auto it = bitOf.find( cp );
				if( it == bitOf.end() ) {
					continue;
				}
				const size_t bit = it->second;
				if( ( coverage.coverage[f][bit / 8] & ( 1u << ( bit % 8 ) ) ) != 0 ) {
					++coveredCount;
				}
			}
			if( coveredCount >= required ) {
				keep.push_back( f );
			}
		}

		if( keep.empty() ) {
			if( error != nullptr ) {
				*error = "no font covers at least 60% of the configured charset";
			}
			return false;
		}

		std::vector<std::string> usableFonts;
		std::vector<std::vector<uint8_t>> usableCoverage;
		usableFonts.reserve( keep.size() );
		usableCoverage.reserve( keep.size() );
		for( size_t f : keep ) {
			usableFonts.push_back( fontFiles[f] );
			usableCoverage.push_back( coverage.coverage[f] );
		}

		if( config.verbose ) {
			std::fprintf( stderr, "usable fonts: %zu of %zu cover >=%zu charset characters\n",
				usableFonts.size(), fontFiles.size(), required );
		}

		fontFiles = std::move( usableFonts );
		coverage.coverage = std::move( usableCoverage );
	}

	const CharsetPlan plan( config, coverage );

	BackgroundPool backgrounds;
	if( !config.backgroundsDir.empty() ) {
		std::string bgError;
		if( !backgrounds.loadDirectory( config.backgroundsDir, 1024, &bgError ) && config.verbose ) {
			std::fprintf( stderr, "warning: %s (falling back to procedural backgrounds)\n", bgError.c_str() );
		}
	}

	int threadCount = config.threads;
	if( threadCount <= 0 ) {
		threadCount = static_cast<int>( std::thread::hardware_concurrency() );
		if( threadCount <= 0 ) {
			threadCount = 4;
		}
	}

	ShardWriter writer( config.outputDir, config.shardSize, config.seed, config.width, config.height,
		config.channels, config.text.charset );
	if( !writer.start( error ) ) {
		return false;
	}

	SharedState state;
	std::atomic<int> finishedWorkers{ 0 };
	const auto startTime = std::chrono::steady_clock::now();

	std::vector<std::thread> workers;
	workers.reserve( static_cast<size_t>( threadCount ) );

	for( int t = 0; t < threadCount; ++t ) {
		workers.emplace_back( [&, t]() {
			FT_Library library = nullptr;
			if( FT_Init_FreeType( &library ) != 0 ) {
				state.fail( "FT_Init_FreeType failed in worker" );
				finishedWorkers.fetch_add( 1 );
				return;
			}
			// The face cache must be destroyed before the FreeType library it
			// was created from, hence the explicit scope.
			{
				FontCache fonts( library, fontFiles );

				while( !state.failed.load( std::memory_order_relaxed ) ) {
					const uint64_t index = state.nextIndex.fetch_add( 1 );
					if( index >= config.count ) {
						break;
					}

					Record record;
					float cer = 0.f;
					if( !produceRecord( state, config, fonts, backgrounds, plan, index, record, cer ) ) {
						break;
					}
					if( !writer.queue().push( std::move( record ) ) ) {
						break;
					}
				}
			}

			FT_Done_FreeType( library );
			finishedWorkers.fetch_add( 1 );
			(void)t;
		} );
	}

	// Progress reporting on the main thread while the workers run.
	uint64_t lastCount = 0;
	auto lastReport = startTime;

	while( finishedWorkers.load( std::memory_order_relaxed ) < threadCount && !state.failed.load() ) {
		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
		const auto now = std::chrono::steady_clock::now();
		const double sinceLast = std::chrono::duration<double>( now - lastReport ).count();
		if( sinceLast >= 1.0 && config.verbose ) {
			const uint64_t produced = std::min<uint64_t>( state.nextIndex.load(), config.count );
			const double rate = ( produced - lastCount ) / sinceLast;
			const double elapsed = std::chrono::duration<double>( now - startTime ).count();
			std::fprintf( stderr, "\r%llu/%llu images  %.1f img/s  elapsed %.0fs   ",
				static_cast<unsigned long long>( produced ),
				static_cast<unsigned long long>( config.count ), rate, elapsed );
			std::fflush( stderr );
			lastCount = produced;
			lastReport = now;
		}
	}

	for( std::thread& worker : workers ) {
		if( worker.joinable() ) {
			worker.join();
		}
	}

	if( state.failed ) {
		if( error != nullptr ) {
			*error = state.error;
		}
		writer.queue().close();
		std::string ignored;
		writer.finish( &ignored );
		return false;
	}

	if( !writer.finish( error ) ) {
		return false;
	}

	const auto endTime = std::chrono::steady_clock::now();
	stats.seconds = std::chrono::duration<double>( endTime - startTime ).count();
	stats.records = writer.recordCount();
	stats.shards = writer.shards().size();
	stats.imagesPerSecond = ( stats.seconds > 0.0 ) ? stats.records / stats.seconds : 0.0;
	stats.inBand = state.inBandCount.load();
	stats.meanCer = ( stats.records > 0 )
		? ( static_cast<double>( state.cerMicroSum.load() ) / 1e6 ) / stats.records : 0.0;

	if( config.verbose ) {
		if( state.contrastRejects.load() > 0 ) {
			std::fprintf( stderr, "contrast-rejected renders (out of band after all attempts): %llu\n",
				static_cast<unsigned long long>( state.contrastRejects.load() ) );
		}
		std::fprintf( stderr, "\n" );
	}
	return true;
}

void applyAugmentProfile( GeneratorConfig& config, const std::string& profile )
{
	if( profile == "full" ) {
		return; // defaults already describe the full stack
	}

	AugmentSettings& a = config.augment;
	const bool none = ( profile == "none" );
	const float scale = none ? 0.f : 0.5f;

	a.rotationP *= scale;
	a.perspectiveP *= scale;
	a.shearP *= scale;
	a.brightnessP *= scale;
	a.contrastP *= scale;
	a.gammaP *= scale;
	a.gaussianBlurP *= scale;
	a.motionBlurP *= scale;
	a.gaussianNoiseP *= scale;
	a.saltPepperP *= scale;
	a.jpegP *= scale;
	a.scanlineP *= scale;
	a.chromaticAberrationP *= scale;

	if( none ) {
		// Without augmentation every sample is by definition fully legible, so
		// the difficulty band must be disabled or every record would be retried.
		config.minCer = 0.f;
		config.maxCer = 1.f;
		config.legibilityAttempts = 1;
	}
}

} // namespace ocr
