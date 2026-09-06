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

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "icu-constant-args.hpp"
#include "icu-origin-day.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUTimeBucketFast {
	static constexpr int64_t DEFAULT_ORIGIN_MICROS_1 = 10959 * Interval::MICROS_PER_DAY;
	static constexpr int64_t DEFAULT_ORIGIN_MICROS_2 = 10957 * Interval::MICROS_PER_DAY;
	static constexpr int64_t LAST_ARITHMETIC_MICROS = 2932896LL * Interval::MICROS_PER_DAY;

	enum class Kind : uint8_t { MICROS, DAYS, MONTHS, NONE };

	static Kind Classify(interval_t width) {
		if (width.months == 0 && width.days == 0 && width.micros > 0) {
			return Kind::MICROS;
		}
		if (width.months == 0 && width.days > 0 && width.micros == 0) {
			return Kind::DAYS;
		}
		if (width.months > 0 && width.days == 0 && width.micros == 0) {
			return Kind::MONTHS;
		}
		return Kind::NONE;
	}

	static inline bool InRange(int64_t micros) {
		return micros >= ZoneLUT::FIRST_ANNO_DOMINI && micros <= LAST_ARITHMETIC_MICROS;
	}

	struct FixedBucket {
		int64_t origin;
		int64_t width;

		[[gnu::always_inline]] inline bool operator()(int64_t ts, int64_t &bucket) const {
			if (!InRange(ts)) {
				return false;
			}
			bucket = origin + DateTrunc::FloorDiv(ts - origin, width) * width;
			return true;
		}
	};

	struct MonthBucket {
		int64_t origin_month;
		int64_t width;

		[[gnu::always_inline]] inline bool operator()(int64_t ts, int64_t &bucket) const {
			if (!InRange(ts)) {
				return false;
			}
			const auto months =
			    DateTrunc::FloorDiv(DateTrunc::MonthIndex(timestamp_t(ts)) - origin_month, width) * width + origin_month;
			bucket = DateTrunc::MonthIndexStart(months).value;
			return true;
		}
	};

	static inline int64_t FixedWidth(Kind kind, interval_t width) {
		return kind == Kind::MICROS ? width.micros : int64_t(width.days) * Interval::MICROS_PER_DAY;
	}

	static inline bool TryBucket(Kind kind, interval_t width, int64_t ts, int64_t origin, int64_t &bucket) {
		if (!InRange(origin)) {
			return false;
		}
		switch (kind) {
		case Kind::MICROS:
		case Kind::DAYS:
			return FixedBucket {origin, FixedWidth(kind, width)}(ts, bucket);
		case Kind::MONTHS:
			return MonthBucket {DateTrunc::MonthIndex(timestamp_t(origin)), width.months}(ts, bucket);
		default:
			return false;
		}
	}

	static bool TryGetConstantWidth(Vector &width_arg, interval_t &width, Kind &kind) {
		if (!ICUConstantArgs::TryGet(width_arg, width)) {
			return false;
		}
		kind = Classify(width);
		return kind != Kind::NONE;
	}

	template <class FUN>
	static bool Execute(Vector &ts_arg, Vector &result, idx_t count, FUN &&fun) {
		bool covered = true;
		UnaryExecutor::Execute<timestamp_tz_t, timestamp_tz_t>(ts_arg, result, count, [&](timestamp_tz_t ts) {
			if (!ts.IsFinite()) {
				return ts;
			}
			int64_t bucket = 0;
			if (!fun(ts.value, bucket)) {
				covered = false;
				return ts;
			}
			return timestamp_tz_t(bucket);
		});
		return covered;
	}

	static bool Dispatch(Kind kind, interval_t width, int64_t origin, Vector &ts_arg, Vector &result, idx_t count) {
		if (!InRange(origin)) {
			return false;
		}
		switch (kind) {
		case Kind::MICROS:
		case Kind::DAYS:
			return Execute(ts_arg, result, count, FixedBucket {origin, FixedWidth(kind, width)});
		case Kind::MONTHS:
			return Execute(ts_arg, result, count, MonthBucket {DateTrunc::MonthIndex(timestamp_t(origin)), width.months});
		default:
			return false;
		}
	}

	static bool TryBinary(DataChunk &args, Vector &result) {
		interval_t width;
		Kind kind;
		if (!TryGetConstantWidth(args.data[0], width, kind)) {
			return false;
		}
		const auto origin = kind == Kind::MONTHS ? DEFAULT_ORIGIN_MICROS_2 : DEFAULT_ORIGIN_MICROS_1;
		return Dispatch(kind, width, origin, args.data[1], result, args.size());
	}

	static bool TryOffset(DataChunk &args, Vector &result) {
		interval_t width;
		Kind kind;
		interval_t offset;
		if (!TryGetConstantWidth(args.data[0], width, kind) || kind == Kind::MONTHS ||
		    !ICUConstantArgs::TryGet(args.data[2], offset)) {
			return false;
		}
		if (offset.months != 0) {
			return false;
		}
		const auto shift = Interval::GetMicro(offset);
		return Execute(args.data[1], result, args.size(), [&](int64_t ts, int64_t &bucket) {
			int64_t shifted = 0;
			int64_t moved = 0;
			if (!TrySubtractOperator::Operation<int64_t, int64_t, int64_t>(ts, shift, shifted) ||
			    !TryBucket(kind, width, shifted, DEFAULT_ORIGIN_MICROS_1, bucket) ||
			    !TryAddOperator::Operation<int64_t, int64_t, int64_t>(bucket, shift, moved)) {
				return false;
			}
			bucket = moved;
			return true;
		});
	}

	static bool TryLocalRange(Vector &ts_arg, idx_t count, int64_t &min, int64_t &max) {
		UnifiedVectorFormat format;
		ts_arg.ToUnifiedFormat(count, format);
		const auto data = UnifiedVectorFormat::GetData<timestamp_tz_t>(format);
		bool any = false;
		for (idx_t i = 0; i < count; i++) {
			const auto index = format.sel->get_index(i);
			if (!format.validity.RowIsValid(index) || !data[index].IsFinite()) {
				continue;
			}
			const auto value = data[index].value;
			min = any ? MinValue(min, value) : value;
			max = any ? MaxValue(max, value) : value;
			any = true;
		}
		return any;
	}

	static bool TryMonthBuckets(const ZoneLUT &lut, int64_t width, int64_t month_origin, Vector &ts_arg, Vector &result,
	                            idx_t count) {
		int64_t min = 0;
		int64_t max = 0;
		if (!TryLocalRange(ts_arg, count, min, max)) {
			return false;
		}
		const int64_t origin_day = DateTrunc::MonthIndexStartDays(month_origin);
		int64_t lo_day = 0;
		int64_t hi_day = 0;
		if (!ICUOriginDay::TryLocalDay(lut, min, lo_day) || !ICUOriginDay::TryLocalDay(lut, max, hi_day) ||
		    !lut.OriginDaysSupported(origin_day, lo_day, hi_day)) {
			return false;
		}
		return Execute(ts_arg, result, count, [&](int64_t ts, int64_t &bucket) {
			int64_t day = 0;
			if (!ICUOriginDay::TryOriginDay(lut, ts, day)) {
				return false;
			}
			const auto first =
			    DateTrunc::FloorDiv(DateTrunc::MonthIndex(day) - month_origin, width) * width + month_origin;
			return ICUOriginDay::TryBucketStart(lut, DateTrunc::MonthIndexStartDays(first), bucket);
		});
	}

	[[gnu::always_inline]] static inline bool TryWall(const ZoneLUT &lut, int64_t micros, int64_t &offset,
	                                                  int64_t &wall) {
		if (!lut.TryOffset(micros, offset)) {
			return false;
		}
		const auto local = micros + offset;
		wall = local - DateTrunc::FloorDiv(local, Interval::MICROS_PER_DAY) * Interval::MICROS_PER_DAY;
		return true;
	}

	struct DayOrigin {
		int64_t micros = 0;
		int64_t offset = 0;
		int64_t wall = 0;
		int64_t local_day = 0;
		int64_t millis = 0;
	};

	struct DayCache {
		static constexpr idx_t SIZE = 4;
		int64_t counts[SIZE] = {0, 0, 0, 0};
		int64_t micros[SIZE] = {0, 0, 0, 0};
		idx_t filled = 0;
		idx_t next = 0;

		bool Find(int64_t count, int64_t &result) const {
			for (idx_t i = 0; i < filled; i++) {
				if (counts[i] == count) {
					result = micros[i];
					return true;
				}
			}
			return false;
		}
		void Put(int64_t count, int64_t result) {
			counts[next] = count;
			micros[next] = result;
			next = (next + 1) % SIZE;
			filled = MinValue(filled + 1, SIZE);
		}
	};

	static bool TryDayOrigin(const ZoneLUT &lut, int64_t micros, DayOrigin &origin) {
		origin.micros = micros;
		origin.millis = micros / Interval::MICROS_PER_MSEC;
		return TryWall(lut, micros, origin.offset, origin.wall) &&
		       ICUOriginDay::TryLocalDay(lut, micros, origin.local_day);
	}

	static inline bool TryCalendarAddDays(const ZoneLUT &lut, int64_t base, int64_t days, int64_t &result) {
		int64_t prev_offset = 0;
		int64_t prev_wall = 0;
		if (!TryWall(lut, base, prev_offset, prev_wall)) {
			return false;
		}
		return TryCalendarAddDays(lut, base, prev_offset, prev_wall, days, result);
	}

	static inline bool TryCalendarAddDays(const ZoneLUT &lut, int64_t base, int64_t prev_offset, int64_t prev_wall,
	                                      int64_t days, int64_t &result) {
		if (days == 0) {
			result = base;
			return true;
		}
		const int64_t t = base + days * Interval::MICROS_PER_DAY;
		int64_t offset = 0;
		int64_t wall = 0;
		if (!TryWall(lut, t, offset, wall)) {
			return false;
		}
		if (wall == prev_wall || offset == prev_offset) {
			result = t;
			return true;
		}
		int64_t adjustment = prev_offset - offset;
		adjustment = adjustment >= 0 ? adjustment % Interval::MICROS_PER_DAY
		                             : -((-adjustment) % Interval::MICROS_PER_DAY);
		if (adjustment == 0) {
			result = t;
			return true;
		}
		const int64_t adjusted = t + adjustment;
		if (!TryWall(lut, adjusted, offset, wall)) {
			return false;
		}
		result = wall != prev_wall && adjustment < 0 ? t : adjusted;
		return true;
	}

	template <class ADD>
	static inline bool SettleCount(int64_t start_ms, int64_t target_ms, int64_t estimate, ADD &&add, idx_t max_steps,
	                               int64_t &count) {
		if (start_ms == target_ms) {
			count = 0;
			return true;
		}
		auto at = [&](int64_t n, int64_t &millis) {
			int64_t micros = 0;
			if (!add(n, micros)) {
				return false;
			}
			millis = micros / Interval::MICROS_PER_MSEC;
			return true;
		};
		count = estimate;
		int64_t here = 0;
		int64_t neighbour = 0;
		for (idx_t step = 0; step < max_steps; step++) {
			if (!at(count, here)) {
				return false;
			}
			if (start_ms < target_ms) {
				if (here > target_ms) {
					count--;
					continue;
				}
				if (!at(count + 1, neighbour)) {
					return false;
				}
				if (neighbour <= target_ms) {
					count++;
					continue;
				}
			} else {
				if (here < target_ms) {
					count++;
					continue;
				}
				if (!at(count - 1, neighbour)) {
					return false;
				}
				if (neighbour >= target_ms) {
					count--;
					continue;
				}
			}
			return true;
		}
		return false;
	}

	static inline bool TryDayBucket(const ZoneLUT &lut, const DayOrigin &origin, DayCache &cache, int64_t width,
	                                int64_t ts, int64_t &bucket) {
		const int64_t target = ts / Interval::MICROS_PER_MSEC;
		const int64_t start = origin.millis;
		int64_t ts_day = 0;
		if (!ICUOriginDay::TryLocalDay(lut, ts, ts_day)) {
			return false;
		}
		auto added_micros = [&](int64_t count, int64_t &added) {
			if (cache.Find(count, added)) {
				return true;
			}
			if (!TryCalendarAddDays(lut, origin.micros, origin.offset, origin.wall, count, added)) {
				return false;
			}
			cache.Put(count, added);
			return true;
		};
		int64_t count = 0;
		if (!SettleCount(start, target, ts_day - origin.local_day, added_micros, 8, count)) {
			return false;
		}
		const int64_t result_days = (count / width) * width;
		if (!added_micros(result_days, bucket)) {
			return false;
		}
		if (ts < bucket) {
			return TryCalendarAddDays(lut, bucket, -width, bucket);
		}
		return true;
	}

	static bool TryZoned(const ZoneLUT &lut, Kind kind, interval_t width, int64_t origin, int64_t month_origin,
	                     Vector &ts_arg, Vector &result, idx_t count) {
		switch (kind) {
		case Kind::MICROS:
			return Dispatch(kind, width, origin, ts_arg, result, count);
		case Kind::DAYS: {
			DayOrigin day_origin;
			if (!TryDayOrigin(lut, origin, day_origin)) {
				return false;
			}
			DayCache cache;
			const int64_t days = width.days;
			return Execute(ts_arg, result, count, [&](int64_t ts, int64_t &bucket) {
				return TryDayBucket(lut, day_origin, cache, days, ts, bucket);
			});
		}
		case Kind::MONTHS:
			return TryMonthBuckets(lut, width.months, month_origin, ts_arg, result, count);
		default:
			return false;
		}
	}

	static bool TryZonedBuckets(DataChunk &args, Vector &result, optional_ptr<Vector> origin_arg) {
		interval_t width;
		Kind kind;
		string tz_name;
		if (!TryGetConstantWidth(args.data[0], width, kind) || !ICUConstantArgs::TryGetString(args.data[2], tz_name)) {
			return false;
		}
		auto lut = ZoneLUT::Get(tz_name);
		if (!lut) {
			return false;
		}
		int64_t origin = 0;
		int64_t month_origin = ICUOriginDay::MONTH_ORIGIN;
		if (origin_arg) {
			timestamp_tz_t given;
			if (!ICUConstantArgs::TryGet(*origin_arg, given) || !given.IsFinite() || !InRange(given.value)) {
				return false;
			}
			origin = given.value;
			if (kind == Kind::MONTHS) {
				int64_t origin_day = 0;
				if (!ICUOriginDay::TryOriginDay(*lut, origin, origin_day)) {
					return false;
				}
				month_origin = DateTrunc::MonthIndex(origin_day);
			}
		} else if (kind != Kind::MONTHS && !lut->TryResolve(DEFAULT_ORIGIN_MICROS_1, origin)) {
			return false;
		}
		return TryZoned(*lut, kind, width, origin, month_origin, args.data[1], result, args.size());
	}

	static bool TryTimeZone(DataChunk &args, Vector &result) {
		return TryZonedBuckets(args, result, nullptr);
	}

	static bool TryTimeZoneOrigin(DataChunk &args, Vector &result) {
		return TryZonedBuckets(args, result, &args.data[3]);
	}

	static bool TryOrigin(DataChunk &args, Vector &result) {
		interval_t width;
		Kind kind;
		timestamp_tz_t origin;
		if (!TryGetConstantWidth(args.data[0], width, kind) || !ICUConstantArgs::TryGet(args.data[2], origin)) {
			return false;
		}
		if (!origin.IsFinite()) {
			return false;
		}
		return Dispatch(kind, width, origin.value, args.data[1], result, args.size());
	}
};

} // namespace duckdb
