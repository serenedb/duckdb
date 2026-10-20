#include "duckdb/common/exception.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/swar.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "re2/segment_plan.h"

#include <absl/strings/ascii.h>

namespace duckdb {

static bool GlobMatchesBracket(char s, const char *pattern, idx_t plen, idx_t &pidx, bool &valid_pattern) {
	if (pidx == plen) {
		valid_pattern = false;
		return false;
	}
	// check the first character
	// if it is an exclamation mark we need to invert our logic
	bool invert = false;
	if (pattern[pidx] == '!') {
		invert = true;
		pidx++;
	}
	bool found_match = invert;
	idx_t start_pos = pidx;
	bool found_closing_bracket = false;
	// now check the remainder of the pattern
	while (pidx < plen) {
		auto p = pattern[pidx];
		// if the first character is a closing bracket, we match it literally
		// otherwise it indicates an end of bracket
		if (p == ']' && pidx > start_pos) {
			// end of bracket found: we are done
			found_closing_bracket = true;
			pidx++;
			break;
		}
		if (pidx + 1 == plen) {
			// no next character!
			break;
		}
		bool matches;
		if (pattern[pidx + 1] == '-') {
			// range! find the next character in the range
			if (pidx + 2 == plen) {
				break;
			}
			auto next_char = pattern[pidx + 2];
			// check if the current character is within the range
			matches = s >= p && s <= next_char;
			// shift the pattern forward past the range
			pidx += 3;
		} else {
			// no range! perform a direct match
			matches = p == s;
			// shift the pattern forward past the character
			pidx++;
		}
		if (found_match == invert && matches) {
			// found a match! set the found_matches flag
			// we keep on pattern matching after this until we reach the end bracket
			// however, we don't need to update the found_match flag anymore
			found_match = !invert;
		}
	}
	if (!found_closing_bracket) {
		// no end of bracket: invalid pattern
		valid_pattern = false;
		return false;
	}
	valid_pattern = true;
	return found_match;
}

bool Glob(const char *string, idx_t slen, const char *pattern, idx_t plen, bool allow_question_mark) {
	idx_t sidx = 0;
	idx_t pidx = 0;
	idx_t star_pidx = 0;
	idx_t star_sidx = 0;
	bool has_star = false;
	// main matching loop
	while (sidx < slen) {
		bool matched = false;
		idx_t next_pidx = pidx;
		if (pidx < plen) {
			auto p = pattern[pidx];
			switch (p) {
			case '*': {
				// asterisk: match any set of characters
				pidx++;
				// skip any subsequent asterisks
				while (pidx < plen && pattern[pidx] == '*') {
					pidx++;
				}
				// if the asterisk is the last character, the pattern always matches
				if (pidx == plen) {
					return true;
				}
				has_star = true;
				// remember the pattern position right after '*', and the current string position
				star_pidx = pidx;
				star_sidx = sidx;
				continue;
			}
			case '?':
				// when enabled: matches anything but null
				if (allow_question_mark) {
					matched = true;
				} else {
					matched = string[sidx] == p;
				}
				next_pidx = pidx + 1;
				break;
			case '[': {
				next_pidx = pidx + 1;
				bool valid_pattern;
				matched = GlobMatchesBracket(string[sidx], pattern, plen, next_pidx, valid_pattern);
				if (!valid_pattern) {
					return false;
				}
				break;
			}
			case '\\':
				// escape character, next character needs to match literally
				pidx++;
				// check that we still have a character remaining
				if (pidx == plen) {
					return false;
				}
				matched = string[sidx] == pattern[pidx];
				next_pidx = pidx + 1;
				break;
			default:
				// not a control character: characters need to match literally
				matched = string[sidx] == p;
				next_pidx = pidx + 1;
				break;
			}
		}
		if (matched) {
			sidx++;
			pidx = next_pidx;
			continue;
		}
		if (!has_star) {
			return false;
		}
		// backtrack: the last '*' consumes one more character and we retry from there
		star_sidx++;
		sidx = star_sidx;
		pidx = star_pidx;
	}
	while (pidx < plen && pattern[pidx] == '*') {
		pidx++;
	}
	// we are finished only if we have consumed the full pattern
	return pidx == plen;
}

namespace {
struct StandardCharacterReader {
	static void NextCharacter(const char *sdata, idx_t slen, idx_t &sidx) {
		sidx++;
		while (sidx < slen && !IsCharacter(sdata[sidx])) {
			sidx++;
		}
	}

	static char Operation(const char *data, idx_t pos) {
		return data[pos];
	}
};

struct ASCIILCaseReader {
	static void NextCharacter(const char *sdata, idx_t slen, idx_t &sidx) {
		StandardCharacterReader::NextCharacter(sdata, slen, sidx);
	}

	static char Operation(const char *data, idx_t pos) {
		return (char)absl::ascii_tolower((uint8_t)data[pos]);
	}
};

template <char PERCENTAGE, char UNDERSCORE, bool HAS_ESCAPE, class READER = StandardCharacterReader>
bool TemplatedLikeOperator(const char *sdata, idx_t slen, const char *pdata, idx_t plen, char escape) {
	idx_t pidx = 0;
	idx_t sidx = 0;
	// position right after the last '%' seen, used to retry the match one character further on mismatch
	bool has_percentage = false;
	idx_t retry_pidx = 0;
	idx_t retry_sidx = 0;
	while (sidx < slen) {
		if (pidx < plen) {
			char pchar = READER::Operation(pdata, pidx);
			char schar = READER::Operation(sdata, sidx);
			if (HAS_ESCAPE && pchar == escape) {
				if (pidx + 1 == plen) {
					throw SyntaxException(Exception::InitializeExtraInfo("INVALID_ESCAPE_SEQUENCE", optional_idx()),
					                      "LIKE pattern must not end with escape character");
				}
				if (READER::Operation(pdata, pidx + 1) == schar) {
					pidx += 2;
					sidx++;
					continue;
				}
			} else if (pchar == UNDERSCORE) {
				pidx++;
				READER::NextCharacter(sdata, slen, sidx);
				continue;
			} else if (pchar == PERCENTAGE) {
				pidx++;
				while (pidx < plen && pdata[pidx] == PERCENTAGE) {
					pidx++;
				}
				if (pidx == plen) {
					return true; /* tail is acceptable */
				}
				has_percentage = true;
				retry_pidx = pidx;
				retry_sidx = sidx;
				continue;
			} else if (pchar == schar) {
				pidx++;
				sidx++;
				continue;
			}
		}
		if (!has_percentage) {
			return false;
		}
		pidx = retry_pidx;
		sidx = ++retry_sidx;
	}
	while (pidx < plen && pdata[pidx] == PERCENTAGE && !(HAS_ESCAPE && escape == PERCENTAGE)) {
		pidx++;
	}
	return pidx == plen && sidx == slen;
}

struct LikeSegment {
	explicit LikeSegment(string pattern_p) : pattern(std::move(pattern_p)), finder(pattern) {
	}

	template <bool IGNORE_CASE>
	bool At(const_data_ptr_t str) const {
		if constexpr (IGNORE_CASE) {
			return duckdb_re2::LiteralFinder::EqualIgnoreCase(const_char_ptr_cast(str), pattern.c_str(),
			                                                  pattern.size());
		}
		return memcmp(str, pattern.c_str(), pattern.size()) == 0;
	}

	template <bool IGNORE_CASE>
	idx_t Find(const_data_ptr_t str, idx_t str_len) const {
		if constexpr (IGNORE_CASE) {
			auto begin = const_char_ptr_cast(str);
			auto found = finder.FindIgnoreCase(pattern, begin, begin + str_len);
			return found ? UnsafeNumericCast<idx_t>(found - begin) : DConstants::INVALID_INDEX;
		}
		return FindStrInStr(str, str_len, const_uchar_ptr_cast(pattern.c_str()), pattern.size(), finder);
	}

	string pattern;
	duckdb_re2::LiteralFinder finder;
};

struct LikeMatcher : public FunctionData {
	LikeMatcher(string like_pattern_p, bool has_escape, char escape, vector<LikeSegment> segments,
	            bool has_start_percentage, bool has_end_percentage, unique_ptr<duckdb_re2::SegmentPlan> plan_p)
	    : like_pattern(std::move(like_pattern_p)), has_escape(has_escape), escape(escape),
	      segments(std::move(segments)), has_start_percentage(has_start_percentage),
	      has_end_percentage(has_end_percentage), plan(std::move(plan_p)) {
	}

	bool HasPlan() const {
		return plan != nullptr;
	}

	bool MatchPlan(string_t &str) {
		return plan->Match(absl::string_view(str.GetData(), str.GetSize()));
	}

	template <bool IGNORE_CASE = false>
	bool Match(string_t &str) {
		auto str_data = const_uchar_ptr_cast(str.GetData());
		auto str_len = str.GetSize();
		idx_t segment_idx = 0;
		idx_t end_idx = segments.size() - 1;
		if (!has_start_percentage) {
			// no start sample_size: match the first part of the string directly
			auto &segment = segments[0];
			if (str_len < segment.pattern.size()) {
				return false;
			}
			if (!segment.At<IGNORE_CASE>(str_data)) {
				return false;
			}
			str_data += segment.pattern.size();
			str_len -= segment.pattern.size();
			segment_idx++;
			if (segments.size() == 1) {
				// only one segment, and it matches
				// we have a match if there is an end sample_size, OR if the memcmp was an exact match (remaining str is
				// empty)
				return has_end_percentage || str_len == 0;
			}
		}
		// main match loop: for every segment in the middle, use Contains to find the needle in the haystack
		for (; segment_idx < end_idx; segment_idx++) {
			auto &segment = segments[segment_idx];
			// find the pattern of the current segment
			idx_t next_offset = segment.Find<IGNORE_CASE>(str_data, str_len);
			if (next_offset == DConstants::INVALID_INDEX) {
				// could not find this pattern in the string: no match
				return false;
			}
			idx_t offset = next_offset + segment.pattern.size();
			str_data += offset;
			str_len -= offset;
		}
		if (!has_end_percentage) {
			end_idx--;
			// no end sample_size: match the final segment now
			auto &segment = segments.back();
			if (str_len < segment.pattern.size()) {
				return false;
			}
			if (!segment.At<IGNORE_CASE>(str_data + str_len - segment.pattern.size())) {
				return false;
			}
			return true;
		} else {
			auto &segment = segments.back();
			// find the pattern of the current segment
			return segment.Find<IGNORE_CASE>(str_data, str_len) != DConstants::INVALID_INDEX;
		}
	}

	static unique_ptr<LikeMatcher> CreateLikeMatcher(string like_pattern, bool has_escape = true, char escape = '\\') {
		return Create(std::move(like_pattern), has_escape, escape, true);
	}

	static unique_ptr<LikeMatcher> CreateLiteralMatcher(string like_pattern, bool has_escape, char escape) {
		return Create(std::move(like_pattern), has_escape, escape, false);
	}

	unique_ptr<FunctionData> Copy() const override {
		if (plan) {
			return CreateLikeMatcher(like_pattern, has_escape, escape);
		}
		return make_uniq<LikeMatcher>(like_pattern, has_escape, escape, segments, has_start_percentage,
		                              has_end_percentage, nullptr);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LikeMatcher>();
		return like_pattern == other.like_pattern && has_escape == other.has_escape && escape == other.escape;
	}

private:
	static unique_ptr<LikeMatcher> Create(string like_pattern, bool has_escape, char escape, bool single_characters) {
		vector<LikeSegment> segments;
		string literal;
		bool has_start_percentage = false;
		bool has_end_percentage = false;
		for (idx_t i = 0; i < like_pattern.size(); i++) {
			auto ch = like_pattern[i];
			if (has_escape && ch == escape) {
				if (i + 1 == like_pattern.size()) {
					return nullptr;
				}
				literal += like_pattern[++i];
				continue;
			}
			if (ch == '_') {
				if (!single_characters) {
					return nullptr;
				}
				unique_ptr<duckdb_re2::SegmentPlan> plan(
				    duckdb_re2::SegmentPlan::MakeLike(like_pattern, has_escape, escape, true));
				if (!plan) {
					return nullptr;
				}
				return make_uniq<LikeMatcher>(std::move(like_pattern), has_escape, escape, vector<LikeSegment>(), false,
				                              false, std::move(plan));
			}
			if (ch != '%') {
				literal += ch;
				continue;
			}
			// special character, push a constant pattern
			if (!literal.empty()) {
				segments.emplace_back(std::move(literal));
				literal.clear();
			}
			// sample_size
			if (i == 0) {
				has_start_percentage = true;
			}
			if (i + 1 == like_pattern.size()) {
				has_end_percentage = true;
			}
		}
		if (!literal.empty()) {
			segments.emplace_back(std::move(literal));
		}
		if (segments.empty()) {
			return nullptr;
		}
		return make_uniq<LikeMatcher>(std::move(like_pattern), has_escape, escape, std::move(segments),
		                              has_start_percentage, has_end_percentage, nullptr);
	}

	string like_pattern;
	bool has_escape;
	char escape;
	vector<LikeSegment> segments;
	bool has_start_percentage;
	bool has_end_percentage;
	unique_ptr<duckdb_re2::SegmentPlan> plan;
};

unique_ptr<FunctionData> LikeBindFunction(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	// pattern is the second argument. If its constant, we can already prepare the pattern and store it for later.
	D_ASSERT(arguments.size() == 2 || arguments.size() == 3);
	for (auto &arg : arguments) {
		if (arg->GetReturnType().id() == LogicalTypeId::VARCHAR &&
		    !StringType::GetCollation(arg->GetReturnType()).empty()) {
			return nullptr;
		}
	}
	auto pattern_str = input.TryGetConstant(1);
	if (pattern_str) {
		return LikeMatcher::CreateLikeMatcher(pattern_str->ToString());
	}
	return nullptr;
}

bool LikeOperatorFunction(const char *s, idx_t slen, const char *pattern, idx_t plen, char escape) {
	return TemplatedLikeOperator<'%', '_', true>(s, slen, pattern, plen, escape);
}

bool LikeOperatorFunction(const char *s, idx_t slen, const char *pattern, idx_t plen) {
	return TemplatedLikeOperator<'%', '_', false>(s, slen, pattern, plen, '\0');
}

bool LikeOperatorFunction(string_t &s, string_t &pat) {
	return LikeOperatorFunction(s.GetData(), s.GetSize(), pat.GetData(), pat.GetSize());
}

bool LikeOperatorFunction(string_t &s, string_t &pat, char escape) {
	return LikeOperatorFunction(s.GetData(), s.GetSize(), pat.GetData(), pat.GetSize(), escape);
}

static void CheckEscapeString(const string_t &escape) {
	// Only one escape character should be allowed
	if (escape.GetSize() > 1) {
		auto extra_info = Exception::InitializeExtraInfo("INVALID_ESCAPE_SEQUENCE", optional_idx());
		extra_info["hint"] = "Escape string must be empty or one character.";
		throw SyntaxException(extra_info, "invalid escape string");
	}
}

char GetEscapeChar(string_t escape) {
	CheckEscapeString(escape);
	return escape.GetSize() == 0 ? '\0' : *escape.GetData();
}

bool CaseCanChangeEscapes(const char *data, idx_t size, char escape) {
	auto letter = static_cast<uint8_t>(StringUtil::CharacterToLower(escape));
	bool found = false;
	for (idx_t i = 0; i < size; i++) {
		auto c = static_cast<uint8_t>(data[i]);
		found |= (c | 0x20) == letter || c >= 0x80;
	}
	return found;
}

idx_t WithBackslashEscape(const char *data, idx_t size, char escape, char *result) {
	idx_t length = 0;
	for (idx_t i = 0; i < size; i++) {
		if (data[i] == escape) {
			result[length++] = '\\';
			if (++i < size) {
				result[length++] = data[i];
			}
		} else if (data[i] == '\\') {
			result[length++] = '\\';
			result[length++] = '\\';
		} else {
			result[length++] = data[i];
		}
	}
	return length;
}

bool LikeEscapeOperator(string_t str, string_t pattern, string_t escape) {
	char escape_char = GetEscapeChar(escape);
	return escape.GetSize() == 0 ? LikeOperatorFunction(str, pattern) : LikeOperatorFunction(str, pattern, escape_char);
}

bool UniformPattern(const Vector &patterns, string_t &pattern) {
	if (patterns.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(patterns)) {
			return false;
		}
		pattern = *ConstantVector::GetData<string_t>(patterns);
		return true;
	}
	auto values = patterns.Values<string_t>();
	if (values.size() == 0 || !values[0].IsValid()) {
		return false;
	}
	pattern = values[0].GetValue();
	for (auto entry : values) {
		if (!entry.IsValid() || entry.GetValue() != pattern) {
			return false;
		}
	}
	return true;
}

struct LikeLocalState : public FunctionLocalState {
	optional_ptr<LikeMatcher> Matcher(string_t pattern, bool has_escape, char escape) {
		if (!prepared || pattern != last_pattern || has_escape != last_has_escape || escape != last_escape) {
			last_pattern.assign(pattern.GetData(), pattern.GetSize());
			last_has_escape = has_escape;
			last_escape = escape;
			matcher = LikeMatcher::CreateLikeMatcher(last_pattern, has_escape, escape);
			prepared = true;
		}
		return matcher.get();
	}

	string last_pattern;
	bool last_has_escape = false;
	char last_escape = '\0';
	bool prepared = false;
	unique_ptr<LikeMatcher> matcher;
};

unique_ptr<FunctionLocalState> LikeInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                  FunctionData *bind_data) {
	return make_uniq<LikeLocalState>();
}

template <bool INVERT>
void MatchAll(LikeMatcher &matcher, const Vector &strings, Vector &result) {
	if (matcher.HasPlan()) {
		UnaryExecutor::Execute<string_t, bool>(strings, result, [&](string_t str) {
			bool match = matcher.MatchPlan(str);
			return INVERT ? !match : match;
		});
		return;
	}
	UnaryExecutor::Execute<string_t, bool>(strings, result, [&](string_t str) {
		bool match = matcher.Match(str);
		return INVERT ? !match : match;
	});
}

template <bool INVERT>
void LikeMatchPatterns(LikeLocalState &lstate, const Vector &strings, const Vector &patterns, bool has_escape,
                       char escape, Vector &result) {
	string_t pattern;
	if (UniformPattern(patterns, pattern)) {
		auto matcher = lstate.Matcher(pattern, has_escape, escape);
		if (matcher) {
			MatchAll<INVERT>(*matcher, strings, result);
			return;
		}
	}
	// use generic like matcher
	if (has_escape) {
		BinaryExecutor::Execute<string_t, string_t, bool>(strings, patterns, result,
		                                                  [&](string_t str, string_t pattern) {
			                                                  bool match = LikeOperatorFunction(str, pattern, escape);
			                                                  return INVERT ? !match : match;
		                                                  });
		return;
	}
	BinaryExecutor::Execute<string_t, string_t, bool>(strings, patterns, result, [&](string_t str, string_t pattern) {
		bool match = LikeOperatorFunction(str, pattern);
		return INVERT ? !match : match;
	});
}

struct ILikeBuffer {
	char *Reserve(idx_t size) {
		if (size > capacity) {
			data = make_unsafe_uniq_array_uninitialized<char>(size);
			capacity = size;
		}
		return data.get();
	}

	string_t Lower(string_t input) {
		auto capacity = 2 * input.GetSize();
		auto result = Reserve(capacity);
		auto lower_size = LowerCase(input.GetData(), input.GetSize(), result, capacity);
		return string_t(result, UnsafeNumericCast<uint32_t>(lower_size));
	}

	unsafe_unique_array<char> data;
	idx_t capacity = 0;
};

bool HasNonAsciiLoweringToAscii(const char *data, idx_t size) {
	idx_t i = 0;
	for (; i + SwarWord::SIZE <= size; i += SwarWord::SIZE) {
		auto word = Load<uint64_t>(const_data_ptr_cast(data + i));
		if (SwarWord::ZeroBytes(word ^ SwarWord::Repeat(0xC4)) | SwarWord::ZeroBytes(word ^ SwarWord::Repeat(0xE2))) {
			break;
		}
	}
	for (; i + 1 < size; i++) {
		auto byte = static_cast<uint8_t>(data[i]);
		auto next = static_cast<uint8_t>(data[i + 1]);
		if ((byte == 0xC4 && next == 0xB0) ||
		    (byte == 0xE2 && next == 0x84 && i + 2 < size && static_cast<uint8_t>(data[i + 2]) == 0xAA)) {
			return true;
		}
	}
	return false;
}

struct ILikePattern {
	template <class READER = StandardCharacterReader>
	bool Match(string_t &str) {
		if (!has_escape) {
			return TemplatedLikeOperator<'%', '_', false, READER>(str.GetData(), str.GetSize(), pattern.GetData(),
			                                                      pattern.GetSize(), '\0');
		}
		return TemplatedLikeOperator<'%', '_', true, READER>(str.GetData(), str.GetSize(), pattern.GetData(),
		                                                     pattern.GetSize(), escape);
	}

	string_t pattern;
	bool has_escape;
	char escape;
	bool ascii;
};

struct ILikeMatcher {
	explicit ILikeMatcher(const ILikePattern &lowered_p) : lowered(lowered_p) {
		auto size = lowered.pattern.GetSize();
		if (!lowered.pattern.IsInlined()) {
			auto copy = buffer.Reserve(size);
			memcpy(copy, lowered.pattern.GetData(), size);
			lowered.pattern = string_t(copy, UnsafeNumericCast<uint32_t>(size));
		}
		matcher = LikeMatcher::CreateLiteralMatcher(string(lowered.pattern.GetData(), size), lowered.has_escape,
		                                            lowered.escape);
	}

	template <bool KNOWN_ASCII>
	bool Match(string_t &str, ILikeBuffer &lowered_str) {
		if (!KNOWN_ASCII) {
			auto ascii_end = FirstNonAscii(str.GetData(), str.GetSize());
			if (ascii_end != str.GetSize()) {
				return MatchNonAscii(str, ascii_end, lowered_str);
			}
		}
		return MatchUnlowered(str);
	}

	bool MatchUnlowered(string_t &str) {
		if (matcher) {
			return matcher->Match<true>(str);
		}
		return lowered.Match<ASCIILCaseReader>(str);
	}

	DUCKDB_NOINLINE bool MatchNonAscii(string_t &str, idx_t ascii_end, ILikeBuffer &lowered_str) {
		if (lowered.ascii && !HasNonAsciiLoweringToAscii(str.GetData() + ascii_end, str.GetSize() - ascii_end)) {
			return MatchUnlowered(str);
		}
		return MatchLowered(str, lowered_str);
	}

	DUCKDB_NOINLINE bool MatchLowered(string_t &str, ILikeBuffer &lowered_str) {
		auto str_lcase = lowered_str.Lower(str);
		if (matcher) {
			return matcher->Match(str_lcase);
		}
		return lowered.Match(str_lcase);
	}

	ILikeBuffer buffer;
	ILikePattern lowered;
	unique_ptr<LikeMatcher> matcher;
};

struct ILikeLocalState : public FunctionLocalState {
	ILikePattern LowerPattern(string_t input, string_t escape) {
		char escape_char = GetEscapeChar(escape);
		bool has_escape = escape.GetSize() != 0;
		if (has_escape && StringUtil::CharacterIsAlpha(escape_char) &&
		    CaseCanChangeEscapes(input.GetData(), input.GetSize(), escape_char)) {
			auto result = escaped.Reserve(2 * input.GetSize());
			auto size = WithBackslashEscape(input.GetData(), input.GetSize(), escape_char, result);
			input = string_t(result, UnsafeNumericCast<uint32_t>(size));
			escape_char = '\\';
		}
		auto lowered = pattern.Lower(input);
		auto ascii = FirstNonAscii(lowered.GetData(), lowered.GetSize()) == lowered.GetSize();
		return {lowered, has_escape, escape_char, ascii};
	}

	ILikeBuffer str;
	ILikeBuffer escaped;
	ILikeBuffer pattern;
	string constant_pattern;
	string constant_escape;
	unique_ptr<ILikeMatcher> constant_matcher;
};

unique_ptr<FunctionLocalState> ILikeInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                   FunctionData *bind_data) {
	return make_uniq<ILikeLocalState>();
}

template <bool KNOWN_ASCII>
bool ILikeOperatorFunction(ILikeLocalState &lstate, string_t str, string_t pattern, string_t escape) {
	auto pat_lcase = lstate.LowerPattern(pattern, escape);
	if (!KNOWN_ASCII) {
		auto data = str.GetData();
		auto size = str.GetSize();
		auto ascii_end = FirstNonAscii(data, size);
		if (ascii_end != size && (!pat_lcase.ascii || HasNonAsciiLoweringToAscii(data + ascii_end, size - ascii_end))) {
			auto str_lcase = lstate.str.Lower(str);
			return pat_lcase.Match(str_lcase);
		}
	}
	return pat_lcase.Match<ASCIILCaseReader>(str);
}

struct GlobOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return Glob(str.GetData(), str.GetSize(), pattern.GetData(), pattern.GetSize());
	}
};

template <bool INVERT>
void LikeEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str = args.data[0];
	const auto &pattern = args.data[1];
	const auto &escape = args.data[2];

	if (escape.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(escape)) {
		auto escape_str = *ConstantVector::GetData<string_t>(escape);
		if (escape_str.GetSize() <= 1) {
			auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LikeLocalState>();
			LikeMatchPatterns<INVERT>(lstate, str, pattern, escape_str.GetSize() == 1, GetEscapeChar(escape_str),
			                          result);
			return;
		}
	}
	TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
	    str, pattern, escape, result, [&](string_t str, string_t pattern, string_t escape) {
		    bool match = LikeEscapeOperator(str, pattern, escape);
		    return INVERT ? !match : match;
	    });
}

template <bool INVERT, bool KNOWN_ASCII>
void ILikeConstantPattern(ILikeLocalState &lstate, const Vector &str_vec, string_t pattern, string_t escape,
                          Vector &result) {
	if (!lstate.constant_matcher || pattern != lstate.constant_pattern || escape != lstate.constant_escape) {
		lstate.constant_matcher.reset();
		lstate.constant_pattern.assign(pattern.GetData(), pattern.GetSize());
		lstate.constant_escape.assign(escape.GetData(), escape.GetSize());
		// lowercase the pattern exactly once, up front
		lstate.constant_matcher = make_uniq<ILikeMatcher>(lstate.LowerPattern(pattern, escape));
	}
	auto &matcher = *lstate.constant_matcher;
	UnaryExecutor::Execute<string_t, bool>(str_vec, result, [&](string_t str) {
		bool match = matcher.Match<KNOWN_ASCII>(str, lstate.str);
		return INVERT ? !match : match;
	});
}

template <bool INVERT>
void ILikeEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<ILikeLocalState>();
	auto &str_vec = args.data[0];
	auto &pattern_vec = args.data[1];
	auto &escape_vec = args.data[2];

	string_t pattern;
	string_t escape;
	if (UniformPattern(pattern_vec, pattern) && UniformPattern(escape_vec, escape)) {
		ILikeConstantPattern<INVERT, false>(lstate, str_vec, pattern, escape, result);
		return;
	}
	// non-constant pattern/escape: fall back to the generic per-row implementation
	TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
	    str_vec, pattern_vec, escape_vec, result, [&](string_t str, string_t pattern, string_t escape) {
		    bool match = ILikeOperatorFunction<false>(lstate, str, pattern, escape);
		    return INVERT ? !match : match;
	    });
}

// Execution function for ILIKE / NOT ILIKE.
// When the pattern is constant we lowercase it exactly once instead of once per row, and we reuse a single
// scratch buffer to lowercase each string value instead of heap-allocating per row.
template <bool INVERT, bool KNOWN_ASCII>
void ILikeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<ILikeLocalState>();
	auto &pattern_vec = input.data[1];
	string_t escape("\\", 1);
	string_t pattern;
	if (UniformPattern(pattern_vec, pattern)) {
		ILikeConstantPattern<INVERT, KNOWN_ASCII>(lstate, input.data[0], pattern, escape, result);
		return;
	}
	// non-constant pattern: fall back to the generic per-row implementation
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    input.data[0], pattern_vec, result, [&](string_t str, string_t pattern) {
		    bool match = ILikeOperatorFunction<KNOWN_ASCII>(lstate, str, pattern, escape);
		    return INVERT ? !match : match;
	    });
}

template <bool INVERT>
unique_ptr<BaseStatistics> ILikePropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() >= 1);
	// can only propagate stats if the children have stats
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(ILikeFunction<INVERT, true>);
	}
	return nullptr;
}

template <bool INVERT>
void RegularLikeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	if (func_expr.BindInfo()) {
		auto &matcher = func_expr.BindInfo()->Cast<LikeMatcher>();
		// use fast like matcher
		MatchAll<INVERT>(matcher, input.data[0], result);
	} else {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LikeLocalState>();
		LikeMatchPatterns<INVERT>(lstate, input.data[0], input.data[1], true, '\\', result);
	}
}

template <bool GLOB>
bool PatternMinSize(const string &pattern, bool has_escape, char escape, idx_t &min_size) {
	min_size = 0;
	for (idx_t i = 0; i < pattern.size(); i++) {
		auto c = pattern[i];
		if (has_escape && c == escape) {
			if (++i == pattern.size()) {
				return false;
			}
		} else if (c == (GLOB ? '*' : '%')) {
			continue;
		} else if (GLOB && c == '[') {
			min_size++;
			return true;
		}
		min_size++;
	}
	return true;
}

template <bool GLOB>
FilterPropagateResult PatternFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto &children = input.function.GetChildren();
	if (children.size() < 2 || children[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	bool has_escape = true;
	char escape = '\\';
	if (children.size() == 3) {
		if (children[2]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		auto &escape_value = children[2]->Cast<BoundConstantExpression>().GetValue();
		if (escape_value.IsNull() || StringValue::Get(escape_value).size() > 1) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		has_escape = !StringValue::Get(escape_value).empty();
		escape = has_escape ? StringValue::Get(escape_value)[0] : '\0';
	}
	auto column_stats = input.ChildStats(0);
	if (!column_stats || column_stats->GetStatsType() != StatisticsType::STRING_STATS) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!column_stats->CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	auto &pattern_value = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (pattern_value.IsNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	idx_t min_size = 0;
	if (!PatternMinSize<GLOB>(StringValue::Get(pattern_value), has_escape, escape, min_size) ||
	    !StringStats::HasMaxStringLength(*column_stats) || StringStats::MaxStringLength(*column_stats) >= min_size) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

} // namespace

ScalarFunction NotLikeFun::GetFunction() {
	ScalarFunction not_like("!~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                        RegularLikeFunction<true>, LikeBindFunction, nullptr, LikeInitLocalState);
	not_like.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	not_like.GetSignature().GetParameter(0).SetName("string");
	not_like.GetSignature().GetParameter(1).SetName("pattern");
	return not_like;
}

ScalarFunction GlobPatternFun::GetFunction() {
	ScalarFunction glob("~~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                    ScalarFunction::BinaryFunction<string_t, string_t, bool, GlobOperator>);
	glob.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	glob.GetSignature().GetParameter(0).SetName("string");
	glob.GetSignature().GetParameter(1).SetName("pattern");
	glob.SetFilterPruneCallback(PatternFilterPrune<true>);
	return glob;
}

ScalarFunction ILikeFun::GetFunction() {
	ScalarFunction ilike("~~*", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                     ILikeFunction<false, false>, nullptr, ILikePropagateStats<false>, ILikeInitLocalState);
	ilike.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	ilike.GetSignature().GetParameter(0).SetName("string");
	ilike.GetSignature().GetParameter(1).SetName("pattern");
	return ilike;
}

ScalarFunction NotILikeFun::GetFunction() {
	ScalarFunction not_ilike("!~~*", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                         ILikeFunction<true, false>, nullptr, ILikePropagateStats<true>, ILikeInitLocalState);
	not_ilike.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	not_ilike.GetSignature().GetParameter(0).SetName("string");
	not_ilike.GetSignature().GetParameter(1).SetName("pattern");
	return not_ilike;
}

ScalarFunction LikeFun::GetFunction() {
	ScalarFunction like("~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                    RegularLikeFunction<false>, LikeBindFunction, nullptr, LikeInitLocalState);
	like.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	like.GetSignature().GetParameter(0).SetName("string");
	like.GetSignature().GetParameter(1).SetName("pattern");
	like.SetFilterPruneCallback(PatternFilterPrune<false>);
	return like;
}

ScalarFunction NotLikeEscapeFun::GetFunction() {
	ScalarFunction not_like_escape("not_like_escape", {}, LogicalType::BOOLEAN, LikeEscapeFunction<true>, nullptr,
	                               nullptr, LikeInitLocalState);
	not_like_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	not_like_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_like_escape;
}

ScalarFunction IlikeEscapeFun::GetFunction() {
	ScalarFunction ilike_escape("ilike_escape", {}, LogicalType::BOOLEAN, ILikeEscapeFunction<false>, nullptr, nullptr,
	                            ILikeInitLocalState);
	ilike_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	ilike_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return ilike_escape;
}

ScalarFunction NotIlikeEscapeFun::GetFunction() {
	ScalarFunction not_ilike_escape("not_ilike_escape", {}, LogicalType::BOOLEAN, ILikeEscapeFunction<true>, nullptr,
	                                nullptr, ILikeInitLocalState);
	not_ilike_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	not_ilike_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_ilike_escape;
}
ScalarFunction LikeEscapeFun::GetFunction() {
	ScalarFunction like_escape("like_escape", {}, LogicalType::BOOLEAN, LikeEscapeFunction<false>, nullptr, nullptr,
	                           LikeInitLocalState);
	like_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	like_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	like_escape.SetFilterPruneCallback(PatternFilterPrune<false>);
	return like_escape;
}

//===--------------------------------------------------------------------===//
// similar_to_escape
//===--------------------------------------------------------------------===//
// The parser rewrites `x SIMILAR TO y` into
// `regexp_full_match(x, similar_to_escape(y))`, so this translation has to live
// beside the parser rather than in a server-side function set -- otherwise the
// rewrite emits a call that a plain DuckDB has no way to resolve.
static string SimilarToRegex(const string_t &pattern_str, std::string_view escape) {
	auto pattern = pattern_str.GetData();
	auto plen = pattern_str.GetSize();

	string result;
	result.reserve(plen + 6);
	result += "^(?:";

	bool after_escape = false;
	idx_t nquotes = 0;
	idx_t bracket_depth = 0;
	idx_t charclass_pos = 0;

	for (idx_t i = 0; i < plen; i++) {
		char pchar = pattern[i];
		if (after_escape) {
			if (pchar == '"' && bracket_depth < 1) {
				// escape-double-quote marks the start/end of the captured substring
				if (nquotes == 0) {
					result += "){1,1}?(";
				} else if (nquotes == 1) {
					result += "){1,1}(?:";
				} else {
					throw InvalidInputException(
					    Exception::InitializeExtraInfo("INVALID_USE_OF_ESCAPE_CHARACTER", optional_idx()),
					    "SQL regular expression may not contain more than two escape-double-quote separators");
				}
				nquotes++;
			} else {
				result += '\\';
				result += pchar;
				charclass_pos = 3;
			}
			after_escape = false;
		} else if (!escape.empty() && pchar == escape.front()) {
			after_escape = true;
		} else if (bracket_depth > 0) {
			// inside a bracket expression the SIMILAR TO metacharacters are literal
			if (pchar == '\\') {
				result += '\\';
			}
			result += pchar;
			if (pchar == ']' && charclass_pos > 2) {
				bracket_depth--;
			} else if (pchar == '[') {
				bracket_depth++;
				charclass_pos = 3;
			} else if (pchar == '^') {
				charclass_pos++;
			} else {
				charclass_pos = 3;
			}
		} else if (pchar == '[') {
			result += pchar;
			bracket_depth = 1;
			charclass_pos = 1;
		} else if (pchar == '%') {
			result += ".*";
		} else if (pchar == '_') {
			result += '.';
		} else if (pchar == '(') {
			// SIMILAR TO parentheses group without capturing
			result += "(?:";
		} else if (pchar == '\\' || pchar == '.' || pchar == '^' || pchar == '$') {
			result += '\\';
			result += pchar;
		} else {
			result += pchar;
		}
	}
	result += ")$";
	return result;
}

static void SimilarToEscapeFunction(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t pattern) {
		return StringVector::AddString(result, SimilarToRegex(pattern, "\\"));
	});
}

static void SimilarToEscapeWithEscapeFunction(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t pattern, string_t escape) {
		    CheckEscapeString(escape);
		    return StringVector::AddString(
		        result, SimilarToRegex(pattern, std::string_view {escape.GetData(), escape.GetSize()}));
	    });
}

ScalarFunctionSet SimilarToEscapeFun::GetFunctions() {
	ScalarFunctionSet similar_to_escape("similar_to_escape");
	similar_to_escape.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, SimilarToEscapeFunction));
	similar_to_escape.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                             SimilarToEscapeWithEscapeFunction));
	similar_to_escape.SetFallible();
	return similar_to_escape;
}

} // namespace duckdb
