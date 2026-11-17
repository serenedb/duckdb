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

#include "catch.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

namespace {

template <class T>
T ValueAt(idx_t row) {
	return T(static_cast<int64_t>(row * 7 + 3));
}

template <class T>
void CheckFixedSizeCopy(const LogicalType &type) {
	constexpr idx_t COUNT = 300;
	constexpr idx_t SOURCE_OFFSET = 17;
	constexpr idx_t TARGET_OFFSET = 5;
	Vector source(type);
	{
		auto writer = FlatVector::Writer<T>(source, COUNT);
		for (idx_t i = 0; i < COUNT; i++) {
			writer.WriteValue(ValueAt<T>(i));
		}
	}

	Vector incremental(type);
	VectorOperations::Copy(source, incremental, COUNT, SOURCE_OFFSET, TARGET_OFFSET);
	const auto incremental_data = FlatVector::GetData<T>(incremental);
	for (idx_t i = 0; i < COUNT - SOURCE_OFFSET; i++) {
		REQUIRE(incremental_data[TARGET_OFFSET + i] == ValueAt<T>(SOURCE_OFFSET + i));
	}

	SelectionVector reversed(COUNT);
	for (idx_t i = 0; i < COUNT; i++) {
		reversed.set_index(i, COUNT - 1 - i);
	}
	Vector selected(type);
	VectorOperations::Copy(source, selected, reversed, COUNT, SOURCE_OFFSET, TARGET_OFFSET);
	const auto selected_data = FlatVector::GetData<T>(selected);
	for (idx_t i = 0; i < COUNT - SOURCE_OFFSET; i++) {
		REQUIRE(selected_data[TARGET_OFFSET + i] == ValueAt<T>(COUNT - 1 - SOURCE_OFFSET - i));
	}
}

} // namespace

TEST_CASE("Fixed-size vector copies with and without a selection", "[vector]") {
	CheckFixedSizeCopy<int8_t>(LogicalType::TINYINT);
	CheckFixedSizeCopy<int16_t>(LogicalType::SMALLINT);
	CheckFixedSizeCopy<int32_t>(LogicalType::INTEGER);
	CheckFixedSizeCopy<int64_t>(LogicalType::BIGINT);
	CheckFixedSizeCopy<hugeint_t>(LogicalType::HUGEINT);
}

} // namespace duckdb
