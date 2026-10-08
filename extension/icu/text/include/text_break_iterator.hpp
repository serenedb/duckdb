//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_break_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "text_data.hpp"
#include "text_dictionary.hpp"
#include "text_locale.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace text {

enum WordBreakStatus : int32_t {
	WORD_NONE = 0,
	WORD_NONE_LIMIT = 100,
	WORD_NUMBER = 100,
	WORD_NUMBER_LIMIT = 200,
	WORD_LETTER = 200,
	WORD_LETTER_LIMIT = 300,
	WORD_KANA = 300,
	WORD_KANA_LIMIT = 400,
	WORD_IDEO = 400,
	WORD_IDEO_LIMIT = 500
};

class BreakIterator {
public:
	static constexpr int64_t DONE = -1;

	BreakIterator(BreakKind kind, const Locale &locale, BreakUnits units);

	bool IsTailored() const {
		return tailored;
	}

	void SetText(const char *text, size_t length);
	int64_t Next();
	int32_t GetRuleStatus() const;
	int64_t Preceding(size_t offset);

private:
	struct Boundary {
		int64_t position;
		int32_t status_index;
	};

	void Restart();
	Boundary HandleNext(int64_t from, bool &dictionary) const;
	Boundary DelegateNext();
	Boundary DictionaryFollowing(int64_t from);
	void PopulateDictionary(int64_t start, int64_t end, int32_t status_index);
	int64_t NativeLength(int64_t start, int64_t end) const;
	Boundary FilteredNext();
	bool IsException(int64_t position) const;
	size_t PreviousCodePoint(size_t position, uint32_t &c) const;

	const BreakRuleSet *rules;
	const SentenceExceptionLocale *exceptions = nullptr;
	BreakUnits units;
	bool tailored = false;

	const uint8_t *data = nullptr;
	size_t size = 0;

	Boundary current {0, 0};
	Boundary peeked {0, 0};
	bool has_peeked = false;
	bool done = false;
	int64_t delegate_position = 0;

	std::vector<int64_t> dictionary_breaks;
	int64_t dictionary_start = 0;
	int64_t dictionary_limit = 0;
	int64_t dictionary_index = -1;
	int32_t dictionary_status_index = 0;
	EngineText engine_text;
	std::vector<int32_t> engine_breaks;
	DictionaryEngines engines;
};

} // namespace text
} // namespace duckdb
