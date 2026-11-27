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
#include "duckdb/common/types/interval.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"

#include <functional>
#include <string>

using namespace duckdb; // NOLINT

namespace {

constexpr idx_t IN_ROWS = STANDARD_VECTOR_SIZE;

unique_ptr<Expression> InList(const LogicalType &type, const vector<Value> &constants) {
	auto in = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
	in->GetChildrenMutable().push_back(make_uniq<BoundReferenceExpression>(type, idx_t(0)));
	for (auto &constant : constants) {
		in->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(constant));
	}
	return std::move(in);
}

void CheckInList(ClientContext &context, Vector &values, const vector<Value> &constants) {
	const ExpressionFilter filter(InList(values.GetType(), constants));
	auto state = TableFilterState::Initialize(context, filter);
	REQUIRE(state->Cast<ExpressionFilterState>().fast_executor.get() != nullptr);
	for (idx_t count : {IN_ROWS, idx_t(1000)}) {
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
				const auto value = values.GetValue(row);
				bool listed = false;
				for (auto &constant : constants) {
					listed =
					    listed || (!value.IsNull() && !constant.IsNull() && Value::NotDistinctFrom(value, constant));
				}
				if (listed) {
					REQUIRE(next < approved);
					REQUIRE(sel.get_index(next++) == row);
				}
			}
			REQUIRE(next == approved);
		}
	}
}

void CheckType(ClientContext &context, const LogicalType &type, const std::function<Value(idx_t)> &value_of) {
	Vector values(type, IN_ROWS);
	for (idx_t row = 0; row < IN_ROWS; row++) {
		values.SetValue(row, row % 7 == 0 ? Value(type) : value_of(row % 10));
	}
	CheckInList(context, values, {value_of(1), value_of(4), value_of(7)});
	CheckInList(context, values, {value_of(2), Value(type), value_of(9)});
	CheckInList(context, values, {value_of(3)});
	CheckInList(context, values, {Value(type), Value(type)});
	if (type.id() != LogicalTypeId::BOOLEAN) {
		CheckInList(context, values, {value_of(10), value_of(11)});
	}
}

} // namespace

TEST_CASE("IN lists of constants select through their filter executor", "[filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	const auto cast = [](const LogicalType &type) {
		return [type](idx_t k) {
			return Value::BIGINT(static_cast<int64_t>(k)).DefaultCastAs(type);
		};
	};
	for (auto &type : {LogicalType::TINYINT, LogicalType::SMALLINT, LogicalType::INTEGER, LogicalType::BIGINT,
	                   LogicalType::HUGEINT, LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::UINTEGER,
	                   LogicalType::UBIGINT, LogicalType::UHUGEINT, LogicalType::FLOAT, LogicalType::DOUBLE}) {
		CheckType(context, type, cast(type));
	}
	CheckType(context, LogicalType::BOOLEAN, [](idx_t k) { return Value::BOOLEAN(k % 2 == 1); });
	CheckType(context, LogicalType::INTERVAL, [](idx_t k) {
		return Value::INTERVAL(Interval::FromMicro(static_cast<int64_t>(k) * Interval::MICROS_PER_SEC));
	});
	CheckType(context, LogicalType::VARCHAR, [](idx_t k) {
		return Value(k % 2 == 0 ? "v" + std::to_string(k) : "a value longer than twelve bytes " + std::to_string(k));
	});
}

TEST_CASE("IN lists of more than four strings keep the expression executor", "[filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	const auto fast = [&](const LogicalType &type, idx_t count) {
		vector<Value> constants;
		for (idx_t k = 0; k < count; k++) {
			constants.push_back(Value::BIGINT(static_cast<int64_t>(k)).DefaultCastAs(type));
		}
		const ExpressionFilter filter(InList(type, constants));
		return TableFilterState::Initialize(context, filter)->Cast<ExpressionFilterState>().fast_executor != nullptr;
	};
	REQUIRE(fast(LogicalType::VARCHAR, 4));
	REQUIRE(!fast(LogicalType::VARCHAR, 5));
	REQUIRE(fast(LogicalType::BIGINT, 32));
	REQUIRE(fast(LogicalType::DOUBLE, 32));
}

TEST_CASE("IN lists keep every offered row one constant matches", "[filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	Vector values(LogicalType::BIGINT, IN_ROWS);
	vector<idx_t> fours;
	for (idx_t row = 0; row < IN_ROWS; row++) {
		values.SetValue(row, row % 7 == 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(static_cast<int64_t>(row % 10)));
		if (row % 7 != 0 && row % 10 == 4) {
			fours.push_back(row);
		}
	}
	Vector all_fours(LogicalType::BIGINT, IN_ROWS);
	for (idx_t row = 0; row < IN_ROWS; row++) {
		all_fours.SetValue(row, Value::BIGINT(4));
	}
	for (auto &constants :
	     {vector<Value> {Value::BIGINT(4), Value::BIGINT(7)}, vector<Value> {Value::BIGINT(9), Value::BIGINT(4)}}) {
		const ExpressionFilter filter(InList(LogicalType::BIGINT, constants));
		auto state = TableFilterState::Initialize(context, filter);
		REQUIRE(state->Cast<ExpressionFilterState>().fast_executor.get() != nullptr);
		INFO(filter.expr->ToString());
		SelectionVector sel(IN_ROWS);
		for (idx_t i = 0; i < fours.size(); i++) {
			sel.set_index(i, fours[i]);
		}
		idx_t approved = fours.size();
		REQUIRE(ColumnSegment::FilterSelection(sel, values, *state, IN_ROWS, approved) == fours.size());
		REQUIRE(approved == fours.size());
		for (idx_t i = 0; i < fours.size(); i++) {
			REQUIRE(sel.get_index(i) == fours[i]);
		}
		SelectionVector every_row;
		approved = IN_ROWS;
		REQUIRE(ColumnSegment::FilterSelection(every_row, all_fours, *state, IN_ROWS, approved) == IN_ROWS);
		REQUIRE(approved == IN_ROWS);
		REQUIRE(!every_row.IsSet());
	}
}

TEST_CASE("IN lists the fast executor cannot compare keep the expression executor", "[filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	const auto fast = [&](unique_ptr<Expression> expr) {
		const ExpressionFilter filter(std::move(expr));
		return TableFilterState::Initialize(context, filter)->Cast<ExpressionFilterState>().fast_executor != nullptr;
	};
	REQUIRE(fast(InList(LogicalType::BIGINT, {Value::BIGINT(1), Value::BIGINT(2)})));
	REQUIRE(!fast(InList(LogicalType::BIGINT, {})));
	REQUIRE(!fast(InList(LogicalType::BIGINT, {Value::BIGINT(1), Value::INTEGER(2)})));
	REQUIRE(!fast(CreateSelectivityOptionalFilterExpression(
	    InList(LogicalType::BIGINT, {Value::BIGINT(1), Value::BIGINT(2)}), LogicalType::BIGINT, 0.5f, idx_t(6))));
}
