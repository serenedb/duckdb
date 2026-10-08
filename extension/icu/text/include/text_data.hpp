//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "text_unit.hpp"

#include <cstdint>

namespace duckdb {
namespace text {

struct CodePointRange {
	uint32_t first;
	uint32_t last;
};

inline bool RangesContain(const CodePointRange *ranges, uint32_t count, uint32_t c) {
	uint32_t lower = 0;
	uint32_t upper = count;
	while (lower < upper) {
		auto middle = (lower + upper) / 2;
		if (c < ranges[middle].first) {
			upper = middle;
		} else if (c > ranges[middle].last) {
			lower = middle + 1;
		} else {
			return true;
		}
	}
	return false;
}

struct BreakScriptRange {
	uint32_t first;
	uint32_t last;
	uint32_t script;
};

inline int32_t FindScript(const BreakScriptRange *ranges, uint32_t count, uint32_t c) {
	uint32_t lower = 0;
	uint32_t upper = count;
	while (lower < upper) {
		auto middle = (lower + upper) / 2;
		if (c < ranges[middle].first) {
			upper = middle;
		} else if (c > ranges[middle].last) {
			lower = middle + 1;
		} else {
			return static_cast<int32_t>(ranges[middle].script);
		}
	}
	return -1;
}

struct BreakCategoryTrie {
	const uint8_t *ascii;
	const uint16_t *bmp;
	const uint16_t *supplementary;
	const uint16_t *chunks;
	const uint8_t *blocks;

	uint8_t Get(uint32_t c) const {
		if (c < 0x80) {
			return ascii[c];
		}
		if (c < 0x10000) {
			return blocks[(static_cast<uint32_t>(bmp[c >> 6]) << 6) | (c & 0x3F)];
		}
		auto chunk = supplementary[(c - 0x10000) >> 12];
		return blocks[(static_cast<uint32_t>(chunks[chunk + ((c >> 6) & 0x3F)]) << 6) | (c & 0x3F)];
	}
};

struct BreakRuleSet {
	const BreakCategoryTrie *categories;
	const uint8_t *states;
	const int32_t *statuses;
	uint32_t category_count;
	uint32_t state_count;
	uint32_t dictionary_categories_start;
	bool bof_required;
	uint32_t status_count;
};

enum class BreakKind : uint8_t { WORD, SENTENCE };

struct BreakRuleLocale {
	const char *locale;
	BreakKind kind;
	const BreakRuleSet *rules;
};

struct BreakDictionary {
	uint32_t unit;
	uint8_t trie_type;
	bool has_values;
	uint32_t transform;
};

enum class BreakDictionaryId : uint8_t { CJ = 0, THAI = 1, LAO = 2, BURMESE = 3, KHMER = 4 };

struct SentenceException {
	uint32_t offset;
	uint32_t length;
	uint32_t value;
};

struct SentenceExceptionLocale {
	const char *locale;
	uint32_t first_backward;
	uint32_t backward_count;
	uint32_t first_forward;
	uint32_t forward_count;
};

extern const BreakRuleSet break_rules_word;
extern const BreakRuleSet break_rules_word_posix;
extern const BreakRuleSet break_rules_sentence;
extern const BreakRuleSet break_rules_sentence_el;
extern const BreakRuleLocale break_rule_locales[];
extern const uint32_t break_rule_locale_count;

extern const CodePointRange break_engine_thai_word[];
extern const uint32_t break_engine_thai_word_count;
extern const CodePointRange break_engine_thai_marks[];
extern const uint32_t break_engine_thai_marks_count;
extern const CodePointRange break_engine_thai_end_word[];
extern const uint32_t break_engine_thai_end_word_count;
extern const CodePointRange break_engine_thai_begin_word[];
extern const uint32_t break_engine_thai_begin_word_count;
extern const CodePointRange break_engine_lao_word[];
extern const uint32_t break_engine_lao_word_count;
extern const CodePointRange break_engine_lao_marks[];
extern const uint32_t break_engine_lao_marks_count;
extern const CodePointRange break_engine_lao_end_word[];
extern const uint32_t break_engine_lao_end_word_count;
extern const CodePointRange break_engine_lao_begin_word[];
extern const uint32_t break_engine_lao_begin_word_count;
extern const CodePointRange break_engine_burmese_word[];
extern const uint32_t break_engine_burmese_word_count;
extern const CodePointRange break_engine_burmese_marks[];
extern const uint32_t break_engine_burmese_marks_count;
extern const CodePointRange break_engine_burmese_end_word[];
extern const uint32_t break_engine_burmese_end_word_count;
extern const CodePointRange break_engine_burmese_begin_word[];
extern const uint32_t break_engine_burmese_begin_word_count;
extern const CodePointRange break_engine_khmer_word[];
extern const uint32_t break_engine_khmer_word_count;
extern const CodePointRange break_engine_khmer_marks[];
extern const uint32_t break_engine_khmer_marks_count;
extern const CodePointRange break_engine_khmer_end_word[];
extern const uint32_t break_engine_khmer_end_word_count;
extern const CodePointRange break_engine_khmer_begin_word[];
extern const uint32_t break_engine_khmer_begin_word_count;
extern const CodePointRange break_engine_cj_word[];
extern const uint32_t break_engine_cj_word_count;
extern const BreakScriptRange break_engine_unhandled[];
extern const uint32_t break_engine_unhandled_count;
extern const CodePointRange nfkc_no_boundary_before[];
extern const uint32_t nfkc_no_boundary_before_count;
extern const CodePointRange nonspacing_marks[];
extern const uint32_t nonspacing_marks_count;

extern const uint32_t sentence_exception_chars[];
extern const SentenceException sentence_exception_entries[];
extern const SentenceExceptionLocale sentence_exception_locales[];
extern const uint32_t sentence_exception_locale_count;

extern const BreakDictionary break_dictionaries[];

extern const uint16_t greek_upper_0370[];
extern const uint16_t greek_upper_1f00[];
extern const uint16_t greek_upper_2126;

extern const char *const iso_languages[];
extern const uint32_t iso_languages_current_count;
extern const uint32_t iso_languages_count;
extern const char *const iso_languages_3[];
extern const uint32_t iso_languages_3_current_count;
extern const uint32_t iso_languages_3_count;
extern const char *const iso_countries[];
extern const uint32_t iso_countries_current_count;
extern const uint32_t iso_countries_count;
extern const char *const iso_countries_3[];
extern const uint32_t iso_countries_3_current_count;
extern const uint32_t iso_countries_3_count;

struct CollationLocale {
	const char *locale;
	const char *collation;
	bool supported;
};

extern const CollationLocale collation_locales[];
extern const uint32_t collation_locale_count;

extern const TextUnit text_units[];
extern const uint32_t text_unit_count;
extern const uint32_t text_normalization_unit;
extern const uint32_t text_case_unit;
extern const char *const text_icu_version;
extern const char *const text_unicode_version;

} // namespace text
} // namespace duckdb
