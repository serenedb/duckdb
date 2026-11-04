#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/table_filter_state.hpp"

using namespace duckdb;

namespace {

unique_ptr<Expression> ColumnBetween(Value lower, Value upper, bool lower_inclusive, bool upper_inclusive) {
	return BoundBetweenExpression::Create(make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0U),
	                                      make_uniq<BoundConstantExpression>(std::move(lower)),
	                                      make_uniq<BoundConstantExpression>(std::move(upper)), lower_inclusive,
	                                      upper_inclusive);
}

vector<idx_t> Selected(ExpressionFilterExecutor &executor, Vector &values, idx_t count) {
	SelectionVector sel;
	idx_t approved = count;
	executor.FilterSelection(sel, values, count, approved);
	vector<idx_t> rows;
	for (idx_t i = 0; i < approved; i++) {
		rows.push_back(sel.IsSet() ? sel.get_index(i) : i);
	}
	return rows;
}

} // namespace

// A column filter kept as `column BETWEEN constant AND constant` (an index scan's column form of a range) runs as
// two constant comparisons instead of through the expression executor, and selects the same rows.
TEST_CASE("BETWEEN over a column and two constants filters with the fast executor", "[filter][between]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	constexpr idx_t COUNT = 10;
	constexpr idx_t NULL_ROW = 5;
	Vector values(LogicalType::INTEGER, COUNT);
	auto data = FlatVector::GetDataMutable<int32_t>(values);
	for (idx_t i = 0; i < COUNT; i++) {
		data[i] = NumericCast<int32_t>(i);
	}
	FlatVector::SetSize(values, count_t(COUNT));
	FlatVector::SetNull(values, NULL_ROW, true);

	for (auto lower_inclusive : {true, false}) {
		for (auto upper_inclusive : {true, false}) {
			auto expr = ColumnBetween(Value::INTEGER(3), Value::INTEGER(7), lower_inclusive, upper_inclusive);
			ExpressionFilterState state(context, *expr);
			REQUIRE(state.fast_executor);
			vector<idx_t> expected;
			for (idx_t i = 0; i < COUNT; i++) {
				const bool above = lower_inclusive ? i >= 3 : i > 3;
				const bool below = upper_inclusive ? i <= 7 : i < 7;
				if (above && below && i != NULL_ROW) {
					expected.push_back(i);
				}
			}
			REQUIRE(Selected(*state.fast_executor, values, COUNT) == expected);
		}
	}

	auto null_bound = ColumnBetween(Value::INTEGER(3), Value(LogicalType::INTEGER), true, true);
	ExpressionFilterState null_state(context, *null_bound);
	REQUIRE(null_state.fast_executor);
	REQUIRE(Selected(*null_state.fast_executor, values, COUNT).empty());

	auto wider_bound = ColumnBetween(Value::BIGINT(3), Value::INTEGER(7), true, true);
	REQUIRE(!ExpressionFilterState(context, *wider_bound).fast_executor);

	auto column_bound =
	    BoundBetweenExpression::Create(make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0U),
	                                   make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0U),
	                                   make_uniq<BoundConstantExpression>(Value::INTEGER(7)), true, true);
	REQUIRE(!ExpressionFilterState(context, *column_bound).fast_executor);
}
