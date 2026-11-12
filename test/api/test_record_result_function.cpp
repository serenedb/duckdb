#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

static LogicalType PairType() {
	return LogicalType::STRUCT({{"a", LogicalType::INTEGER}, {"b", LogicalType::VARCHAR}});
}

static void Pair(DataChunk &args, ExpressionState &, Vector &result) {
	result.Reference(Value::STRUCT({{"a", Value::INTEGER(1)}, {"b", Value("x")}}), count_t(args.size()));
}

TEST_CASE("A record function called in FROM yields its fields as columns", "[api][function]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "record_result");
	ScalarFunction record("record_pair", {}, PairType(), Pair);
	record.GetSignature().SetRecordResult();
	loader.RegisterFunction(std::move(record));
	loader.RegisterFunction(ScalarFunction("struct_pair", {}, PairType(), Pair));
	Connection con(db);

	auto result = con.Query("SELECT * FROM record_pair()");
	REQUIRE(result->ColumnCount() == 2);
	REQUIRE(result->ColumnName(0) == "a");
	REQUIRE(result->ColumnName(1) == "b");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {"x"}));

	result = con.Query("SELECT record_pair()");
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->GetTypes()[0] == PairType());

	result = con.Query("SELECT * FROM struct_pair()");
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->GetTypes()[0] == PairType());

	result = con.Query("SELECT * FROM struct_pack(a := 1, b := 'x')");
	REQUIRE(result->ColumnCount() == 1);
}
