#include "catch.hpp"
#include "duckdb/formats/csv_encoder.hpp"

using namespace duckdb;

TEST_CASE("CSV specials are found at every position of a field", "[csv]") {
	CsvSpecials specials;
	specials.Add(',');
	specials.Add('"');
	specials.Add('\n');
	specials.Add('\r');
	for (idx_t size = 0; size <= 130; size++) {
		string field(size, 'a');
		REQUIRE(!specials.In(field));
		for (idx_t at = 0; at < size; at++) {
			for (const char special : {',', '"', '\n', '\r'}) {
				field[at] = special;
				REQUIRE(specials.In(field));
				field[at] = 'a';
			}
		}
	}
	REQUIRE(!specials.In(string(70, '\0')));
}

TEST_CASE("CSV fields are quoted with their escapes, copied or in place", "[csv]") {
	struct Case {
		string field;
		char quote;
		char escape;
		string quoted;
	};
	const vector<Case> cases = {
	    {"", '"', '"', "\"\""},
	    {"a", '"', '"', "\"a\""},
	    {"a\"b", '"', '"', "\"a\"\"b\""},
	    {"\"\"", '"', '"', "\"\"\"\"\"\""},
	    {"a\\b\"c", '"', '\\', "\"a\\\\b\\\"c\""},
	    {"a\"b", '"', '\0', "\"a\"b\""},
	};
	for (auto &test : cases) {
		INFO("field " << test.field);
		REQUIRE(test.field.size() + CsvEscapes(test.field, test.quote, test.escape) + 2 == test.quoted.size());
		string copied(test.quoted.size(), '\0');
		REQUIRE(CsvWriteQuoted(&copied[0], test.field, test.quote, test.escape) == test.quoted.size());
		REQUIRE(copied == test.quoted);
		string in_place = test.field;
		in_place.resize(test.quoted.size());
		REQUIRE(CsvQuoteInPlace(&in_place[0], test.field.size(), test.quote, test.escape) == test.quoted.size());
		REQUIRE(in_place == test.quoted);
	}
}
