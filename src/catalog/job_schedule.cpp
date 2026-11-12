#include "duckdb/catalog/job_schedule.hpp"

#include "duckdb/common/enum_util.hpp"

namespace duckdb {

string JobSchedule::ToString() const {
	string result = EnumUtil::ToString(kind) + " ";
	result += "INTERVAL '" + interval.ToString() + "'";
	if (offset.GetValue<interval_t>() != interval_t()) {
		result += " OFFSET INTERVAL '" + offset.ToString() + "'";
	}
	if (randomize.GetValue<interval_t>() != interval_t()) {
		result += " RANDOMIZE FOR INTERVAL '" + randomize.ToString() + "'";
	}
	if (concurrent) {
		result += " CONCURRENT";
	}
	return result;
}

static bool SameInterval(const Value &left, const Value &right) {
	auto lhs = left.GetValue<interval_t>();
	auto rhs = right.GetValue<interval_t>();
	return lhs.months == rhs.months && lhs.days == rhs.days && lhs.micros == rhs.micros;
}

bool JobSchedule::operator==(const JobSchedule &rhs) const {
	return kind == rhs.kind && SameInterval(interval, rhs.interval) && SameInterval(offset, rhs.offset) &&
	       SameInterval(randomize, rhs.randomize) && concurrent == rhs.concurrent;
}

} // namespace duckdb
