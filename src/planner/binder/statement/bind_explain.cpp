#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

BoundStatement Binder::Bind(ExplainStatement &stmt) {
	BoundStatement result;

	auto output_shape = Settings::Get<ExplainOutputFormatSetting>(context);

	// bind the underlying statement
	auto plan = Bind(*stmt.stmt);
	// The unoptimized logical plan can only be captured here (the optimizer mutates the tree in
	// place), but rendering it walks the whole plan - skip it unless the current mode outputs it.
	string logical_plan_unopt;
	if (stmt.explain_type == ExplainType::EXPLAIN_STANDARD &&
	    Settings::Get<ExplainOutputSetting>(context) == ExplainOutputType::ALL) {
		logical_plan_unopt = plan.plan->ToString(context, stmt.format);
	}
	auto explain = make_uniq<LogicalExplain>(std::move(plan.plan), stmt.explain_type, stmt.format, output_shape);
	explain->logical_plan_unopt = std::move(logical_plan_unopt);

	result.plan = std::move(explain);
	if (output_shape == ExplainFormatShape::PG) {
		result.names = {"QUERY PLAN"};
		result.types = {LogicalType::VARCHAR};
	} else {
		result.names = {"explain_key", "explain_value"};
		result.types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	}

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

} // namespace duckdb
