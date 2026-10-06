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

#include "core_functions/scalar/list_distinct_strings.hpp"

#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

namespace duckdb {

static constexpr idx_t LINEAR_SCAN_LIMIT = 32;

bool ListDistinctStrings::TryExecute(DataChunk &args, Vector &result) {
	auto &input = args.data[0];
	auto &child_type = ListType::GetChildType(input.GetType());
	if (child_type.id() != LogicalTypeId::VARCHAR || !StringType::GetCollation(child_type).empty() ||
	    input.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	StringVector::AddHeapReference(ListVector::GetChildMutable(result), ListVector::GetChild(input));
	auto lists = input.Values<VectorListType<string_t>>();
	auto writer = FlatVector::Writer<VectorListType<string_t>>(result, args.size());
	vector<string_t> seen;
	string_set_t seen_set;
	for (idx_t row = 0; row < args.size(); row++) {
		auto entry = lists[row];
		if (!entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto list = writer.WriteDynamicList();
		const auto length = entry.GetListLength();
		const bool linear = length <= LINEAR_SCAN_LIMIT;
		seen.clear();
		seen_set.clear();
		for (idx_t i = 0; i < length; i++) {
			auto element = entry.GetChildValue(i);
			if (!element.IsValid()) {
				continue;
			}
			auto value = element.GetValue();
			if (linear) {
				bool duplicate = false;
				for (auto &previous : seen) {
					if (previous == value) {
						duplicate = true;
						break;
					}
				}
				if (duplicate) {
					continue;
				}
				seen.push_back(value);
			} else if (!seen_set.insert(value).second) {
				continue;
			}
			list.WriteElement().WriteStringRef(value);
		}
	}
	return true;
}

} // namespace duckdb
