#include "duckdb/catalog/job_schedule.hpp"

#include "duckdb/common/enum_util.hpp"

namespace duckdb {

string JobSchedule::ToString() const {
	string result = EnumUtil::ToString(kind) + " ";
	result += "INTERVAL '" + interval.ToString() + "'";
	if (offset.GetValue<interval_t>() != interval_t()) {
		result += " OFFSET INTERVAL '" + offset.ToString() + "'";
	}
	if (concurrent) {
		result += " CONCURRENT";
	}
	return result;
}

bool JobSchedule::operator==(const JobSchedule &rhs) const {
	return kind == rhs.kind && interval.GetValue<interval_t>() == rhs.interval.GetValue<interval_t>() &&
	       offset.GetValue<interval_t>() == rhs.offset.GetValue<interval_t>() && concurrent == rhs.concurrent;
}

} // namespace duckdb
