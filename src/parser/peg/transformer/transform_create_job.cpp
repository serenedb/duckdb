#include "duckdb/parser/parsed_data/create_job_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

namespace duckdb {

unique_ptr<CreateStatement>
PEGTransformerFactory::TransformCreateJobStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
                                              const QualifiedName &qualified_name, JobScheduleInfo job_schedule,
                                              const optional<bool> &job_suspended, unique_ptr<SQLStatement> statement) {
	if (statement->type == StatementType::TRANSACTION_STATEMENT) {
		throw ParserException("CREATE JOB body cannot be a transaction statement");
	}
	if (transformer.ParamCount() > 0) {
		throw ParserException("CREATE JOB cannot have parameters");
	}
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateJobInfo>();
	info->SetQualifiedName(qualified_name);
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	info->schedule.kind = job_schedule.kind;
	info->interval_expr = std::move(job_schedule.interval);
	info->offset_expr = std::move(job_schedule.offset);
	info->suspended = job_suspended && *job_suspended;
	info->body = statement->ToString();
	result->info = std::move(info);
	return result;
}

JobScheduleInfo PEGTransformerFactory::TransformJobEvery(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> job_interval,
                                                         optional<unique_ptr<ParsedExpression>> job_offset) {
	JobScheduleInfo result;
	result.kind = JobScheduleKind::EVERY;
	result.interval = std::move(job_interval);
	if (job_offset) {
		result.offset = std::move(*job_offset);
	}
	return result;
}

JobScheduleInfo PEGTransformerFactory::TransformJobAfter(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> job_interval) {
	JobScheduleInfo result;
	result.kind = JobScheduleKind::AFTER;
	result.interval = std::move(job_interval);
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformJobIntervalShort(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal, const DatePartSpecifier &interval) {
	return TransformIntervalLiteral(transformer, std::move(number_literal), interval);
}

bool PEGTransformerFactory::TransformJobSuspended(PEGTransformer &transformer) {
	return true;
}

} // namespace duckdb
