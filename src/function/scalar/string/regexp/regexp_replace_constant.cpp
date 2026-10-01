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

#include "duckdb/function/scalar/regexp_replace_constant.hpp"

#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

namespace duckdb {

void RegexpReplaceConstant::Execute(const Vector &strings, string_t replace, RegexLocalState &lstate, Vector &result) {
	const auto &re = lstate.constant_pattern;
	const duckdb_re2::StringPiece rewrite(replace.GetData(), replace.GetSize());
	bool rewrite_checked = false;
	const int nvec = 1 + duckdb_re2::RE2::MaxSubmatch(rewrite);
	StringVector::AddHeapReference(result, strings);
	std::string buffer;
	duckdb_re2::StringPiece vec[10];
	UnaryExecutor::Execute<string_t, string_t>(strings, result, [&](string_t input) {
		if (!rewrite_checked) {
			std::string rewrite_error;
			if (!re.CheckRewriteString(rewrite, &rewrite_error)) {
				throw InvalidInputException("Invalid replacement string for regexp_replace: %s", rewrite_error);
			}
			rewrite_checked = true;
		}
		const duckdb_re2::StringPiece piece(input.GetData(), input.GetSize());
		if (!re.Match(piece, 0, piece.size(), duckdb_re2::RE2::UNANCHORED, vec, nvec)) {
			return input;
		}
		const auto match_begin = static_cast<idx_t>(vec[0].data() - input.GetData());
		const auto match_end = match_begin + vec[0].size();
		buffer.assign(input.GetData(), match_begin);
		re.Rewrite(&buffer, rewrite, vec, nvec);
		buffer.append(input.GetData() + match_end, input.GetSize() - match_end);
		return StringVector::AddString(result, buffer);
	});
}

} // namespace duckdb
