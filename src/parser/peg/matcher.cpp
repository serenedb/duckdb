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
	for (auto &entry : matchers) {
		entry->first_set = MatcherFirstSet {false, false, {}};
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto &entry : matchers) {
			auto &matcher = *entry;
			MatcherFirstSet updated {false, false, {}};
			switch (matcher.Type()) {
			case MatcherType::KEYWORD: {
				auto literal_id = matcher.Cast<KeywordMatcher>().GetLiteralId();
				if (literal_id.IsValid()) {
					updated.AddLiteral(literal_id.GetIndex());
				} else {
					updated.any_token = true;
				}
				break;
			}
			case MatcherType::LIST:
				updated.nullable = true;
				for (auto &child : matcher.Cast<ListMatcher>().matchers) {
					auto &child_set = child.get().first_set;
					updated.MergeStart(child_set);
					if (!child_set.nullable) {
						updated.nullable = false;
						break;
					}
				}
				break;
			case MatcherType::CHOICE:
				for (auto &child : matcher.Cast<ChoiceMatcher>().matchers) {
					auto &child_set = child.get().first_set;
					updated.MergeStart(child_set);
					updated.nullable = updated.nullable || child_set.nullable;
				}
				break;
			case MatcherType::OPTIONAL:
				updated.MergeStart(matcher.Cast<OptionalMatcher>().GetChildMatcher().first_set);
				updated.nullable = true;
				break;
			case MatcherType::REPEAT: {
				auto &child_set = matcher.Cast<RepeatMatcher>().GetChildMatcher().first_set;
				updated.MergeStart(child_set);
				updated.nullable = child_set.nullable;
				break;
			}
			default:
				updated.nullable = true;
				updated.any_token = true;
				break;
			}
			if (!(updated == matcher.first_set)) {
				matcher.first_set = std::move(updated);
				changed = true;
			}
		}
	}
	for (auto &entry : matchers) {
		auto &matcher = *entry;
		if (matcher.first_set.nullable || matcher.first_set.any_token) {
			matcher.first_set.literals.clear();
			continue;
		}
		matcher.first_set_table = table;
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

ParseResultAllocator::ParseResultAllocator() : arena(Allocator::DefaultAllocator()) {
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
