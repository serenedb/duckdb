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

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "icu-datefunc.hpp"
#include "icu-origin-day.hpp"
#include "icu-timebucket-fast.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUScalarFast {
	static constexpr int64_t MAX_DAYS = 1000000;
	static constexpr int64_t MAX_MONTHS = 100000;
	static constexpr idx_t SEARCH_STEPS = 16;
	static constexpr int64_t MSECS_PER_HOUR = Interval::MSECS_PER_SEC * Interval::SECS_PER_HOUR;

	static inline int64_t TargetMillis(int64_t micros) {
		return micros / Interval::MICROS_PER_MSEC;
	}

	static inline int64_t StartMillis(int64_t micros) {
		return DateTrunc::FloorDiv(micros, Interval::MICROS_PER_MSEC);
	}

	static inline bool TryLocal(const ZoneLUT &lut, int64_t micros, int64_t &offset, int64_t &local) {
		if (!lut.TryOffset(micros, offset)) {
			return false;
		}
		local = micros + offset;
		return true;
	}

	static inline bool Usable(const ICUDateFunc::BindData &info) {
		return info.lut && info.lut->IsValid();
	}

	static inline bool TryLocalDate(const ZoneLUT &lut, int64_t micros, int32_t &year, int32_t &month, int32_t &day,
	                                int64_t &time) {
		int64_t offset = 0;
		int64_t local = 0;
		if (!TryLocal(lut, micros, offset, local)) {
			return false;
		}
		date_t date;
		dtime_t time_of_day;
		Timestamp::Convert(timestamp_t(local), date, time_of_day);
		Date::Convert(date, year, month, day);
		time = time_of_day.micros;
		return true;
	}

	static inline bool TryAddMonths(const ZoneLUT &lut, int64_t base, int64_t months, int64_t &result) {
		if (months == 0) {
			result = base;
			return true;
		}
		if (months < -MAX_MONTHS || months > MAX_MONTHS) {
			return false;
		}
		int32_t year, month, day_of_month;
		int64_t time = 0;
		if (!TryLocalDate(lut, base, year, month, day_of_month, time)) {
			return false;
		}
		const int64_t index = int64_t(year) * 12 + (month - 1) + months;
		const int64_t new_year = DateTrunc::FloorDiv<int64_t>(index, 12);
		const auto new_month = int32_t(index - new_year * 12) + 1;
		if (new_year < 1 || new_year > 9999) {
			return false;
		}
		const auto new_day = MinValue<int32_t>(day_of_month, Date::MonthDays(int32_t(new_year), new_month));
		const auto start = Date::FromDate(int32_t(new_year), new_month, new_day);
		return lut.TryResolve(int64_t(start.days) * Interval::MICROS_PER_DAY + time, result);
	}

	static inline bool TryAddDays(const ZoneLUT &lut, int64_t base, int64_t days, int64_t &result) {
		if (days == 0) {
			result = base;
			return true;
		}
		if (days < -MAX_DAYS || days > MAX_DAYS) {
			return false;
		}
		return ICUTimeBucketFast::TryCalendarAddDays(lut, base, days, result);
	}

	static inline bool TryAddInterval(const ZoneLUT &lut, timestamp_tz_t ts, interval_t interval, int64_t &out) {
		if (!ts.IsFinite()) {
			out = ts.value;
			return true;
		}
		int64_t millis = ts.value / Interval::MICROS_PER_MSEC;
		int64_t micros = ts.value % Interval::MICROS_PER_MSEC;
		micros += interval.micros % Interval::MICROS_PER_MSEC;
		if (micros >= Interval::MICROS_PER_MSEC) {
			micros -= Interval::MICROS_PER_MSEC;
			++millis;
		} else if (micros < 0) {
			micros += Interval::MICROS_PER_MSEC;
			--millis;
		}
		int64_t t = 0;
		if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(millis, Interval::MICROS_PER_MSEC, t)) {
			return false;
		}
		const int64_t elapsed = (interval.micros / Interval::MICROS_PER_MSEC) * Interval::MICROS_PER_MSEC;
		const bool negative = interval.months < 0 || interval.days < 0 || interval.micros < 0;
		if (negative) {
			if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(t, elapsed, t)) {
				return false;
			}
			if (!TryAddMonths(lut, t, interval.months, t) || !TryAddDays(lut, t, interval.days, t)) {
				return false;
			}
		} else {
			if (!TryAddMonths(lut, t, interval.months, t) || !TryAddDays(lut, t, interval.days, t)) {
				return false;
			}
			if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(t, elapsed, t)) {
				return false;
			}
		}
		int64_t offset = 0;
		if (!lut.TryOffset(t, offset)) {
			return false;
		}
		out = t + micros;
		return true;
	}

	enum class Operation : uint8_t { ADD, SUBTRACT, AGE };

	static inline bool TryComponents(const ZoneLUT &lut, int64_t micros, TimestampComponents &out) {
		int64_t offset = 0;
		int64_t local = 0;
		if (!TryLocal(lut, micros, offset, local)) {
			return false;
		}
		out = Timestamp::GetComponents(timestamp_t(local));
		return true;
	}

	static inline bool TryAge(const ZoneLUT &lut, timestamp_tz_t end, timestamp_tz_t start, interval_t &out) {
		if (!end.IsFinite() || !start.IsFinite()) {
			return false;
		}
		TimestampComponents end_data, start_data;
		if (!TryComponents(lut, end.value, end_data) || !TryComponents(lut, start.value, start_data)) {
			return false;
		}
		out = Interval::GetAge(end_data, start_data, start > end);
		return true;
	}

	static inline bool TryDaySteps(const ZoneLUT &lut, int64_t origin, int64_t end, int64_t unit, int64_t &count) {
		ICUTimeBucketFast::DayOrigin day_origin;
		int64_t end_day = 0;
		if (!ICUTimeBucketFast::TryDayOrigin(lut, origin, day_origin) || !ICUOriginDay::TryLocalDay(lut, end, end_day)) {
			return false;
		}
		auto add = [&](int64_t n, int64_t &added) {
			return ICUTimeBucketFast::TryCalendarAddDays(lut, day_origin.micros, day_origin.offset, day_origin.wall,
			                                             n * unit, added);
		};
		return ICUTimeBucketFast::SettleCount(day_origin.millis, TargetMillis(end),
		                                      (end_day - day_origin.local_day) / unit, add, SEARCH_STEPS, count);
	}

	static inline bool TryDayCount(const ZoneLUT &lut, int64_t origin, int64_t end, int64_t &count, int64_t &added) {
		return TryDaySteps(lut, origin, end, 1, count) && ICUTimeBucketFast::TryCalendarAddDays(lut, origin, count, added);
	}

	static inline bool TrySubtract(const ZoneLUT &lut, timestamp_tz_t end, timestamp_tz_t start, interval_t &out) {
		if (!end.IsFinite() || !start.IsFinite()) {
			return false;
		}
		if (start > end) {
			interval_t negated;
			if (!TrySubtract(lut, start, end, negated)) {
				return false;
			}
			out = Interval::Invert(negated);
			return true;
		}
		if (end.value < 0 && end.value % Interval::MICROS_PER_MSEC != 0) {
			return false;
		}
		const int64_t start_ms = StartMillis(start.value);
		const uint64_t start_micros = uint64_t(start.value - start_ms * Interval::MICROS_PER_MSEC);
		int64_t end_value = end.value;
		uint64_t end_micros = uint64_t(end_value % Interval::MICROS_PER_MSEC);
		if (start_micros > end_micros) {
			end_value -= Interval::MICROS_PER_MSEC;
			end_micros += Interval::MICROS_PER_MSEC;
		}
		int64_t days = 0;
		int64_t added = 0;
		if (!TryDayCount(lut, start_ms * Interval::MICROS_PER_MSEC, end_value, days, added)) {
			return false;
		}
		const int64_t remainder = TargetMillis(end_value) - added / Interval::MICROS_PER_MSEC;
		if (remainder < 0) {
			return false;
		}
		out.months = 0;
		out.days = NumericCast<int32_t>(days);
		out.micros = remainder * Interval::MICROS_PER_MSEC + int64_t(end_micros - start_micros);
		return true;
	}

	template <class TA, class TB>
	struct IntervalArithmetic {
		template <class FALLBACK>
		static bool Try(DataChunk &, Vector &, const ICUDateFunc::BindData &, Operation, FALLBACK &&) {
			return false;
		}
	};

	template <class A, class B, class R, class ROW, class FALLBACK>
	static bool ZonedBinary(DataChunk &args, Vector &result, const ICUDateFunc::BindData &info, ROW &&row,
	                        FALLBACK &&fallback) {
		if (!Usable(info)) {
			return false;
		}
		const auto &lut = *info.lut;
		BinaryExecutor::Execute<A, B, R>(args.data[0], args.data[1], result, args.size(), [&](A a, B b) {
			R out;
			return row(lut, a, b, out) ? out : fallback(a, b);
		});
		return true;
	}

	static inline void Components(date_t date, dtime_t time, int64_t offset, int32_t (&data)[8]) {
		Date::Convert(date, data[0], data[1], data[2]);
		Time::Convert(time, data[3], data[4], data[5], data[6]);
		data[7] = int32_t(offset / Interval::MICROS_PER_SEC);
	}

	template <class WRITE>
	static inline void Emit(Vector &result, idx_t length, string_t &out, WRITE &&write) {
		out = StringVector::EmptyString(result, length);
		write(out.GetDataWriteable());
		out.Finalize();
	}

	static inline bool FormatRow(const ZoneLUT &lut, timestamp_tz_t input, StrfTimeFormat &format, const char *tz_name,
	                             Vector &result, string_t &out) {
		int64_t offset = 0;
		if (!lut.TryOffset(input.value, offset)) {
			return false;
		}
		date_t date;
		dtime_t time;
		Timestamp::Convert(timestamp_t(input.value + offset), date, time);
		int32_t data[8];
		Components(date, time, offset, data);
		Emit(result, format.GetLength(date, time, data[7], tz_name), out,
		     [&](char *target) { format.FormatString(date, data, tz_name, target); });
		return true;
	}

	static inline bool FormatRow(const ZoneLUT &lut, timestamp_tz_ns_t input, StrfTimeFormat &format,
	                             const char *tz_name, Vector &result, string_t &out) {
		int64_t offset = 0;
		if (!lut.TryOffset(DateTrunc::FloorDiv(input.value, Interval::NANOS_PER_MICRO), offset)) {
			return false;
		}
		date_t date;
		dtime_t time;
		int32_t nanos = 0;
		Timestamp::Convert(timestamp_ns_t(input.value + offset * Interval::NANOS_PER_MICRO), date, time, nanos);
		int32_t data[8];
		Components(date, time, offset, data);
		data[6] = UnsafeNumericCast<int32_t>(int64_t(data[6]) * Interval::NANOS_PER_MICRO + nanos);
		Emit(result, format.GetLength(date, data, tz_name), out,
		     [&](char *target) { format.FormatStringNS(date, data, tz_name, target); });
		return true;
	}

	template <class T, class FALLBACK>
	static inline string_t FormatOrFallback(const ZoneLUT &lut, T input, StrfTimeFormat &format, const char *tz_name,
	                                        Vector &result, FALLBACK &&fallback) {
		if (!input.IsFinite()) {
			return StringVector::AddString(result, Date::ToInfinity(input));
		}
		string_t out;
		if (FormatRow(lut, input, format, tz_name, result, out)) {
			return out;
		}
		return fallback();
	}

	template <class T>
	static bool ZonedSource(const Vector &source) {
		const auto expected =
		    std::is_same<T, timestamp_tz_ns_t>::value ? LogicalTypeId::TIMESTAMP_TZ_NS : LogicalTypeId::TIMESTAMP_TZ;
		return source.GetType().id() == expected;
	}

	template <class T, class FALLBACK>
	static bool TryStrftime(const ICUDateFunc::BindData &info, const Vector &source, idx_t count,
	                        StrfTimeFormat &format, const char *tz_name, Vector &result, FALLBACK &&fallback) {
		if (!ZonedSource<T>(source) || !Usable(info)) {
			return false;
		}
		const auto &lut = *info.lut;
		UnaryExecutor::Execute<T, string_t>(source, result, count, [&](T input) {
			return FormatOrFallback(lut, input, format, tz_name, result, [&] { return fallback(input); });
		});
		return true;
	}

	template <class T, class FALLBACK>
	static bool TryStrftimeDynamic(const ICUDateFunc::BindData &info, const Vector &source, const Vector &formats,
	                               idx_t count, const char *tz_name, Vector &result, FALLBACK &&fallback) {
		if (!ZonedSource<T>(source) || !Usable(info)) {
			return false;
		}
		const auto &lut = *info.lut;
		unordered_map<string, unique_ptr<StrfTimeFormat>> cache;
		string_t last_spec;
		optional_ptr<StrfTimeFormat> last_format;
		BinaryExecutor::Execute<T, string_t, string_t>(source, formats, result, count, [&](T input, string_t spec) {
			if (!input.IsFinite()) {
				return StringVector::AddString(result, Date::ToInfinity(input));
			}
			if (!last_format || spec != last_spec) {
				const auto key = spec.GetString();
				auto entry = cache.find(key);
				if (entry == cache.end()) {
					auto format = make_uniq<StrfTimeFormat>();
					if (!StrTimeFormat::ParseFormatSpecifier(key, *format).empty()) {
						return fallback(input, spec);
					}
					entry = cache.emplace(key, std::move(format)).first;
				}
				last_spec = spec;
				last_format = entry->second.get();
			}
			return FormatOrFallback(lut, input, *last_format, tz_name, result, [&] { return fallback(input, spec); });
		});
		return true;
	}

	static inline bool TryMonthIndex(const ZoneLUT &lut, int64_t micros, int64_t &index) {
		int64_t offset = 0;
		int64_t local = 0;
		if (!TryLocal(lut, micros, offset, local)) {
			return false;
		}
		index = DateTrunc::MonthIndex(timestamp_t(local));
		return true;
	}

	static inline bool TrySub(const ZoneLUT &lut, DatePartSpecifier part, int64_t start, int64_t end, int64_t &value) {
		const int64_t start_ms = StartMillis(start);
		const int64_t target_ms = TargetMillis(end);
		const int64_t origin = start_ms * Interval::MICROS_PER_MSEC;
		switch (part) {
		case DatePartSpecifier::HOUR:
			value = (target_ms - start_ms) / MSECS_PER_HOUR;
			return true;
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::WEEK:
			return TryDaySteps(lut, origin, end, part == DatePartSpecifier::DAY ? 1 : 7, value);
		case DatePartSpecifier::MONTH:
		case DatePartSpecifier::QUARTER:
		case DatePartSpecifier::YEAR:
		case DatePartSpecifier::DECADE:
		case DatePartSpecifier::CENTURY:
		case DatePartSpecifier::MILLENNIUM: {
			const int64_t unit = part == DatePartSpecifier::MONTH || part == DatePartSpecifier::QUARTER ? 1 : 12;
			int64_t origin_index = 0;
			int64_t end_index = 0;
			if (!TryMonthIndex(lut, origin, origin_index) || !TryMonthIndex(lut, end, end_index)) {
				return false;
			}
			auto add = [&](int64_t count, int64_t &added) { return TryAddMonths(lut, origin, count * unit, added); };
			if (!ICUTimeBucketFast::SettleCount(start_ms, target_ms, (end_index - origin_index) / unit, add,
			                                    SEARCH_STEPS, value)) {
				return false;
			}
			value /= YearDivisor(part);
			return true;
		}
		default:
			return false;
		}
	}

	static inline int64_t YearDivisor(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::QUARTER:
			return 3;
		case DatePartSpecifier::DECADE:
			return 10;
		case DatePartSpecifier::CENTURY:
			return 100;
		case DatePartSpecifier::MILLENNIUM:
			return 1000;
		default:
			return 1;
		}
	}

	template <class ADJUST>
	static inline bool TryResolveAdjusted(const ZoneLUT &lut, int64_t start, ADJUST &&adjust, int64_t &out) {
		int32_t y, m, d;
		int64_t time = 0;
		if (!TryLocalDate(lut, start, y, m, d, time) || !adjust(y, m)) {
			return false;
		}
		return lut.TryResolve(int64_t(Date::FromDate(y, m, d).days) * Interval::MICROS_PER_DAY + time, out);
	}

	static inline bool TryTruncYears(const ZoneLUT &lut, int64_t micros, int64_t multiple, int64_t &out) {
		int64_t year_start = 0;
		if (!TryTruncMonth(lut, micros, true, year_start)) {
			return false;
		}
		auto floor_year = [&](int32_t &y, int32_t &) {
			if (y < 1) {
				return false;
			}
			y = int32_t((int64_t(y) / multiple) * multiple);
			return y >= 1;
		};
		return TryResolveAdjusted(lut, year_start, floor_year, out);
	}

	static inline bool SupportedPart(DatePartSpecifier part, bool truncate) {
		switch (part) {
		case DatePartSpecifier::HOUR:
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::WEEK:
		case DatePartSpecifier::MONTH:
		case DatePartSpecifier::QUARTER:
		case DatePartSpecifier::YEAR:
		case DatePartSpecifier::DECADE:
		case DatePartSpecifier::CENTURY:
		case DatePartSpecifier::MILLENNIUM:
			return true;
		case DatePartSpecifier::MICROSECONDS:
		case DatePartSpecifier::MILLISECONDS:
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::MINUTE:
			return truncate;
		default:
			return false;
		}
	}

	static inline bool TryTruncMonth(const ZoneLUT &lut, int64_t micros, bool year, int64_t &out) {
		int64_t offset = 0;
		int64_t local = 0;
		if (!TryLocal(lut, micros, offset, local)) {
			return false;
		}
		const auto days = DateTrunc::ToDays(timestamp_t(local));
		const int64_t start = year ? DateTrunc::YearOperator::Days(days) : DateTrunc::MonthOperator::Days(days);
		return lut.TryResolve(start * Interval::MICROS_PER_DAY, out);
	}

	static inline bool TryTruncQuarter(const ZoneLUT &lut, int64_t micros, int64_t &out) {
		int64_t month_start = 0;
		if (!TryTruncMonth(lut, micros, false, month_start)) {
			return false;
		}
		auto floor_quarter = [](int32_t &, int32_t &m) {
			m = ((m - 1) / 3) * 3 + 1;
			return true;
		};
		return TryResolveAdjusted(lut, month_start, floor_quarter, out);
	}

	static inline bool TryTrunc(const ZoneLUT &lut, DatePartSpecifier part, int64_t micros, int64_t &out) {
		int64_t offset = 0;
		int64_t local = 0;
		switch (part) {
		case DatePartSpecifier::MICROSECONDS:
			out = micros;
			return true;
		case DatePartSpecifier::MILLISECONDS:
			out = StartMillis(micros) * Interval::MICROS_PER_MSEC;
			return true;
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::MINUTE: {
			const int64_t unit = part == DatePartSpecifier::SECOND ? Interval::MICROS_PER_SEC : Interval::MICROS_PER_MINUTE;
			if (!TryLocal(lut, micros, offset, local)) {
				return false;
			}
			out = DateTrunc::FloorDiv(local, unit) * unit - offset;
			return true;
		}
		case DatePartSpecifier::HOUR:
			if (!TryLocal(lut, micros, offset, local)) {
				return false;
			}
			return lut.TryResolve(DateTrunc::FloorDiv(local, Interval::MICROS_PER_HOUR) * Interval::MICROS_PER_HOUR, out);
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::WEEK: {
			if (!TryLocal(lut, micros, offset, local)) {
				return false;
			}
			const int64_t day = DateTrunc::FloorDiv(local, Interval::MICROS_PER_DAY);
			return lut.TryResolveDay(day - ZoneLUT::FIRST_DAY, day * Interval::MICROS_PER_DAY, out);
		}
		case DatePartSpecifier::MONTH:
			return TryTruncMonth(lut, micros, false, out);
		case DatePartSpecifier::QUARTER:
			return TryTruncQuarter(lut, micros, out);
		case DatePartSpecifier::YEAR:
			return TryTruncMonth(lut, micros, true, out);
		case DatePartSpecifier::DECADE:
		case DatePartSpecifier::CENTURY:
		case DatePartSpecifier::MILLENNIUM:
			return TryTruncYears(lut, micros, YearDivisor(part), out);
		default:
			return false;
		}
	}

	static inline bool TryDiff(const ZoneLUT &lut, DatePartSpecifier part, int64_t start, int64_t end, int64_t &value) {
		int64_t ts = 0;
		int64_t te = 0;
		if (!TryTrunc(lut, part, start, ts) || !TryTrunc(lut, part, end, te)) {
			return false;
		}
		switch (part) {
		case DatePartSpecifier::MICROSECONDS:
			value = te - ts;
			return true;
		case DatePartSpecifier::MILLISECONDS:
			value = (te - ts) / Interval::MICROS_PER_MSEC;
			return true;
		case DatePartSpecifier::SECOND:
			value = (te - ts) / Interval::MICROS_PER_SEC;
			return true;
		case DatePartSpecifier::MINUTE:
			value = (te - ts) / Interval::MICROS_PER_MINUTE;
			return true;
		default:
			return TrySub(lut, part, ts, te, value);
		}
	}

	template <class FALLBACK>
	static bool TryDateSub(const ICUDateFunc::BindData &info, DatePartSpecifier part, const Vector &start,
	                       const Vector &end, idx_t count, Vector &result, FALLBACK &&fallback) {
		return TryDifference(info, part, start, end, count, result, false, fallback);
	}

	template <class FALLBACK>
	static bool TryDateDiff(const ICUDateFunc::BindData &info, DatePartSpecifier part, const Vector &start,
	                        const Vector &end, idx_t count, Vector &result, FALLBACK &&fallback) {
		return TryDifference(info, part, start, end, count, result, true, fallback);
	}

	static inline bool ZonedPair(const ICUDateFunc::BindData &info, const Vector &start, const Vector &end) {
		return start.GetType().id() == LogicalTypeId::TIMESTAMP_TZ &&
		       end.GetType().id() == LogicalTypeId::TIMESTAMP_TZ && Usable(info);
	}

	static inline bool TryDifferenceRow(const ZoneLUT &lut, DatePartSpecifier part, bool truncate, int64_t start,
	                                    int64_t end, int64_t &value) {
		return truncate ? TryDiff(lut, part, start, end, value) : TrySub(lut, part, start, end, value);
	}

	template <class FALLBACK>
	static bool TryDifference(const ICUDateFunc::BindData &info, DatePartSpecifier part, const Vector &start,
	                          const Vector &end, idx_t count, Vector &result, bool truncate, FALLBACK &&fallback) {
		if (!ZonedPair(info, start, end) || !SupportedPart(part, truncate)) {
			return false;
		}
		const auto &lut = *info.lut;
		BinaryExecutor::Execute<timestamp_tz_t, timestamp_tz_t, int64_t>(
		    start, end, result, count, [&](timestamp_tz_t a, timestamp_tz_t b) -> optional<int64_t> {
			    if (!a.IsFinite() || !b.IsFinite()) {
				    return nullopt;
			    }
			    int64_t value = 0;
			    if (!TryDifferenceRow(lut, part, truncate, a.value, b.value, value)) {
				    return fallback(a, b);
			    }
			    return value;
		    });
		return true;
	}

	template <class FALLBACK>
	static bool TryDifferenceDynamic(const ICUDateFunc::BindData &info, const Vector &parts, const Vector &start,
	                                 const Vector &end, Vector &result, bool truncate, FALLBACK &&fallback) {
		if (!ZonedPair(info, start, end)) {
			return false;
		}
		const auto &lut = *info.lut;
		string_t last_spec;
		bool have_last = false;
		bool last_supported = false;
		DatePartSpecifier last_part = DatePartSpecifier::INVALID;
		TernaryExecutor::Execute<string_t, timestamp_tz_t, timestamp_tz_t, int64_t>(
		    parts, start, end, result, [&](string_t spec, timestamp_tz_t a, timestamp_tz_t b) -> optional<int64_t> {
			    if (!a.IsFinite() || !b.IsFinite()) {
				    return nullopt;
			    }
			    if (!have_last || spec != last_spec) {
				    last_spec = spec;
				    have_last = true;
				    last_supported =
				        TryGetDatePartSpecifier(spec.GetString(), last_part) && SupportedPart(last_part, truncate);
			    }
			    int64_t value = 0;
			    if (!last_supported || !TryDifferenceRow(lut, last_part, truncate, a.value, b.value, value)) {
				    return fallback(spec, a, b);
			    }
			    return value;
		    });
		return true;
	}
};

template <>
struct ICUScalarFast::IntervalArithmetic<timestamp_tz_t, timestamp_tz_t> {
	template <class FALLBACK>
	static bool Try(DataChunk &args, Vector &result, const ICUDateFunc::BindData &info, Operation operation,
	                FALLBACK &&fallback) {
		if (operation == Operation::ADD) {
			return false;
		}
		auto row = [&](const ZoneLUT &lut, timestamp_tz_t end, timestamp_tz_t start, interval_t &out) {
			return operation == Operation::AGE ? TryAge(lut, end, start, out) : TrySubtract(lut, end, start, out);
		};
		return ZonedBinary<timestamp_tz_t, timestamp_tz_t, interval_t>(args, result, info, row, fallback);
	}
};

template <>
struct ICUScalarFast::IntervalArithmetic<timestamp_tz_t, interval_t> {
	template <class FALLBACK>
	static bool Try(DataChunk &args, Vector &result, const ICUDateFunc::BindData &info, Operation operation,
	                FALLBACK &&fallback) {
		if (operation == Operation::AGE) {
			return false;
		}
		const bool negate = operation == Operation::SUBTRACT;
		auto row = [&](const ZoneLUT &lut, timestamp_tz_t ts, interval_t interval, timestamp_tz_t &out) {
			int64_t value = 0;
			if (!TryAddInterval(lut, ts, negate ? Interval::Invert(interval) : interval, value)) {
				return false;
			}
			out = timestamp_tz_t(value);
			return true;
		};
		return ZonedBinary<timestamp_tz_t, interval_t, timestamp_tz_t>(args, result, info, row, fallback);
	}
};

template <>
struct ICUScalarFast::IntervalArithmetic<interval_t, timestamp_tz_t> {
	template <class FALLBACK>
	static bool Try(DataChunk &args, Vector &result, const ICUDateFunc::BindData &info, Operation operation,
	                FALLBACK &&fallback) {
		if (operation != Operation::ADD) {
			return false;
		}
		auto row = [&](const ZoneLUT &lut, interval_t interval, timestamp_tz_t ts, timestamp_tz_t &out) {
			int64_t value = 0;
			if (!TryAddInterval(lut, ts, interval, value)) {
				return false;
			}
			out = timestamp_tz_t(value);
			return true;
		};
		return ZonedBinary<interval_t, timestamp_tz_t, timestamp_tz_t>(args, result, info, row, fallback);
	}
};

} // namespace duckdb
