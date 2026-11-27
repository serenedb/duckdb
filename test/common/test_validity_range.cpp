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
#include "duckdb/common/types/validity_range.hpp"

namespace duckdb {

namespace {

constexpr idx_t RANGE_ROWS = 3 * ValidityMask::BITS_PER_VALUE + 17;

template <class INVALID>
void CheckRanges(INVALID &&invalid) {
	ValidityMask mask(RANGE_ROWS);
	mask.Initialize(RANGE_ROWS);
	for (idx_t row = 0; row < RANGE_ROWS; row++) {
		if (invalid(row)) {
			mask.SetInvalid(row);
		}
	}
	for (idx_t from = 0; from <= RANGE_ROWS; from++) {
		idx_t valid = 0;
		for (idx_t to = from; to <= RANGE_ROWS; to++) {
			REQUIRE(CountValid(mask, from, to) == valid);
			if (to < RANGE_ROWS && !invalid(to)) {
				valid++;
			}
		}
	}
}

} // namespace

TEST_CASE("CountValid counts the valid rows of every range", "[validity]") {
	ValidityMask unallocated(RANGE_ROWS);
	REQUIRE(CountValid(unallocated, 5, RANGE_ROWS) == RANGE_ROWS - 5);

	CheckRanges([](idx_t) { return false; });
	CheckRanges([](idx_t) { return true; });
	CheckRanges([](idx_t row) { return row % 7 == 3; });
	CheckRanges([](idx_t row) { return row % 7 != 3; });
	CheckRanges([](idx_t row) { return row % ValidityMask::BITS_PER_VALUE == 63; });
	CheckRanges(
	    [](idx_t row) { return row >= ValidityMask::BITS_PER_VALUE && row < 2 * ValidityMask::BITS_PER_VALUE; });
}

} // namespace duckdb
