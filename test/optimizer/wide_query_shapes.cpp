#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static idx_t CheckSharedLambdaBodies(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return 0;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	idx_t lambdas = 0;
	for (auto &child : function.GetChildren()) {
		if (child->GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
			lambdas += CheckSharedLambdaBodies(*child);
			continue;
		}
		auto body = child->Cast<BoundLambdaExpression>().LambdaExpr();
		REQUIRE(body.get() == function.BindInfo()->Cast<LambdaFunctionData>().GetLambdaExpression().get());
		auto copy = child->Copy();
		REQUIRE(body.get() == copy->Cast<BoundLambdaExpression>().LambdaExpr().get());
		lambdas += 1 + CheckSharedLambdaBodies(*body);
	}
	return lambdas;
}

TEST_CASE("Nested lambdas share one body with their bind data and copies", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	string expr = "x";
	for (idx_t k = 0; k < 200; k++) {
		expr = StringUtil::Format("list_transform([%d], lambda v%d: %s + v%d)[1]", k, k, expr, k);
	}
	con.BeginTransaction();
	auto plan = con.ExtractPlan("SELECT " + expr + " FROM range(3) t(x)");
	reference<LogicalOperator> op = *plan;
	while (op.get().type != LogicalOperatorType::LOGICAL_PROJECTION) {
		op = *op.get().children[0];
	}
	REQUIRE(CheckSharedLambdaBodies(*op.get().Cast<LogicalProjection>().expressions[0]) == 200);
	con.Rollback();

	auto result = con.Query("SELECT " + expr + " FROM range(3) t(x) ORDER BY 1");
	REQUIRE(CHECK_COLUMN(result, 0, {19900, 19901, 19902}));
}
