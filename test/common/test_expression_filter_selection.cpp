////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"

#include <functional>

using namespace duckdb; // NOLINT

namespace {

unique_ptr<Expression> Column() {
	return make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, idx_t(0));
}

unique_ptr<Expression> Constant(int64_t value) {
	return make_uniq<BoundConstantExpression>(Value::BIGINT(value));
}

unique_ptr<Expression> ColumnEquals(int64_t value) {
	return BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, Column(), Constant(value));
}

unique_ptr<Expression> InList(const vector<int64_t> &values) {
	auto in = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
	in->GetChildrenMutable().push_back(Column());
	for (auto value : values) {
		in->GetChildrenMutable().push_back(Constant(value));
	}
	return std::move(in);
}

void CheckFilter(ClientContext &context, Vector &values, unique_ptr<Expression> expr,
                 const std::function<bool(int64_t)> &holds) {
	const ExpressionFilter filter(std::move(expr));
	auto state = TableFilterState::Initialize(context, filter);
	auto data = FlatVector::GetData<int64_t>(values);
	auto &validity = FlatVector::Validity(values);
	for (idx_t count : {idx_t(STANDARD_VECTOR_SIZE), idx_t(1000)}) {
		for (idx_t every : {idx_t(1), idx_t(3)}) {
			INFO(filter.expr->ToString() << ", " << count << " rows, every " << every);
			SelectionVector sel;
			idx_t offered = count;
			if (every != 1) {
				sel.Initialize(count);
				offered = 0;
				for (idx_t row = 0; row < count; row += every) {
					sel.set_index(offered++, row);
				}
			}
			idx_t approved = offered;
			const auto returned = ColumnSegment::FilterSelection(sel, values, *state, count, approved);
			REQUIRE(returned == approved);
			idx_t next = 0;
			for (idx_t row = 0; row < count; row += every) {
				if (validity.RowIsValid(row) && holds(data[row])) {
					REQUIRE(next < approved);
					REQUIRE(sel.get_index(next++) == row);
				}
			}
			REQUIRE(next == approved);
		}
	}
}

} // namespace

TEST_CASE("Expression filters select over an unset and a set selection alike", "[filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	Vector values(LogicalType::BIGINT, STANDARD_VECTOR_SIZE);
	auto data = FlatVector::GetDataMutable<int64_t>(values);
	auto &validity = FlatVector::ValidityMutable(values);
	for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
		data[row] = static_cast<int64_t>(row % 10);
		if (row % 7 == 0) {
			validity.SetInvalid(row);
		}
	}
	FlatVector::SetSize(values, STANDARD_VECTOR_SIZE);
	auto &context = *con.context;

	CheckFilter(context, values, InList({1, 7, 42}), [](int64_t v) { return v == 1 || v == 7; });
	auto not_in = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	not_in->GetChildrenMutable().push_back(InList({1, 7}));
	CheckFilter(context, values, std::move(not_in), [](int64_t v) { return v != 1 && v != 7; });
	CheckFilter(context, values,
	            make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, ColumnEquals(2), ColumnEquals(9)),
	            [](int64_t v) { return v == 2 || v == 9; });
	CheckFilter(context, values, InList({42, 43}), [](int64_t) { return false; });
}

TEST_CASE("Expression filters offered no row select none", "[filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	Vector values(LogicalType::BIGINT, STANDARD_VECTOR_SIZE);
	auto data = FlatVector::GetDataMutable<int64_t>(values);
	for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
		data[row] = static_cast<int64_t>(row % 10);
	}
	FlatVector::SetSize(values, STANDARD_VECTOR_SIZE);
	auto &context = *con.context;

	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(ColumnEquals(3));
	expressions.push_back(make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND,
	    BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Column(), Constant(2)),
	    BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHANOREQUALTO, Column(), Constant(5))));
	expressions.push_back(InList({1, 7}));
	for (auto &expr : expressions) {
		const ExpressionFilter filter(std::move(expr));
		auto state = TableFilterState::Initialize(context, filter);
		INFO(filter.expr->ToString());
		REQUIRE(state->Cast<ExpressionFilterState>().fast_executor.get() != nullptr);
		SelectionVector sel;
		idx_t approved = 0;
		REQUIRE(ColumnSegment::FilterSelection(sel, values, *state, STANDARD_VECTOR_SIZE, approved) == 0);
		REQUIRE(approved == 0);
		REQUIRE(!sel.IsSet());
	}
}
