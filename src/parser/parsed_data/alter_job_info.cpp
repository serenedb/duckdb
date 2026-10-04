#include "duckdb/parser/parsed_data/alter_job_info.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/parsed_data/create_job_info.hpp"

namespace duckdb {

unique_ptr<AlterInfo> AlterJobInfo::Copy() const {
	auto result = make_uniq<AlterJobInfo>(alter_job_type, GetAlterEntryData());
	result->schedule = schedule;
	result->parsed_schedule = parsed_schedule ? parsed_schedule->Copy() : nullptr;
	result->new_name = new_name;
	return std::move(result);
}

string AlterJobInfo::ToString() const {
	string result = "ALTER JOB " + GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	switch (alter_job_type) {
	case AlterJobType::SUSPEND:
		result += " SUSPEND";
		break;
	case AlterJobType::RESUME:
		result += " RESUME";
		break;
	case AlterJobType::SET_SCHEDULE:
		result += " SET SCHEDULE " + CreateJobInfo::ScheduleToString(schedule, parsed_schedule);
		break;
	case AlterJobType::RENAME:
		result += " RENAME TO " + SQLIdentifier(new_name);
		break;
	}
	return result + ";";
}

} // namespace duckdb
