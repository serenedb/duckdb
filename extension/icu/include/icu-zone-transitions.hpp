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
#include "duckdb/main/client_context.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUZoneTransitions {
	static bool Collect(ClientContext &context, int64_t from, int64_t to, vector<int64_t> &transitions) {
		Value tz_value;
		if (!context.TryGetCurrentSetting("TimeZone", tz_value) || tz_value.IsNull()) {
			return false;
		}
		string tz_name = tz_value.ToString();
		auto lut = ZoneLUT::Get(tz_name);
		if (!lut) {
			return false;
		}
		if (lut->HasFixedOffset()) {
			return true;
		}
		const int64_t first = DateTrunc::FloorDiv(from, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
		const int64_t last = DateTrunc::FloorDiv(to, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
		if (first < 0 || last >= ZoneLUT::DAY_COUNT) {
			return false;
		}
		for (int64_t day = first; day <= last; day++) {
			const auto &entry = lut->InstantEntry(day);
			if (entry.transition == ZoneLUT::MULTIPLE_TRANSITIONS) {
				return false;
			}
			if (entry.transition != ZoneLUT::NO_TRANSITION) {
				transitions.push_back(entry.transition);
			}
		}
		return true;
	}
};

} // namespace duckdb
