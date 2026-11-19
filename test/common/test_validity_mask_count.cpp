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
#include "duckdb/common/types/validity_mask.hpp"

namespace duckdb {

namespace {

constexpr idx_t ROWS = 3 * ValidityMask::BITS_PER_VALUE + 17;

template <class INVALID>
void CheckCounts(INVALID &&invalid) {
	ValidityMask mask(ROWS);
	mask.Initialize(ROWS);
	for (idx_t row = 0; row < ROWS; row++) {
		if (invalid(row)) {
			mask.SetInvalid(row);
		}
	}
	idx_t valid = 0;
	for (idx_t count = 0; count <= ROWS; count++) {
		REQUIRE(mask.CountValid(count) == valid);
		REQUIRE(mask.CheckAllValid(count) == (valid == count));
		REQUIRE(mask.CheckAllInvalid(count) == (valid == 0));
		if (count < ROWS && !invalid(count)) {
			valid++;
		}
	}
}

} // namespace

TEST_CASE("CountValid counts the valid rows of every prefix", "[validity]") {
	ValidityMask unallocated(ROWS);
	REQUIRE(unallocated.CountValid(ROWS) == ROWS);

	CheckCounts([](idx_t) { return false; });
	CheckCounts([](idx_t) { return true; });
	CheckCounts([](idx_t row) { return row % 7 == 3; });
	CheckCounts([](idx_t row) { return row % 7 != 3; });
	CheckCounts([](idx_t row) { return row < ValidityMask::BITS_PER_VALUE; });
	CheckCounts([](idx_t row) { return row / ValidityMask::BITS_PER_VALUE == 1; });
	CheckCounts([](idx_t row) { return row + 1 == ROWS; });
	CheckCounts(
	    [](idx_t row) { return row == 0 || row % ValidityMask::BITS_PER_VALUE == ValidityMask::BITS_PER_VALUE - 1; });
}

} // namespace duckdb
