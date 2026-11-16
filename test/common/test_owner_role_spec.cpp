#include "catch.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_tablespace_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"

namespace duckdb {

namespace {

unique_ptr<SQLStatement> ParseOwnerStatement(const string &query) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	return std::move(parser.statements[0]);
}

string SubscriptionOwner(const string &role) {
	auto statement = ParseOwnerStatement("ALTER SUBSCRIPTION sub OWNER TO " + role);
	return statement->Cast<PragmaStatement>().info->parameters[2]->Cast<ConstantExpression>().GetLiteral().text;
}

string TablespaceOwner(const string &role) {
	auto statement = ParseOwnerStatement("CREATE TABLESPACE ts OWNER " + role + " LOCATION '/tmp'");
	return statement->Cast<CreateStatement>().info->Cast<CreateTablespaceInfo>().owner.GetIdentifierName();
}

string TableOwner(const string &role) {
	auto statement = ParseOwnerStatement("ALTER TABLE tbl OWNER TO " + role);
	return statement->Cast<AlterStatement>().info->Cast<AlterPermissionsInfo>().new_owner;
}

} // namespace

TEST_CASE("An owner is the role named as written, or the session role in upper case", "[parser]") {
	for (auto owner : {SubscriptionOwner, TablespaceOwner, TableOwner}) {
		REQUIRE(owner("reporter") == "reporter");
		REQUIRE(owner("\"Reporter\"") == "Reporter");
		REQUIRE(owner("admin") == "admin");
		REQUIRE(owner("current_user") == "CURRENT_USER");
		REQUIRE(owner("SESSION_USER") == "SESSION_USER");
		REQUIRE(owner("current_role") == "CURRENT_ROLE");
	}
}

} // namespace duckdb
