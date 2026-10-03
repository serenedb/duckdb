#include "text_break_iterator.hpp"

#include "text_utf8.hpp"

#include <string>

namespace duckdb {
namespace text {

namespace {

constexpr uint32_t START_STATE = 1;
constexpr uint32_t STOP_STATE = 0;
constexpr uint32_t CATEGORY_EOF = 1;
constexpr uint32_t CATEGORY_BOF = 2;
constexpr uint32_t EXCEPTION_PARTIAL = 1;
constexpr uint32_t EXCEPTION_MATCH = 2;

enum class RunMode : uint8_t { RUN, START, END };

const uint8_t *Row(const BreakRuleSet &rules, uint32_t state) {
	return rules.states + static_cast<size_t>(state) * (2 + rules.category_count);
}

class ExceptionWalk {
public:
	ExceptionWalk(uint32_t first, uint32_t count) : lower(first), upper(first + count) {
	}

	TrieResult Next(uint32_t c) {
		uint32_t first = upper;
		uint32_t last = upper;
		for (uint32_t i = lower; i < upper; i++) {
			auto &entry = sentence_exception_entries[i];
			if (entry.length > depth && sentence_exception_chars[entry.offset + depth] == c) {
				if (first == upper) {
					first = i;
				}
				last = i + 1;
			}
		}
		depth++;
		if (first == upper) {
			lower = upper;
			return TrieResult::NO_MATCH;
		}
		lower = first;
		upper = last;
		auto &front = sentence_exception_entries[lower];
		bool has_next = upper - lower > 1 || front.length > depth;
		if (front.length != depth) {
			return TrieResult::NO_VALUE;
		}
		value = front.value;
		return has_next ? TrieResult::INTERMEDIATE_VALUE : TrieResult::FINAL_VALUE;
	}

	uint32_t GetValue() const {
		return value;
	}

private:
	uint32_t lower;
	uint32_t upper;
	uint32_t depth = 0;
	uint32_t value = 0;
};

} // namespace

BreakIterator::BreakIterator(BreakKind kind, const Locale &locale, BreakUnits units)
    : rules(kind == BreakKind::WORD ? &break_rules_word : &break_rules_sentence), units(units) {
	std::string name(locale.GetBaseName());
	do {
		for (uint32_t i = 0; i < break_rule_locale_count; i++) {
			auto &entry = break_rule_locales[i];
			if (entry.kind == kind && name == entry.locale) {
				rules = entry.rules;
				tailored = true;
				break;
			}
		}
	} while (!tailored && Locale::Truncate(name));

	if (kind != BreakKind::SENTENCE || locale.GetKeyword("ss") != "standard") {
		return;
	}
	name = std::string(locale.GetBaseName());
	do {
		for (uint32_t i = 0; i < sentence_exception_locale_count; i++) {
			if (name == sentence_exception_locales[i].locale) {
				exceptions = &sentence_exception_locales[i];
				break;
			}
		}
	} while (!exceptions && Locale::Truncate(name));
}

void BreakIterator::SetText(const char *text, size_t length) {
	data = reinterpret_cast<const uint8_t *>(text);
	size = length;
	Restart();
}

void BreakIterator::Restart() {
	current = {0, 0};
	delegate_position = 0;
	has_peeked = false;
	done = false;
	dictionary_breaks.clear();
	dictionary_start = 0;
	dictionary_limit = 0;
	dictionary_index = -1;
	dictionary_status_index = 0;
}

int64_t BreakIterator::Next() {
	if (has_peeked) {
		current = peeked;
		has_peeked = false;
		return current.position;
	}
	if (done) {
		return DONE;
	}
	auto next = FilteredNext();
	if (next.position == DONE) {
		done = true;
		return DONE;
	}
	current = next;
	return current.position;
}

int32_t BreakIterator::GetRuleStatus() const {
	auto index = current.status_index;
	return rules->statuses[index + rules->statuses[index]];
}

int64_t BreakIterator::Preceding(size_t offset) {
	if (offset > size) {
		while (current.position != static_cast<int64_t>(size) && Next() != DONE) {
		}
		return current.position;
	}
	auto adjusted = offset;
	if (adjusted < size && (data[adjusted] & 0xC0) == 0x80) {
		auto start = adjusted;
		auto limit = adjusted > 3 ? adjusted - 3 : 0;
		while (start > limit && (data[start] & 0xC0) == 0x80) {
			start--;
		}
		auto index = start;
		DecodeUtf8(data, size, index);
		if (index > adjusted) {
			adjusted = start;
		}
	}
	if (static_cast<int64_t>(adjusted) <= current.position) {
		Restart();
	}
	if (adjusted == 0) {
		return DONE;
	}
	for (;;) {
		if (!has_peeked) {
			if (done) {
				break;
			}
			peeked = FilteredNext();
			if (peeked.position == DONE) {
				done = true;
				break;
			}
			has_peeked = true;
		}
		if (peeked.position >= static_cast<int64_t>(adjusted)) {
			break;
		}
		current = peeked;
		has_peeked = false;
	}
	return current.position;
}

BreakIterator::Boundary BreakIterator::HandleNext(int64_t from, bool &dictionary) const {
	auto index = static_cast<size_t>(from);
	if (index >= size) {
		return {DONE, 0};
	}
	auto &categories = *rules->categories;
	int64_t c = DecodeUtf8(data, size, index);
	int64_t position = from;
	int32_t status_index = 0;
	uint32_t dictionary_count = 0;
	uint32_t state = START_STATE;
	auto row = Row(*rules, state);
	uint32_t category = 0;
	auto mode = RunMode::RUN;
	if (rules->bof_required) {
		category = CATEGORY_BOF;
		mode = RunMode::START;
	}
	for (;;) {
		if (c < 0) {
			if (mode == RunMode::END) {
				break;
			}
			mode = RunMode::END;
			category = CATEGORY_EOF;
		}
		if (mode == RunMode::RUN) {
			category = categories.Get(static_cast<uint32_t>(c));
			dictionary_count += category >= rules->dictionary_categories_start;
		}
		state = row[2 + category];
		row = Row(*rules, state);
		if (row[0]) {
			if (mode != RunMode::START) {
				position = static_cast<int64_t>(index);
			}
			status_index = row[1];
		}
		if (state == STOP_STATE) {
			break;
		}
		if (mode == RunMode::RUN) {
			c = index < size ? static_cast<int64_t>(DecodeUtf8(data, size, index)) : -1;
		} else if (mode == RunMode::START) {
			mode = RunMode::RUN;
		}
	}
	if (position == from) {
		index = static_cast<size_t>(from);
		DecodeUtf8(data, size, index);
		position = static_cast<int64_t>(index);
		status_index = 0;
	}
	dictionary = dictionary_count > 0;
	return {position, status_index};
}

BreakIterator::Boundary BreakIterator::DelegateNext() {
	auto next = DictionaryFollowing(delegate_position);
	if (next.position == DONE) {
		bool dictionary;
		next = HandleNext(delegate_position, dictionary);
		if (next.position != DONE && dictionary) {
			PopulateDictionary(delegate_position, next.position, next.status_index);
			auto divided = DictionaryFollowing(delegate_position);
			if (divided.position != DONE) {
				next = divided;
			}
		}
	}
	if (next.position != DONE) {
		delegate_position = next.position;
	}
	return next;
}

BreakIterator::Boundary BreakIterator::DictionaryFollowing(int64_t from) {
	if (from >= dictionary_limit || from < dictionary_start) {
		dictionary_index = -1;
		return {DONE, 0};
	}
	auto count = static_cast<int64_t>(dictionary_breaks.size());
	if (dictionary_index >= 0 && dictionary_index < count && dictionary_breaks[dictionary_index] == from) {
		if (++dictionary_index >= count) {
			dictionary_index = -1;
			return {DONE, 0};
		}
		return {dictionary_breaks[dictionary_index], dictionary_status_index};
	}
	for (dictionary_index = 0; dictionary_index < count; dictionary_index++) {
		if (dictionary_breaks[dictionary_index] > from) {
			return {dictionary_breaks[dictionary_index], dictionary_status_index};
		}
	}
	dictionary_index = -1;
	return {DONE, 0};
}

int64_t BreakIterator::NativeLength(int64_t start, int64_t end) const {
	if (units == BreakUnits::UTF8) {
		return end - start;
	}
	int64_t length = 0;
	auto index = static_cast<size_t>(start);
	while (index < static_cast<size_t>(end) && length < 2) {
		length += Utf16Length(DecodeUtf8(data, size, index));
	}
	return length;
}

void BreakIterator::PopulateDictionary(int64_t start, int64_t end, int32_t status_index) {
	if (NativeLength(start, end) <= 1) {
		return;
	}
	dictionary_breaks.clear();
	dictionary_start = 0;
	dictionary_limit = 0;
	dictionary_index = -1;
	dictionary_status_index = status_index;
	engine_text.Load(data, size, static_cast<size_t>(start), static_cast<size_t>(end), units);
	engine_breaks.clear();
	if (engines.FindBreaks(engine_text, *rules->categories, rules->dictionary_categories_start, engine_breaks) <= 0) {
		return;
	}
	for (auto position : engine_breaks) {
		dictionary_breaks.push_back(static_cast<int64_t>(engine_text.ByteOffset(position)));
	}
	if (start < dictionary_breaks.front()) {
		dictionary_breaks.insert(dictionary_breaks.begin(), start);
	}
	if (end > dictionary_breaks.back()) {
		dictionary_breaks.push_back(end);
	}
	dictionary_index = 0;
	dictionary_start = dictionary_breaks.front();
	dictionary_limit = dictionary_breaks.back();
}

BreakIterator::Boundary BreakIterator::FilteredNext() {
	auto next = DelegateNext();
	if (exceptions && exceptions->backward_count > 0) {
		while (next.position != DONE && next.position != static_cast<int64_t>(size) && IsException(next.position)) {
			next = DelegateNext();
		}
	}
	return next;
}

size_t BreakIterator::PreviousCodePoint(size_t position, uint32_t &c) const {
	auto start = position - 1;
	auto limit = position > 4 ? position - 4 : 0;
	while (start > limit && (data[start] & 0xC0) == 0x80) {
		start--;
	}
	auto index = start;
	c = DecodeUtf8(data, position, index);
	if (index == position) {
		return start;
	}
	c = REPLACEMENT_CHARACTER;
	return position - 1;
}

bool BreakIterator::IsException(int64_t boundary) const {
	auto position = static_cast<size_t>(boundary);
	uint32_t c;
	auto before = PreviousCodePoint(position, c);
	if (c == ' ') {
		position = before;
	}
	ExceptionWalk backward(exceptions->first_backward, exceptions->backward_count);
	int64_t best_position = -1;
	uint32_t best_value = 0;
	while (position > 0) {
		position = PreviousCodePoint(position, c);
		auto walked = backward.Next(c);
		if (TrieHasValue(walked)) {
			best_position = static_cast<int64_t>(position);
			best_value = backward.GetValue();
		}
		if (!TrieHasNext(walked)) {
			break;
		}
	}
	if (best_position < 0) {
		return false;
	}
	if (best_value == EXCEPTION_MATCH) {
		return true;
	}
	if (best_value != EXCEPTION_PARTIAL || exceptions->forward_count == 0) {
		return false;
	}
	ExceptionWalk forward(exceptions->first_forward, exceptions->forward_count);
	auto walked = TrieResult::INTERMEDIATE_VALUE;
	auto index = static_cast<size_t>(best_position);
	while (index < size) {
		walked = forward.Next(DecodeUtf8(data, size, index));
		if (!TrieHasNext(walked)) {
			break;
		}
	}
	return walked != TrieResult::NO_MATCH;
}

} // namespace text
} // namespace duckdb
