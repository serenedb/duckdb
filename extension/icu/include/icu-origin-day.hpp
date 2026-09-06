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

#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUOriginDay {
	static constexpr int64_t DAY_ORIGIN = 10959;
	static constexpr int64_t MONTH_ORIGIN = 2000 * 12;

	[[gnu::always_inline]] static inline bool TryLocalDay(const ZoneLUT &lut, int64_t micros, int64_t &local_day) {
		int64_t day = 0;
		int64_t offset = 0;
		if (!lut.TryInstantDay(micros, day, offset)) {
			return false;
		}
		local_day = DateTrunc::FloorDiv(micros + offset, Interval::MICROS_PER_DAY);
		return true;
	}

	[[gnu::always_inline]] static inline bool TryOriginDay(const ZoneLUT &lut, int64_t micros, int64_t &day) {
		if (!TryLocalDay(lut, micros, day)) {
			return false;
		}
		if (lut.HasFixedOffset()) {
			return true;
		}
		const auto index = day - ZoneLUT::FIRST_DAY;
		if (index < 0 || index >= ZoneLUT::DAY_COUNT) {
			return false;
		}
		const auto &entry = lut.WallEntry(index);
		if (ZoneLUT::MidnightRepeats(entry, day) &&
		    micros < day * Interval::MICROS_PER_DAY - int64_t(entry.after) * Interval::MICROS_PER_SEC) {
			day--;
		}
		return true;
	}

	[[gnu::always_inline]] static inline bool TryBucketStart(const ZoneLUT &lut, int64_t first_day, int64_t &start) {
		return lut.TryResolveDay(first_day - ZoneLUT::FIRST_DAY, first_day * Interval::MICROS_PER_DAY, start);
	}
};

} // namespace duckdb
