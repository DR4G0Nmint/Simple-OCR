#include "generator/text.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace ocr {

bool loadWordList( const std::string& path, std::vector<std::string>& words, std::string* error )
{
	std::ifstream in( path );
	if( !in ) {
		if( error != nullptr ) {
			*error = "cannot open wordlist: " + path;
		}
		return false;
	}

	std::string line;
	while( std::getline( in, line ) ) {
		// Trim whitespace and skip empties; keep only plain ASCII words so that
		// they are renderable by the widest range of fonts.
		while( !line.empty() && std::isspace( static_cast<unsigned char>( line.back() ) ) ) {
			line.pop_back();
		}
		size_t start = 0;
		while( start < line.size() && std::isspace( static_cast<unsigned char>( line[start] ) ) ) {
			++start;
		}
		line.erase( 0, start );

		if( line.empty() || line.size() > 24 ) {
			continue;
		}
		bool ascii = true;
		for( char c : line ) {
			if( static_cast<unsigned char>( c ) >= 0x80 ) {
				ascii = false;
				break;
			}
		}
		if( ascii ) {
			words.push_back( line );
		}
	}

	if( words.empty() ) {
		if( error != nullptr ) {
			*error = "wordlist is empty: " + path;
		}
		return false;
	}
	return true;
}

std::string randomStringFrom( const std::string& alphabet, int length, Rng& rng )
{
	std::string result;
	if( alphabet.empty() ) {
		return result;
	}

	// Runs longer than three identical characters are useless for OCR training:
	// they are visually ambiguous ("?????") and CTC can only separate repeats
	// with blanks. Resample a few times per position instead.
	const int maxRun = 3;
	result.reserve( static_cast<size_t>( length ) );
	for( int i = 0; i < length; ++i ) {
		char c = alphabet[rng.uniformInt( 0, static_cast<int>( alphabet.size() ) - 1 )];
		int run = 1;
		for( int k = static_cast<int>( result.size() ) - 1; k >= 0 && result[k] == c; --k ) {
			++run;
		}
		// If the run limit is hit, take the next different character rather than
		// gambling on a redraw (the alphabet may be concentrated on few chars).
		if( run > maxRun && alphabet.size() > 1 ) {
			const size_t start = static_cast<size_t>( rng.uniformInt( 0, static_cast<int>( alphabet.size() ) - 1 ) );
			for( size_t step = 0; step < alphabet.size(); ++step ) {
				const char candidate = alphabet[( start + step ) % alphabet.size()];
				if( candidate != result.back() ) {
					c = candidate;
					break;
				}
			}
		}
		result.push_back( c );
	}
	return result;
}

std::string pickWord( const std::vector<std::string>& words, Rng& rng )
{
	if( words.empty() ) {
		return std::string();
	}
	return words[rng.uniformInt( 0, static_cast<int>( words.size() ) - 1 )];
}

std::string generateText( const TextConfig& config, Rng& rng )
{
	const int lo = std::max( 1, config.minLength );
	const int hi = std::max( lo, config.maxLength );

	if( config.mode == TextMode::Words && !config.words.empty() ) {
		for( int attempt = 0; attempt < 16; ++attempt ) {
			const std::string& word = config.words[rng.uniformInt( 0, static_cast<int>( config.words.size() ) - 1 )];
			if( static_cast<int>( word.size() ) >= lo && static_cast<int>( word.size() ) <= hi ) {
				return word;
			}
		}
		// Nothing in range: fall through and generate characters instead.
	}

	std::string alphabet = config.charset;
	if( config.mode == TextMode::Numeric || alphabet.empty() ) {
		alphabet = "0123456789";
	}

	const int length = rng.uniformInt( lo, hi );
	std::string result;
	result.reserve( static_cast<size_t>( length ) );
	for( int i = 0; i < length; ++i ) {
		result.push_back( alphabet[rng.uniformInt( 0, static_cast<int>( alphabet.size() ) - 1 )] );
	}
	return result;
}

} // namespace ocr
