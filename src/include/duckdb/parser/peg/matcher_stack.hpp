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
	~MatchStack();

	MatcherResult Execute(MatchInput input);

private:
	static constexpr idx_t INITIAL_FRAME_CAPACITY = 64;
	static constexpr idx_t FRAMES_PER_EXPRESSION_LEVEL = 64;
	static constexpr idx_t MAX_RECURSION_DEPTH = 1024;

	MatcherResult MatchChild(const Matcher &matcher, MatchState &state, idx_t depth);
	MatcherResult Match(const Matcher &matcher, MatchState &state, idx_t depth);
	MatcherResult MatchComposite(const Matcher &matcher, MatchState &state, idx_t depth);
	MatcherResult MatchList(const ListMatcher &matcher, MatchState &state, idx_t depth);
	MatcherResult MatchChoice(const ChoiceMatcher &matcher, MatchState &state, idx_t depth);
	MatcherResult MatchOptional(const OptionalMatcher &matcher, MatchState &state, idx_t depth);
	MatcherResult MatchRepeat(const RepeatMatcher &matcher, MatchState &state, idx_t depth);
	MatcherResult ExecuteFrames(MatchInput input, idx_t depth);
	MatcherResult ExecuteAtomicMatcher(MatchInput input);
	void DestroyTopFrame();
	void PushFrame(MatchInput input);
	void InitializeFrame(MatchStackFrame &frame);
	//! Returns true when the frame has completed.
	bool ExecuteFrame(MatchStackFrame &frame);
	MatcherResult FinalizeFrame(MatchStackFrame &frame);

private:
	vector<MatchStackFrame> frames;
	idx_t max_frames = 0;
	idx_t recursion_limit = 0;
	idx_t frame_limit = 0;
};

} // namespace duckdb
