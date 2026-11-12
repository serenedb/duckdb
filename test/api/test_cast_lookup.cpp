#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

static constexpr char FROM_PAIR[] = "from pair";
static constexpr char FROM_OTHER_PAIR[] = "from other pair";
static constexpr char FROM_ANY_STRUCT[] = "from any struct";
static constexpr char TO_PAIR_LIST[] = "to pair list";
static constexpr char TO_ANY_LIST[] = "to any list";
static constexpr char TO_PAIR[] = "to pair";
static constexpr char TO_ANY_STRUCT[] = "to any struct";

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

TEST_CASE("Casts registered from two aliases of one type stay apart", "[api][cast]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "cast_lookup");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	auto other_pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("other_pair");
	loader.RegisterType("pair", pair);
	loader.RegisterType("other_pair", other_pair);
	loader.RegisterCastFunction(pair, LogicalType::VARCHAR, Refuse<FROM_PAIR>);
	loader.RegisterCastFunction(other_pair, LogicalType::VARCHAR, Refuse<FROM_OTHER_PAIR>);
	loader.RegisterCastFunction(LogicalType::VARCHAR, pair, Refuse<FROM_PAIR>);
	loader.RegisterCastFunction(LogicalType::VARCHAR, other_pair, Refuse<FROM_OTHER_PAIR>);
	Connection con(db);

	RequireCast(con, "SELECT {'a': 1}::pair::VARCHAR", FROM_PAIR);
	RequireCast(con, "SELECT {'a': 1}::other_pair::VARCHAR", FROM_OTHER_PAIR);
	RequireCast(con, "SELECT 'x'::VARCHAR::pair", FROM_PAIR);
	RequireCast(con, "SELECT 'x'::VARCHAR::other_pair", FROM_OTHER_PAIR);
}

TEST_CASE("A type's casts fall back to the casts registered for its family", "[api][cast]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "cast_lookup");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	loader.RegisterType("pair", pair);
	loader.RegisterCastFunction(pair, LogicalType::VARCHAR, Refuse<FROM_PAIR>);
	loader.RegisterCastFunction(LogicalType::STRUCT({{"any", LogicalType::ANY}}), LogicalType::UUID,
	                            Refuse<FROM_ANY_STRUCT>);
	Connection con(db);

	RequireCast(con, "SELECT {'a': 1}::pair::VARCHAR", FROM_PAIR);
	RequireCast(con, "SELECT {'a': 1}::pair::UUID", FROM_ANY_STRUCT);
	RequireCast(con, "SELECT {'a': 1}::UUID", FROM_ANY_STRUCT);
}

TEST_CASE("A cast to a list of an aliased type is found next to the casts to other lists", "[api][cast]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "cast_lookup");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	loader.RegisterType("pair", pair);
	loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::LIST(pair), Refuse<TO_PAIR_LIST>);
	loader.RegisterCastFunction(LogicalType(LogicalTypeId::STRING_LITERAL), LogicalType::LIST(pair),
	                            Refuse<TO_PAIR_LIST>);
	Connection con(db);

	RequireCast(con, "SELECT 'x'::VARCHAR::pair[]", TO_PAIR_LIST);
	RequireCast(con, "SELECT 'x'::pair[]", TO_PAIR_LIST);
	auto result = con.Query("SELECT '[1, 2]'::VARCHAR::INTEGER[] = [1, 2], length('abc')");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(CHECK_COLUMN(result, 1, {3}));

	DuckDB any_list_db(nullptr);
	ExtensionLoader any_list_loader(*any_list_db.instance, "cast_lookup");
	any_list_loader.RegisterType("pair", pair);
	any_list_loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::LIST(pair), Refuse<TO_PAIR_LIST>);
	any_list_loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::ANY),
	                                     Refuse<TO_ANY_LIST>);
	Connection any_list_con(any_list_db);

	RequireCast(any_list_con, "SELECT 'x'::VARCHAR::pair[]", TO_PAIR_LIST);
	RequireCast(any_list_con, "SELECT '[1, 2]'::VARCHAR::INTEGER[]", TO_ANY_LIST);
}

TEST_CASE("A cast to an aliased type applies to that alias, a cast to its family to every alias", "[api][cast]") {
	DuckDB db(nullptr);
	ExtensionLoader loader(*db.instance, "cast_lookup");
	auto pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("pair");
	auto other_pair = LogicalType::STRUCT({{"a", LogicalType::INTEGER}}).WithAlias("other_pair");
	loader.RegisterType("pair", pair);
	loader.RegisterType("other_pair", other_pair);
	loader.RegisterCastFunction(LogicalType::VARCHAR, pair, Refuse<TO_PAIR>);
	Connection con(db);

	RequireCast(con, "SELECT '{''a'': 1}'::VARCHAR::pair", TO_PAIR);
	auto result = con.Query("SELECT ('{''a'': 1}'::VARCHAR::other_pair).a");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::STRUCT({{"any", LogicalType::ANY}}),
	                            Refuse<TO_ANY_STRUCT>);
	RequireCast(con, "SELECT '{''a'': 1}'::VARCHAR::pair", TO_PAIR);
	RequireCast(con, "SELECT '{''a'': 1}'::VARCHAR::other_pair", TO_ANY_STRUCT);
}
