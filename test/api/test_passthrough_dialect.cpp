#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/statement/passthrough_statement.hpp"

using namespace duckdb;

namespace {

//! Parse with the grammar used while CONNECT-ed
vector<unique_ptr<SQLStatement>> ParsePassthrough(const string &query) {
	ParserOptions options;
	options.grammar = CompiledGrammar::Passthrough();
	Parser parser(options);
	parser.ParseQuery(query);
	return std::move(parser.statements);
}

} // namespace

TEST_CASE("The passthrough grammar does not interpret statements", "[api][passthrough]") {
	// syntax DuckDB does not have parses fine - it is meant for the remote
	for (auto &query : vector<string> {"CREATE GRANT;", "GRANT ALL PRIVILEGES ON DATABASE pg TO bob;", "SELECT 42;",
	                                   "CREATE TABLE t (i INTEGER);"}) {
		auto statements = ParsePassthrough(query);
		REQUIRE(statements.size() == 1);
		REQUIRE(statements[0]->type == StatementType::PASSTHROUGH_STATEMENT);
		// the statement carries its own source text, terminator included
		REQUIRE(statements[0]->query == query);
	}
}

TEST_CASE("The passthrough grammar still interprets DISCONNECT", "[api][passthrough]") {
	auto statements = ParsePassthrough("DISCONNECT;");
	REQUIRE(statements.size() == 1);
	REQUIRE(statements[0]->type == StatementType::DISCONNECT_STATEMENT);
}

TEST_CASE("The passthrough grammar splits statements like DuckDB does", "[api][passthrough]") {
	auto statements = ParsePassthrough("CREATE GRANT; DISCONNECT; SELECT 'a;b';");
	REQUIRE(statements.size() == 3);
	REQUIRE(statements[0]->type == StatementType::PASSTHROUGH_STATEMENT);
	REQUIRE(statements[1]->type == StatementType::DISCONNECT_STATEMENT);
	// the ';' inside the string literal is not a boundary
	REQUIRE(statements[2]->type == StatementType::PASSTHROUGH_STATEMENT);
	REQUIRE(StringUtil::Contains(statements[2]->query, "'a;b'"));
}
