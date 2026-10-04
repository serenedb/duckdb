#include "duckdb/parser/parsed_data/create_job_info.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"

namespace duckdb {

void CreateJobInfo::SetBody(const string &sql) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1) {
		throw ParserException("Job body must be a single statement: %s", sql);
	}
	body = std::move(parser.statements[0]);
}

unique_ptr<CreateInfo> CreateJobInfo::Copy() const {
	auto result = make_uniq<CreateJobInfo>();
	CopyProperties(*result);
	result->schedule = schedule;
	result->suspended = suspended;
	result->body = body->Copy();
	result->parsed_schedule = parsed_schedule ? parsed_schedule->Copy() : nullptr;
	return std::move(result);
}

string CreateJobInfo::ScheduleToString(const JobSchedule &schedule,
                                       const unique_ptr<JobScheduleInfo> &parsed_schedule) {
	if (!parsed_schedule) {
		return schedule.ToString();
	}
	string result = EnumUtil::ToString(parsed_schedule->kind) + " (";
	result += parsed_schedule->interval->ToString() + ")";
	if (parsed_schedule->offset) {
		result += " OFFSET (" + parsed_schedule->offset->ToString() + ")";
	}
	if (parsed_schedule->randomize) {
		result += " RANDOMIZE FOR (" + parsed_schedule->randomize->ToString() + ")";
	}
	if (parsed_schedule->concurrent) {
		result += " CONCURRENT";
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
	result += " " + ScheduleToString(schedule, parsed_schedule);
	if (suspended) {
		result += " SUSPENDED";
	}
	return result + " AS " + body->ToString() + ";";
}

} // namespace duckdb
