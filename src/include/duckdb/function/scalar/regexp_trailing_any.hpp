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

#pragma once

#include "duckdb/function/scalar/regexp.hpp"

namespace duckdb {

// A trailing `.*$` accepts the rest of the input exactly when it holds no newline, so a pattern `P.*$`
// can be matched as `P` plus a newline check on the tail: RE2 then tracks submatches over P alone.
struct RegexpTrailingAny {
	enum class Result : uint8_t { MATCH, NO_MATCH, FALLBACK };

	//! P for a constant pattern `P.*$` whose `.` does not match newlines, empty otherwise
	static string HeadPattern(const string &pattern, const duckdb_re2::RE2::Options &options);
	//! Extracts `group_index` of `P.*$` using the compiled head P; FALLBACK when the tail holds a newline
	static Result Extract(const string_t &input, const RE2 &head, int8_t group_index, string_t &output);
	static unique_ptr<FunctionLocalState> InitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
	                                                     FunctionData *bind_data);
};

} // namespace duckdb
