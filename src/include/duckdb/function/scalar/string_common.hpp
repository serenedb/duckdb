#pragma once

#include <bit>

#include "duckdb/common/swar.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "re2/literal_finder.h"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

bool IsAscii(const char *input, idx_t n);

//! Returns the index of the first byte with the high bit set, or n if all bytes are ASCII
inline idx_t FirstNonAscii(const char *input, idx_t n) {
	// Check 8 bytes at a time
	idx_t i = 0;
	for (; i + SwarWord::SIZE <= n; i += SwarWord::SIZE) {
		auto high = Load<uint64_t>(const_data_ptr_cast(input + i)) & SwarWord::MSB;
		if (high) {
			// non-ascii character in the next 8 bytes
			return i + SwarWord::FirstFlagged(high);
		}
	}
	if (i == n) {
		return n;
	}
	if (n >= SwarWord::SIZE) {
		auto high = Load<uint64_t>(const_data_ptr_cast(input + n - SwarWord::SIZE)) & SwarWord::MSB;
		return high ? n - SwarWord::SIZE + SwarWord::FirstFlagged(high) : n;
	}
	// Less than 8 bytes remain
	for (; i < n; i++) {
		if (input[i] & 0x80) {
			return i;
		}
	}
	return n;
}

//! Decodes one codepoint and returns its length - throws an InternalException on invalid UTF-8
idx_t DecodeCodepoint(const char *input, idx_t size, int32_t &codepoint);
void FillRepeated(ExpressionState &state, char *data, idx_t unit_size, idx_t size);
inline uint64_t Utf8ContinuationBytes(uint64_t word) {
	return word & ~(word << 1) & SwarWord::MSB;
}

inline idx_t Utf8CharacterCount(const char *input, idx_t n) {
	idx_t continuation = 0;
	idx_t i = 0;
	for (; i + SwarWord::SIZE <= n; i += SwarWord::SIZE) {
		continuation += std::popcount(Utf8ContinuationBytes(Load<uint64_t>(const_data_ptr_cast(input + i))));
	}
	if (i == n) {
		return n - continuation;
	}
	if (n >= SwarWord::SIZE) {
		const auto unseen = ~uint64_t(0) << ((SwarWord::SIZE - (n - i)) * 8);
		const auto last = Load<uint64_t>(const_data_ptr_cast(input + n - SwarWord::SIZE));
		return n - continuation - std::popcount(Utf8ContinuationBytes(last) & unseen);
	}
	for (; i < n; i++) {
		continuation += (static_cast<uint8_t>(input[i]) & 0xC0) == 0x80;
	}
	return n - continuation;
}
idx_t SegmentedGraphemeCount(const char *input, idx_t size);
idx_t LowerLength(const char *input_data, idx_t input_length);
idx_t LowerCase(const char *input_data, idx_t input_length, char *result_data, idx_t result_capacity);
idx_t FindStrInStr(const string_t &haystack_s, const string_t &needle_s);
idx_t FindStrInStr(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle, idx_t needle_size);
idx_t FindStrInStr(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle, idx_t needle_size,
                   const duckdb_re2::LiteralFinder &finder);
string_t SubstringASCII(Vector &result, string_t input, int64_t offset, int64_t length);
string_t SubstringUnicode(Vector &result, string_t input, int64_t offset, int64_t length);
string_t SubstringGrapheme(Vector &result, string_t input, int64_t offset, int64_t length);
//! Whether the offset and length are within the range supported by substring - it throws for values outside of it
bool SubstringInSupportedRange(int64_t offset, int64_t length);
//! Turns PostgreSQL's substring offset and length into a positive offset and a non-negative length with the same
//! result - it throws for a negative length
void PostgresSubstringBounds(int64_t &offset, int64_t &length);
//! The position after the part PostgreSQL's overlay replaces - it throws for a start before the first position
int64_t PostgresOverlayEnd(int64_t start, int64_t count);
unique_ptr<BaseStatistics> PropagateStringSliceStats(FunctionStatisticsInput &input, idx_t start_character_index,
                                                     optional_idx character_count);
//! Common util zonemap pruning for `prefix(s, constant)`.
FilterPropagateResult PrefixFilterPrune(const FunctionStatisticsPruneInput &input);

ScalarFunction GetStringContains();
DUCKDB_API bool Glob(const char *s, idx_t slen, const char *pattern, idx_t plen, bool allow_question_mark = true);

static inline bool IsCharacter(char c) {
	return (c & 0xc0) != 0x80;
}

template <class TA, class TR>
static inline TR Length(TA input) {
	return UnsafeNumericCast<TR>(Utf8CharacterCount(input.GetData(), input.GetSize()));
}

template <class TA, class TR>
static inline TR GraphemeCount(TA input) {
	auto input_data = input.GetData();
	auto input_length = input.GetSize();
	if (FirstNonAscii(input_data, input_length) == input_length) {
		// ASCII: every byte is a grapheme cluster
		return UnsafeNumericCast<TR>(input_length);
	}
	return UnsafeNumericCast<TR>(SegmentedGraphemeCount(input_data, input_length));
}

} // namespace duckdb
