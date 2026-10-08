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

#include "core_functions/scalar/fast_replace.hpp"

#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "re2/literal_finder.h"

#include <cstring>

namespace duckdb {

static string_t Replace(const string_t &haystack, const string_t &needle, const string_t &thread, Vector &result,
                        optional_ptr<const duckdb_re2::LiteralFinder> finder, vector<idx_t> &positions) {
	auto haystack_data = const_uchar_ptr_cast(haystack.GetData());
	auto haystack_size = haystack.GetSize();
	auto needle_data = const_uchar_ptr_cast(needle.GetData());
	auto needle_size = needle.GetSize();
	if (needle_size == 0 || needle_size > haystack_size) {
		return haystack;
	}
	positions.clear();
	idx_t pos = 0;
	while (haystack_size - pos >= needle_size) {
		auto found = finder ? FindStrInStr(haystack_data + pos, haystack_size - pos, needle_data, needle_size, *finder)
		                    : FindStrInStr(haystack_data + pos, haystack_size - pos, needle_data, needle_size);
		if (found == DConstants::INVALID_INDEX) {
			break;
		}
		positions.push_back(pos + found);
		pos += found + needle_size;
	}
	if (positions.empty()) {
		return haystack;
	}
	auto thread_data = thread.GetData();
	auto thread_size = thread.GetSize();
	auto result_size = haystack_size - positions.size() * needle_size + positions.size() * thread_size;
	auto target = StringVector::EmptyString(result, result_size);
	auto out = target.GetDataWriteable();
	idx_t copied = 0;
	for (auto position : positions) {
		memcpy(out, haystack_data + copied, position - copied);
		out += position - copied;
		memcpy(out, thread_data, thread_size);
		out += thread_size;
		copied = position + needle_size;
	}
	memcpy(out, haystack_data + copied, haystack_size - copied);
	target.Finalize();
	return target;
}

void FastReplace::Execute(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &haystack_vector = args.data[0];
	const auto &needle_vector = args.data[1];
	const auto &thread_vector = args.data[2];

	unique_ptr<duckdb_re2::LiteralFinder> finder;
	if (needle_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(needle_vector)) {
		auto needle = ConstantVector::GetData<string_t>(needle_vector)[0];
		if (needle.GetSize() > 1) {
			finder = make_uniq<duckdb_re2::LiteralFinder>(absl::string_view(needle.GetData(), needle.GetSize()));
		}
	}
	StringVector::AddHeapReference(result, haystack_vector);
	vector<idx_t> positions;
	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    haystack_vector, needle_vector, thread_vector, result,
	    [&](string_t input_string, string_t needle_string, string_t thread_string) {
		    return Replace(input_string, needle_string, thread_string, result, finder.get(), positions);
	    });
}

} // namespace duckdb
