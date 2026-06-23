//===----------------------------------------------------------------------===//
//                         DuckDB
//
// matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/allocator.hpp"
#include <absl/strings/ascii.h>
#include <mutex>

namespace duckdb {
class PEGTransformerFactory;
class ParseResultAllocator;
class Matcher;
class MatcherAllocator;

enum class SuggestionState : uint8_t {
	SUGGEST_KEYWORD,
	SUGGEST_CATALOG_NAME,
	SUGGEST_SCHEMA_NAME,
	SUGGEST_TABLE_NAME,
	SUGGEST_TYPE_NAME,
	SUGGEST_COLUMN_NAME,
	SUGGEST_FILE_NAME,
	SUGGEST_DIRECTORY,
	SUGGEST_VARIABLE,
	SUGGEST_SCALAR_FUNCTION_NAME,
	SUGGEST_TABLE_FUNCTION_NAME,
	SUGGEST_PRAGMA_NAME,
	SUGGEST_SETTING_NAME,
	SUGGEST_RESERVED_VARIABLE
};

enum class CandidateType { KEYWORD, IDENTIFIER, LITERAL };

struct AutoCompleteCandidate {
	// NOLINTNEXTLINE: allow implicit conversion from string
	AutoCompleteCandidate(string candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : candidate(std::move(candidate_p)), suggestion_type(suggestion_type), score_bonus(score_bonus),
	      candidate_type(candidate_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from const char*
	AutoCompleteCandidate(const char *candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : AutoCompleteCandidate(string(candidate_p), suggestion_type, score_bonus, candidate_type) {
	}

	string candidate;
	//! Type being suggested
	SuggestionState suggestion_type;
	//! The higher the score bonus, the more likely this candidate will be chosen
	int32_t score_bonus;
	//! The type of candidate we are suggesting - this modifies how we handle quoting/case sensitivity
	CandidateType candidate_type;
	//! Extra char to push at the back
	char extra_char = '\0';
	//! Suggestion position
	idx_t suggestion_pos = 0;
	//! The final score
	optional_idx score;
};

struct AutoCompleteSuggestion {
	AutoCompleteSuggestion(string text_p, idx_t pos, string type_p, idx_t score, char extra_char_p)
	    : text(std::move(text_p)), pos(pos), type(std::move(type_p)), score(score), extra_char(extra_char_p) {
	}

	string text;
	idx_t pos;
	string type;
	idx_t score;
	char extra_char;
};

enum class MatchResultType { SUCCESS, FAIL };

enum class SuggestionType { OPTIONAL, MANDATORY };

struct MatcherToken {
	// NOLINTNEXTLINE: allow implicit conversion from text
	MatcherToken(string text_p, idx_t offset_p, TokenType type_p, bool unterminated_p = false)
	    : type(type_p), text(std::move(text_p)), offset(offset_p), unterminated(unterminated_p) {
		length = text.length();
	}

	TokenType type;
	string text;
	idx_t offset = 0;
	idx_t length = 0;
	bool unterminated = false;
};

struct MatcherSuggestion {
	// NOLINTNEXTLINE: allow implicit conversion from auto-complete candidate
	MatcherSuggestion(AutoCompleteCandidate keyword_p) : keyword(std::move(keyword_p)), type(keyword.suggestion_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from suggestion state
	MatcherSuggestion(SuggestionState type, char extra_char = '\0')
	    : keyword("", type), type(type), extra_char(extra_char) {
	}

	//! Literal suggestion
	AutoCompleteCandidate keyword;
	SuggestionState type;
	char extra_char = '\0';
};

struct MatchState {
	MatchState(vector<MatcherToken> &tokens, vector<MatcherSuggestion> &suggestions,
	           reference_set_t<const Matcher> &added_suggestions, ParseResultAllocator &allocator,
	           idx_t &max_token_index, bool preserve_identifier_case_p = true)
	    : tokens(tokens), suggestions(suggestions), added_suggestions(added_suggestions), token_index(0),
	      allocator(allocator), max_token_index(max_token_index), preserve_identifier_case(preserve_identifier_case_p) {
	}
	MatchState(MatchState &state)
	    : tokens(state.tokens), suggestions(state.suggestions), added_suggestions(state.added_suggestions),
	      token_index(state.token_index), allocator(state.allocator), max_token_index(state.max_token_index),
	      preserve_identifier_case(state.preserve_identifier_case) {
	}

	vector<MatcherToken> &tokens;
	vector<MatcherSuggestion> &suggestions;
	reference_set_t<const Matcher> &added_suggestions;
	idx_t token_index;
	ParseResultAllocator &allocator;
	idx_t &max_token_index;
	bool preserve_identifier_case = true;

	void UpdateMaxTokenIndex() {
		if (token_index > max_token_index) {
			max_token_index = token_index;
		}
	}

	idx_t GetMaxTokenIndex() const {
		return max_token_index;
	}

	void AddSuggestion(MatcherSuggestion suggestion);
};

enum class MatcherType { KEYWORD, LIST, OPTIONAL, CHOICE, REPEAT, VARIABLE, STRING_LITERAL, NUMBER_LITERAL, OPERATOR };

class Matcher {
public:
	explicit Matcher(MatcherType type) : type(type) {
	}
	virtual ~Matcher() = default;

	//! Match
	virtual MatchResultType Match(MatchState &state) const = 0;
	virtual optional_ptr<ParseResult> MatchParseResult(MatchState &state) const = 0;
	virtual SuggestionType AddSuggestion(MatchState &state) const;
	virtual SuggestionType AddSuggestionInternal(MatchState &state) const = 0;
	virtual string ToString() const = 0;
	void Print() const;

	MatcherType Type() const {
		return type;
	}
	void SetName(string name_p) {
		name = std::move(name_p);
	}
	string GetName() const;

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			throw InternalException("Failed to cast matcher to type - matcher type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			throw InternalException("Failed to cast matcher to type - matcher type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	MatcherType type;
	string name;
};

class MatcherAllocator {
public:
	Matcher &Allocate(unique_ptr<Matcher> matcher);

private:
	vector<unique_ptr<Matcher>> matchers;
};

class ParseResultAllocator {
public:
	explicit ParseResultAllocator(Allocator &allocator) : arena(allocator) {
	}

	template <class T, class... ARGS>
	optional_ptr<ParseResult> Make(ARGS &&...args) {
		return optional_ptr<ParseResult>(arena.Make<T>(std::forward<ARGS>(args)...));
	}

	//! Scratch stack the List/Repeat matchers gather child results in before arena-copying them.
	//! Reused across nodes (mark on entry, pop back to the mark afterwards), so building a node's
	//! children costs no per-node heap allocation.
	vector<ParseResult *> &ChildScratch() {
		return child_scratch;
	}

	//! Copy a (processed) string into the arena and return a view over it (not null-terminated -- parse
	//! results are read as string_view). For text that is not a verbatim token (unquoted/case-folded
	//! identifiers, string literals).
	std::string_view CopyString(std::string_view str) {
		if (str.empty()) {
			return {};
		}
		auto *mem = char_ptr_cast(arena.AllocateAligned(str.size()));
		memcpy(mem, str.data(), str.size());
		return std::string_view(mem, str.size());
	}

	//! Like CopyString but ASCII-lowercases while copying (one pass, no temporary). The PG-default
	//! identifier path (preserve_identifier_case=false) lowercases every unquoted identifier.
	std::string_view CopyStringLower(std::string_view str) {
		if (str.empty()) {
			return {};
		}
		auto *mem = char_ptr_cast(arena.AllocateAligned(str.size()));
		absl::ascii_internal::AsciiStrToLower(mem, str.data(), str.size());
		return std::string_view(mem, str.size());
	}

	//! Copy the child references on the scratch from `mark` to the top into an arena array, pop them
	//! off the scratch, and return a view over the arena copy (empty span when nothing was gathered).
	std::span<reference<ParseResult>> CopyChildren(idx_t mark) {
		idx_t count = child_scratch.size() - mark;
		if (count == 0) {
			return {};
		}
		auto *arr =
		    reinterpret_cast<reference<ParseResult> *>(arena.AllocateAligned(count * sizeof(reference<ParseResult>)));
		for (idx_t i = 0; i < count; i++) {
			new (&arr[i]) reference<ParseResult>(*child_scratch[mark + i]);
		}
		child_scratch.resize(mark);
		return std::span<reference<ParseResult>>(arr, count);
	}

private:
	ArenaAllocator arena;
	vector<ParseResult *> child_scratch;
};

struct PEGMatcher {
	MatcherAllocator allocator;

	Matcher &Root() {
		return *root;
	}

	static shared_ptr<PEGMatcher> Get(ClientContext &context);
	static shared_ptr<PEGMatcher> Get(DatabaseInstance &db);

private:
	friend struct ParserCache;
	optional_ptr<Matcher> root;
};

//! Per-database cache holder for the compiled PEG root matcher and transformer factory.
//! Both are always invalidated together, so they share one mutex and one Invalidate() call.
struct ParserCache {
	shared_ptr<PEGMatcher> GetMatcher();
	shared_ptr<PEGTransformerFactory> GetTransformerFactory();
	void Invalidate();

private:
	mutex mutex;
	shared_ptr<PEGMatcher> matcher;
	shared_ptr<PEGTransformerFactory> transformer_factory;
};

} // namespace duckdb
