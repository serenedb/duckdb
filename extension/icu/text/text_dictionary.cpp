// The tries and the dictionary break engines are ports of ICU 78.3 (bytestrie.cpp,
// ucharstrie.cpp, dictionarydata.cpp, dictbe.cpp, rbbi_cache.cpp).
// Copyright (C) 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

#include "text_dictionary.hpp"

#include "text_normalizer.hpp"
#include "text_utf8.hpp"

#include <algorithm>
#include <limits>
#include <mutex>

namespace duckdb {
namespace text {

namespace {

namespace bytes_trie {
constexpr int32_t MAX_BRANCH_LINEAR_SUB_NODE_LENGTH = 5;
constexpr int32_t MIN_LINEAR_MATCH = 0x10;
constexpr int32_t MAX_LINEAR_MATCH_LENGTH = 0x10;
constexpr int32_t MIN_VALUE_LEAD = MIN_LINEAR_MATCH + MAX_LINEAR_MATCH_LENGTH;
constexpr int32_t VALUE_IS_FINAL = 1;
constexpr int32_t MIN_ONE_BYTE_VALUE_LEAD = MIN_VALUE_LEAD / 2;
constexpr int32_t MAX_ONE_BYTE_VALUE = 0x40;
constexpr int32_t MIN_TWO_BYTE_VALUE_LEAD = MIN_ONE_BYTE_VALUE_LEAD + MAX_ONE_BYTE_VALUE + 1;
constexpr int32_t MAX_TWO_BYTE_VALUE = 0x1AFF;
constexpr int32_t MIN_THREE_BYTE_VALUE_LEAD = MIN_TWO_BYTE_VALUE_LEAD + (MAX_TWO_BYTE_VALUE >> 8) + 1;
constexpr int32_t FOUR_BYTE_VALUE_LEAD = 0x7E;
constexpr int32_t MAX_ONE_BYTE_DELTA = 0xBF;
constexpr int32_t MIN_TWO_BYTE_DELTA_LEAD = MAX_ONE_BYTE_DELTA + 1;
constexpr int32_t MIN_THREE_BYTE_DELTA_LEAD = 0xF0;
constexpr int32_t FOUR_BYTE_DELTA_LEAD = 0xFE;

int32_t ReadValue(const uint8_t *pos, int32_t lead) {
	if (lead < MIN_TWO_BYTE_VALUE_LEAD) {
		return lead - MIN_ONE_BYTE_VALUE_LEAD;
	}
	if (lead < MIN_THREE_BYTE_VALUE_LEAD) {
		return ((lead - MIN_TWO_BYTE_VALUE_LEAD) << 8) | *pos;
	}
	if (lead < FOUR_BYTE_VALUE_LEAD) {
		return ((lead - MIN_THREE_BYTE_VALUE_LEAD) << 16) | (pos[0] << 8) | pos[1];
	}
	if (lead == FOUR_BYTE_VALUE_LEAD) {
		return (pos[0] << 16) | (pos[1] << 8) | pos[2];
	}
	return static_cast<int32_t>((static_cast<uint32_t>(pos[0]) << 24) | (pos[1] << 16) | (pos[2] << 8) | pos[3]);
}

const uint8_t *SkipValue(const uint8_t *pos, int32_t lead) {
	if (lead >= (MIN_TWO_BYTE_VALUE_LEAD << 1)) {
		if (lead < (MIN_THREE_BYTE_VALUE_LEAD << 1)) {
			++pos;
		} else if (lead < (FOUR_BYTE_VALUE_LEAD << 1)) {
			pos += 2;
		} else {
			pos += 3 + ((lead >> 1) & 1);
		}
	}
	return pos;
}

const uint8_t *SkipValue(const uint8_t *pos) {
	int32_t lead = *pos++;
	return SkipValue(pos, lead);
}

const uint8_t *JumpByDelta(const uint8_t *pos) {
	int32_t delta = *pos++;
	if (delta >= MIN_TWO_BYTE_DELTA_LEAD) {
		if (delta < MIN_THREE_BYTE_DELTA_LEAD) {
			delta = ((delta - MIN_TWO_BYTE_DELTA_LEAD) << 8) | *pos++;
		} else if (delta < FOUR_BYTE_DELTA_LEAD) {
			delta = ((delta - MIN_THREE_BYTE_DELTA_LEAD) << 16) | (pos[0] << 8) | pos[1];
			pos += 2;
		} else if (delta == FOUR_BYTE_DELTA_LEAD) {
			delta = (pos[0] << 16) | (pos[1] << 8) | pos[2];
			pos += 3;
		} else {
			delta =
			    static_cast<int32_t>((static_cast<uint32_t>(pos[0]) << 24) | (pos[1] << 16) | (pos[2] << 8) | pos[3]);
			pos += 4;
		}
	}
	return pos + delta;
}

const uint8_t *SkipDelta(const uint8_t *pos) {
	int32_t delta = *pos++;
	if (delta >= MIN_TWO_BYTE_DELTA_LEAD) {
		if (delta < MIN_THREE_BYTE_DELTA_LEAD) {
			++pos;
		} else if (delta < FOUR_BYTE_DELTA_LEAD) {
			pos += 2;
		} else {
			pos += 3 + (delta & 1);
		}
	}
	return pos;
}

TrieResult ValueResult(int32_t node) {
	return static_cast<TrieResult>(static_cast<uint8_t>(TrieResult::INTERMEDIATE_VALUE) - (node & VALUE_IS_FINAL));
}
} // namespace bytes_trie

} // namespace

TrieResult BytesTrie::BranchNext(const uint8_t *pos, int32_t length, int32_t in_byte) {
	using namespace bytes_trie;
	if (length == 0) {
		length = *pos++;
	}
	++length;
	while (length > MAX_BRANCH_LINEAR_SUB_NODE_LENGTH) {
		if (in_byte < *pos++) {
			length >>= 1;
			pos = JumpByDelta(pos);
		} else {
			length = length - (length >> 1);
			pos = SkipDelta(pos);
		}
	}
	do {
		if (in_byte == *pos++) {
			TrieResult result;
			int32_t node = *pos;
			if (node & VALUE_IS_FINAL) {
				result = TrieResult::FINAL_VALUE;
			} else {
				++pos;
				node >>= 1;
				int32_t delta;
				if (node < MIN_TWO_BYTE_VALUE_LEAD) {
					delta = node - MIN_ONE_BYTE_VALUE_LEAD;
				} else if (node < MIN_THREE_BYTE_VALUE_LEAD) {
					delta = ((node - MIN_TWO_BYTE_VALUE_LEAD) << 8) | *pos++;
				} else if (node < FOUR_BYTE_VALUE_LEAD) {
					delta = ((node - MIN_THREE_BYTE_VALUE_LEAD) << 16) | (pos[0] << 8) | pos[1];
					pos += 2;
				} else if (node == FOUR_BYTE_VALUE_LEAD) {
					delta = (pos[0] << 16) | (pos[1] << 8) | pos[2];
					pos += 3;
				} else {
					delta = static_cast<int32_t>((static_cast<uint32_t>(pos[0]) << 24) | (pos[1] << 16) |
					                             (pos[2] << 8) | pos[3]);
					pos += 4;
				}
				pos += delta;
				node = *pos;
				result = node >= MIN_VALUE_LEAD ? ValueResult(node) : TrieResult::NO_VALUE;
			}
			position = pos;
			return result;
		}
		--length;
		pos = SkipValue(pos);
	} while (length > 1);
	if (in_byte == *pos++) {
		position = pos;
		int32_t node = *pos;
		return node >= MIN_VALUE_LEAD ? ValueResult(node) : TrieResult::NO_VALUE;
	}
	position = nullptr;
	return TrieResult::NO_MATCH;
}

TrieResult BytesTrie::NextImpl(const uint8_t *pos, int32_t in_byte) {
	using namespace bytes_trie;
	for (;;) {
		int32_t node = *pos++;
		if (node < MIN_LINEAR_MATCH) {
			return BranchNext(pos, node, in_byte);
		} else if (node < MIN_VALUE_LEAD) {
			int32_t length = node - MIN_LINEAR_MATCH;
			if (in_byte == *pos++) {
				remaining_match_length = --length;
				position = pos;
				return (length < 0 && (node = *pos) >= MIN_VALUE_LEAD) ? ValueResult(node) : TrieResult::NO_VALUE;
			}
			break;
		} else if (node & VALUE_IS_FINAL) {
			break;
		} else {
			pos = SkipValue(pos, node);
		}
	}
	position = nullptr;
	return TrieResult::NO_MATCH;
}

TrieResult BytesTrie::First(int32_t in_byte) {
	remaining_match_length = -1;
	if (in_byte < 0) {
		in_byte += 0x100;
	}
	return NextImpl(bytes, in_byte);
}

TrieResult BytesTrie::Next(int32_t in_byte) {
	using namespace bytes_trie;
	const uint8_t *pos = position;
	if (!pos) {
		return TrieResult::NO_MATCH;
	}
	if (in_byte < 0) {
		in_byte += 0x100;
	}
	int32_t length = remaining_match_length;
	if (length >= 0) {
		if (in_byte == *pos++) {
			remaining_match_length = --length;
			position = pos;
			int32_t node;
			return (length < 0 && (node = *pos) >= MIN_VALUE_LEAD) ? ValueResult(node) : TrieResult::NO_VALUE;
		}
		position = nullptr;
		return TrieResult::NO_MATCH;
	}
	return NextImpl(pos, in_byte);
}

int32_t BytesTrie::GetValue() const {
	const uint8_t *pos = position;
	int32_t lead = *pos++;
	return bytes_trie::ReadValue(pos, lead >> 1);
}

namespace {

namespace uchars_trie {
constexpr int32_t MAX_BRANCH_LINEAR_SUB_NODE_LENGTH = 5;
constexpr int32_t MIN_LINEAR_MATCH = 0x30;
constexpr int32_t MAX_LINEAR_MATCH_LENGTH = 0x10;
constexpr int32_t MIN_VALUE_LEAD = MIN_LINEAR_MATCH + MAX_LINEAR_MATCH_LENGTH;
constexpr int32_t NODE_TYPE_MASK = MIN_VALUE_LEAD - 1;
constexpr int32_t VALUE_IS_FINAL = 0x8000;
constexpr int32_t MAX_ONE_UNIT_VALUE = 0x3FFF;
constexpr int32_t MIN_TWO_UNIT_VALUE_LEAD = MAX_ONE_UNIT_VALUE + 1;
constexpr int32_t THREE_UNIT_VALUE_LEAD = 0x7FFF;
constexpr int32_t MAX_ONE_UNIT_NODE_VALUE = 0xFF;
constexpr int32_t MIN_TWO_UNIT_NODE_VALUE_LEAD = MIN_VALUE_LEAD + ((MAX_ONE_UNIT_NODE_VALUE + 1) << 6);
constexpr int32_t THREE_UNIT_NODE_VALUE_LEAD = 0x7FC0;
constexpr int32_t MAX_ONE_UNIT_DELTA = 0xFBFF;
constexpr int32_t MIN_TWO_UNIT_DELTA_LEAD = MAX_ONE_UNIT_DELTA + 1;
constexpr int32_t THREE_UNIT_DELTA_LEAD = 0xFFFF;

int32_t ReadValue(const uint16_t *pos, int32_t lead) {
	if (lead < MIN_TWO_UNIT_VALUE_LEAD) {
		return lead;
	}
	if (lead < THREE_UNIT_VALUE_LEAD) {
		return ((lead - MIN_TWO_UNIT_VALUE_LEAD) << 16) | *pos;
	}
	return (pos[0] << 16) | pos[1];
}

const uint16_t *SkipValue(const uint16_t *pos, int32_t lead) {
	if (lead >= MIN_TWO_UNIT_VALUE_LEAD) {
		if (lead < THREE_UNIT_VALUE_LEAD) {
			++pos;
		} else {
			pos += 2;
		}
	}
	return pos;
}

const uint16_t *SkipValue(const uint16_t *pos) {
	int32_t lead = *pos++;
	return SkipValue(pos, lead & 0x7FFF);
}

int32_t ReadNodeValue(const uint16_t *pos, int32_t lead) {
	if (lead < MIN_TWO_UNIT_NODE_VALUE_LEAD) {
		return (lead >> 6) - 1;
	}
	if (lead < THREE_UNIT_NODE_VALUE_LEAD) {
		return (((lead & 0x7FC0) - MIN_TWO_UNIT_NODE_VALUE_LEAD) << 10) | *pos;
	}
	return (pos[0] << 16) | pos[1];
}

const uint16_t *SkipNodeValue(const uint16_t *pos, int32_t lead) {
	if (lead >= MIN_TWO_UNIT_NODE_VALUE_LEAD) {
		if (lead < THREE_UNIT_NODE_VALUE_LEAD) {
			++pos;
		} else {
			pos += 2;
		}
	}
	return pos;
}

const uint16_t *JumpByDelta(const uint16_t *pos) {
	int32_t delta = *pos++;
	if (delta >= MIN_TWO_UNIT_DELTA_LEAD) {
		if (delta == THREE_UNIT_DELTA_LEAD) {
			delta = (pos[0] << 16) | pos[1];
			pos += 2;
		} else {
			delta = ((delta - MIN_TWO_UNIT_DELTA_LEAD) << 16) | *pos++;
		}
	}
	return pos + delta;
}

const uint16_t *SkipDelta(const uint16_t *pos) {
	int32_t delta = *pos++;
	if (delta >= MIN_TWO_UNIT_DELTA_LEAD) {
		if (delta == THREE_UNIT_DELTA_LEAD) {
			pos += 2;
		} else {
			++pos;
		}
	}
	return pos;
}

TrieResult ValueResult(int32_t node) {
	return static_cast<TrieResult>(static_cast<uint8_t>(TrieResult::INTERMEDIATE_VALUE) - (node >> 15));
}
} // namespace uchars_trie

} // namespace

TrieResult UCharsTrie::BranchNext(const uint16_t *pos, int32_t length, int32_t unit) {
	using namespace uchars_trie;
	if (length == 0) {
		length = *pos++;
	}
	++length;
	while (length > MAX_BRANCH_LINEAR_SUB_NODE_LENGTH) {
		if (unit < *pos++) {
			length >>= 1;
			pos = JumpByDelta(pos);
		} else {
			length = length - (length >> 1);
			pos = SkipDelta(pos);
		}
	}
	do {
		if (unit == *pos++) {
			TrieResult result;
			int32_t node = *pos;
			if (node & VALUE_IS_FINAL) {
				result = TrieResult::FINAL_VALUE;
			} else {
				++pos;
				int32_t delta;
				if (node < MIN_TWO_UNIT_VALUE_LEAD) {
					delta = node;
				} else if (node < THREE_UNIT_VALUE_LEAD) {
					delta = ((node - MIN_TWO_UNIT_VALUE_LEAD) << 16) | *pos++;
				} else {
					delta = (pos[0] << 16) | pos[1];
					pos += 2;
				}
				pos += delta;
				node = *pos;
				result = node >= MIN_VALUE_LEAD ? ValueResult(node) : TrieResult::NO_VALUE;
			}
			position = pos;
			return result;
		}
		--length;
		pos = SkipValue(pos);
	} while (length > 1);
	if (unit == *pos++) {
		position = pos;
		int32_t node = *pos;
		return node >= MIN_VALUE_LEAD ? ValueResult(node) : TrieResult::NO_VALUE;
	}
	position = nullptr;
	return TrieResult::NO_MATCH;
}

TrieResult UCharsTrie::NextImpl(const uint16_t *pos, int32_t unit) {
	using namespace uchars_trie;
	int32_t node = *pos++;
	for (;;) {
		if (node < MIN_LINEAR_MATCH) {
			return BranchNext(pos, node, unit);
		} else if (node < MIN_VALUE_LEAD) {
			int32_t length = node - MIN_LINEAR_MATCH;
			if (unit == *pos++) {
				remaining_match_length = --length;
				position = pos;
				return (length < 0 && (node = *pos) >= MIN_VALUE_LEAD) ? ValueResult(node) : TrieResult::NO_VALUE;
			}
			break;
		} else if (node & VALUE_IS_FINAL) {
			break;
		} else {
			pos = SkipNodeValue(pos, node);
			node &= NODE_TYPE_MASK;
		}
	}
	position = nullptr;
	return TrieResult::NO_MATCH;
}

TrieResult UCharsTrie::First(int32_t unit) {
	remaining_match_length = -1;
	return NextImpl(units, unit);
}

TrieResult UCharsTrie::Next(int32_t unit) {
	using namespace uchars_trie;
	const uint16_t *pos = position;
	if (!pos) {
		return TrieResult::NO_MATCH;
	}
	int32_t length = remaining_match_length;
	if (length >= 0) {
		if (unit == *pos++) {
			remaining_match_length = --length;
			position = pos;
			int32_t node;
			return (length < 0 && (node = *pos) >= MIN_VALUE_LEAD) ? ValueResult(node) : TrieResult::NO_VALUE;
		}
		position = nullptr;
		return TrieResult::NO_MATCH;
	}
	return NextImpl(pos, unit);
}

int32_t UCharsTrie::GetValue() const {
	using namespace uchars_trie;
	const uint16_t *pos = position;
	int32_t lead = *pos++;
	return (lead & VALUE_IS_FINAL) ? ReadValue(pos, lead & 0x7FFF) : ReadNodeValue(pos, lead);
}

void EngineText::Load(const uint8_t *data, size_t size, size_t start, size_t end, BreakUnits units) {
	chars.clear();
	native.clear();
	bytes.clear();
	size_t position = start;
	int32_t current = 0;
	auto decode = [&]() {
		bytes.push_back(position);
		native.push_back(current);
		auto before = position;
		auto c = DecodeUtf8(data, size, position);
		chars.push_back(c);
		current +=
		    units == BreakUnits::UTF8 ? static_cast<int32_t>(position - before) : static_cast<int32_t>(Utf16Length(c));
	};
	while (position < end) {
		decode();
	}
	limit = chars.size();
	if (position < size) {
		decode();
	}
	count = chars.size();
	bytes.push_back(position);
	native.push_back(current);
	index = 0;
}

size_t EngineText::Find(int32_t position) const {
	auto entry = std::upper_bound(native.begin(), native.begin() + static_cast<int64_t>(count) + 1, position);
	if (entry == native.begin()) {
		return 0;
	}
	return static_cast<size_t>(entry - native.begin()) - 1;
}

void EngineText::SetNativeIndex(int32_t position) {
	index = Find(position);
}

void EngineText::MoveIndex32(int32_t delta) {
	auto target = static_cast<int64_t>(index) + delta;
	index = static_cast<size_t>(std::max<int64_t>(0, std::min<int64_t>(target, static_cast<int64_t>(count))));
}

size_t EngineText::ByteOffset(int32_t position) const {
	return bytes[Find(position)];
}

namespace {

constexpr int32_t TRANSFORM_TYPE_OFFSET = 0x1000000;
constexpr int32_t TRANSFORM_TYPE_MASK = 0x7F000000;
constexpr int32_t TRANSFORM_OFFSET_MASK = 0x1FFFFF;
constexpr uint8_t TRIE_TYPE_UCHARS = 1;

const uint8_t *LoadDictionary(BreakDictionaryId id) {
	static const uint8_t *loaded[5] = {};
	static std::once_flag once[5];
	auto index = static_cast<size_t>(id);
	std::call_once(once[index], [&]() { loaded[index] = LoadUnit(text_units[break_dictionaries[index].unit]).data; });
	return loaded[index];
}

} // namespace

DictionaryMatcher::DictionaryMatcher(BreakDictionaryId id)
    : trie(LoadDictionary(id)), trie_type(break_dictionaries[static_cast<size_t>(id)].trie_type),
      transform(break_dictionaries[static_cast<size_t>(id)].transform) {
}

int32_t DictionaryMatcher::Transform(int32_t c) const {
	if ((static_cast<int32_t>(transform) & TRANSFORM_TYPE_MASK) == TRANSFORM_TYPE_OFFSET) {
		if (c == 0x200D) {
			return 0xFF;
		}
		if (c == 0x200C) {
			return 0xFE;
		}
		int32_t delta = c - (static_cast<int32_t>(transform) & TRANSFORM_OFFSET_MASK);
		if (delta < 0 || 0xFD < delta) {
			return -1;
		}
		return delta;
	}
	return c;
}

int32_t DictionaryMatcher::Matches(EngineText &text, int32_t max_length, int32_t limit, int32_t *lengths,
                                   int32_t *cp_lengths, int32_t *values, int32_t &prefix) const {
	int32_t starting_index = text.NativeIndex();
	int32_t word_count = 0;
	int32_t code_points_matched = 0;
	auto run = [&](auto &trie, auto map) {
		for (int32_t c = text.Next32(); c >= 0; c = text.Next32()) {
			auto result = code_points_matched == 0 ? trie.First(map(c)) : trie.Next(map(c));
			int32_t length_matched = text.NativeIndex() - starting_index;
			code_points_matched += 1;
			if (TrieHasValue(result)) {
				if (word_count < limit) {
					if (values) {
						values[word_count] = trie.GetValue();
					}
					if (lengths) {
						lengths[word_count] = length_matched;
					}
					if (cp_lengths) {
						cp_lengths[word_count] = code_points_matched;
					}
					++word_count;
				}
				if (result == TrieResult::FINAL_VALUE) {
					break;
				}
			} else if (result == TrieResult::NO_MATCH) {
				break;
			}
			if (length_matched >= max_length) {
				break;
			}
		}
	};
	if (trie_type == TRIE_TYPE_UCHARS) {
		UCharsTrie trie(GetUnits());
		run(trie, [](int32_t c) { return c; });
	} else {
		BytesTrie trie(this->trie);
		run(trie, [this](int32_t c) { return Transform(c); });
	}
	prefix = code_points_matched;
	return word_count;
}

namespace {

struct CodePointSet {
	const CodePointRange *ranges;
	uint32_t count;

	bool Contains(int32_t c) const {
		return c >= 0 && RangesContain(ranges, count, static_cast<uint32_t>(c));
	}
};

struct SoutheastAsianEngine {
	BreakDictionaryId dictionary;
	CodePointSet word;
	CodePointSet marks;
	CodePointSet end_word;
	CodePointSet begin_word;
	bool thai;
};

const SoutheastAsianEngine &GetEngine(BreakDictionaryId id) {
	static const SoutheastAsianEngine engines[] = {
	    {BreakDictionaryId::CJ, {}, {}, {}, {}, false},
	    {BreakDictionaryId::THAI,
	     {break_engine_thai_word, break_engine_thai_word_count},
	     {break_engine_thai_marks, break_engine_thai_marks_count},
	     {break_engine_thai_end_word, break_engine_thai_end_word_count},
	     {break_engine_thai_begin_word, break_engine_thai_begin_word_count},
	     true},
	    {BreakDictionaryId::LAO,
	     {break_engine_lao_word, break_engine_lao_word_count},
	     {break_engine_lao_marks, break_engine_lao_marks_count},
	     {break_engine_lao_end_word, break_engine_lao_end_word_count},
	     {break_engine_lao_begin_word, break_engine_lao_begin_word_count},
	     false},
	    {BreakDictionaryId::BURMESE,
	     {break_engine_burmese_word, break_engine_burmese_word_count},
	     {break_engine_burmese_marks, break_engine_burmese_marks_count},
	     {break_engine_burmese_end_word, break_engine_burmese_end_word_count},
	     {break_engine_burmese_begin_word, break_engine_burmese_begin_word_count},
	     false},
	    {BreakDictionaryId::KHMER,
	     {break_engine_khmer_word, break_engine_khmer_word_count},
	     {break_engine_khmer_marks, break_engine_khmer_marks_count},
	     {break_engine_khmer_end_word, break_engine_khmer_end_word_count},
	     {break_engine_khmer_begin_word, break_engine_khmer_begin_word_count},
	     false},
	};
	return engines[static_cast<size_t>(id)];
}

constexpr int32_t LOOKAHEAD = 3;
constexpr int32_t ROOT_COMBINE_THRESHOLD = 3;
constexpr int32_t PREFIX_COMBINE_THRESHOLD = 3;
constexpr int32_t MIN_WORD_SPAN = 4;
constexpr int32_t POSSIBLE_WORD_LIST_MAX = 20;
constexpr int32_t THAI_PAIYANNOI = 0x0E2F;
constexpr int32_t THAI_MAIYAMOK = 0x0E46;

bool IsThaiSuffix(int32_t c) {
	return c == THAI_PAIYANNOI || c == THAI_MAIYAMOK;
}

class PossibleWord {
public:
	int32_t Candidates(EngineText &text, const DictionaryMatcher &dictionary, int32_t range_end) {
		int32_t start = text.NativeIndex();
		if (start != offset) {
			offset = start;
			count = dictionary.Matches(text, range_end - start, POSSIBLE_WORD_LIST_MAX, cu_lengths, cp_lengths, nullptr,
			                           prefix);
			if (count <= 0) {
				text.SetNativeIndex(start);
			}
		}
		if (count > 0) {
			text.SetNativeIndex(start + cu_lengths[count - 1]);
		}
		current = count - 1;
		mark = current;
		return count;
	}
	int32_t AcceptMarked(EngineText &text) {
		text.SetNativeIndex(offset + cu_lengths[mark]);
		return cu_lengths[mark];
	}
	bool BackUp(EngineText &text) {
		if (current > 0) {
			text.SetNativeIndex(offset + cu_lengths[--current]);
			return true;
		}
		return false;
	}
	int32_t LongestPrefix() const {
		return prefix;
	}
	void MarkCurrent() {
		mark = current;
	}
	int32_t MarkedCpLength() const {
		return cp_lengths[mark];
	}

private:
	int32_t count = 0;
	int32_t prefix = 0;
	int32_t offset = -1;
	int32_t mark = 0;
	int32_t current = 0;
	int32_t cu_lengths[POSSIBLE_WORD_LIST_MAX] = {};
	int32_t cp_lengths[POSSIBLE_WORD_LIST_MAX] = {};
};

} // namespace

int32_t DictionaryEngines::DivideSoutheastAsian(BreakDictionaryId id, EngineText &text, int32_t range_start,
                                                int32_t range_end, std::vector<int32_t> &breaks) {
	auto &engine = GetEngine(id);
	DictionaryMatcher dictionary(id);
	if (engine.thai) {
		text.SetNativeIndex(range_start);
		text.MoveIndex32(MIN_WORD_SPAN);
		if (text.NativeIndex() >= range_end) {
			return 0;
		}
		text.SetNativeIndex(range_start);
	} else if (range_end - range_start < MIN_WORD_SPAN) {
		return 0;
	}

	uint32_t words_found = 0;
	int32_t cp_word_length = 0;
	int32_t cu_word_length = 0;
	int32_t current;
	PossibleWord words[LOOKAHEAD];

	text.SetNativeIndex(range_start);
	while ((current = text.NativeIndex()) < range_end) {
		cp_word_length = 0;
		cu_word_length = 0;

		auto &word = words[words_found % LOOKAHEAD];
		int32_t candidates = word.Candidates(text, dictionary, range_end);
		if (candidates == 1) {
			cu_word_length = word.AcceptMarked(text);
			cp_word_length = word.MarkedCpLength();
			words_found += 1;
		} else if (candidates > 1) {
			bool found_best = text.NativeIndex() >= range_end;
			while (!found_best) {
				auto &second = words[(words_found + 1) % LOOKAHEAD];
				if (second.Candidates(text, dictionary, range_end) > 0) {
					word.MarkCurrent();
					if (text.NativeIndex() >= range_end) {
						break;
					}
					do {
						if (words[(words_found + 2) % LOOKAHEAD].Candidates(text, dictionary, range_end)) {
							word.MarkCurrent();
							found_best = true;
							break;
						}
					} while (second.BackUp(text));
					if (found_best) {
						break;
					}
				}
				if (!word.BackUp(text)) {
					break;
				}
			}
			cu_word_length = word.AcceptMarked(text);
			cp_word_length = word.MarkedCpLength();
			words_found += 1;
		}

		int32_t uc = 0;
		if (text.NativeIndex() < range_end && cp_word_length < ROOT_COMBINE_THRESHOLD) {
			auto &next = words[words_found % LOOKAHEAD];
			if (next.Candidates(text, dictionary, range_end) <= 0 &&
			    (cu_word_length == 0 || next.LongestPrefix() < PREFIX_COMBINE_THRESHOLD)) {
				int32_t remaining = range_end - (current + cu_word_length);
				int32_t chars = 0;
				for (;;) {
					int32_t pc_index = text.NativeIndex();
					int32_t pc = text.Next32();
					int32_t pc_size = text.NativeIndex() - pc_index;
					chars += pc_size;
					remaining -= pc_size;
					if (remaining <= 0) {
						break;
					}
					uc = text.Current32();
					if (engine.end_word.Contains(pc) && engine.begin_word.Contains(uc)) {
						int32_t candidate_count =
						    words[(words_found + 1) % LOOKAHEAD].Candidates(text, dictionary, range_end);
						text.SetNativeIndex(current + cu_word_length + chars);
						if (candidate_count > 0) {
							break;
						}
					}
				}
				if (cu_word_length <= 0) {
					words_found += 1;
				}
				cu_word_length += chars;
			} else {
				text.SetNativeIndex(current + cu_word_length);
			}
		}

		int32_t position;
		while ((position = text.NativeIndex()) < range_end && engine.marks.Contains(text.Current32())) {
			text.Next32();
			cu_word_length += text.NativeIndex() - position;
		}

		if (engine.thai && text.NativeIndex() < range_end && cu_word_length > 0) {
			if (words[words_found % LOOKAHEAD].Candidates(text, dictionary, range_end) <= 0 &&
			    IsThaiSuffix(uc = text.Current32())) {
				if (uc == THAI_PAIYANNOI) {
					if (!IsThaiSuffix(text.Previous32())) {
						text.Next32();
						int32_t paiyannoi_index = text.NativeIndex();
						text.Next32();
						cu_word_length += text.NativeIndex() - paiyannoi_index;
						uc = text.Current32();
					} else {
						text.Next32();
					}
				}
				if (uc == THAI_MAIYAMOK) {
					if (text.Previous32() != THAI_MAIYAMOK) {
						text.Next32();
						int32_t maiyamok_index = text.NativeIndex();
						text.Next32();
						cu_word_length += text.NativeIndex() - maiyamok_index;
					} else {
						text.Next32();
					}
				}
			} else {
				text.SetNativeIndex(current + cu_word_length);
			}
		}

		if (cu_word_length > 0) {
			breaks.push_back(current + cu_word_length);
		}
	}

	if (!breaks.empty() && breaks.back() >= range_end) {
		breaks.pop_back();
		words_found -= 1;
	}
	return static_cast<int32_t>(words_found);
}

namespace {

constexpr uint32_t MAX_SNLP = 255;
constexpr int32_t MAX_KATAKANA_LENGTH = 8;
constexpr int32_t MAX_KATAKANA_GROUP_LENGTH = 20;
constexpr int32_t MAX_WORD_SIZE = 20;

uint32_t KatakanaCost(int32_t word_length) {
	static constexpr uint32_t COSTS[MAX_KATAKANA_LENGTH + 1] = {8192, 984, 408, 240, 204, 252, 300, 372, 480};
	return word_length > MAX_KATAKANA_LENGTH ? 8192 : COSTS[word_length];
}

bool IsKatakana(uint32_t c) {
	return (c >= 0x30A1 && c <= 0x30FE && c != 0x30FB) || (c >= 0xFF66 && c <= 0xFF9F);
}

bool IsHangulSyllable(uint32_t c) {
	return c >= 0xAC00 && c <= 0xD7A3;
}

int32_t MatchCj(const uint16_t *trie_units, const std::vector<uint32_t> &text, size_t start, int32_t max_length,
                int32_t limit, int32_t *cp_lengths, int32_t *values) {
	UCharsTrie trie(trie_units);
	int32_t word_count = 0;
	int32_t code_points_matched = 0;
	int32_t length_matched = 0;
	for (size_t i = start; i < text.size(); i++) {
		auto c = static_cast<int32_t>(text[i]);
		auto result = code_points_matched == 0 ? trie.First(c) : trie.Next(c);
		length_matched += static_cast<int32_t>(Utf16Length(text[i]));
		code_points_matched += 1;
		if (TrieHasValue(result)) {
			if (word_count < limit) {
				values[word_count] = trie.GetValue();
				cp_lengths[word_count] = code_points_matched;
				++word_count;
			}
			if (result == TrieResult::FINAL_VALUE) {
				break;
			}
		} else if (result == TrieResult::NO_MATCH) {
			break;
		}
		if (length_matched >= max_length) {
			break;
		}
	}
	return word_count;
}

} // namespace

int32_t DictionaryEngines::DivideCj(EngineText &text, int32_t range_start, int32_t range_end,
                                    std::vector<int32_t> &breaks) {
	if (range_start >= range_end) {
		return 0;
	}
	cj_chars.clear();
	cj_map.clear();
	text.SetNativeIndex(range_start);
	while (text.NativeIndex() < range_end) {
		auto position = text.NativeIndex();
		auto c = text.Next32();
		if (c < 0) {
			break;
		}
		cj_chars.push_back(static_cast<uint32_t>(c));
		cj_map.push_back(position);
	}
	cj_map.push_back(range_end);

	if (!Normalizer::IsNormalized(NormalizationForm::NFKC, cj_chars.data(), cj_chars.size(), cj_normalized)) {
		cj_normalized.clear();
		cj_normalized_map.clear();
		size_t source = 0;
		while (source < cj_chars.size()) {
			auto fragment_start = source;
			cj_fragment.clear();
			for (;;) {
				cj_fragment.push_back(cj_chars[source]);
				source++;
				if (source == cj_chars.size() || Normalizer::HasNfkcBoundaryBefore(cj_chars[source])) {
					break;
				}
			}
			Normalizer::Normalize(NormalizationForm::NFKC, cj_fragment.data(), cj_fragment.size(),
			                      cj_fragment_normalized);
			cj_normalized.insert(cj_normalized.end(), cj_fragment_normalized.begin(), cj_fragment_normalized.end());
			cj_normalized_map.resize(cj_normalized.size(), cj_map[fragment_start]);
		}
		cj_normalized_map.push_back(cj_map[cj_chars.size()]);
		std::swap(cj_chars, cj_normalized);
		std::swap(cj_map, cj_normalized_map);
	}

	auto code_points = static_cast<int32_t>(cj_chars.size());
	constexpr auto UNREACHED = std::numeric_limits<uint32_t>::max();
	cj_best.assign(static_cast<size_t>(code_points) + 1, UNREACHED);
	cj_best[0] = 0;
	cj_previous.assign(static_cast<size_t>(code_points) + 1, -1);
	cj_values.assign(static_cast<size_t>(code_points) + 1, 0);
	cj_lengths.assign(static_cast<size_t>(code_points) + 1, 0);
	DictionaryMatcher dictionary(BreakDictionaryId::CJ);
	auto trie = dictionary.GetUnits();

	bool previous_katakana = false;
	for (int32_t i = 0; i < code_points; ++i) {
		if (cj_best[i] == UNREACHED) {
			continue;
		}
		int32_t count = MatchCj(trie, cj_chars, static_cast<size_t>(i), MAX_WORD_SIZE, code_points, cj_lengths.data(),
		                        cj_values.data());
		if ((count == 0 || cj_lengths[0] != 1) && !IsHangulSyllable(cj_chars[i])) {
			cj_values[count] = MAX_SNLP;
			cj_lengths[count] = 1;
			count++;
		}
		for (int32_t j = 0; j < count; j++) {
			uint32_t cost = cj_best[i] + static_cast<uint32_t>(cj_values[j]);
			int32_t end = cj_lengths[j] + i;
			if (cost < cj_best[end]) {
				cj_best[end] = cost;
				cj_previous[end] = i;
			}
		}
		bool katakana = IsKatakana(cj_chars[i]);
		int32_t run_length = 1;
		if (!previous_katakana && katakana) {
			int32_t j = i + 1;
			while (j < code_points && run_length < MAX_KATAKANA_GROUP_LENGTH && IsKatakana(cj_chars[j])) {
				j++;
				run_length++;
			}
			if (run_length < MAX_KATAKANA_GROUP_LENGTH) {
				uint32_t cost = cj_best[i] + KatakanaCost(run_length);
				if (cost < cj_best[i + run_length]) {
					cj_best[i + run_length] = cost;
					cj_previous[i + run_length] = i;
				}
			}
		}
		previous_katakana = katakana;
	}

	cj_boundaries.clear();
	if (cj_best[code_points] == UNREACHED) {
		cj_boundaries.push_back(code_points);
	} else {
		for (int32_t i = code_points; i > 0; i = cj_previous[i]) {
			cj_boundaries.push_back(i);
		}
	}
	if (breaks.empty() || breaks.back() < range_start) {
		cj_boundaries.push_back(0);
	}

	int32_t previous_position = -1;
	int32_t corrected = 0;
	for (auto boundary = cj_boundaries.rbegin(); boundary != cj_boundaries.rend(); ++boundary) {
		int32_t position = cj_map[static_cast<size_t>(*boundary)];
		if (position > previous_position) {
			if (position != range_start) {
				breaks.push_back(position);
				corrected++;
			}
		}
		previous_position = position;
	}
	if (!breaks.empty() && breaks.back() == range_end) {
		breaks.pop_back();
		corrected--;
	}
	return corrected;
}

int32_t DictionaryEngines::FindBreaks(EngineText &text, const BreakCategoryTrie &categories,
                                      uint32_t dictionary_categories_start, std::vector<int32_t> &breaks) {
	static const CodePointSet cj {break_engine_cj_word, break_engine_cj_word_count};
	static const BreakDictionaryId southeast_asian[] = {BreakDictionaryId::THAI, BreakDictionaryId::LAO,
	                                                    BreakDictionaryId::BURMESE, BreakDictionaryId::KHMER};

	auto category_of = [&](int32_t c) -> uint32_t {
		return c < 0 ? 0 : categories.Get(static_cast<uint32_t>(c));
	};
	int32_t range_end = text.NativeLimit();
	text.SetNativeIndex(0);
	int32_t c = text.Current32();
	uint32_t category = category_of(c);
	int32_t found = 0;

	for (;;) {
		int32_t current;
		while ((current = text.NativeIndex()) < range_end && category < dictionary_categories_start) {
			text.Next32();
			c = text.Current32();
			category = category_of(c);
		}
		if (current >= range_end) {
			break;
		}

		auto span = [&](const CodePointSet &set) {
			int32_t end = text.NativeIndex();
			int32_t ch = text.Current32();
			while ((end = text.NativeIndex()) < range_end && set.Contains(ch)) {
				text.Next32();
				ch = text.Current32();
			}
			return end;
		};
		bool handled = false;
		for (auto id : southeast_asian) {
			auto &engine = GetEngine(id);
			if (engine.word.Contains(c)) {
				int32_t end = span(engine.word);
				found += DivideSoutheastAsian(id, text, current, end, breaks);
				text.SetNativeIndex(end);
				handled = true;
				break;
			}
		}
		if (!handled && cj.Contains(c)) {
			int32_t end = span(cj);
			found += DivideCj(text, current, end, breaks);
			text.SetNativeIndex(end);
			handled = true;
		}
		if (!handled) {
			auto script = FindScript(break_engine_unhandled, break_engine_unhandled_count, static_cast<uint32_t>(c));
			text.Next32();
			int32_t ch = text.Current32();
			while (text.NativeIndex() < range_end && ch >= 0 &&
			       FindScript(break_engine_unhandled, break_engine_unhandled_count, static_cast<uint32_t>(ch)) ==
			           script) {
				text.Next32();
				ch = text.Current32();
			}
		}
		c = text.Current32();
		category = category_of(c);
	}
	return found;
}

} // namespace text
} // namespace duckdb
