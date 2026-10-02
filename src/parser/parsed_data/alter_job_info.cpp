#include "duckdb/parser/parsed_data/alter_job_info.hpp"

#include "duckdb/parser/parsed_data/create_job_info.hpp"

namespace duckdb {

AlterJobInfo::AlterJobInfo() : AlterInfo(AlterType::ALTER_JOB), alter_job_type(AlterJobType::SUSPEND) {
}

AlterJobInfo::AlterJobInfo(AlterJobType alter_job_type_p, AlterEntryData data)
    : AlterInfo(AlterType::ALTER_JOB, std::move(data.qualified_name), data.if_not_found),
      alter_job_type(alter_job_type_p) {
}

AlterJobInfo::~AlterJobInfo() {
}

CatalogType AlterJobInfo::GetCatalogType() const {
	return CatalogType::JOB_ENTRY;
}

unique_ptr<AlterInfo> AlterJobInfo::Copy() const {
	auto result = make_uniq<AlterJobInfo>(alter_job_type, GetAlterEntryData());
	result->schedule = schedule;
	result->interval_expr = interval_expr ? interval_expr->Copy() : nullptr;
	result->offset_expr = offset_expr ? offset_expr->Copy() : nullptr;
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
		result += " SET SCHEDULE " + CreateJobInfo::ScheduleToString(schedule, interval_expr, offset_expr);
		break;
	}
	return result + ";";
}

} // namespace duckdb
