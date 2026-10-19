#include "duckdb/parser/parsed_data/create_job_info.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"

namespace duckdb {

bool CreateJobInfo::IsScript(const vector<unique_ptr<SQLStatement>> &body) {
	return body.size() != 1 || body[0]->type == StatementType::MULTI_STATEMENT;
}

string CreateJobInfo::BodyToString(const vector<unique_ptr<SQLStatement>> &body) {
	if (!IsScript(body)) {
		return body[0]->ToString();
	}
	return StringUtil::Join(body, "", [](const unique_ptr<SQLStatement> &statement) {
		D_ASSERT(!statement->query.empty());
		return statement->query;
	});
}

vector<unique_ptr<SQLStatement>> CreateJobInfo::CopyBody(const vector<unique_ptr<SQLStatement>> &body) {
	vector<unique_ptr<SQLStatement>> result;
	result.reserve(body.size());
	for (auto &statement : body) {
		result.push_back(statement->Copy());
	}
	return result;
}

void CreateJobInfo::SetBody(const string &sql) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(sql);
	body = std::move(parser.statements);
}

unique_ptr<CreateInfo> CreateJobInfo::Copy() const {
	auto result = make_uniq<CreateJobInfo>();
	CopyProperties(*result);
	result->schedule = schedule;
	result->suspended = suspended;
	result->body = CopyBody(body);
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
	auto body_sql = BodyToString(body);
	return result + " AS " + (IsScript(body) ? KeywordHelper::WriteQuoted(body_sql) : body_sql) + ";";
}

} // namespace duckdb
