#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

static void AliasOverload(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value("alias"), count_t(args.size()));
}

static void BaseOverload(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value("base"), count_t(args.size()));
}

TEST_CASE("An overload for an aliased type wins over the overload for its base type", "[api][scalar_function]") {
	DuckDB db(nullptr);
	auto id_type = LogicalType(LogicalTypeId::UBIGINT).WithAlias("id");
	ExtensionLoader loader(*db.instance, "alias_type_overload");
	loader.RegisterType("id", id_type);
	ScalarFunctionSet kind("kind");
	kind.AddFunction(ScalarFunction({LogicalType::UBIGINT}, LogicalType::VARCHAR, BaseOverload));
	kind.AddFunction(ScalarFunction({id_type}, LogicalType::VARCHAR, AliasOverload));
	loader.RegisterFunction(kind);
	loader.RegisterFunction(ScalarFunction("base_only", {LogicalType::UBIGINT}, LogicalType::VARCHAR, BaseOverload));
	Connection con(db);

	auto result = con.Query("SELECT kind(1::id), kind(1::UBIGINT), base_only(1::id)");
	REQUIRE(CHECK_COLUMN(result, 0, {"alias"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"base"}));
	REQUIRE(CHECK_COLUMN(result, 2, {"base"}));
}

TEST_CASE("A SQL function declared to return an aliased type returns that type", "[api][scalar_function]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "alias_type_result");
	loader.RegisterType("cents", LogicalType::DECIMAL(10, 2).WithAlias("cents"));
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION in_cents() RETURNS cents LANGUAGE sql AS 'SELECT 1.5'"));
	auto result = con.Query("SELECT in_cents()::VARCHAR, typeof(in_cents())");
	REQUIRE(CHECK_COLUMN(result, 0, {"1.50"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"cents"}));
}
