#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

static void UpperPair(DataChunk &args, ExpressionState &, Vector &result) {
	auto writer = FlatVector::Writer<string_t>(result, args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		writer.WriteValue(string_t("pair"));
	}
}

static void UpperInteger(DataChunk &args, ExpressionState &, Vector &result) {
	auto writer = FlatVector::Writer<string_t>(result, args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		writer.WriteValue(string_t("integer"));
	}
}

TEST_CASE("A type-scoped overload binds only calls with an argument of its own type", "[api][function]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "type_scoped");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	loader.RegisterType("pair", pair);
	ScalarFunction upper("upper", {pair}, LogicalType::VARCHAR, UpperPair);
	upper.GetSignature().SetTypeScoped();
	loader.AddFunctionOverload(std::move(upper));
	Connection con(db);

	auto result = con.Query("SELECT upper({'a': 1}::pair), upper('x'), upper(NULL)");
	REQUIRE(CHECK_COLUMN(result, 0, {"pair"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"X"}));
	REQUIRE(CHECK_COLUMN(result, 2, {Value()}));
	REQUIRE(result->GetTypes()[2] == LogicalType::VARCHAR);

	auto prepared = con.Prepare("SELECT upper($1)");
	REQUIRE(!prepared->HasError());
	LogicalType parameter;
	REQUIRE(prepared->TryGetParameterType("1", parameter));
	REQUIRE(parameter == LogicalType::VARCHAR);

	result = con.Query("SELECT upper(1)");
	REQUIRE(result->HasError());
	REQUIRE(!StringUtil::Contains(result->GetError(), "pair"));
}

TEST_CASE("An overload added after a type-scoped one binds calls without an argument of that type", "[api][function]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "type_scoped");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	loader.RegisterType("pair", pair);
	ScalarFunction scoped("upper", {pair}, LogicalType::VARCHAR, UpperPair);
	scoped.GetSignature().SetTypeScoped();
	loader.AddFunctionOverload(std::move(scoped));
	loader.AddFunctionOverload(ScalarFunction("upper", {LogicalType::INTEGER}, LogicalType::VARCHAR, UpperInteger));
	Connection con(db);

	auto result = con.Query("SELECT upper(1), upper({'a': 1}::pair), upper('x')");
	REQUIRE(CHECK_COLUMN(result, 0, {"integer"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"pair"}));
	REQUIRE(CHECK_COLUMN(result, 2, {"X"}));
}
