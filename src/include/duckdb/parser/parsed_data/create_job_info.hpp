//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_job_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_data/job_schedule_info.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

struct CreateJobInfo : public CreateInfo {
	CreateJobInfo() : CreateInfo(CatalogType::JOB_ENTRY) {
	}

	void SetBody(const string &sql);

	JobSchedule schedule;
	bool suspended = false;
	unique_ptr<SQLStatement> body;
	unique_ptr<JobScheduleInfo> parsed_schedule;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;

	static string ScheduleToString(const JobSchedule &schedule, const unique_ptr<JobScheduleInfo> &parsed_schedule);
};

} // namespace duckdb
