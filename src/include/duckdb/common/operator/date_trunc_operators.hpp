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
#include "duckdb/common/exception.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

struct DateTruncTable {
	static constexpr int32_t FIRST_YEAR = 1900;
	static constexpr int32_t FIRST_DAY = -25567;
	static constexpr int32_t YEAR_COUNT = Date::YEAR_INTERVAL;
	static constexpr int32_t DAY_COUNT = Date::DAYS_PER_YEAR_INTERVAL;
	static constexpr int32_t MONTH_COUNT = YEAR_COUNT * Interval::MONTHS_PER_YEAR;

	DateTruncTable();

	static inline bool Contains(int64_t days) {
		return uint64_t(days - FIRST_DAY) < uint64_t(DAY_COUNT);
	}
	inline int32_t Month(int64_t days) const {
		return day_month[days - FIRST_DAY];
	}
	inline int64_t MonthStart(int32_t month) const {
		return month_start[month];
	}

	uint16_t day_month[DAY_COUNT];
	int32_t month_start[MONTH_COUNT + 1];

	DUCKDB_API static const DateTruncTable INSTANCE;
};

struct DateTruncUnit {
	enum class Unit : uint8_t { MICROS, DAYS, MONTHS };

	Unit unit = Unit::MICROS;
	int64_t width = 1;
	int64_t anchor = 0;
};

struct DateTrunc {
	static constexpr int64_t MAX_TIMESTAMP_DAYS = NumericLimits<int64_t>::Maximum() / Interval::MICROS_PER_DAY;
	static constexpr int64_t EPOCH_MONDAY = -3;

	static bool TryGetUnit(DatePartSpecifier part, DateTruncUnit &unit) {
		unit = DateTruncUnit();
		switch (part) {
		case DatePartSpecifier::MICROSECONDS:
			return true;
		case DatePartSpecifier::MILLISECONDS:
			unit.width = Interval::MICROS_PER_MSEC;
			return true;
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::EPOCH:
			unit.width = Interval::MICROS_PER_SEC;
			return true;
		case DatePartSpecifier::MINUTE:
			unit.width = Interval::MICROS_PER_MINUTE;
			return true;
		case DatePartSpecifier::HOUR:
			unit.width = Interval::MICROS_PER_HOUR;
			return true;
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::DOW:
		case DatePartSpecifier::ISODOW:
		case DatePartSpecifier::DOY:
		case DatePartSpecifier::JULIAN_DAY:
			unit.unit = DateTruncUnit::Unit::DAYS;
			return true;
		case DatePartSpecifier::WEEK:
		case DatePartSpecifier::YEARWEEK:
			unit.unit = DateTruncUnit::Unit::DAYS;
			unit.width = Interval::DAYS_PER_WEEK;
			unit.anchor = EPOCH_MONDAY;
			return true;
		case DatePartSpecifier::MONTH:
			unit.unit = DateTruncUnit::Unit::MONTHS;
			return true;
		case DatePartSpecifier::QUARTER:
			unit.unit = DateTruncUnit::Unit::MONTHS;
			unit.width = Interval::MONTHS_PER_QUARTER;
			return true;
		case DatePartSpecifier::YEAR:
			unit.unit = DateTruncUnit::Unit::MONTHS;
			unit.width = Interval::MONTHS_PER_YEAR;
			return true;
		case DatePartSpecifier::DECADE:
			unit.unit = DateTruncUnit::Unit::MONTHS;
			unit.width = Interval::MONTHS_PER_DECADE;
			return true;
		case DatePartSpecifier::CENTURY:
			unit.unit = DateTruncUnit::Unit::MONTHS;
			unit.width = Interval::MONTHS_PER_CENTURY;
			return true;
		case DatePartSpecifier::MILLENNIUM:
			unit.unit = DateTruncUnit::Unit::MONTHS;
			unit.width = Interval::MONTHS_PER_MILLENIUM;
			return true;
		default:
			return false;
		}
	}

	template <class T>
	static inline T FloorDiv(T value, T divisor) {
		return value / divisor - (value % divisor < 0);
	}

	static inline int32_t ToDays(date_t input) {
		return input.days;
	}

	static inline int32_t ToDays(timestamp_t input) {
		return UnsafeNumericCast<int32_t>(FloorDiv(input.value, Interval::MICROS_PER_DAY));
	}

	[[noreturn]] static void ThrowOutOfRange() {
		throw ConversionException("Date and time not in timestamp range");
	}

	static inline timestamp_t FromDays(int64_t days) {
		if (DUCKDB_UNLIKELY(days < -MAX_TIMESTAMP_DAYS || days > MAX_TIMESTAMP_DAYS)) {
			ThrowOutOfRange();
		}
		return timestamp_t(days * Interval::MICROS_PER_DAY);
	}

	static inline timestamp_t TruncFixed(timestamp_t input, int64_t interval_us) {
		return timestamp_t(FloorDiv(input.value, interval_us) * interval_us);
	}

	static inline timestamp_t TruncFixed(date_t input, int64_t) {
		return FromDays(input.days);
	}

	struct YearDay {
		int64_t year;
		int64_t year_start;
		int32_t doy;
		bool leap;
	};

	static inline YearDay ToYearDay(int32_t days) {
		int32_t cycle = 0;
		uint32_t n = static_cast<uint32_t>(days);
		if (DUCKDB_UNLIKELY(n >= static_cast<uint32_t>(Date::DAYS_PER_YEAR_INTERVAL))) {
			cycle = FloorDiv(days, Date::DAYS_PER_YEAR_INTERVAL);
			n = UnsafeNumericCast<uint32_t>(days - cycle * Date::DAYS_PER_YEAR_INTERVAL);
		}
		uint32_t year_offset = n / 365;
		year_offset -= n < UnsafeNumericCast<uint32_t>(Date::CUMULATIVE_YEAR_DAYS[year_offset]);
		const int32_t year_start = Date::CUMULATIVE_YEAR_DAYS[year_offset];
		return {int64_t(cycle) * Date::YEAR_INTERVAL + year_offset + Date::EPOCH_YEAR,
		        int64_t(cycle) * Date::DAYS_PER_YEAR_INTERVAL + year_start, UnsafeNumericCast<int32_t>(n) - year_start,
		        Date::CUMULATIVE_YEAR_DAYS[year_offset + 1] - year_start == 366};
	}

	static inline int64_t YearStart(int64_t year) {
		const int64_t offset = year - Date::EPOCH_YEAR;
		const int64_t cycle = FloorDiv(offset, int64_t(Date::YEAR_INTERVAL));
		return cycle * Date::DAYS_PER_YEAR_INTERVAL + Date::CUMULATIVE_YEAR_DAYS[offset - cycle * Date::YEAR_INTERVAL];
	}

	static inline int32_t MonthOf(const YearDay &yd) {
		return (yd.leap ? Date::LEAP_MONTH_PER_DAY_OF_YEAR : Date::MONTH_PER_DAY_OF_YEAR)[yd.doy];
	}

	static inline int64_t MonthStart(const YearDay &yd, int32_t month) {
		return yd.year_start + (yd.leap ? Date::CUMULATIVE_LEAP_DAYS : Date::CUMULATIVE_DAYS)[month - 1];
	}
	static inline bool TryTableMonthStart(int32_t days, int64_t months, int64_t &start) {
		if (!DateTruncTable::Contains(days)) {
			return false;
		}
		auto &table = DateTruncTable::INSTANCE;
		const int64_t month = table.Month(days);
		start = table.MonthStart(UnsafeNumericCast<int32_t>(month - month % months));
		return true;
	}

	static inline int64_t MonthIndex(int64_t days) {
		if (DateTruncTable::Contains(days)) {
			return int64_t(DateTruncTable::FIRST_YEAR) * Interval::MONTHS_PER_YEAR + DateTruncTable::INSTANCE.Month(days);
		}
		const auto yd = ToYearDay(UnsafeNumericCast<int32_t>(days));
		return yd.year * Interval::MONTHS_PER_YEAR + MonthOf(yd) - 1;
	}
	static inline int64_t MonthIndex(timestamp_t input) {
		return MonthIndex(int64_t(ToDays(input)));
	}
	static inline int64_t MonthIndexStartDays(int64_t months) {
		const auto year = FloorDiv(months, int64_t(Interval::MONTHS_PER_YEAR));
		const auto month = months - year * Interval::MONTHS_PER_YEAR;
		if (DUCKDB_UNLIKELY(year < NumericLimits<int32_t>::Minimum() || year > NumericLimits<int32_t>::Maximum())) {
			ThrowOutOfRange();
		}
		const bool leap = Date::IsLeapYear(UnsafeNumericCast<int32_t>(year));
		return YearStart(year) + (leap ? Date::CUMULATIVE_LEAP_DAYS : Date::CUMULATIVE_DAYS)[month];
	}
	static inline timestamp_t MonthIndexStart(int64_t months) {
		return FromDays(MonthIndexStartDays(months));
	}

	struct MillenniumOperator {
		static inline int64_t Days(int32_t days) {
			return YearStart((ToYearDay(days).year / 1000) * 1000);
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct CenturyOperator {
		static inline int64_t Days(int32_t days) {
			int64_t start = 0;
			if (TryTableMonthStart(days, Interval::MONTHS_PER_CENTURY, start)) {
				return start;
			}
			return YearStart((ToYearDay(days).year / 100) * 100);
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct DecadeOperator {
		static inline int64_t Days(int32_t days) {
			int64_t start = 0;
			if (TryTableMonthStart(days, Interval::MONTHS_PER_DECADE, start)) {
				return start;
			}
			return YearStart((ToYearDay(days).year / 10) * 10);
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct YearOperator {
		static inline int64_t Days(int32_t days) {
			int64_t start = 0;
			if (TryTableMonthStart(days, Interval::MONTHS_PER_YEAR, start)) {
				return start;
			}
			return ToYearDay(days).year_start;
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct QuarterOperator {
		static inline int64_t Days(int32_t days) {
			int64_t start = 0;
			if (TryTableMonthStart(days, Interval::MONTHS_PER_QUARTER, start)) {
				return start;
			}
			const auto yd = ToYearDay(days);
			return MonthStart(yd, 1 + ((MonthOf(yd) - 1) / 3) * 3);
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct MonthOperator {
		static inline int64_t Days(int32_t days) {
			int64_t start = 0;
			if (TryTableMonthStart(days, 1, start)) {
				return start;
			}
			const auto yd = ToYearDay(days);
			return MonthStart(yd, MonthOf(yd));
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct WeekOperator {
		static inline int64_t Days(int32_t days) {
			return EPOCH_MONDAY +
			       FloorDiv(days - EPOCH_MONDAY, int64_t(Interval::DAYS_PER_WEEK)) * Interval::DAYS_PER_WEEK;
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct ISOYearOperator {
		static inline int64_t Days(int32_t days) {
			const auto thursday = UnsafeNumericCast<int32_t>(WeekOperator::Days(days) + 3);
			const auto january_fourth = YearStart(ToYearDay(thursday).year) + 3;
			return WeekOperator::Days(UnsafeNumericCast<int32_t>(january_fourth));
		}

		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return FromDays(Days(ToDays(input)));
		}
	};

	struct DayOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TruncFixed(input, Interval::MICROS_PER_DAY);
		}
	};

	struct HourOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TruncFixed(input, Interval::MICROS_PER_HOUR);
		}
	};

	struct MinuteOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TruncFixed(input, Interval::MICROS_PER_MINUTE);
		}
	};

	struct SecondOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TruncFixed(input, Interval::MICROS_PER_SEC);
		}
	};

	struct MillisecondOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TruncFixed(input, Interval::MICROS_PER_MSEC);
		}
	};

	struct MicrosecondOperator {
		template <class TA, class TR>
		static inline TR Operation(TA input) {
			return TruncFixed(input, 1);
		}
	};

	template <class FUNC>
	static inline auto Dispatch(DatePartSpecifier type, FUNC &&func) -> decltype(func(DayOperator())) {
		switch (type) {
		case DatePartSpecifier::MILLENNIUM:
			return func(MillenniumOperator());
		case DatePartSpecifier::CENTURY:
			return func(CenturyOperator());
		case DatePartSpecifier::DECADE:
			return func(DecadeOperator());
		case DatePartSpecifier::YEAR:
			return func(YearOperator());
		case DatePartSpecifier::QUARTER:
			return func(QuarterOperator());
		case DatePartSpecifier::MONTH:
			return func(MonthOperator());
		case DatePartSpecifier::WEEK:
		case DatePartSpecifier::YEARWEEK:
			return func(WeekOperator());
		case DatePartSpecifier::ISOYEAR:
			return func(ISOYearOperator());
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::DOW:
		case DatePartSpecifier::ISODOW:
		case DatePartSpecifier::DOY:
		case DatePartSpecifier::JULIAN_DAY:
			return func(DayOperator());
		case DatePartSpecifier::HOUR:
			return func(HourOperator());
		case DatePartSpecifier::MINUTE:
			return func(MinuteOperator());
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::EPOCH:
			return func(SecondOperator());
		case DatePartSpecifier::MILLISECONDS:
			return func(MillisecondOperator());
		case DatePartSpecifier::MICROSECONDS:
			return func(MicrosecondOperator());
		default:
			throw NotImplementedException("Specifier type not implemented for DATETRUNC");
		}
	}

	template <class TA, class TR>
	static inline TR Element(DatePartSpecifier type, TA element) {
		if (!element.IsFinite()) {
			return Cast::template Operation<TA, TR>(element);
		}
		return Dispatch(type, [&](auto op) { return decltype(op)::template Operation<TA, TR>(element); });
	}
};

template <>
inline interval_t DateTrunc::MillenniumOperator::Operation(interval_t input) {
	input.days = 0;
	input.micros = 0;
	input.months = (input.months / Interval::MONTHS_PER_MILLENIUM) * Interval::MONTHS_PER_MILLENIUM;
	return input;
}

template <>
inline interval_t DateTrunc::CenturyOperator::Operation(interval_t input) {
	input.days = 0;
	input.micros = 0;
	input.months = (input.months / Interval::MONTHS_PER_CENTURY) * Interval::MONTHS_PER_CENTURY;
	return input;
}

template <>
inline interval_t DateTrunc::DecadeOperator::Operation(interval_t input) {
	input.days = 0;
	input.micros = 0;
	input.months = (input.months / Interval::MONTHS_PER_DECADE) * Interval::MONTHS_PER_DECADE;
	return input;
}

template <>
inline interval_t DateTrunc::YearOperator::Operation(interval_t input) {
	input.days = 0;
	input.micros = 0;
	input.months = (input.months / Interval::MONTHS_PER_YEAR) * Interval::MONTHS_PER_YEAR;
	return input;
}

template <>
inline interval_t DateTrunc::QuarterOperator::Operation(interval_t input) {
	input.days = 0;
	input.micros = 0;
	input.months = (input.months / Interval::MONTHS_PER_QUARTER) * Interval::MONTHS_PER_QUARTER;
	return input;
}

template <>
inline interval_t DateTrunc::MonthOperator::Operation(interval_t input) {
	input.days = 0;
	input.micros = 0;
	return input;
}

template <>
inline interval_t DateTrunc::WeekOperator::Operation(interval_t input) {
	input.micros = 0;
	input.days = (input.days / Interval::DAYS_PER_WEEK) * Interval::DAYS_PER_WEEK;
	return input;
}

template <>
inline interval_t DateTrunc::ISOYearOperator::Operation(interval_t input) {
	return YearOperator::Operation<interval_t, interval_t>(input);
}

template <>
inline interval_t DateTrunc::DayOperator::Operation(interval_t input) {
	input.micros = 0;
	return input;
}

template <>
inline interval_t DateTrunc::HourOperator::Operation(interval_t input) {
	input.micros = (input.micros / Interval::MICROS_PER_HOUR) * Interval::MICROS_PER_HOUR;
	return input;
}

template <>
inline interval_t DateTrunc::MinuteOperator::Operation(interval_t input) {
	input.micros = (input.micros / Interval::MICROS_PER_MINUTE) * Interval::MICROS_PER_MINUTE;
	return input;
}

template <>
inline interval_t DateTrunc::SecondOperator::Operation(interval_t input) {
	input.micros = (input.micros / Interval::MICROS_PER_SEC) * Interval::MICROS_PER_SEC;
	return input;
}

template <>
inline interval_t DateTrunc::MillisecondOperator::Operation(interval_t input) {
	input.micros = (input.micros / Interval::MICROS_PER_MSEC) * Interval::MICROS_PER_MSEC;
	return input;
}

template <>
inline interval_t DateTrunc::MicrosecondOperator::Operation(interval_t input) {
	return input;
}

} // namespace duckdb
