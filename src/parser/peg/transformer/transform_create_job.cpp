#include "duckdb/parser/expression/function_expression.hpp"
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
	info->parsed_schedule = make_uniq<JobScheduleInfo>(std::move(job_schedule));
	info->suspended = job_suspended && *job_suspended;
	info->body = std::move(statement);
	result->info = std::move(info);
	return result;
}

JobScheduleInfo PEGTransformerFactory::TransformJobEvery(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> job_interval,
                                                         optional<unique_ptr<ParsedExpression>> job_offset,
                                                         optional<unique_ptr<ParsedExpression>> job_randomize,
                                                         const optional<bool> &job_concurrent) {
	JobScheduleInfo result;
	result.kind = JobScheduleKind::EVERY;
	result.interval = std::move(job_interval);
	if (job_offset) {
		result.offset = std::move(*job_offset);
	}
	if (job_randomize) {
		result.randomize = std::move(*job_randomize);
	}
	result.concurrent = job_concurrent && *job_concurrent;
	return result;
}

JobScheduleInfo PEGTransformerFactory::TransformJobAfter(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> job_interval,
                                                         optional<unique_ptr<ParsedExpression>> job_randomize) {
	JobScheduleInfo result;
	result.kind = JobScheduleKind::AFTER;
	result.interval = std::move(job_interval);
	if (job_randomize) {
		result.randomize = std::move(*job_randomize);
	}
	return result;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformJobIntervalShort(PEGTransformer &transformer,
                                                 vector<unique_ptr<ParsedExpression>> job_interval_unit) {
	auto result = std::move(job_interval_unit[0]);
	transformer.AddDepth(job_interval_unit.size() - 1);
	for (idx_t i = 1; i < job_interval_unit.size(); i++) {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(result));
		children.push_back(std::move(job_interval_unit[i]));
		auto sum = make_uniq<FunctionExpression>(Identifier("+"), std::move(children));
		sum->IsOperatorMutable() = true;
		result = std::move(sum);
	}
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformJobIntervalUnit(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal, const DatePartSpecifier &interval) {
	return TransformIntervalLiteral(transformer, std::move(number_literal), interval);
}

bool PEGTransformerFactory::TransformJobConcurrent(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformJobSuspended(PEGTransformer &transformer) {
	return true;
}

} // namespace duckdb
