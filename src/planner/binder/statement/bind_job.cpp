#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_job_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"

namespace duckdb {

static Value FoldJobInterval(Binder &binder, ClientContext &context, unique_ptr<ParsedExpression> &expr) {
	ConstantBinder constant_binder(binder, context, "job schedule");
	auto bound = constant_binder.Bind(expr);
	if (bound->HasParameter()) {
		throw NotImplementedException("Job schedules cannot have parameters");
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound, true);
	if (value.IsNull()) {
		throw BinderException("Job schedule interval must not be NULL");
	}
	return value.DefaultCastAs(LogicalType::INTERVAL);
}

void Binder::BindJobSchedule(JobSchedule &schedule, unique_ptr<JobScheduleInfo> &parsed_schedule) {
	if (!parsed_schedule) {
		return;
	}
	schedule.kind = parsed_schedule->kind;
	schedule.concurrent = parsed_schedule->concurrent;
	schedule.interval = FoldJobInterval(*this, context, parsed_schedule->interval);
	if (parsed_schedule->offset) {
		schedule.offset = FoldJobInterval(*this, context, parsed_schedule->offset);
	}
	if (parsed_schedule->randomize) {
		schedule.randomize = FoldJobInterval(*this, context, parsed_schedule->randomize);
	}
	parsed_schedule.reset();
}

SchemaCatalogEntry &Binder::BindCreateJobInfo(CreateJobInfo &info) {
	auto &schema = BindCreateSchema(info);
	BindJobSchedule(info.schedule, info.parsed_schedule);
	return schema;
}

} // namespace duckdb
