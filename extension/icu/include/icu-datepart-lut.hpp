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
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "icu-datefunc.hpp"
#include "icu-zone-lut.hpp"

namespace duckdb {

struct ICUDatePartLUT {
	struct LocalTime {
		timestamp_t wall;
		int64_t offset;
		int32_t days;
		int64_t time_of_day;
	};

	template <class RESULT>
	using adapter_t = RESULT (*)(const LocalTime &local);

	[[gnu::always_inline]] static inline bool TryLocalTime(const ZoneLUT &lut, timestamp_tz_t input, LocalTime &local) {
		if (!lut.TryOffset(input.value, local.offset)) {
			return false;
		}
		local.wall = timestamp_t(input.value + local.offset);
		if (local.wall.value < ZoneLUT::FIRST_ANNO_DOMINI) {
			return false;
		}
		local.days = DateTrunc::ToDays(local.wall);
		local.time_of_day = local.wall.value - int64_t(local.days) * Interval::MICROS_PER_DAY;
		return true;
	}

	static inline int32_t LocalDays(const LocalTime &local) {
		return local.days;
	}

	static inline int64_t LocalTimeOfDay(const LocalTime &local) {
		return local.time_of_day;
	}

	static inline DateTrunc::YearDay LocalYearDay(const LocalTime &local) {
		return DateTrunc::ToYearDay(LocalDays(local));
	}

	static int64_t LocalEra(const LocalTime &local) {
		return 1;
	}

	static int64_t LocalYear(const LocalTime &local) {
		return LocalYearDay(local).year;
	}

	static int64_t LocalDecade(const LocalTime &local) {
		return LocalYear(local) / 10;
	}

	static int64_t LocalCentury(const LocalTime &local) {
		return ((LocalYear(local) - 1) / 100) + 1;
	}

	static int64_t LocalMillenium(const LocalTime &local) {
		return ((LocalYear(local) - 1) / 1000) + 1;
	}

	static int64_t LocalMonth(const LocalTime &local) {
		return DateTrunc::MonthOf(LocalYearDay(local));
	}

	static int64_t LocalQuarter(const LocalTime &local) {
		return (LocalMonth(local) - 1) / Interval::MONTHS_PER_QUARTER + 1;
	}

	static int64_t LocalDay(const LocalTime &local) {
		const auto yd = LocalYearDay(local);
		return yd.doy - (DateTrunc::MonthStart(yd, DateTrunc::MonthOf(yd)) - yd.year_start) + 1;
	}

	static int64_t LocalISODayOfWeek(const LocalTime &local) {
		return Date::ExtractISODayOfTheWeek(date_t(LocalDays(local)));
	}

	static int64_t LocalDayOfWeek(const LocalTime &local) {
		return LocalISODayOfWeek(local) % 7;
	}

	static int64_t LocalWeek(const LocalTime &local) {
		return Date::ExtractISOWeekNumber(date_t(LocalDays(local)));
	}

	static int64_t LocalISOYear(const LocalTime &local) {
		return Date::ExtractISOYearNumber(date_t(LocalDays(local)));
	}

	static int64_t LocalYearWeek(const LocalTime &local) {
		const auto iyyy = LocalISOYear(local);
		const auto ww = LocalWeek(local);
		return iyyy * 100 + ((iyyy > 0) ? ww : -ww);
	}

	static int64_t LocalDayOfYear(const LocalTime &local) {
		return LocalYearDay(local).doy + 1;
	}

	static int64_t LocalHour(const LocalTime &local) {
		return LocalTimeOfDay(local) / Interval::MICROS_PER_HOUR;
	}

	static int64_t LocalMinute(const LocalTime &local) {
		return (LocalTimeOfDay(local) % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
	}

	static int64_t LocalSecond(const LocalTime &local) {
		return (LocalTimeOfDay(local) % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_SEC;
	}

	static int64_t LocalMillisecond(const LocalTime &local) {
		return (LocalTimeOfDay(local) % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_MSEC;
	}

	static int64_t LocalMicrosecond(const LocalTime &local) {
		return LocalTimeOfDay(local) % Interval::MICROS_PER_MINUTE;
	}

	static int64_t LocalTimezone(const LocalTime &local) {
		return local.offset / Interval::MICROS_PER_SEC;
	}

	static int64_t LocalTimezoneHour(const LocalTime &local) {
		return LocalTimezone(local) / Interval::SECS_PER_HOUR;
	}

	static int64_t LocalTimezoneMinute(const LocalTime &local) {
		return (LocalTimezone(local) % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
	}

	static double LocalEpoch(const LocalTime &local) {
		const int64_t instant = local.wall.value - local.offset;
		const int64_t millis = DateTrunc::FloorDiv(instant, Interval::MICROS_PER_MSEC);
		const uint64_t micros = UnsafeNumericCast<uint64_t>(instant - millis * Interval::MICROS_PER_MSEC);
		auto result = double(millis) / Interval::MSECS_PER_SEC;
		result += micros / double(Interval::MICROS_PER_SEC);
		return result;
	}

	static double LocalJulianDay(const LocalTime &local) {
		double result = double(LocalTimeOfDay(local));
		result /= Interval::MICROS_PER_DAY;
		result += double(Date::ExtractJulianDay(date_t(LocalDays(local))));
		return result;
	}

	static date_t LocalLastDay(const LocalTime &local) {
		const auto yd = LocalYearDay(local);
		const auto month = DateTrunc::MonthOf(yd);
		const auto days = yd.leap ? Date::LEAP_DAYS[month] : Date::NORMAL_DAYS[month];
		return date_t(UnsafeNumericCast<int32_t>(DateTrunc::MonthStart(yd, month) + days - 1));
	}

	static string_t LocalMonthName(const LocalTime &local) {
		return Date::MONTH_NAMES[LocalMonth(local) - 1];
	}

	static string_t LocalDayName(const LocalTime &local) {
		return Date::DAY_NAMES[LocalDayOfWeek(local)];
	}

	static adapter_t<int64_t> BigintFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::YEAR:
			return LocalYear;
		case DatePartSpecifier::MONTH:
			return LocalMonth;
		case DatePartSpecifier::DAY:
			return LocalDay;
		case DatePartSpecifier::DECADE:
			return LocalDecade;
		case DatePartSpecifier::CENTURY:
			return LocalCentury;
		case DatePartSpecifier::MILLENNIUM:
			return LocalMillenium;
		case DatePartSpecifier::MICROSECONDS:
			return LocalMicrosecond;
		case DatePartSpecifier::MILLISECONDS:
			return LocalMillisecond;
		case DatePartSpecifier::SECOND:
			return LocalSecond;
		case DatePartSpecifier::MINUTE:
			return LocalMinute;
		case DatePartSpecifier::HOUR:
			return LocalHour;
		case DatePartSpecifier::DOW:
			return LocalDayOfWeek;
		case DatePartSpecifier::ISODOW:
			return LocalISODayOfWeek;
		case DatePartSpecifier::WEEK:
			return LocalWeek;
		case DatePartSpecifier::ISOYEAR:
			return LocalISOYear;
		case DatePartSpecifier::DOY:
			return LocalDayOfYear;
		case DatePartSpecifier::QUARTER:
			return LocalQuarter;
		case DatePartSpecifier::YEARWEEK:
			return LocalYearWeek;
		case DatePartSpecifier::ERA:
			return LocalEra;
		case DatePartSpecifier::TIMEZONE:
			return LocalTimezone;
		case DatePartSpecifier::TIMEZONE_HOUR:
			return LocalTimezoneHour;
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return LocalTimezoneMinute;
		default:
			return nullptr;
		}
	}

	static adapter_t<double> DoubleFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::EPOCH:
			return LocalEpoch;
		case DatePartSpecifier::JULIAN_DAY:
			return LocalJulianDay;
		default:
			return nullptr;
		}
	}

	template <class RESULT>
	static adapter_t<RESULT> Factory(const string &name);

	template <class RESULT>
	static bool TryExecute(const ZoneLUT *lut, Vector &input, Vector &result, idx_t count, adapter_t<RESULT> adapter) {
		if (!lut || !adapter) {
			return false;
		}
		bool covered = true;
		UnaryExecutor::Execute<timestamp_tz_t, RESULT>(input, result, count,
		                                               [&](timestamp_tz_t value) -> optional<RESULT> {
			                                               if (!value.IsFinite()) {
				                                               return nullopt;
			                                               }
			                                               LocalTime local;
			                                               if (!TryLocalTime(*lut, value, local)) {
				                                               covered = false;
				                                               return nullopt;
			                                               }
			                                               return adapter(local);
		                                               });
		return covered;
	}

	template <class RESULT>
	static bool TryUnary(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = expr.BindInfo()->Cast<ICUDateFunc::BindData>();
		return TryExecute<RESULT>(info.lut.get(), args.data[0], result, args.size(),
		                          Factory<RESULT>(expr.Function().GetName().GetIdentifierName()));
	}

	static bool TryBinary(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &part_arg = args.data[0];
		if (part_arg.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(part_arg)) {
			return false;
		}
		DatePartSpecifier part;
		if (!TryGetDatePartSpecifier(ConstantVector::GetData<string_t>(part_arg)->GetString(), part)) {
			return false;
		}
		auto &expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = expr.BindInfo()->Cast<ICUDateFunc::BindData>();
		return TryExecute<int64_t>(info.lut.get(), args.data[1], result, args.size(), BigintFactory(part));
	}
};

template <>
inline ICUDatePartLUT::adapter_t<int64_t> ICUDatePartLUT::Factory(const string &name) {
	DatePartSpecifier part;
	return TryGetDatePartSpecifier(name, part) ? BigintFactory(part) : nullptr;
}

template <>
inline ICUDatePartLUT::adapter_t<double> ICUDatePartLUT::Factory(const string &name) {
	DatePartSpecifier part;
	return TryGetDatePartSpecifier(name, part) ? DoubleFactory(part) : nullptr;
}

template <>
inline ICUDatePartLUT::adapter_t<date_t> ICUDatePartLUT::Factory(const string &name) {
	return name == "last_day" ? LocalLastDay : nullptr;
}

template <>
inline ICUDatePartLUT::adapter_t<string_t> ICUDatePartLUT::Factory(const string &name) {
	if (name == "monthname") {
		return LocalMonthName;
	}
	return name == "dayname" ? LocalDayName : nullptr;
}

} // namespace duckdb
