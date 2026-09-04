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

#include "include/icu-zone-lut.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "icu-helpers.hpp"
#include "unicode/tzrule.h"
#include "unicode/tztrans.h"

namespace duckdb {

namespace {

constexpr int64_t MSECS_PER_DAY = Interval::MICROS_PER_DAY / Interval::MICROS_PER_MSEC;
constexpr int32_t MSECS_PER_SEC = Interval::MICROS_PER_SEC / Interval::MICROS_PER_MSEC;

bool OffsetSeconds(int32_t millis, int32_t &seconds) {
	seconds = millis / MSECS_PER_SEC;
	return seconds * MSECS_PER_SEC == millis;
}

int64_t DayOf(int64_t micros) {
	return DateTrunc::FloorDiv(micros, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
}

void Fill(unsafe_vector<ZoneDay> &days, int64_t &next, int64_t end, int32_t offset) {
	const auto limit = MinValue<int64_t>(end, ZoneLUT::DAY_COUNT);
	for (; next < limit; next++) {
		days[UnsafeNumericCast<idx_t>(next)] = {ZoneLUT::NO_TRANSITION, offset, offset};
	}
}

void Attach(unsafe_vector<ZoneDay> &days, int64_t &next, int64_t first, int64_t last, const ZoneDay &transition) {
	Fill(days, next, first, transition.before);
	for (auto day = MaxValue<int64_t>(first, 0); day <= last && day < ZoneLUT::DAY_COUNT; day++) {
		auto &entry = days[UnsafeNumericCast<idx_t>(day)];
		if (day < next) {
			entry.transition = ZoneLUT::MULTIPLE_TRANSITIONS;
		} else {
			entry = transition;
		}
	}
	next = MaxValue<int64_t>(next, MinValue<int64_t>(last + 1, ZoneLUT::DAY_COUNT));
}

} // namespace

ZoneLUT::ZoneLUT(const icu::BasicTimeZone &tz) {
	D_ASSERT(Date::FromDate(FIRST_YEAR, 1, 1).days == FIRST_DAY);
	UErrorCode status = U_ZERO_ERROR;
	const auto rule_count = tz.countTransitionRules(status);
	if (U_FAILURE(status)) {
		valid = false;
		return;
	}

	int32_t raw_offset = 0;
	int32_t dst_offset = 0;
	if (rule_count == 0) {
		tz.getOffset(0, false, raw_offset, dst_offset, status);
		if (U_FAILURE(status)) {
			valid = false;
			return;
		}
		fixed = true;
		fixed_offset = int64_t(raw_offset + dst_offset) * Interval::MICROS_PER_MSEC;
		const auto limit = NumericLimits<int64_t>::Maximum() - 1;
		offset_min_input = -limit - MinValue<int64_t>(fixed_offset, 0);
		offset_max_input = limit - MaxValue<int64_t>(fixed_offset, 0);
		resolve_min_wall = -limit + MaxValue<int64_t>(fixed_offset, 0);
		resolve_max_wall = limit + MinValue<int64_t>(fixed_offset, 0);
		hour_bucket_first_day = fixed_offset % Interval::MICROS_PER_HOUR == 0 ? 0 : DAY_COUNT;
		minute_bucket_first_day = fixed_offset % Interval::MICROS_PER_MINUTE == 0 ? 0 : DAY_COUNT;
		day_bucket_first_day = 0;
		return;
	}

	const int64_t begin_ms = (int64_t(FIRST_DAY) - 2) * MSECS_PER_DAY;
	const int64_t end_ms = (int64_t(FIRST_DAY) + DAY_COUNT + 2) * MSECS_PER_DAY;
	tz.getOffset(UDate(begin_ms), false, raw_offset, dst_offset, status);
	int32_t current = 0;
	if (U_FAILURE(status) || !OffsetSeconds(raw_offset + dst_offset, current)) {
		valid = false;
		return;
	}

	instants.resize(UnsafeNumericCast<idx_t>(DAY_COUNT));
	walls.resize(UnsafeNumericCast<idx_t>(DAY_COUNT));
	int64_t next_instant = 0;
	int64_t next_wall = 0;
	icu::TimeZoneTransition transition;
	UDate base = UDate(begin_ms);
	UBool inclusive = true;
	while (tz.getNextTransition(base, inclusive, transition)) {
		const auto millis = int64_t(transition.getTime());
		if (millis >= end_ms) {
			break;
		}
		int32_t after = 0;
		if (!OffsetSeconds(transition.getTo()->getRawOffset() + transition.getTo()->getDSTSavings(), after)) {
			valid = false;
			return;
		}
		const int64_t micros = millis * Interval::MICROS_PER_MSEC;
		const ZoneDay day {micros, current, after};
		const auto instant_day = DayOf(micros);
		Attach(instants, next_instant, instant_day, instant_day, day);
		const auto wall_before = DayOf(micros + int64_t(current) * Interval::MICROS_PER_SEC);
		const auto wall_after = DayOf(micros + int64_t(after) * Interval::MICROS_PER_SEC);
		Attach(walls, next_wall, MinValue(wall_before, wall_after), MaxValue(wall_before, wall_after), day);
		current = after;
		base = UDate(millis);
		inclusive = false;
	}
	Fill(instants, next_instant, DAY_COUNT, current);
	Fill(walls, next_wall, DAY_COUNT, current);
	hour_bucket_first_day = 0;
	minute_bucket_first_day = 0;
	day_bucket_first_day = 0;
	for (int64_t day = 0; day < DAY_COUNT; day++) {
		const auto &entry = instants[UnsafeNumericCast<idx_t>(day)];
		const auto &wall = walls[UnsafeNumericCast<idx_t>(day)];
		const bool multiple = entry.transition == MULTIPLE_TRANSITIONS || wall.transition == MULTIPLE_TRANSITIONS;
		const bool whole_hours = entry.before % Interval::SECS_PER_HOUR == 0 && entry.after % Interval::SECS_PER_HOUR == 0 &&
		                         (entry.transition == NO_TRANSITION || entry.transition % Interval::MICROS_PER_HOUR == 0);
		const bool whole_minutes =
		    entry.before % Interval::SECS_PER_MINUTE == 0 && entry.after % Interval::SECS_PER_MINUTE == 0;
		if (multiple) {
			day_bucket_first_day = day + 1;
		}
		if (multiple || !whole_hours) {
			hour_bucket_first_day = day + 1;
		}
		if (multiple || !whole_minutes) {
			minute_bucket_first_day = day + 1;
		}
		if (multiple || (wall.transition != NO_TRANSITION &&
		                 AbsValue(int64_t(wall.before) - int64_t(wall.after)) >= Interval::SECS_PER_DAY)) {
			jump_days.push_back(day);
		} else if (MidnightRepeats(wall, day + FIRST_DAY)) {
			repeated_midnights.emplace_back(day, wall.after);
		}
	}
}

bool ZoneLUT::MidnightRepeats(const ZoneDay &entry, int64_t local_day) {
	if (entry.transition == NO_TRANSITION || entry.transition == MULTIPLE_TRANSITIONS || entry.before <= entry.after) {
		return false;
	}
	const int64_t midnight = local_day * Interval::MICROS_PER_DAY;
	return midnight >= entry.transition + int64_t(entry.after) * Interval::MICROS_PER_SEC &&
	       midnight < entry.transition + int64_t(entry.before) * Interval::MICROS_PER_SEC;
}

bool ZoneLUT::OriginDaysSupported(int64_t origin_day, int64_t lo_day, int64_t hi_day) const {
	if (fixed) {
		return true;
	}
	const auto origin_index = origin_day - FIRST_DAY;
	if (origin_index < 0 || origin_index >= DAY_COUNT) {
		return false;
	}
	int64_t origin_offset = 0;
	if (!TryOffset(Resolve(walls[UnsafeNumericCast<idx_t>(origin_index)], origin_day * Interval::MICROS_PER_DAY),
	               origin_offset)) {
		return false;
	}
	const auto lo = MinValue(MinValue(lo_day, hi_day), origin_day) - 1 - FIRST_DAY;
	const auto hi = MaxValue(MaxValue(lo_day, hi_day), origin_day) + 1 - FIRST_DAY;
	auto jump = std::lower_bound(jump_days.begin(), jump_days.end(), lo);
	if (jump != jump_days.end() && *jump <= hi) {
		return false;
	}
	auto repeated = std::lower_bound(repeated_midnights.begin(), repeated_midnights.end(), lo,
	                                 [](const pair<int64_t, int32_t> &entry, int64_t day) { return entry.first < day; });
	for (; repeated != repeated_midnights.end() && repeated->first <= hi; repeated++) {
		if (int64_t(repeated->second) * Interval::MICROS_PER_SEC != origin_offset) {
			return false;
		}
	}
	return true;
}

shared_ptr<const ZoneLUT> ZoneLUT::Get(const icu::TimeZone &tz) {
	struct CacheEntry {
		shared_ptr<const ZoneLUT> lut;
		uint64_t last_use;
	};
	static constexpr idx_t CACHE_LIMIT = 16;
	static mutex lock;
	static unordered_map<string, CacheEntry> cache;
	static uint64_t clock = 0;

	icu::UnicodeString id;
	tz.getID(id);
	string key;
	id.toUTF8String(key);

	lock_guard<mutex> guard(lock);
	auto entry = cache.find(key);
	if (entry != cache.end()) {
		entry->second.last_use = ++clock;
		return entry->second.lut;
	}
	shared_ptr<const ZoneLUT> lut;
	if (auto basic = dynamic_cast<const icu::BasicTimeZone *>(&tz)) {
		auto built = make_shared_ptr<ZoneLUT>(*basic);
		if (built->IsValid()) {
			lut = std::move(built);
		}
	}
	if (cache.size() >= CACHE_LIMIT) {
		auto victim = cache.begin();
		for (auto it = cache.begin(); it != cache.end(); ++it) {
			if (it->second.last_use < victim->second.last_use) {
				victim = it;
			}
		}
		cache.erase(victim);
	}
	cache.emplace(key, CacheEntry {lut, ++clock});
	return lut;
}

shared_ptr<const ZoneLUT> ZoneLUT::Get(string &tz_name) {
	auto tz = ICUHelpers::TryGetTimeZone(tz_name);
	return tz ? Get(*tz) : nullptr;
}

} // namespace duckdb
