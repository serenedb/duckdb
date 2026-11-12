#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct EvenScanData : public TableFunctionData {
	bool even = false;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<EvenScanData>();
		copy->even = even;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other) const override {
		return even == other.Cast<EvenScanData>().even;
	}
};

struct EvenScanState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> EvenScanBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &types,
                                      vector<Identifier> &names) {
	types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	return make_uniq<EvenScanData>();
}

unique_ptr<GlobalTableFunctionState> EvenScanInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<EvenScanState>();
}

void EvenScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<EvenScanState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	state.done = true;
	const auto even = input.bind_data->Cast<EvenScanData>().even;
	vector<int32_t> values;
	for (int32_t value = 1; value <= 4; value++) {
		if (!even || value % 2 == 0) {
			values.push_back(value);
		}
	}
	auto writer = FlatVector::Writer<int32_t>(output.data[0], values.size());
	for (auto value : values) {
		writer.WriteValue(value);
	}
	output.SetChildCardinality(values.size());
}

void EvenScanFilters(ClientContext &, LogicalGet &, FunctionData *bind_data, vector<unique_ptr<Expression>> &filters) {
	for (idx_t i = 0; i < filters.size(); i++) {
		auto &filter = *filters[i];
		if (filter.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
		    filter.Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName() != "is_even") {
			continue;
		}
		bind_data->Cast<EvenScanData>().even = true;
		filters.erase_at(i);
		i--;
	}
}

void IsEven(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<int32_t, bool>(args.data[0], result, [](int32_t value) { return value % 2 == 0; });
}

string Explain(Connection &con, const string &query) {
	auto result = con.Query("EXPLAIN " + query);
	REQUIRE_NO_FAIL(*result);
	string plan;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		for (idx_t col = 0; col < result->ColumnCount(); col++) {
			plan += result->Collection().GetValue(col, row).ToString();
		}
	}
	return plan;
}

} // namespace

TEST_CASE("Filter pullup keeps a filter its scan takes below INTERSECT and EXCEPT", "[optimizer][filter_pullup]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "pullup_filters_scan");
	TableFunction even_scan("even_scan", {}, EvenScan, EvenScanBind, EvenScanInit);
	even_scan.pushdown_complex_filter = EvenScanFilters;
	loader.RegisterFunction(even_scan);
	loader.RegisterFunction(ScalarFunction("is_even", {LogicalType::INTEGER}, LogicalType::BOOLEAN, IsEven));
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers AS SELECT * FROM (VALUES (2), (3)) t(i)"));

	vector<std::pair<string, int32_t>> queries {
	    {"SELECT i FROM even_scan() WHERE is_even(i) EXCEPT SELECT i FROM numbers", 4},
	    {"SELECT i FROM even_scan() WHERE is_even(i) INTERSECT SELECT i FROM numbers", 2},
	    {"SELECT i FROM numbers INTERSECT SELECT i FROM even_scan() WHERE is_even(i)", 2},
	    {"SELECT i FROM (SELECT i FROM even_scan()) s WHERE is_even(i) EXCEPT ALL SELECT i FROM numbers", 4}};
	for (auto &query : queries) {
		INFO(query.first);
		auto result = con.Query(query.first);
		REQUIRE(CHECK_COLUMN(result, 0, {query.second}));
		REQUIRE_FALSE(StringUtil::Contains(Explain(con, query.first), "is_even"));
	}
}
