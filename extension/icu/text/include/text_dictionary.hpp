//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_dictionary.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "text_data.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace text {

enum class BreakUnits : uint8_t { UTF16, UTF8 };

enum class TrieResult : uint8_t { NO_MATCH = 0, NO_VALUE = 1, FINAL_VALUE = 2, INTERMEDIATE_VALUE = 3 };

inline bool TrieHasValue(TrieResult result) {
	return result >= TrieResult::FINAL_VALUE;
}
inline bool TrieHasNext(TrieResult result) {
	return (static_cast<uint8_t>(result) & 1) != 0;
}

class BytesTrie {
public:
	explicit BytesTrie(const uint8_t *bytes) : bytes(bytes), position(bytes), remaining_match_length(-1) {
	}

	TrieResult First(int32_t in_byte);
	TrieResult Next(int32_t in_byte);
	int32_t GetValue() const;

private:
	TrieResult NextImpl(const uint8_t *pos, int32_t in_byte);
	TrieResult BranchNext(const uint8_t *pos, int32_t length, int32_t in_byte);

	const uint8_t *bytes;
	const uint8_t *position;
	int32_t remaining_match_length;
};

class UCharsTrie {
public:
	explicit UCharsTrie(const uint16_t *units) : units(units), position(units), remaining_match_length(-1) {
	}

	TrieResult First(int32_t unit);
	TrieResult Next(int32_t unit);
	int32_t GetValue() const;

private:
	TrieResult NextImpl(const uint16_t *pos, int32_t unit);
	TrieResult BranchNext(const uint16_t *pos, int32_t length, int32_t unit);

	const uint16_t *units;
	const uint16_t *position;
	int32_t remaining_match_length;
};

class EngineText {
public:
	void Load(const uint8_t *data, size_t size, size_t start, size_t end, BreakUnits units);

	int32_t NativeIndex() const {
		return native[index];
	}
	int32_t NativeLimit() const {
		return native[limit];
	}
	void SetNativeIndex(int32_t position);
	int32_t Next32() {
		if (index >= count) {
			return -1;
		}
		return static_cast<int32_t>(chars[index++]);
	}
	int32_t Current32() const {
		return index < count ? static_cast<int32_t>(chars[index]) : -1;
	}
	int32_t Previous32() {
		if (index == 0) {
			return -1;
		}
		return static_cast<int32_t>(chars[--index]);
	}
	void MoveIndex32(int32_t delta);
	size_t ByteOffset(int32_t position) const;

private:
	size_t Find(int32_t position) const;

	std::vector<uint32_t> chars;
	std::vector<int32_t> native;
	std::vector<size_t> bytes;
	size_t count = 0;
	size_t limit = 0;
	size_t index = 0;
};

class DictionaryMatcher {
public:
	explicit DictionaryMatcher(BreakDictionaryId id);

	int32_t Matches(EngineText &text, int32_t max_length, int32_t limit, int32_t *lengths, int32_t *cp_lengths,
	                int32_t *values, int32_t &prefix) const;

	const uint16_t *GetUnits() const {
		return reinterpret_cast<const uint16_t *>(trie);
	}

private:
	int32_t Transform(int32_t c) const;

	const uint8_t *trie;
	uint8_t trie_type;
	uint32_t transform;
};

class DictionaryEngines {
public:
	int32_t FindBreaks(EngineText &text, const BreakCategoryTrie &categories, uint32_t dictionary_categories_start,
	                   std::vector<int32_t> &breaks);

private:
	int32_t DivideSoutheastAsian(BreakDictionaryId id, EngineText &text, int32_t range_start, int32_t range_end,
	                             std::vector<int32_t> &breaks);
	int32_t DivideCj(EngineText &text, int32_t range_start, int32_t range_end, std::vector<int32_t> &breaks);

	std::vector<uint32_t> cj_chars;
	std::vector<int32_t> cj_map;
	std::vector<uint32_t> cj_normalized;
	std::vector<int32_t> cj_normalized_map;
	std::vector<uint32_t> cj_fragment;
	std::vector<uint32_t> cj_fragment_normalized;
	std::vector<uint32_t> cj_best;
	std::vector<int32_t> cj_previous;
	std::vector<int32_t> cj_values;
	std::vector<int32_t> cj_lengths;
	std::vector<int32_t> cj_boundaries;
};

} // namespace text
} // namespace duckdb
