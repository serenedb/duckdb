#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"

using namespace duckdb;

TEST_CASE("Kernels that build one huge value stop at the query deadline", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT repeat('a', 100000000) AS a, repeat('ab', 30000000) AS ab, "
	                          "repeat('a,', 60000000) AS csv"));

	struct DeadlineQuery {
		idx_t timeout_ms;
		const char *sql;
	};
	const DeadlineQuery queries[] = {{100, "SELECT len(list_resize([true], n)) FROM (VALUES (2000000000)) t(n)"},
	                                 {100, "SELECT len(list_resize([true], 2000000000))"},
	                                 {10, "SELECT length(lpad('x', 1000000000, 'y'))"},
	                                 {10, "SELECT length(rpad('x', 1000000000, 'y'))"},
	                                 {100, "SELECT length(replace(a, 'a', 'bb')) FROM big"},
	                                 {100, "SELECT length(regexp_replace(a, 'a', 'b', 'g')) FROM big"},
	                                 {100, "SELECT len(regexp_extract_all(ab, 'a')) FROM big"},
	                                 {100, "SELECT len(string_split(csv, ',')) FROM big"}};
	for (auto &query : queries) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("SET max_execution_time = %d", query.timeout_ms)));
		auto start = std::chrono::steady_clock::now();
		auto result = con.Query(query.sql);
		auto elapsed = std::chrono::steady_clock::now() - start;
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "maximum execution time"));
		REQUIRE(elapsed < std::chrono::seconds(2));
	}
}
