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

#include "duckdb/common/types/data_chunk.hpp"

#include <cstring>

namespace duckdb {

template <class T, class OP>
bool TryLeastGreatestWithoutNulls(DataChunk &args, Vector &result) {
	const idx_t count = args.size();
	bool any_flat = false;
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		auto &column = args.data[col_idx];
		if (column.GetVectorType() == VectorType::FLAT_VECTOR) {
			if (!FlatVector::Validity(column).CheckAllValid(count)) {
				return false;
			}
			any_flat = true;
		} else if (column.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(column)) {
			return false;
		}
	}
	if (!any_flat) {
		return false;
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	FlatVector::ValidityMutable(result).Reset();
	auto out = FlatVector::GetDataMutable<T>(result);
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		auto &column = args.data[col_idx];
		if (column.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			const T value = ConstantVector::GetData<T>(column)[0];
			if (col_idx == 0) {
				for (idx_t i = 0; i < count; i++) {
					out[i] = value;
				}
				continue;
			}
			for (idx_t i = 0; i < count; i++) {
				out[i] = OP::template Operation<T>(value, out[i]) ? value : out[i];
			}
			continue;
		}
		auto input = FlatVector::GetData<T>(column);
		if (col_idx == 0) {
			memcpy(out, input, count * sizeof(T));
			continue;
		}
		for (idx_t i = 0; i < count; i++) {
			out[i] = OP::template Operation<T>(input[i], out[i]) ? input[i] : out[i];
		}
	}
	return true;
}

} // namespace duckdb
