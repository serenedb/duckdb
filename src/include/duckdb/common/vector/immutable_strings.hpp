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

#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/vector_buffer.hpp"

namespace duckdb {

struct ImmutableStrings {
	DUCKDB_API static bool Certified(const AuxiliaryDataSet &set);
	DUCKDB_API static unique_ptr<AuxiliaryDataHolder> Reference(buffer_ptr<AuxiliaryDataSet> set);
	DUCKDB_API static void Copy(const Vector &source, Vector &target, idx_t source_count, idx_t source_offset,
	                            idx_t target_offset);
	DUCKDB_API static void Copy(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
	                            idx_t source_offset, idx_t target_offset);
	DUCKDB_API static void Append(Vector &list, const Vector &source, idx_t count);
};

} // namespace duckdb
