#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
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

void Binder::BindJobSchedule(JobSchedule &schedule, unique_ptr<ParsedExpression> &interval_expr,
                             unique_ptr<ParsedExpression> &offset_expr) {
	if (interval_expr) {
		schedule.interval = FoldJobInterval(*this, context, interval_expr);
		interval_expr.reset();
	}
	if (offset_expr) {
		schedule.offset = FoldJobInterval(*this, context, offset_expr);
		offset_expr.reset();
	}
}

SchemaCatalogEntry &Binder::BindCreateJobInfo(CreateJobInfo &info) {
	auto &schema = BindCreateSchema(info);
	BindJobSchedule(info.schedule, info.interval_expr, info.offset_expr);
	if (info.temporary) {
		info.search_path = ClientData::Get(context).catalog_search_path->GetSetPaths();
	}
	return schema;
}

} // namespace duckdb
