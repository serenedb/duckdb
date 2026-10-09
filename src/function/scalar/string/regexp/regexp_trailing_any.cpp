////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "duckdb/function/scalar/regexp_trailing_any.hpp"

#include "duckdb/common/string_util.hpp"
#include "re2/regexp.h"

namespace duckdb {

static constexpr idx_t TRAILING_ANY_SIZE = 3;

static bool IsDotWithoutNewline(duckdb_re2::Regexp &regexp) {
	if (regexp.op() != duckdb_re2::kRegexpCharClass) {
		return false;
	}
	auto cc = regexp.cc();
	auto range = cc->begin();
	return cc->end() - range == 2 && range[0].lo == 0 && range[0].hi == '\n' - 1 && range[1].lo == '\n' + 1;
}

string RegexpTrailingAny::HeadPattern(const string &pattern, const duckdb_re2::RE2::Options &options) {
	if (options.dot_nl() || options.never_nl() || options.literal() || options.longest_match() ||
	    pattern.size() <= TRAILING_ANY_SIZE || !StringUtil::EndsWith(pattern, ".*$")) {
		return string();
	}
	idx_t backslashes = 0;
	for (idx_t i = pattern.size() - TRAILING_ANY_SIZE; i > 0 && pattern[i - 1] == '\\'; i--) {
		backslashes++;
	}
	if (backslashes % 2 != 0) {
		return string();
	}
	RE2 re(pattern, options);
	if (!re.ok()) {
		return string();
	}
	auto regexp = re.Regexp();
	auto nsub = regexp->nsub();
	if (regexp->op() != duckdb_re2::kRegexpConcat || nsub < 3) {
		return string();
	}
	auto subs = regexp->sub();
	auto star = subs[nsub - 2];
	if (subs[nsub - 1]->op() != duckdb_re2::kRegexpEndText || star->op() != duckdb_re2::kRegexpStar ||
	    star->nsub() != 1 || !IsDotWithoutNewline(*star->sub()[0])) {
		return string();
	}
	return pattern.substr(0, pattern.size() - TRAILING_ANY_SIZE);
}

RegexpTrailingAny::Result RegexpTrailingAny::Extract(const string_t &input, const RE2 &head, int8_t group_index,
                                                     string_t &output) {
	D_ASSERT(group_index >= 0 && group_index <= head.NumberOfCapturingGroups());
	duckdb_re2::StringPiece in_piece(input.GetData(), input.GetSize());
	duckdb_re2::StringPiece submatch[10];
	if (!head.Match(in_piece, 0, in_piece.size(), duckdb_re2::RE2::UNANCHORED, submatch, group_index + 1)) {
		return Result::NO_MATCH;
	}
	auto head_end = submatch[0].data() + submatch[0].size();
	auto input_end = input.GetData() + input.GetSize();
	if (memchr(head_end, '\n', UnsafeNumericCast<size_t>(input_end - head_end))) {
		return Result::FALLBACK;
	}
	if (group_index == 0) {
		output = string_t(submatch[0].data(), UnsafeNumericCast<uint32_t>(input_end - submatch[0].data()));
	} else {
		output = string_t(submatch[group_index].data(), UnsafeNumericCast<uint32_t>(submatch[group_index].size()));
	}
	return Result::MATCH;
}

unique_ptr<FunctionLocalState> RegexpTrailingAny::InitLocalState(ExpressionState &state,
                                                                 const BoundFunctionExpression &expr,
                                                                 FunctionData *bind_data) {
	auto &info = bind_data->Cast<RegexpExtractBindData>();
	if (!info.constant_pattern) {
		return nullptr;
	}
	auto result = make_uniq<RegexLocalState>(info);
	if (!info.head_pattern.empty() && info.group_index >= 0 &&
	    info.group_index <= result->constant_pattern.NumberOfCapturingGroups()) {
		result->head_pattern = make_uniq<RE2>(info.head_pattern, info.options);
		if (!result->head_pattern->ok()) {
			result->head_pattern.reset();
		}
	}
	return std::move(result);
}

} // namespace duckdb
