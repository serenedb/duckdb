#include "duckdb/catalog/job_schedule.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"

namespace duckdb {

static int64_t FloorDivide(int64_t value, int64_t divisor) {
	auto quotient = value / divisor;
	if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
		quotient--;
	}
	return quotient;
}

static bool IsNegative(const interval_t &value) {
	return value.months < 0 || value.days < 0 || value.micros < 0;
}

static bool IsZero(const interval_t &value) {
	return value.months == 0 && value.days == 0 && value.micros == 0;
}

static int64_t DayTimeMicros(const interval_t &value) {
	return int64_t(value.days) * Interval::MICROS_PER_DAY + value.micros;
}

void JobSchedule::Verify() const {
	auto every = interval.GetValue<interval_t>();
	auto shift = offset.GetValue<interval_t>();
	if (IsNegative(every) || IsZero(every)) {
		throw InvalidInputException("job schedule interval must be positive, got %s", interval.ToString());
	}
	if (IsNegative(shift)) {
		throw InvalidInputException("job schedule offset must not be negative, got %s", offset.ToString());
	}
	if (kind == JobScheduleKind::AFTER) {
		if (!IsZero(shift)) {
			throw InvalidInputException("OFFSET is not allowed with AFTER");
		}
		return;
	}
	if (every.months != 0) {
		if (every.days != 0 || every.micros != 0) {
			throw InvalidInputException("EVERY interval cannot mix months with days or time, got %s",
			                            interval.ToString());
		}
		if (shift.months >= every.months) {
			throw InvalidInputException("job schedule offset %s must be shorter than the interval %s",
			                            offset.ToString(), interval.ToString());
		}
		return;
	}
	if (shift.months != 0 || DayTimeMicros(shift) >= DayTimeMicros(every)) {
		throw InvalidInputException("job schedule offset %s must be shorter than the interval %s", offset.ToString(),
		                            interval.ToString());
	}
}

timestamp_t JobSchedule::NextRun(timestamp_t after) const {
	auto every = interval.GetValue<interval_t>();
	auto shift = offset.GetValue<interval_t>();
	if (kind == JobScheduleKind::AFTER) {
		return Interval::Add(after, every);
	}
	if (every.months != 0) {
		date_t date;
		dtime_t time;
		Timestamp::Convert(after, date, time);
		int32_t year, month, day;
		Date::Convert(date, year, month, day);
		auto months = (int64_t(year) - 2000) * 12 + (month - 1);
		auto bucket = (FloorDivide(months, every.months) - 1) * every.months;
		for (;;) {
			auto bucket_year = int32_t(2000 + FloorDivide(bucket, 12));
			auto bucket_month = int32_t(bucket - FloorDivide(bucket, 12) * 12 + 1);
			auto start = Timestamp::FromDatetime(Date::FromDate(bucket_year, bucket_month, 1), dtime_t(0));
			auto candidate = Interval::Add(start, shift);
			if (candidate > after) {
				return candidate;
			}
			bucket += every.months;
		}
	}
	auto width = DayTimeMicros(every);
	auto origin = Timestamp::GetEpochMicroSeconds(Timestamp::FromDatetime(Date::FromDate(2000, 1, 3), dtime_t(0)));
	auto micros = Timestamp::GetEpochMicroSeconds(after);
	auto bucket = origin + (FloorDivide(micros - origin, width) - 1) * width;
	for (;;) {
		auto candidate = Interval::Add(Timestamp::FromEpochMicroSeconds(bucket), shift);
		if (candidate > after) {
			return candidate;
		}
		bucket += width;
	}
}

string JobSchedule::ToString() const {
	string result = kind == JobScheduleKind::EVERY ? "EVERY " : "AFTER ";
	result += "INTERVAL '" + interval.ToString() + "'";
	if (!IsZero(offset.GetValue<interval_t>())) {
		result += " OFFSET INTERVAL '" + offset.ToString() + "'";
	}
	return result;
}

bool JobSchedule::operator==(const JobSchedule &rhs) const {
	return kind == rhs.kind && interval.GetValue<interval_t>() == rhs.interval.GetValue<interval_t>() &&
	       offset.GetValue<interval_t>() == rhs.offset.GetValue<interval_t>();
}

} // namespace duckdb
