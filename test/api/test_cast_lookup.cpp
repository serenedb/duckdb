#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

static constexpr char FROM_VARCHAR_LIST[] = "from VARCHAR[]";
static constexpr char FROM_ANY_LIST[] = "from any list";
static constexpr char FROM_ANY_TYPE[] = "from any type";
static constexpr char FROM_INTEGER[] = "from INTEGER";
static constexpr char FROM_DATE[] = "from DATE";
static constexpr char FROM_DATE_TO_ANY_TYPE[] = "from DATE to any type";

template <const char *MESSAGE>
static BoundCastInfo Refuse(BindCastInput &, const LogicalType &, const LogicalType &) {
	throw ConversionException(MESSAGE);
}

static void RequireCast(Connection &con, const string &query, const string &message) {
	auto result = con.Query(query);
	INFO(query);
	REQUIRE(result->HasError());
	INFO(result->GetError());
	REQUIRE(StringUtil::Contains(result->GetError(), message));
}

TEST_CASE("A cast registered for a family of types applies where a member's own casts miss the target", "[api][cast]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "cast_lookup");
	loader.RegisterType("bytes", LogicalType(LogicalTypeId::BLOB).WithAlias("bytes"));
	loader.RegisterCastFunction(LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BLOB, Refuse<FROM_VARCHAR_LIST>);
	loader.RegisterCastFunction(LogicalType::LIST(LogicalType::ANY), LogicalType::UUID, Refuse<FROM_ANY_LIST>);
	loader.RegisterCastFunction(LogicalType::ANY, LogicalType::UUID, Refuse<FROM_ANY_TYPE>);
	loader.RegisterCastFunction(LogicalType::INTEGER, LogicalType::BLOB, Refuse<FROM_INTEGER>);
	loader.RegisterCastFunction(LogicalType::DATE, LogicalType::BLOB, Refuse<FROM_DATE>);
	loader.RegisterCastFunction(LogicalType::DATE, LogicalType::ANY, Refuse<FROM_DATE_TO_ANY_TYPE>);
	Connection con(db);

	RequireCast(con, "SELECT ['a']::BLOB", FROM_VARCHAR_LIST);
	RequireCast(con, "SELECT ['a']::UUID", FROM_ANY_LIST);
	RequireCast(con, "SELECT [1]::UUID", FROM_ANY_LIST);
	RequireCast(con, "SELECT 1::INTEGER::BLOB", FROM_INTEGER);
	RequireCast(con, "SELECT 1::INTEGER::UUID", FROM_ANY_TYPE);
	RequireCast(con, "SELECT true::UUID", FROM_ANY_TYPE);
	RequireCast(con, "SELECT DATE '2026-10-15'::BLOB", FROM_DATE);
	RequireCast(con, "SELECT DATE '2026-10-15'::bytes", FROM_DATE_TO_ANY_TYPE);
	RequireCast(con, "SELECT DATE '2026-10-15'::UUID", FROM_DATE_TO_ANY_TYPE);
}

static constexpr char FROM_PAIR[] = "from pair";

static void IsPair(DataChunk &args, ExpressionState &, Vector &result) {
	auto writer = FlatVector::Writer<bool>(result, args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		writer.WriteValue(true);
	}
}

TEST_CASE("A cast registered from a type does not stop the type's own values from binding", "[api][cast]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "cast_lookup");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	loader.RegisterType("pair", pair);
	loader.RegisterCastFunction(pair, LogicalType::STRUCT({{"any", LogicalType::ANY}}), Refuse<FROM_PAIR>);
	loader.RegisterFunction(ScalarFunction("is_pair", {pair}, LogicalType::BOOLEAN, IsPair));
	Connection con(db);

	auto result = con.Query("SELECT is_pair({'a': 1}::pair)");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	RequireCast(con, "SELECT {'a': 1}::pair::STRUCT(b INTEGER)", FROM_PAIR);
}
