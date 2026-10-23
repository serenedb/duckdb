#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/create_job_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"

namespace duckdb {

static ParserOptions JobBodyParserOptions(const ParserOptions &statement_options) {
	auto options = ParserOptions::Builtin();
	options.identifier_case_mode = statement_options.identifier_case_mode;
	options.integer_division = statement_options.integer_division;
	options.regex_match_operator_semantics = statement_options.regex_match_operator_semantics;
	return options;
}

unique_ptr<CreateStatement>
PEGTransformerFactory::TransformCreateJobStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
                                              const QualifiedName &qualified_name, JobScheduleInfo job_schedule,
                                              const optional<bool> &job_suspended, unique_ptr<SQLStatement> statement) {
	transformer.PivotEntryCheck("job");
	if (statement->type == StatementType::MULTI_STATEMENT && statement->query.empty()) {
		throw ParserException(
		    "CREATE JOB body cannot be a statement that runs as several statements, unless it is quoted: AS $$ ... $$");
	}
	if (statement->type != StatementType::MULTI_STATEMENT && CreateJobInfo::CarriesScript(*statement)) {
		throw ParserException(
		    "CREATE JOB body cannot create a job whose body is quoted, unless it is quoted: AS $$ ... $$");
	}
	if (transformer.ParamCount() > 0) {
		throw ParserException("CREATE JOB cannot have parameters");
	}
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateJobInfo>();
	if (statement->type == StatementType::MULTI_STATEMENT) {
		info->body = std::move(statement->Cast<MultiStatement>().statements);
	} else {
		info->body.push_back(std::move(statement));
	}
	if (info->body.size() == 1 && info->body[0]->type == StatementType::TRANSACTION_STATEMENT) {
		throw ParserException("CREATE JOB body cannot be a transaction statement");
	}
	if (CreateJobInfo::IsScript(info->body)) {
		info->body_parser_options = JobBodyParserOptions(transformer.options);
	}
	info->SetQualifiedName(qualified_name);
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	info->parsed_schedule = make_uniq<JobScheduleInfo>(std::move(job_schedule));
	info->suspended = job_suspended && *job_suspended;
	result->info = std::move(info);
	return result;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformJobScript(PEGTransformer &transformer,
                                                                   const string &string_literal) {
	Parser parser(JobBodyParserOptions(transformer.options));
	parser.ParseQuery(string_literal);
	if (parser.statements.empty()) {
		throw ParserException("CREATE JOB body cannot be empty");
	}
	for (auto &statement : parser.statements) {
		if (!statement->named_param_map.empty()) {
			throw ParserException("CREATE JOB cannot have parameters");
		}
	}
	auto script = make_uniq<MultiStatement>();
	script->statements = std::move(parser.statements);
	script->query = string_literal;
	return std::move(script);
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
