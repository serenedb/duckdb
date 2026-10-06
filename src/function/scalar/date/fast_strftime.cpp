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

#include "duckdb/function/scalar/fast_strftime.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/types/cast_helpers.hpp"

#include <cstring>

namespace duckdb {

bool FastStrftime::Compile(const string &format_string) {
	ops.clear();
	literals.clear();
	granularity = Interval::MICROS_PER_DAY;
	idx_t literal_start = 0;
	auto flush_literal = [&]() {
		if (literals.size() > literal_start) {
			ops.push_back({Field::LITERAL, UnsafeNumericCast<uint32_t>(literal_start),
			               UnsafeNumericCast<uint32_t>(literals.size() - literal_start)});
		}
		literal_start = literals.size();
	};
	bool has_field = false;
	for (idx_t i = 0; i < format_string.size(); i++) {
		if (format_string[i] != '%') {
			literals += format_string[i];
			continue;
		}
		if (i + 1 == format_string.size()) {
			return false;
		}
		auto spec = format_string[++i];
		Field field;
		int64_t unit = Interval::MICROS_PER_DAY;
		switch (spec) {
		case '%':
			literals += '%';
			continue;
		case 'Y':
			field = Field::YEAR;
			break;
		case 'm':
			field = Field::MONTH;
			break;
		case 'd':
			field = Field::DAY;
			break;
		case 'H':
			field = Field::HOUR;
			unit = Interval::MICROS_PER_HOUR;
			break;
		case 'M':
			field = Field::MINUTE;
			unit = Interval::MICROS_PER_MINUTE;
			break;
		case 'S':
			field = Field::SECOND;
			unit = Interval::MICROS_PER_SEC;
			break;
		default:
			return false;
		}
		flush_literal();
		ops.push_back({field, 0, 0});
		granularity = MinValue(granularity, unit);
		has_field = true;
	}
	flush_literal();
	return has_field;
}

static char *WriteTwo(char *target, int32_t value) {
	memcpy(target, fmt::detail::digits2(UnsafeNumericCast<size_t>(value)), 2);
	return target + 2;
}

void FastStrftime::Execute(const Vector &input, Vector &result) const {
	int64_t last_key = NumericLimits<int64_t>::Minimum();
	string_t last_value;
	int32_t cached_days = NumericLimits<int32_t>::Minimum();
	int32_t year = 0;
	int32_t month = 0;
	int32_t day = 0;
	UnaryExecutor::Execute<timestamp_t, string_t>(input, result, [&](timestamp_t ts) {
		if (!ts.IsFinite()) {
			return StringVector::AddString(result, Timestamp::ToString(ts));
		}
		auto key = ts.value >= 0 ? ts.value / granularity : NumericLimits<int64_t>::Minimum();
		if (key == last_key && key != NumericLimits<int64_t>::Minimum()) {
			return last_value;
		}
		date_t date;
		dtime_t time;
		Timestamp::Convert(ts, date, time);
		if (date.days != cached_days) {
			Date::Convert(date, year, month, day);
			cached_days = date.days;
		}
		int32_t hour, minute, second, micros;
		Time::Convert(time, hour, minute, second, micros);

		uint32_t abs_year = UnsafeNumericCast<uint32_t>(year < 0 ? -year : year);
		bool fixed_year = year >= 0 && year <= 9999;
		idx_t year_size = fixed_year ? 4 : (year < 0 ? 1 : 0) + NumericHelper::UnsignedLength<uint32_t>(abs_year);
		idx_t size = 0;
		for (auto &op : ops) {
			size += op.field == Field::LITERAL ? op.size : op.field == Field::YEAR ? year_size : 2;
		}
		auto target = StringVector::EmptyString(result, size);
		auto out = target.GetDataWriteable();
		for (auto &op : ops) {
			switch (op.field) {
			case Field::LITERAL:
				memcpy(out, literals.data() + op.offset, op.size);
				out += op.size;
				break;
			case Field::YEAR:
				if (fixed_year) {
					out = WriteTwo(out, int32_t(abs_year / 100));
					out = WriteTwo(out, int32_t(abs_year % 100));
				} else {
					if (year < 0) {
						*out++ = '-';
					}
					auto len = NumericHelper::UnsignedLength<uint32_t>(abs_year);
					NumericHelper::FormatUnsigned(abs_year, out + len);
					out += len;
				}
				break;
			case Field::MONTH:
				out = WriteTwo(out, month);
				break;
			case Field::DAY:
				out = WriteTwo(out, day);
				break;
			case Field::HOUR:
				out = WriteTwo(out, hour);
				break;
			case Field::MINUTE:
				out = WriteTwo(out, minute);
				break;
			case Field::SECOND:
				out = WriteTwo(out, second);
				break;
			}
		}
		target.Finalize();
		last_key = key;
		last_value = target;
		return target;
	});
}

} // namespace duckdb
