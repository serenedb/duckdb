#include "duckdb/common/operator/date_trunc_operators.hpp"

namespace duckdb {

DateTruncTable::DateTruncTable() {
	D_ASSERT(DateTrunc::YearStart(FIRST_YEAR) == FIRST_DAY);
	for (int32_t month = 0; month <= MONTH_COUNT; month++) {
		const int32_t year = FIRST_YEAR + month / Interval::MONTHS_PER_YEAR;
		const int32_t month_of_year = month % Interval::MONTHS_PER_YEAR;
		const auto days = (Date::IsLeapYear(year) ? Date::CUMULATIVE_LEAP_DAYS : Date::CUMULATIVE_DAYS)[month_of_year];
		month_start[month] = UnsafeNumericCast<int32_t>(DateTrunc::YearStart(year) + days);
	}
	for (int32_t month = 0; month < MONTH_COUNT; month++) {
		for (auto day = month_start[month]; day < month_start[month + 1]; day++) {
			day_month[day - FIRST_DAY] = UnsafeNumericCast<uint16_t>(month);
		}
	}
}

const DateTruncTable DateTruncTable::INSTANCE;

} // namespace duckdb
