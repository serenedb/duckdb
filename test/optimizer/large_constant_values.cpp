#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

using namespace duckdb;

static const Expression &ProjectedExpression(const LogicalOperator &plan) {
	reference<const LogicalOperator> op = plan;
	while (op.get().type != LogicalOperatorType::LOGICAL_PROJECTION) {
		REQUIRE(op.get().children.size() == 1);
		op = *op.get().children[0];
	}
	auto &projection = op.get().Cast<LogicalProjection>();
	REQUIRE(projection.expressions.size() == 1);
	return *projection.expressions[0];
}

static bool HasConstantChild(const Expression &expr, const Value &value) {
	for (auto &child : expr.Cast<BoundFunctionExpression>().GetChildren()) {
		if (child->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		    child->Cast<BoundConstantExpression>().GetValue() == value) {
			return true;
		}
	}
	return false;
}

TEST_CASE("Constant expressions with large results are not folded into values", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT i AS x FROM range(10) r(i)"));
	con.BeginTransaction();

	auto plan = con.ExtractPlan("SELECT list_reverse(range(3)) FROM t");
	REQUIRE(ProjectedExpression(*plan).GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);

	plan = con.ExtractPlan("SELECT range(1024)::BIGINT[1024] FROM t");
	REQUIRE(ProjectedExpression(*plan).GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);

	plan = con.ExtractPlan("SELECT list_reverse(range(1000000)) FROM t");
	auto &large = ProjectedExpression(*plan);
	REQUIRE(large.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION);
	REQUIRE(large.Cast<BoundFunctionExpression>().GetChildren()[0]->GetExpressionClass() ==
	        ExpressionClass::BOUND_FUNCTION);

	plan = con.ExtractPlan("SELECT x + len(list_reverse(range(1000000))) FROM t");
	auto &sum = ProjectedExpression(*plan);
	REQUIRE(sum.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION);
	REQUIRE(HasConstantChild(sum, Value::BIGINT(1000000)));
	con.Rollback();

	auto result = con.Query("SELECT sum(list_reverse(range(1000000))[x + 1]) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::HUGEINT(9999945)}));
	result = con.Query("SELECT len(list_resize([1], 10000000)), length(repeat('ab', 5000000))");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(10000000)}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(10000000)}));
}

static const Expression &LenArgument(const Expression &sum, idx_t i) {
	auto &len = sum.Cast<BoundFunctionExpression>().GetChildren()[i]->Cast<BoundFunctionExpression>();
	return *len.GetChildren()[0];
}

static idx_t RangeCalls(const Expression &expr) {
	idx_t count = expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	              expr.Cast<BoundFunctionExpression>().Function().GetName() == "range";
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { count += RangeCalls(child); });
	return count;
}

TEST_CASE("A repeated constant macro argument becomes a literal only when it is small", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE MACRO twice_len(l) AS len(l) + len(l)"));
	REQUIRE_NO_FAIL(con.Query("CREATE MACRO either_len(c, l) AS CASE WHEN c THEN len(l) ELSE -len(l) END"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_optimizer"));
	con.BeginTransaction();

	auto plan = con.ExtractPlan("SELECT twice_len(range(3))");
	auto &small = ProjectedExpression(*plan);
	REQUIRE(LenArgument(small, 0).GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);
	REQUIRE(LenArgument(small, 1).GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);

	for (auto count : {10000, 1000000}) {
		plan = con.ExtractPlan(StringUtil::Format("SELECT twice_len(range(%d))", count));
		REQUIRE(RangeCalls(ProjectedExpression(*plan)) == 1);
		plan = con.ExtractPlan(StringUtil::Format("SELECT either_len(true, range(%d))", count));
		REQUIRE(RangeCalls(ProjectedExpression(*plan)) > 0);
	}
	con.Rollback();
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_optimizer"));

	auto result = con.Query("SELECT twice_len(range(1000000)), either_len(false, range(10000))");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(2000000)}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(-10000)}));
}

TEST_CASE("Kernels that build one huge value stop at the query deadline", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT repeat('a', 100000000) AS a, repeat('ab', 30000000) AS ab, "
	                          "repeat('a,', 60000000) AS csv"));

	struct DeadlineQuery {
		idx_t timeout_ms;
		const char *sql;
	};
	const DeadlineQuery queries[] = {{100, "SELECT len(list_resize([true], n)) FROM (VALUES (2000000000)) t(n)"},
	                                 {100, "SELECT len(list_resize([true], 2000000000))"},
	                                 {10, "SELECT length(lpad('x', 1000000000, 'y'))"},
	                                 {10, "SELECT length(rpad('x', 1000000000, 'y'))"},
	                                 {100, "SELECT length(replace(a, 'a', 'bb')) FROM big"},
	                                 {100, "SELECT length(regexp_replace(a, 'a', 'b', 'g')) FROM big"},
	                                 {100, "SELECT len(regexp_extract_all(ab, 'a')) FROM big"},
	                                 {100, "SELECT len(string_split(csv, ',')) FROM big"}};
	for (auto &query : queries) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("SET max_execution_time = %d", query.timeout_ms)));
		auto start = std::chrono::steady_clock::now();
		auto result = con.Query(query.sql);
		auto elapsed = std::chrono::steady_clock::now() - start;
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "maximum execution time"));
		REQUIRE(elapsed < std::chrono::seconds(2));
	}
}
