#include "catch.hpp"
#include "duckdb/formats/csv_encoder.hpp"

#include <random>

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

TEST_CASE("CSV specials find the first byte of any set, from any position", "[csv]") {
	std::mt19937_64 rng(31);
	for (idx_t round = 0; round < 2000; round++) {
		CsvSpecials specials;
		bool member[256] = {};
		bool high_used[16] = {};
		idx_t groups = 0;
		const idx_t count = 1 + rng() % 12;
		for (idx_t i = 0; i < count; i++) {
			const auto value = static_cast<uint8_t>(rng());
			if (!high_used[value >> 4] && groups == 8) {
				REQUIRE_THROWS(specials.Add(static_cast<char>(value)));
				continue;
			}
			groups += !high_used[value >> 4];
			high_used[value >> 4] = true;
			member[value] = true;
			specials.Add(static_cast<char>(value));
		}
		string field(rng() % 200, '\0');
		for (auto &c : field) {
			c = static_cast<char>(rng() % 4 == 0 ? rng() : 'a' + rng() % 3);
		}
		const idx_t from = field.empty() ? 0 : rng() % (field.size() + 1);
		idx_t expected = from;
		while (expected < field.size() && !member[static_cast<uint8_t>(field[expected])]) {
			expected++;
		}
		REQUIRE(specials.Find(field, from) == expected);
		REQUIRE(specials.In(field) == (specials.Find(field, 0) < field.size()));
		for (idx_t value = 0; value < 256; value++) {
			string probe(1 + rng() % 100, member['a'] ? '\0' : 'a');
			if (member[static_cast<uint8_t>(probe[0])]) {
				continue;
			}
			const idx_t at = rng() % probe.size();
			probe[at] = static_cast<char>(value);
			REQUIRE(specials.Find(probe, 0) == (member[value] ? at : probe.size()));
		}
	}
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
