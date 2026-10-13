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

TEST_CASE("Filters derived from a wide disjunction keep the plan shallow", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query(
	    "CREATE TABLE t AS SELECT i AS a, i % 7 AS b, DATE '2000-01-01' + i::INTEGER AS d FROM range(100000) r(i)"));

	vector<string> terms;
	for (idx_t i = 0; i < 50000; i++) {
		terms.push_back(StringUtil::Format("(a = %d AND b = %d)", i, i % 7));
	}
	auto result = con.Query("SELECT count(*) FROM t WHERE " + StringUtil::Join(terms, " OR "));
	REQUIRE(CHECK_COLUMN(result, 0, {50000}));

	vector<string> years;
	for (idx_t year = 1; year <= 20000; year++) {
		years.push_back(to_string(year));
	}
	result = con.Query("SELECT count(*) FROM t WHERE year(d) IN (" + StringUtil::Join(years, ", ") + ")");
	REQUIRE(CHECK_COLUMN(result, 0, {100000}));
}

TEST_CASE("Order dependencies of a wide UNION ALL stay linear", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	vector<string> branches;
	for (idx_t i = 0; i < 20000; i++) {
		branches.push_back(StringUtil::Format("SELECT %d AS v", i));
	}
	auto result = con.Query(StringUtil::Join(branches, " UNION ALL "));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 20000);
	idx_t row = 0;
	for (auto &chunk : result->Collection().Chunks()) {
		for (idx_t i = 0; i < chunk.size(); i++) {
			REQUIRE(chunk.GetValue(0, i) == Value::INTEGER(NumericCast<int32_t>(row++)));
		}
	}
	REQUIRE(row == 20000);
}

TEST_CASE("Decorrelating a long chain of EXISTS subqueries stops at the query deadline", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_execution_time = 1000"));

	vector<string> terms;
	for (idx_t k = 0; k < 400; k++) {
		terms.push_back(
		    StringUtil::Format("EXISTS (SELECT 1 FROM range(10) s%d(b) WHERE s%d.b = (t.a + %d) %% 10)", k, k, k));
	}
	auto start = std::chrono::steady_clock::now();
	auto result = con.Query("SELECT count(*) FROM range(10) t(a) WHERE " + StringUtil::Join(terms, " AND "));
	auto elapsed = std::chrono::steady_clock::now() - start;
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "maximum execution time"));
	REQUIRE(elapsed < std::chrono::seconds(20));
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
