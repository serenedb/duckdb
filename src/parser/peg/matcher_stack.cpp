#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/parser/peg/matcher/literal_choice_matcher.hpp"

namespace duckdb {

MatchStack::MatchStack() {
}

MatchStack::~MatchStack() {
	// Child processes can reference state owned by their parents.
	while (!frames.empty()) {
		DestroyTopFrame();
	}
}

void MatchStack::DestroyTopFrame() {
	D_ASSERT(!frames.empty());
	auto &processes = frames.back().match_state.context.processes;
	auto process_mark = frames.back().process_mark;
	frames.pop_back();
	processes.Rewind(process_mark);
}

optional<MatcherResult> PackratMatchState::TryLoadCachedResult(const Matcher &matcher, MatchState &state) {
	D_ASSERT(IsEnabled(matcher, state));
	auto token_index = state.token_iterator.Position();
	auto cached_result = state.context.packrat_cache->Lookup(matcher.GetPackratSlot().GetIndex(), token_index);
	if (!cached_result) {
		token_index_before = token_index;
		max_token_index_before = state.GetMaxTokenIndex();
		return nullopt;
	}

	state.token_iterator.SetPosition(cached_result->token_index_after);
	state.context.max_token_index = MaxValue(state.context.max_token_index, cached_result->max_token_index_seen);
	if (cached_result->success) {
		return MatcherResult::Success(cached_result->result);
	}
	return MatcherResult::Failure();
}

void PackratMatchState::StoreResult(const Matcher &matcher, MatchState &state, const MatcherResult &result) const {
	if (!token_index_before.IsValid()) {
		return;
	}
	ParserPackratEntry cache_entry;
	cache_entry.success = result.IsSuccess();
	cache_entry.token_index_after = state.token_iterator.Position();
	cache_entry.max_token_index_seen = MaxValue(max_token_index_before, state.GetMaxTokenIndex());
	cache_entry.result = result.GetParseResult();
	state.context.packrat_cache->Store(matcher.GetPackratSlot().GetIndex(), token_index_before.GetIndex(), cache_entry);
}

MatchStackFrame::MatchStackFrame(MatchInput input)
    : matcher(input.matcher), match_state(input.state), process_mark(input.state.context.processes.Mark()) {
}

bool MatchStackFrame::IsInitialized() const {
	return process || result;
}

[[gnu::always_inline]] static inline bool IsEmptyOptional(const Matcher &matcher, MatchState &state) {
	return matcher.Type() == MatcherType::OPTIONAL && matcher.HasBuiltInMatch() &&
	       !static_cast<const OptionalMatcher &>(matcher).GetChildMatcher().CanStartAt(state);
}

[[gnu::always_inline]] static inline MatcherResult EmptyOptional(const Matcher &matcher, MatchState &state) {
	state.rule = matcher.GetRule();
	return static_cast<const OptionalMatcher &>(matcher).EmptyResult(state);
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchMemoized(const Matcher &matcher, MatchState &state, idx_t depth) {
	PackratMatchState packrat_state;
	auto cached_result = packrat_state.TryLoadCachedResult(matcher, state);
	if (cached_result) {
		return *cached_result;
	}
	auto result = matcher.IsAtomic() ? static_cast<const AtomicMatcher &>(matcher).MatchAtomic(state)
	                                 : MatchComposite(matcher, state, depth);
	packrat_state.StoreResult(matcher, state, result);
	return result;
}

[[gnu::aligned(64)]] MatcherResult MatchStack::ExecuteAtomicMatcher(MatchInput input) {
	auto &matcher = input.matcher;
	auto &state = input.state;
	D_ASSERT(matcher.IsAtomic());
	state.rule = matcher.GetRule();
	if (PackratMatchState::IsEnabled(matcher, state)) {
		return MatchMemoized(matcher, state, 0);
	}
	return static_cast<const AtomicMatcher &>(matcher).MatchAtomic(state);
}

MatcherResult MatchStack::MatchChild(const Matcher &matcher, MatchState &state, idx_t depth) {
	if (!matcher.IsAtomic() && !matcher.CanStartAt(state)) {
		return MatcherResult::Failure();
	}
	return MatchStartedChild(matcher, state, depth);
}

MatcherResult MatchStack::MatchStartedChild(const Matcher &matcher, MatchState &state, idx_t depth) {
	if (matcher.IsAtomic()) {
		return ExecuteAtomicMatcher({matcher, state});
	}
	if (IsEmptyOptional(matcher, state)) {
		return EmptyOptional(matcher, state);
	}
	return Match(matcher, state, depth + 1);
}

[[gnu::aligned(64)]] MatcherResult MatchStack::Match(const Matcher &matcher, MatchState &state, idx_t depth) {
	if (depth >= recursion_limit || !matcher.HasBuiltInMatch()) {
		return ExecuteFrames({matcher, state}, depth);
	}
	state.rule = matcher.GetRule();
	if (PackratMatchState::IsEnabled(matcher, state)) {
		return MatchMemoized(matcher, state, depth);
	}
	return MatchComposite(matcher, state, depth);
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchComposite(const Matcher &matcher, MatchState &state, idx_t depth) {
	switch (matcher.Type()) {
	case MatcherType::LIST:
		return MatchList(static_cast<const ListMatcher &>(matcher), state, depth);
	case MatcherType::OPTIONAL:
		return MatchOptional(static_cast<const OptionalMatcher &>(matcher), state, depth);
	case MatcherType::CHOICE:
		return MatchChoice(static_cast<const ChoiceMatcher &>(matcher), state, depth);
	default:
		D_ASSERT(matcher.Type() == MatcherType::REPEAT);
		return MatchRepeat(static_cast<const RepeatMatcher &>(matcher), state, depth);
	}
}

static optional_idx StartOffset(const MatchState &state) {
	auto current = state.token_iterator.Current();
	return current ? optional_idx(current->offset) : optional_idx();
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchList(const ListMatcher &matcher, MatchState &state, idx_t depth) {
	if (matcher.chain_core.IsValid() && state.BuildParseResult() && !matcher.suppress_suggestions) {
		return MatchChainLevel(matcher, state, depth);
	}
	MatchState list_state(state);
	return ContinueList(matcher, state, list_state, state.context.allocator.ChildCount(), 0, depth);
}

[[gnu::always_inline]] static inline bool PrefixesEmpty(const ListMatcher &matcher, MatchState &state) {
	auto &children = matcher.matchers;
	auto core_index = matcher.chain_core.GetIndex();
	for (idx_t i = 0; i < core_index; i++) {
		if (!IsEmptyOptional(children[i], state)) {
			return false;
		}
	}
	return true;
}

bool MatchStack::IsNestedChainLevel(const Matcher &matcher, const MatchState &state, idx_t depth) const {
	if (matcher.Type() != MatcherType::LIST || depth >= recursion_limit || !matcher.HasBuiltInMatch() ||
	    PackratMatchState::IsEnabled(matcher, state)) {
		return false;
	}
	auto &list = static_cast<const ListMatcher &>(matcher);
	return list.chain_core.IsValid() && !list.suppress_suggestions;
}

MatcherResult MatchStack::CloseChainLevel(const ListMatcher &matcher, MatchState &state, MatchState &list_state,
                                          MatcherResult core_result, idx_t depth, bool nested) {
	auto &children = matcher.matchers;
	auto core_index = matcher.chain_core.GetIndex();
	auto end = core_index + 1;
	while (end < children.size() && IsEmptyOptional(children[end], list_state)) {
		end++;
	}
	if (end == children.size() && core_result.HasParseResult() && core_result.GetParseResult()->GetRule()) {
		core_result.GetParseResult()->collapsed = true;
		return core_result;
	}
	auto &allocator = state.context.allocator;
	auto children_begin = allocator.ChildCount();
	for (idx_t i = 0; i < end; i++) {
		auto child_result = i == core_index ? core_result : EmptyOptional(children[i], list_state);
		if (child_result.HasParseResult()) {
			allocator.PushChild(*child_result.GetParseResult());
		}
	}
	if (!nested) {
		return ContinueList(matcher, state, list_state, children_begin, end, depth);
	}
	MatchState level_state(state);
	level_state.rule = matcher.GetRule();
	return ContinueList(matcher, level_state, list_state, children_begin, end, depth);
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchChainLevel(const ListMatcher &matcher, MatchState &state,
                                                               idx_t depth) {
	if (!PrefixesEmpty(matcher, state)) {
		MatchState list_state(state);
		return ContinueList(matcher, state, list_state, state.context.allocator.ChildCount(), 0, depth);
	}
	MatchState list_state(state);
	const ListMatcher *outer_levels[MAX_CHAIN_LEVELS];
	idx_t nesting = 0;
	auto level = &matcher;
	while (nesting < MAX_CHAIN_LEVELS) {
		auto &core = level->matchers[level->chain_core.GetIndex()].get();
		if (!IsNestedChainLevel(core, list_state, depth + nesting + 1)) {
			break;
		}
		auto &nested_level = static_cast<const ListMatcher &>(core);
		if (!nested_level.CanStartAt(list_state)) {
			return MatcherResult::Failure();
		}
		list_state.rule = nested_level.GetRule();
		if (!PrefixesEmpty(nested_level, list_state)) {
			break;
		}
		outer_levels[nesting++] = level;
		level = &nested_level;
	}
	auto result = MatchChild(level->matchers[level->chain_core.GetIndex()], list_state, depth + nesting);
	while (true) {
		if (!result.IsSuccess()) {
			return MatcherResult::Failure();
		}
		result = CloseChainLevel(*level, state, list_state, result, depth + nesting, nesting > 0);
		if (nesting == 0) {
			break;
		}
		level = outer_levels[--nesting];
	}
	if (result.IsSuccess()) {
		state.token_iterator.SetPosition(list_state.token_iterator);
	}
	return result;
}

[[gnu::aligned(64)]] MatcherResult MatchStack::ContinueList(const ListMatcher &matcher, MatchState &state,
                                                            MatchState &list_state, idx_t children_begin,
                                                            idx_t next_child, idx_t depth) {
	auto &allocator = state.context.allocator;
	auto &suggestions = state.context.suggestions;
	auto saved_suggestion_size = matcher.suppress_suggestions ? suggestions.size() : 0;
	auto start_offset = StartOffset(state);
	for (idx_t i = next_child; i < matcher.matchers.size(); i++) {
		auto &child = matcher.matchers[i].get();
		auto current = list_state.token_iterator.Current();
		if (current && current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			if (matcher.suppress_suggestions) {
				matcher.DiscardSuggestions(suggestions, saved_suggestion_size);
				allocator.DiscardChildren(children_begin);
				return MatcherResult::Failure();
			}
			if (child.AddSuggestion(list_state) == SuggestionType::OPTIONAL) {
				continue;
			}
			state.token_iterator.SetPosition(list_state.token_iterator);
			allocator.DiscardChildren(children_begin);
			return MatcherResult::Failure();
		}
		auto child_result = MatchChild(child, list_state, depth);
		if (!child_result.IsSuccess()) {
			matcher.DiscardSuggestions(suggestions, saved_suggestion_size);
			allocator.DiscardChildren(children_begin);
			return MatcherResult::Failure();
		}
		if (child_result.HasParseResult()) {
			allocator.PushChild(*child_result.GetParseResult());
		}
	}
	state.token_iterator.SetPosition(list_state.token_iterator);
	matcher.DiscardSuggestions(suggestions, saved_suggestion_size);
	if (matcher.IsCollapsible()) {
		auto collapsible = ListMatcher::FindCollapsibleResult(allocator.PendingChildren(children_begin));
		if (collapsible) {
			allocator.DiscardChildren(children_begin);
			collapsible->collapsed = true;
			return MatcherResult::Success(collapsible);
		}
	}
	auto children = allocator.TakeChildren(children_begin);
	return state.AllocateParseResult<ListParseResult>(children, start_offset);
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchChoice(const ChoiceMatcher &matcher, MatchState &state,
                                                           idx_t depth) {
	idx_t child_index = 0;
	idx_t child_end = matcher.matchers.size();
	if (matcher.dispatch_on_literal) {
		child_index = static_cast<const LiteralChoiceMatcher &>(matcher).SelectChild(state.token_iterator);
		child_end = MinValue(child_index + 1, child_end);
	}
	for (; child_index < child_end; child_index++) {
		auto &child = matcher.matchers[child_index].get();
		if (!child.IsAtomic() && !child.CanStartAt(state)) {
			continue;
		}
		MatchState child_state(state);
		auto child_result = MatchStartedChild(child, child_state, depth);
		if (!child_result.IsSuccess()) {
			continue;
		}
		if (!child_result.HasParseResult() || (matcher.IsCollapsible() && child_result.GetParseResult()->GetRule())) {
			state.token_iterator.SetPosition(child_state.token_iterator);
			return child_result;
		}
		auto start_offset = StartOffset(state);
		state.token_iterator.SetPosition(child_state.token_iterator);
		return state.AllocateParseResult<ChoiceParseResult>(*child_result.GetParseResult(), child_index, start_offset);
	}
	return MatcherResult::Failure();
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchOptional(const OptionalMatcher &matcher, MatchState &state,
                                                             idx_t depth) {
	MatchState child_state(state);
	auto start_offset = StartOffset(child_state);
	auto child_result = MatchChild(matcher.GetChildMatcher(), child_state, depth);
	if (!child_result.IsSuccess()) {
		return matcher.EmptyResult(state);
	}
	state.token_iterator.SetPosition(child_state.token_iterator);
	if (!child_result.HasParseResult()) {
		return MatcherResult::Success();
	}
	return state.AllocateParseResult<OptionalParseResult>(child_result.GetParseResult(), start_offset);
}

[[gnu::aligned(64)]] MatcherResult MatchStack::MatchRepeat(const RepeatMatcher &matcher, MatchState &state,
                                                           idx_t depth) {
	auto &allocator = state.context.allocator;
	auto &child = matcher.GetChildMatcher();
	MatchState repeat_state(state);
	auto children_begin = allocator.ChildCount();
	auto start_offset = StartOffset(repeat_state);
	auto child_result = MatchChild(child, repeat_state, depth);
	if (!child_result.IsSuccess()) {
		return MatcherResult::Failure();
	}
	while (true) {
		if (child_result.HasParseResult()) {
			allocator.PushChild(*child_result.GetParseResult());
		}
		state.token_iterator.SetPosition(repeat_state.token_iterator);
		auto current = repeat_state.token_iterator.Current();
		if (current && current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			child.AddSuggestion(state);
			break;
		}
		child_result = MatchChild(child, repeat_state, depth);
		if (!child_result.IsSuccess()) {
			break;
		}
	}
	auto children = allocator.TakeChildren(children_begin);
	return state.AllocateParseResult<RepeatParseResult>(children, start_offset);
}

void MatchStack::PushFrame(MatchInput input) {
	if (frames.size() == frames.capacity() && frames.size() >= frame_limit) {
		ParserException::ThrowMaxExpressionDepth(input.state.context.max_expression_depth);
	}
	input.state.rule = input.matcher.GetRule();
	frames.emplace_back(input);
}

void MatchStack::InitializeFrame(MatchStackFrame &frame) {
	auto &matcher = frame.matcher;
	auto &state = frame.match_state;
	if (PackratMatchState::IsEnabled(matcher, state)) {
		auto cached_result = frame.packrat_state.TryLoadCachedResult(matcher, state);
		if (cached_result) {
			frame.result = *cached_result;
			return;
		}
	}
	frame.process = matcher.StartMatch(state);
}

bool MatchStack::ExecuteFrame(MatchStackFrame &frame) {
	if (!frame.IsInitialized()) {
		InitializeFrame(frame);
		D_ASSERT(frame.IsInitialized());
	}
	if (frame.result) {
		return true;
	}
	D_ASSERT(frame.process);
	auto step = frame.process->Resume(frame.child_result);
	frame.child_result.reset();
	if (!step.HasChild()) {
		frame.result = step.GetResult();
		return true;
	}
	auto child = step.GetChild();
	if (child.matcher.IsAtomic()) {
		frame.child_result = ExecuteAtomicMatcher(child);
		return false;
	}
	if (!child.matcher.CanStartAt(child.state)) {
		frame.child_result = MatcherResult::Failure();
		return false;
	}
	if (IsEmptyOptional(child.matcher, child.state)) {
		frame.child_result = EmptyOptional(child.matcher, child.state);
		return false;
	}
	PushFrame(child);
	return false;
}

MatcherResult MatchStack::FinalizeFrame(MatchStackFrame &frame) {
	if (!frame.result) {
		throw InternalException("Trying to finalize a frame without a stored result");
	}
	auto result = *frame.result;
	auto &matcher = frame.matcher;
	auto &state = frame.match_state;
	frame.packrat_state.StoreResult(matcher, state, result);
	return result;
}

MatcherResult MatchStack::Execute(MatchInput input) {
	D_ASSERT(frames.empty());
	if (input.matcher.IsAtomic()) {
		return ExecuteAtomicMatcher(input);
	}
	auto max_expression_depth = input.state.context.max_expression_depth;
	max_frames = max_expression_depth > NumericLimits<idx_t>::Maximum() / FRAMES_PER_EXPRESSION_LEVEL
	                 ? NumericLimits<idx_t>::Maximum()
	                 : max_expression_depth * FRAMES_PER_EXPRESSION_LEVEL;
	recursion_limit = MinValue(max_frames, MAX_RECURSION_DEPTH);
	return Match(input.matcher, input.state, 0);
}

MatcherResult MatchStack::ExecuteFrames(MatchInput input, idx_t depth) {
	D_ASSERT(frames.empty());
	D_ASSERT(depth <= max_frames);
	frame_limit = max_frames - depth;
	if (frames.capacity() == 0) {
		frames.reserve(INITIAL_FRAME_CAPACITY);
	}
	PushFrame(input);
	while (!frames.empty()) {
		if (!ExecuteFrame(frames.back())) {
			continue;
		}
		auto result = FinalizeFrame(frames.back());
		DestroyTopFrame();
		if (frames.empty()) {
			return result;
		}
		auto &parent = frames.back();
		D_ASSERT(!parent.child_result);
		parent.child_result = result;
	}
	throw InternalException("Matcher stack completed without a result");
}

} // namespace duckdb
