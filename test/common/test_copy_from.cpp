#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/formats/copy_from.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "test_helpers.hpp"

#include <fstream>

using namespace duckdb;

namespace {

template <CopyFormat FORMAT>
unique_ptr<FunctionData> BindPostgresCopy(ClientContext &context, CopyFromFunctionBindInput &input,
                                          vector<Identifier> &names, vector<LogicalType> &types) {
	return PostgresCopyFromBind(context, input, names, types, FORMAT);
}

template <CopyFormat FORMAT>
CopyFunction PostgresCopyFunction(const char *name) {
	CopyFunction function {Identifier(name)};
	function.copy_from_bind = BindPostgresCopy<FORMAT>;
	function.copy_from_function = ReadCSVTableFunction::GetFunction();
	return function;
}

void RegisterPostgresCopy(DuckDB &db) {
	auto &instance = *db.instance;
	ExtensionLoader loader(instance, "postgres_copy_test");
	loader.RegisterFunction(PostgresCopyFunction<CopyFormat::TEXT>("text"));
	auto &catalog = Catalog::GetSystemCatalog(instance);
	auto transaction = CatalogTransaction::GetSystemTransaction(instance);
	CreateCopyFunctionInfo info(PostgresCopyFunction<CopyFormat::CSV>("csv"));
	info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	info.SetQualifiedName(
	    QualifiedName(info.GetQualifiedName().Catalog(), *catalog.GetDefaultSchema(), info.GetQualifiedName().Name()));
	catalog.CreateCopyFunction(transaction, info);
}

string WriteInput(const string &name, const string &bytes) {
	const auto path = TestCreatePath(name);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return path;
}

struct CopyFailure {
	string message;
	string sqlstate;
	string context;
};

CopyFailure FailedCopy(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	REQUIRE(result->HasError());
	auto &error = result->GetErrorObject();
	const auto extra = [&](const char *key) {
		const auto entry = error.ExtraInfo().find(key);
		return entry == error.ExtraInfo().end() ? string() : entry->second;
	};
	return {error.RawMessage(), extra("sqlstate"), extra("context")};
}

class RecordedNotices : public CopyNotices {
public:
	void Notice(const string &message) override {
		messages.push_back(message);
	}

	vector<string> messages;
};

} // namespace

TEST_CASE("COPY in csv mode ignores DuckDB's read options at their defaults and reads others with DuckDB", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE quoted (a VARCHAR, b VARCHAR)"));
	const auto copy = "COPY quoted FROM '" + WriteInput("copy_from_duckdb_defaults.csv", "\"\",\n") + "' (FORMAT csv, ";
	REQUIRE_NO_FAIL(con.Query(copy + "allow_quoted_nulls false, comment '', skip 0)"));
	auto result = con.Query("SELECT a IS NULL, length(a), b IS NULL FROM quoted");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BOOLEAN(false)}));
	REQUIRE(CHECK_COLUMN(result, 1, {0}));
	REQUIRE(CHECK_COLUMN(result, 2, {Value::BOOLEAN(true)}));
	for (const auto options : {"allow_quoted_nulls true)", "allow_quoted_nulls 2)"}) {
		INFO(options);
		REQUIRE_NO_FAIL(con.Query("DELETE FROM quoted"));
		REQUIRE_NO_FAIL(con.Query(copy + options));
		result = con.Query("SELECT a IS NULL, b IS NULL FROM quoted");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BOOLEAN(true)}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::BOOLEAN(true)}));
	}
}

TEST_CASE("COPY in csv mode hands DuckDB's reader the delimiter under any of its names", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE piped (a VARCHAR, b VARCHAR)"));
	const auto path = WriteInput("copy_from_piped.csv", "1|x\n2|y\n");
	for (const auto delimiter : {"DELIMITER", "SEP"}) {
		INFO(delimiter);
		REQUIRE_NO_FAIL(con.Query("DELETE FROM piped"));
		REQUIRE_NO_FAIL(
		    con.Query("COPY piped FROM '" + path + "' (FORMAT csv, allow_quoted_nulls true, " + delimiter + " '|')"));
		auto result = con.Query("SELECT a, b FROM piped ORDER BY a");
		REQUIRE(CHECK_COLUMN(result, 0, {"1", "2"}));
		REQUIRE(CHECK_COLUMN(result, 1, {"x", "y"}));
	}
}

TEST_CASE("COPY matches a FORCE_NOT_NULL column to the table's column of another case", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE forced (a VARCHAR, b VARCHAR)"));
	const auto path = WriteInput("copy_from_forced.csv", ",x\n");
	REQUIRE_NO_FAIL(con.Query("COPY forced FROM '" + path + "' (FORMAT csv, FORCE_NOT_NULL (A))"));
	auto result = con.Query("SELECT a IS NULL, length(a), b FROM forced");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BOOLEAN(false)}));
	REQUIRE(CHECK_COLUMN(result, 1, {0}));
	REQUIRE(CHECK_COLUMN(result, 2, {"x"}));
}

TEST_CASE("COPY puts the column default where the DEFAULT marker stands in a column it parses in place", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE defaulted (a INTEGER DEFAULT 7, b VARCHAR)"));
	const auto path = WriteInput("copy_from_defaulted.txt", "D\tx\n5\ty\n12\tz\n");
	auto result = con.Query("COPY defaulted FROM '" + path + "' (FORMAT text, DEFAULT 'D', RENDERING duckdb)");
	REQUIRE(CHECK_COLUMN(result, 0, {3}));
	result = con.Query("SELECT a, b FROM defaulted ORDER BY b");
	REQUIRE(CHECK_COLUMN(result, 0, {7, 5, 12}));
	REQUIRE(CHECK_COLUMN(result, 1, {"x", "y", "z"}));
}

TEST_CASE("COPY with HEADER MATCH reads an empty file as no rows", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE matched (a VARCHAR, b VARCHAR)"));
	const auto path = WriteInput("copy_from_header_empty.csv", "");
	auto result = con.Query("COPY matched FROM '" + path + "' (FORMAT csv, HEADER MATCH)");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("COPY with HEADER MATCH compares a header field that is the DEFAULT marker as it is written", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE matched (a VARCHAR, b VARCHAR)"));
	const auto path = WriteInput("copy_from_header_default.csv", "a,b\n1,2\n");
	auto result = con.Query("COPY matched FROM '" + path + "' (FORMAT csv, HEADER MATCH, DEFAULT 'a')");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT a, b FROM matched");
	REQUIRE(CHECK_COLUMN(result, 0, {"1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"2"}));
}

TEST_CASE("COPY reports a quoted field that opens its data and never closes", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE quotes (a VARCHAR)"));
	const auto path = WriteInput("copy_from_unterminated.csv", "\"abc");
	const auto failure = FailedCopy(con, "COPY quotes FROM '" + path + "' (FORMAT csv)");
	REQUIRE(failure.message == "unterminated CSV quoted field");
	REQUIRE(failure.sqlstate == "22P04");
	REQUIRE(failure.context == "COPY quotes, line 1: \"\"abc\"");
}

TEST_CASE("COPY in csv mode keeps an escape outside quotes as it is", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE escaped (a VARCHAR)"));
	const auto path = WriteInput("copy_from_escape.csv", "a\\b\n");
	auto result = con.Query("COPY escaped FROM '" + path + "' (FORMAT csv, ESCAPE '\\')");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT a FROM escaped");
	REQUIRE(CHECK_COLUMN(result, 0, {"a\\b"}));
}

TEST_CASE("COPY names the column an empty line misses and prints the line as empty", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE pairs (a VARCHAR, b VARCHAR)"));
	const auto path = WriteInput("copy_from_empty_line.txt", "1\t2\n\n3\t4\n");
	const auto failure = FailedCopy(con, "COPY pairs FROM '" + path + "' (FORMAT text)");
	REQUIRE(failure.message == "missing data for column \"b\"");
	REQUIRE(failure.sqlstate == "22P04");
	REQUIRE(failure.context == "COPY pairs, line 2: \"\"");
}

TEST_CASE("COPY reads an end-of-copy marker with more fields on its line as a misplaced marker", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE pairs (a VARCHAR, b VARCHAR)"));
	const auto path = WriteInput("copy_from_marker_fields.txt", "1\t2\n\\.\tx\n");
	const auto failure = FailedCopy(con, "COPY pairs FROM '" + path + "' (FORMAT text)");
	REQUIRE(failure.message == "end-of-copy marker is not alone on its line");
	REQUIRE(failure.sqlstate == "22P04");
	REQUIRE(failure.context == "COPY pairs, line 2, column a: \"\\.\"");
}

TEST_CASE("COPY refuses an end-of-copy marker in a range after the range of the first one", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE marked (a VARCHAR)"));
	string data;
	for (idx_t row = 0; row < 400000; row++) {
		data += "a\n";
	}
	data += "\\.\n" + string(400000, '\n') + "\\.\n";
	const auto path = WriteInput("copy_from_two_markers.txt", data);
	const auto failure = FailedCopy(con, "COPY marked FROM '" + path + "' (FORMAT text)");
	REQUIRE(failure.message == "end-of-copy marker is followed by data");
	REQUIRE(failure.sqlstate == "22P04");
}

TEST_CASE("COPY with ON_ERROR IGNORE notices only the rows it skips, and needs no one to notice", "[copy]") {
	DuckDB db(nullptr);
	RegisterPostgresCopy(db);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE numbers (a INTEGER)"));
	const auto clean = "COPY numbers FROM '" + WriteInput("copy_from_clean.txt", "1\n2\n") +
	                   "' (FORMAT text, ON_ERROR IGNORE, RENDERING duckdb)";
	const auto mixed = "COPY numbers FROM '" + WriteInput("copy_from_mixed.txt", "3\nx\n4\n") +
	                   "' (FORMAT text, ON_ERROR IGNORE, RENDERING duckdb)";
	auto result = con.Query(clean);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	auto notices = make_shared_ptr<RecordedNotices>();
	con.context->registered_state->Insert(CopyNotices::NAME, notices);
	result = con.Query(clean);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(notices->messages.empty());
	result = con.Query(mixed);
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(notices->messages == vector<string> {"1 row was skipped due to data type incompatibility"});
	con.context->registered_state->Remove(CopyNotices::NAME);
	result = con.Query("SELECT sum(a), count(*) FROM numbers");
	REQUIRE(CHECK_COLUMN(result, 0, {13}));
	REQUIRE(CHECK_COLUMN(result, 1, {6}));
}
