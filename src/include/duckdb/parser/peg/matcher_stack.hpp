//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/matcher_stack.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/matcher/choice_matcher.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher/optional_matcher.hpp"
#include "duckdb/parser/peg/matcher/repeat_matcher.hpp"

namespace duckdb {

struct PackratMatchState {
	static bool IsEnabled(const Matcher &matcher, const MatchState &state) {
		return state.context.packrat_cache && matcher.IsPackratMemoized();
	}

	optional<MatcherResult> TryLoadCachedResult(const Matcher &matcher, MatchState &state);
	void StoreResult(const Matcher &matcher, MatchState &state, const MatcherResult &result) const;

private:
	optional_idx token_index_before;
	idx_t max_token_index_before = 0;
};

struct MatchStackFrame {
public:
	explicit MatchStackFrame(MatchInput input);

public:
	bool IsInitialized() const;

public:
	const Matcher &matcher;
	MatchState &match_state;
	arena_ptr<MatchProcess> process;
	optional<MatcherResult> child_result;
	optional<MatcherResult> result;
	PackratMatchState packrat_state;
	idx_t process_mark;
};

class MatchStack {
public:
	MatchStack();

	MatcherResult Execute(MatchInput input);

private:
	static constexpr idx_t INITIAL_FRAME_CAPACITY = 64;
	static constexpr idx_t MAX_CHAIN_LEVELS = 32;

	[[gnu::always_inline]] MatcherResult MatchChild(const Matcher &matcher, MatchState &state);
	[[gnu::always_inline]] MatcherResult MatchStartedChild(const Matcher &matcher, MatchState &state);
	MatcherResult Match(const Matcher &matcher, MatchState &state);
	MatcherResult MatchComposite(const Matcher &matcher, MatchState &state);
	MatcherResult MatchList(const ListMatcher &matcher, MatchState &state);
	MatcherResult MatchChainLevel(const ListMatcher &matcher, MatchState &state);
	[[gnu::always_inline]] MatcherResult CloseChainLevel(const ListMatcher &matcher, MatchState &state,
	                                                     MatchState &list_state, MatcherResult core_result, bool nested,
	                                                     bool suffixes_empty);
	MatcherResult ContinueList(const ListMatcher &matcher, MatchState &state, MatchState &list_state,
	                           idx_t children_begin, idx_t next_child);
	MatcherResult MatchChoice(const ChoiceMatcher &matcher, MatchState &state);
	MatcherResult MatchOptional(const OptionalMatcher &matcher, MatchState &state);
	MatcherResult MatchRepeat(const RepeatMatcher &matcher, MatchState &state);
	MatcherResult MatchMemoized(const Matcher &matcher, MatchState &state);
	MatcherResult ExecuteFrames(MatchInput input);
	MatcherResult ExecuteAtomicMatcher(MatchInput input);
	void DestroyTopFrame();
	void PushFrame(MatchInput input);
	void InitializeFrame(MatchStackFrame &frame);
	//! Returns true when the frame has completed.
	bool ExecuteFrame(MatchStackFrame &frame);
	MatcherResult FinalizeFrame(MatchStackFrame &frame);

private:
	vector<MatchStackFrame> frames;
};

} // namespace duckdb
