// Label content generation: random characters, words from a list, or digits.
#pragma once

#include "common/rng.h"

#include <string>
#include <vector>

namespace ocr {

enum class TextMode {
	RandomChar,
	Words,
	Numeric
};

struct TextConfig {
	std::string charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
		".,:;!?'-()/";
	TextMode mode = TextMode::RandomChar;
	int minLength = 3;
	int maxLength = 20;
	std::vector<std::string> words;
};

bool loadWordList( const std::string& path, std::vector<std::string>& words, std::string* error );

std::string generateText( const TextConfig& config, Rng& rng );

// Draws `length` characters uniformly from `alphabet`.
std::string randomStringFrom( const std::string& alphabet, int length, Rng& rng );

// Picks a word from the list (list must be non-empty).
std::string pickWord( const std::vector<std::string>& words, Rng& rng );

} // namespace ocr
