#include "duckdb/parser/parsed_data/create_job_info.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

CreateJobInfo::CreateJobInfo() : CreateInfo(CatalogType::JOB_ENTRY) {
}

unique_ptr<CreateInfo> CreateJobInfo::Copy() const {
	auto result = make_uniq<CreateJobInfo>();
	CopyProperties(*result);
	result->schedule = schedule;
	result->suspended = suspended;
	result->body = body;
	result->search_path = search_path;
	result->interval_expr = interval_expr ? interval_expr->Copy() : nullptr;
	result->offset_expr = offset_expr ? offset_expr->Copy() : nullptr;
	return std::move(result);
}

string CreateJobInfo::ScheduleToString(const JobSchedule &schedule, const unique_ptr<ParsedExpression> &interval_expr,
                                       const unique_ptr<ParsedExpression> &offset_expr) {
	if (!interval_expr) {
		return schedule.ToString();
	}
	string result = schedule.kind == JobScheduleKind::EVERY ? "EVERY (" : "AFTER (";
	result += interval_expr->ToString() + ")";
	if (offset_expr) {
		result += " OFFSET (" + offset_expr->ToString() + ")";
	}
	return result;
}

string CreateJobInfo::ToString() const {
	string result = "CREATE";
	if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		result += " OR REPLACE";
	}
	if (temporary) {
		result += " TEMPORARY";
	}
	result += " JOB ";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		result += "IF NOT EXISTS ";
	}
	result += QualifiedNameToString();
	result += " " + ScheduleToString(schedule, interval_expr, offset_expr);
	if (suspended) {
		result += " SUSPENDED";
	}
	return result + " AS " + body + ";";
}

} // namespace duckdb
