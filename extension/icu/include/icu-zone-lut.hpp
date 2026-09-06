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

#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "unicode/basictz.h"

namespace duckdb {

struct ZoneDay {
	int64_t transition = 0;
	int32_t before = 0;
	int32_t after = 0;
};

class ZoneLUT {
public:
	static constexpr int64_t NO_TRANSITION = NumericLimits<int64_t>::Maximum();
	static constexpr int64_t MULTIPLE_TRANSITIONS = NumericLimits<int64_t>::Minimum();
	static inline const int64_t FIRST_ANNO_DOMINI = DateTrunc::FromDays(DateTrunc::YearStart(1)).value;

	explicit ZoneLUT(const icu::BasicTimeZone &tz);

	static shared_ptr<const ZoneLUT> Get(const icu::TimeZone &tz);
	static shared_ptr<const ZoneLUT> Get(string &tz_name);

	bool IsValid() const {
		return valid;
	}

	bool HasFixedOffset() const {
		return fixed;
	}

	int64_t FixedOffset() const {
		return fixed_offset;
	}

	[[gnu::always_inline]] inline bool TryOffset(int64_t micros, int64_t &offset) const {
		if (fixed) {
			offset = fixed_offset;
			return micros >= offset_min_input && micros <= offset_max_input;
		}
		const auto day = InstantDay(micros);
		if (!day) {
			return false;
		}
		offset = Offset(*day, micros);
		return true;
	}

	[[gnu::always_inline]] inline bool TryResolve(int64_t wall, int64_t &instant) const {
		if (fixed) {
			instant = wall - fixed_offset;
			return wall >= resolve_min_wall && wall <= resolve_max_wall;
		}
		const auto day = WallDay(wall);
		if (!day) {
			return false;
		}
		instant = Resolve(*day, wall);
		return true;
	}

	[[gnu::always_inline]] inline bool TryShiftBack(int64_t wall, int64_t offset, int64_t &instant) const {
		instant = wall - offset;
		return !fixed || (wall >= resolve_min_wall && wall <= resolve_max_wall);
	}

	[[gnu::always_inline]] inline bool TryInstantDay(int64_t micros, int64_t &day, int64_t &offset) const {
		if (fixed) {
			offset = fixed_offset;
			return micros >= offset_min_input && micros <= offset_max_input;
		}
		day = DayIndex(micros);
		const auto entry = Entry(instants, day);
		if (!entry) {
			return false;
		}
		offset = Offset(*entry, micros);
		return true;
	}

	[[gnu::always_inline]] inline bool TryResolveDay(int64_t day, int64_t wall, int64_t &instant) const {
		if (fixed) {
			instant = wall - fixed_offset;
			return wall >= resolve_min_wall && wall <= resolve_max_wall;
		}
		const auto entry = Entry(walls, day);
		if (!entry) {
			return false;
		}
		instant = Resolve(*entry, wall);
		return true;
	}

	static inline int64_t DayStart(int64_t day) {
		return (day + DateTruncTable::FIRST_DAY) * Interval::MICROS_PER_DAY;
	}

	int64_t HourBucketFirstDay() const {
		return hour_bucket_first_day;
	}
	int64_t MinuteBucketFirstDay() const {
		return minute_bucket_first_day;
	}
	int64_t DayBucketFirstDay() const {
		return day_bucket_first_day;
	}
	[[gnu::always_inline]] inline const ZoneDay &InstantEntry(int64_t day) const {
		return instants[UnsafeNumericCast<idx_t>(day)];
	}

	[[gnu::always_inline]] inline const ZoneDay &WallEntry(int64_t day) const {
		return walls[UnsafeNumericCast<idx_t>(day)];
	}

	static bool MidnightRepeats(const ZoneDay &entry, int64_t local_day);
	bool OriginDaysSupported(int64_t origin_day, int64_t lo_day, int64_t hi_day) const;

	[[gnu::always_inline]] inline const ZoneDay *InstantDay(int64_t micros) const {
		return Find(instants, micros);
	}

	[[gnu::always_inline]] inline const ZoneDay *WallDay(int64_t wall) const {
		return Find(walls, wall);
	}

	[[gnu::always_inline]] static inline int64_t Offset(const ZoneDay &day, int64_t micros) {
		return int64_t(micros >= day.transition ? day.after : day.before) * Interval::MICROS_PER_SEC;
	}

	[[gnu::always_inline]] static inline int64_t Resolve(const ZoneDay &day, int64_t wall) {
		const int64_t after = wall - int64_t(day.after) * Interval::MICROS_PER_SEC;
		return after >= day.transition ? after : wall - int64_t(day.before) * Interval::MICROS_PER_SEC;
	}

private:
	[[gnu::always_inline]] static inline int64_t DayIndex(int64_t micros) {
		return DateTrunc::FloorDiv(micros, Interval::MICROS_PER_DAY) - DateTruncTable::FIRST_DAY;
	}

	[[gnu::always_inline]] static inline const ZoneDay *Entry(const unsafe_vector<ZoneDay> &days, int64_t day) {
		if (day < 0 || day >= DateTruncTable::DAY_COUNT) {
			return nullptr;
		}
		const auto &entry = days[UnsafeNumericCast<idx_t>(day)];
		return entry.transition == MULTIPLE_TRANSITIONS ? nullptr : &entry;
	}

	[[gnu::always_inline]] static inline const ZoneDay *Find(const unsafe_vector<ZoneDay> &days, int64_t micros) {
		return Entry(days, DayIndex(micros));
	}

	unsafe_vector<ZoneDay> instants;
	unsafe_vector<ZoneDay> walls;
	bool valid = true;
	bool fixed = false;
	int64_t fixed_offset = 0;
	int64_t offset_min_input = 0;
	int64_t offset_max_input = 0;
	int64_t resolve_min_wall = 0;
	int64_t resolve_max_wall = 0;
	int64_t hour_bucket_first_day = DateTruncTable::DAY_COUNT;
	int64_t minute_bucket_first_day = DateTruncTable::DAY_COUNT;
	int64_t day_bucket_first_day = DateTruncTable::DAY_COUNT;
	vector<int64_t> jump_days;
	vector<pair<int64_t, int32_t>> repeated_midnights;
};

} // namespace duckdb
