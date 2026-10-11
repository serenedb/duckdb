#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

struct RejectingScan {
	static constexpr idx_t ROWS = 2000;

	struct GlobalState : public GlobalTableFunctionState {
		GlobalState(vector<column_t> column_ids_p, vector<idx_t> projection_ids_p)
		    : column_ids(std::move(column_ids_p)), projection_ids(std::move(projection_ids_p)) {
		}

		vector<column_t> column_ids;
		vector<idx_t> projection_ids;
		idx_t next = 1;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
		return_types.emplace_back(LogicalType::INTEGER);
		names.emplace_back("id");
		return_types.emplace_back(LogicalType::INTEGER);
		names.emplace_back("a");
		return make_uniq<TableFunctionData>();
	}

	static unique_ptr<GlobalTableFunctionState> GlobalInit(ClientContext &context, TableFunctionInitInput &input) {
		return make_uniq<GlobalState>(input.column_ids, input.projection_ids);
	}

	static void Function(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
		auto &state = data.global_state->Cast<GlobalState>();
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, ROWS + 1 - state.next);
		const auto outputs = state.projection_ids.empty() ? state.column_ids.size() : state.projection_ids.size();
		for (idx_t output_idx = 0; output_idx < outputs; output_idx++) {
			const auto column =
			    state.column_ids[state.projection_ids.empty() ? output_idx : state.projection_ids[output_idx]];
			if (column > 1) {
				throw InternalException("Unexpected rejecting_scan column id");
			}
			auto values = FlatVector::ScatterWriter<int32_t>(output.data[output_idx]);
			for (idx_t row = 0; row < count; row++) {
				const auto id = state.next + row;
				values[row] = NumericCast<int32_t>(column == 0 ? id : id % 7);
			}
		}
		state.next += count;
		output.SetChildCardinality(count);
	}

	static TableFilterPushdown RejectAll(FunctionData &bind_data, idx_t column_idx, TableFilter &filter) {
		return TableFilterPushdown::Reject;
	}

	static unique_ptr<NodeStatistics> Cardinality(ClientContext &context, const FunctionData *bind_data) {
		return make_uniq<NodeStatistics>(ROWS);
	}

	static void Register(Connection &con) {
		con.BeginTransaction();
		TableFunction function("rejecting_scan", {}, Function, Bind, GlobalInit);
		function.projection_pushdown = true;
		function.filter_pushdown = true;
		function.filter_prune = true;
		function.supports_pushdown_filter = RejectAll;
		function.cardinality = Cardinality;
		CreateTableFunctionInfo info(function);
		Catalog::GetSystemCatalog(*con.context).CreateTableFunction(*con.context, info);
		con.Commit();
	}
};

TEST_CASE("A rejected table filter keeps the scan's output columns", "[tablefunction]") {
	DuckDB db(nullptr);
	Connection con(db);
	RejectingScan::Register(con);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE many AS SELECT i::INTEGER AS id FROM range(1, 20001) r(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE few AS SELECT i::INTEGER AS id FROM range(1, 11) r(i)"));

	SECTION("hash join, the scan builds") {
		auto result = con.Query("SELECT count(*) FROM many JOIN rejecting_scan() s ON s.id = many.id WHERE s.a = 3");
		REQUIRE(CHECK_COLUMN(result, 0, {286}));
	}
	SECTION("hash join, the scan probes") {
		auto result = con.Query("SELECT count(*) FROM few JOIN rejecting_scan() s ON s.id = few.id WHERE s.a = 3");
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
	}
	SECTION("left join") {
		auto result = con.Query(
		    "SELECT count(s.id) FROM many LEFT JOIN (SELECT * FROM rejecting_scan() WHERE a = 3) s ON s.id = many.id");
		REQUIRE(CHECK_COLUMN(result, 0, {286}));
	}
	SECTION("nested loop join") {
		auto result = con.Query("SELECT count(*) FROM few JOIN rejecting_scan() s ON s.id <> few.id WHERE s.a = 3");
		REQUIRE(CHECK_COLUMN(result, 0, {2858}));
	}
	SECTION("cross product") {
		auto result = con.Query("SELECT count(s.id) FROM few CROSS JOIN rejecting_scan() s WHERE s.a = 3");
		REQUIRE(CHECK_COLUMN(result, 0, {2860}));
	}
	SECTION("window") {
		auto result = con.Query(
		    "SELECT max(r) FROM (SELECT row_number() OVER (ORDER BY id) AS r FROM rejecting_scan() WHERE a = 3)");
		REQUIRE(CHECK_COLUMN(result, 0, {286}));
	}
	SECTION("partitioned window") {
		auto result = con.Query("SELECT count(*) FROM (SELECT rank() OVER (PARTITION BY id % 2 ORDER BY id) AS r "
		                        "FROM rejecting_scan() WHERE a = 3) WHERE r = 1");
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
	}
}
