#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/peg/matcher/choice_matcher.hpp"
#include "duckdb/parser/peg/matcher/keyword_matcher.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher/optional_matcher.hpp"
#include "duckdb/parser/peg/matcher/repeat_matcher.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/peg_parser.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"

#include <absl/strings/ascii.h>

namespace duckdb {

MatcherResult Matcher::MatchParseResult(MatchState &state) const {
	MatchInput input {*this, state};
	MatchStack stack;
	return stack.Execute(input);
}

SuggestionType Matcher::AddSuggestion(MatchState &state) const {
	if (!state.added_suggestions) {
		auto &scopes = state.context.suggestion_scopes;
		scopes.push_back(make_uniq<reference_set_t<const Matcher>>());
		state.added_suggestions = *scopes.back();
	}
	auto &added_suggestions = *state.added_suggestions;
	auto entry = added_suggestions.find(*this);
	if (entry != added_suggestions.end()) {
		return SuggestionType::MANDATORY;
	}
	added_suggestions.insert(*this);
	return AddSuggestionInternal(state);
}

string Matcher::GetName() const {
	if (name.empty()) {
		return ToString();
	}
	return name;
}

void Matcher::Print() const {
	Printer::Print(ToString());
}

void MatchState::AddSuggestion(MatcherSuggestion suggestion) {
	context.suggestions.push_back(std::move(suggestion));
}

Matcher &MatcherAllocator::Allocate(unique_ptr<Matcher> matcher) {
	auto &result = *matcher;
	matchers.push_back(std::move(matcher));
	return result;
}

void MatcherAllocator::SetPackratMemoized(Matcher &matcher) {
	if (!matcher.packrat_slot.IsValid()) {
		matcher.packrat_slot = optional_idx(packrat_slots++);
	}
}

void MatcherAllocator::ComputeFirstSets(const GrammarLiteralTable &table) {
	vector<reference<Matcher>> composites;
	for (auto &entry : matchers) {
		auto &matcher = *entry;
		auto &first_set = matcher.first_set;
		first_set = MatcherFirstSet {false, false, {}};
		switch (matcher.Type()) {
		case MatcherType::KEYWORD: {
			auto literal_id = matcher.Cast<KeywordMatcher>().GetLiteralId();
			if (literal_id.IsValid()) {
				first_set.AddLiteral(literal_id.GetIndex());
			} else {
				first_set.any_token = true;
			}
			break;
		}
		case MatcherType::LIST:
		case MatcherType::CHOICE:
		case MatcherType::OPTIONAL:
		case MatcherType::REPEAT:
			composites.push_back(matcher);
			break;
		default: {
			auto token_classes =
			    matcher.IsAtomic() ? static_cast<const AtomicMatcher &>(matcher).FirstTokenClasses() : uint8_t(0);
			if (token_classes) {
				first_set.token_classes = token_classes;
				first_set.word_categories = static_cast<const AtomicMatcher &>(matcher).FirstWordCategories();
				break;
			}
			first_set.nullable = true;
			first_set.any_token = true;
			break;
		}
		}
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto entry = composites.rbegin(); entry != composites.rend(); entry++) {
			auto &matcher = entry->get();
			auto &first_set = matcher.first_set;
			bool nullable;
			switch (matcher.Type()) {
			case MatcherType::LIST:
				nullable = true;
				for (auto &child : matcher.Cast<ListMatcher>().matchers) {
					auto &child_set = child.get().first_set;
					changed |= first_set.MergeChanged(child_set);
					if (!child_set.nullable) {
						nullable = false;
						break;
					}
				}
				break;
			case MatcherType::CHOICE:
				nullable = false;
				for (auto &child : matcher.Cast<ChoiceMatcher>().matchers) {
					auto &child_set = child.get().first_set;
					changed |= first_set.MergeChanged(child_set);
					nullable = nullable || child_set.nullable;
				}
				break;
			case MatcherType::OPTIONAL:
				changed |= first_set.MergeChanged(matcher.Cast<OptionalMatcher>().GetChildMatcher().first_set);
				nullable = true;
				break;
			default: {
				auto &child_set = matcher.Cast<RepeatMatcher>().GetChildMatcher().first_set;
				changed |= first_set.MergeChanged(child_set);
				nullable = child_set.nullable;
				break;
			}
			}
			if (nullable && !first_set.nullable) {
				first_set.nullable = true;
				changed = true;
			}
		}
	}
	ComputeAfterWordSets();
	for (auto &entry : matchers) {
		auto &matcher = *entry;
		if (matcher.first_set.nullable || matcher.first_set.any_token) {
			matcher.first_set.literals.clear();
			matcher.after_word_set.literals.clear();
			continue;
		}
		matcher.first_set_table = table;
		matcher.checks_after_word = (matcher.first_set.token_classes & MatcherTokenClass::WORD) &&
		                            !matcher.can_end_after_word && !matcher.after_word_set.any_token;
		if (!matcher.checks_after_word) {
			matcher.after_word_set.literals.clear();
		}
	}
}

bool Matcher::CanFollowWord(MatchState &state) const {
	auto &tokens = state.token_iterator;
	auto info = tokens.CurrentLiteralInfo(*first_set_table);
	if (first_set.HasLiteral(info.LiteralId())) {
		return true;
	}
	if (info.IsKeyword() && !info.HasAnyFlags(first_set.word_categories)) {
		return false;
	}
	auto next = tokens.Position() + 1;
	if (next >= tokens.Size()) {
		return true;
	}
	auto &next_token = tokens.GetToken(next);
	if (next_token.type == TokenType::END_OF_INPUT_AUTOCOMPLETE ||
	    (next_token.token_classes & after_word_set.token_classes) ||
	    after_word_set.HasLiteral(tokens.LiteralInfoAt(next, *first_set_table).LiteralId())) {
		return true;
	}
	state.context.max_token_index = MaxValue(state.context.max_token_index, next);
	return false;
}

void MatcherAllocator::ComputeAfterWordSets() {
	vector<reference<Matcher>> composites;
	for (auto &entry : matchers) {
		auto &matcher = *entry;
		matcher.after_word_set = MatcherFirstSet {false, false, {}};
		matcher.can_end_after_word = false;
		if (!matcher.first_set.any_token && !(matcher.first_set.token_classes & MatcherTokenClass::WORD)) {
			continue;
		}
		switch (matcher.Type()) {
		case MatcherType::LIST:
		case MatcherType::CHOICE:
		case MatcherType::OPTIONAL:
		case MatcherType::REPEAT:
			composites.push_back(matcher);
			break;
		default: {
			auto token_classes =
			    matcher.IsAtomic() ? static_cast<const AtomicMatcher &>(matcher).FirstTokenClasses() : uint8_t(0);
			if (!token_classes) {
				matcher.after_word_set.any_token = true;
				matcher.can_end_after_word = true;
			} else {
				matcher.can_end_after_word = token_classes & MatcherTokenClass::WORD;
			}
			break;
		}
		}
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto entry = composites.rbegin(); entry != composites.rend(); entry++) {
			auto &matcher = entry->get();
			auto &after_word_set = matcher.after_word_set;
			bool can_end = false;
			switch (matcher.Type()) {
			case MatcherType::LIST: {
				bool prefix_nullable = true;
				bool prefix_one_word = false;
				for (auto &child_entry : matcher.Cast<ListMatcher>().matchers) {
					auto &child = child_entry.get();
					if (prefix_nullable) {
						changed |= after_word_set.MergeChanged(child.after_word_set);
					}
					if (prefix_one_word) {
						changed |= after_word_set.MergeChanged(child.first_set);
					}
					auto one_word =
					    (prefix_nullable && child.can_end_after_word) || (prefix_one_word && child.first_set.nullable);
					prefix_nullable = prefix_nullable && child.first_set.nullable;
					prefix_one_word = one_word;
					if (!prefix_nullable && !prefix_one_word) {
						break;
					}
				}
				can_end = prefix_one_word;
				break;
			}
			case MatcherType::CHOICE:
				for (auto &child : matcher.Cast<ChoiceMatcher>().matchers) {
					changed |= after_word_set.MergeChanged(child.get().after_word_set);
					can_end = can_end || child.get().can_end_after_word;
				}
				break;
			case MatcherType::OPTIONAL: {
				auto &child = matcher.Cast<OptionalMatcher>().GetChildMatcher();
				changed |= after_word_set.MergeChanged(child.after_word_set);
				can_end = child.can_end_after_word;
				break;
			}
			default: {
				auto &child = matcher.Cast<RepeatMatcher>().GetChildMatcher();
				changed |= after_word_set.MergeChanged(child.after_word_set);
				if (child.can_end_after_word) {
					changed |= after_word_set.MergeChanged(child.first_set);
				}
				can_end = child.can_end_after_word;
				break;
			}
			}
			if (can_end && !matcher.can_end_after_word) {
				matcher.can_end_after_word = true;
				changed = true;
			}
		}
	}
}

data_ptr_t MatchProcessArena::AllocateInNewChunk(idx_t size) {
	if (size > CHUNK_SIZE) {
		throw InternalException("A match process of %llu bytes does not fit a process arena chunk", size);
	}
	if (position + size > CHUNK_SIZE) {
		chunk_index++;
		position = 0;
	}
	if (chunk_index >= chunks.size()) {
		chunks.push_back(Allocator::DefaultAllocator().Allocate(CHUNK_SIZE));
	}
	auto result = chunks[chunk_index].get() + position;
	position += size;
	return result;
}

ParseResultAllocator::ParseResultAllocator() : arena(Allocator::DefaultAllocator(), INITIAL_ARENA_CAPACITY) {
}

std::string_view ParseResultAllocator::Lower(std::string_view text) {
	auto first = std::find_if(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
	if (first == text.end()) {
		return text;
	}
	auto data = AllocateText(text.size());
	absl::ascii_internal::AsciiStrToLower(data, text.data(), text.size());
	return std::string_view(data, text.size());
}

std::string_view ParseResultAllocator::Upper(std::string_view text) {
	auto first = std::find_if(text.begin(), text.end(), [](char c) { return c >= 'a' && c <= 'z'; });
	if (first == text.end()) {
		return text;
	}
	auto data = AllocateText(text.size());
	absl::ascii_internal::AsciiStrToUpper(data, text.data(), text.size());
	return std::string_view(data, text.size());
}

idx_t ParseResultAllocator::CopyUnquoted(std::string_view body, char quote, char *target) {
	idx_t size = 0;
	for (idx_t i = 0; i < body.size(); i++) {
		target[size++] = body[i];
		if (body[i] == quote && i + 1 < body.size() && body[i + 1] == quote) {
			i++;
		}
	}
	return size;
}

std::string_view ParseResultAllocator::Unquote(std::string_view body, char quote) {
	if (body.find(quote) == std::string_view::npos) {
		return body;
	}
	auto data = AllocateText(body.size());
	return FinishText(data, body.size(), CopyUnquoted(body, quote, data));
}

std::span<reference<ParseResult>> ParseResultAllocator::TakeChildren(idx_t begin) {
	auto count = children.size() - begin;
	if (count == 0) {
		return {};
	}
	auto data =
	    reinterpret_cast<reference<ParseResult> *>(arena.AllocateAligned(count * sizeof(reference<ParseResult>)));
	std::uninitialized_copy(children.begin() + NumericCast<int64_t>(begin), children.end(), data);
	DiscardChildren(begin);
	return {data, count};
}

} // namespace duckdb
