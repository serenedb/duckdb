//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/job_schedule.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/job_schedule_kind.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {
class Serializer;
class Deserializer;

struct JobSchedule {
	JobScheduleKind kind = JobScheduleKind::EVERY;
	Value interval = Value::INTERVAL(interval_t {0, 0, 0});
	Value offset = Value::INTERVAL(interval_t {0, 0, 0});

	void Verify() const;
	timestamp_t NextRun(timestamp_t after) const;
	string ToString() const;

	bool operator==(const JobSchedule &rhs) const;

	void Serialize(Serializer &serializer) const;
	static JobSchedule Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
