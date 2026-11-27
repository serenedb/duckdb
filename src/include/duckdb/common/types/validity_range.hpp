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

#include "duckdb/common/types/validity_mask.hpp"

#include <bit>

namespace duckdb {

//! The number of valid rows of the mask in [from, to).
inline idx_t CountValid(const ValidityMask &mask, idx_t from, idx_t to) {
	D_ASSERT(from <= to);
	if (mask.CannotHaveNull() || from == to) {
		return to - from;
	}
	constexpr idx_t BITS = ValidityMask::BITS_PER_VALUE;
	constexpr auto ALL = ValidityMask::ValidityBuffer::MAX_ENTRY;
	const auto *data = mask.GetData();
	auto entry = from / BITS;
	const auto last = (to - 1) / BITS;
	auto word = data[entry] & (ALL << (from % BITS));
	idx_t valid = 0;
	while (entry != last) {
		valid += static_cast<idx_t>(std::popcount(word));
		word = data[++entry];
	}
	if (const auto ragged = to % BITS; ragged != 0) {
		word &= ALL >> (BITS - ragged);
	}
	return valid + static_cast<idx_t>(std::popcount(word));
}

} // namespace duckdb
