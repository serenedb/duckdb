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

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <span>

namespace duckdb {

class RowSort {
public:
	RowSort(const vector<LogicalType> &types, vector<OrderModifiers> modifiers, idx_t capacity);

	void Append(DataChunk &keys);

	void Sort(std::span<sel_t> order);

private:
	vector<OrderModifiers> modifiers;
	idx_t sort_width;
	LogicalType key_type;
	SortKeyType sort_key_type;
	idx_t capacity;
	idx_t count;
	AllocatedData keys;
	ArenaAllocator heap;
};

} // namespace duckdb
