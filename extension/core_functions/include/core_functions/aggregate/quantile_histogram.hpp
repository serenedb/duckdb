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

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"

#include <type_traits>

namespace duckdb {

//! Selects order statistics of 8- and 16-bit integers by counting values, when there are enough of them that a
//! counting pass over the value domain beats partial sorting.
template <class T>
struct QuantileHistogram {
	static constexpr bool SUPPORTED = std::is_integral<T>::value && !std::is_same<T, bool>::value && sizeof(T) <= 2;
	static constexpr idx_t DOMAIN = idx_t(1) << (8 * sizeof(T));
	static constexpr idx_t MIN_VALUES_PER_BUCKET = 8;

	static bool Select(const T *data, idx_t n, idx_t first, idx_t second, bool desc, T result[2]) {
		if (n < DOMAIN * MIN_VALUES_PER_BUCKET) {
			return false;
		}
		using UNSIGNED = typename std::make_unsigned<T>::type;
		constexpr UNSIGNED BIAS = std::is_signed<T>::value ? UNSIGNED(UNSIGNED(1) << (8 * sizeof(T) - 1)) : 0;
		vector<idx_t> counts(DOMAIN, 0);
		for (idx_t i = 0; i < n; i++) {
			counts[UNSIGNED(UNSIGNED(data[i]) ^ BIAS)]++;
		}
		idx_t ranks[2] = {first, second};
		for (idx_t r = 0; r < 2; r++) {
			idx_t remaining = ranks[r];
			for (idx_t step = 0; step < DOMAIN; step++) {
				idx_t bucket = desc ? DOMAIN - 1 - step : step;
				if (remaining < counts[bucket]) {
					result[r] = T(UNSIGNED(UNSIGNED(bucket) ^ BIAS));
					break;
				}
				remaining -= counts[bucket];
			}
		}
		return true;
	}
};

} // namespace duckdb
