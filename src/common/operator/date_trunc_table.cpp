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
