#include "catch.hpp"
#include "duckdb/formats/value_parsers.hpp"

#include <cstring>
#include <random>

using namespace duckdb;

namespace {

bool ParseFast(const string &text, double &out) {
	char buffer[64] = {};
	memcpy(buffer, text.data(), text.size());
	return FastDouble(buffer, text.size(), buffer + sizeof(buffer), out, '.');
}

bool ParseFull(const string &text, double &out) {
	char buffer[64] = {};
	memcpy(buffer, text.data(), text.size());
	return DoubleParser<double> {buffer + sizeof(buffer), '.', false}(buffer, text.size(), out);
}

uint64_t Bits(double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

} // namespace

TEST_CASE("A short plain decimal parses to the double fast_float gives", "[formats]") {
	std::mt19937_64 random(42);
	for (idx_t digits = 1; digits <= 15; digits++) {
		for (idx_t round = 0; round < 4000; round++) {
			string mantissa;
			for (idx_t i = 0; i < digits; i++) {
				mantissa += static_cast<char>('0' + random() % 10);
			}
			const idx_t integer_digits = 1 + random() % digits;
			string text = round % 3 == 0 ? "-" : "";
			text += mantissa.substr(0, integer_digits);
			if (integer_digits < digits) {
				text += "." + mantissa.substr(integer_digits);
			}
			double fast = 0;
			REQUIRE(ParseFast(text, fast));
			double expected = 0;
			REQUIRE(TryDoubleCast<double>(text.data(), text.size(), expected, false, '.'));
			INFO(text);
			REQUIRE(Bits(fast) == Bits(expected));
		}
	}
	for (const string text : {"0", "-0", "0.0", "-0.0", "0.1", "0.000000000000001", "999999999999999",
	                          "99999999999999.9", "1.5", "9007199254740991", "123456789012345.6"}) {
		double fast = 0;
		double full = 0;
		double expected = 0;
		INFO(text);
		REQUIRE(TryDoubleCast<double>(text.data(), text.size(), expected, false, '.'));
		REQUIRE(ParseFull(text, full));
		REQUIRE(Bits(full) == Bits(expected));
		if (ParseFast(text, fast)) {
			REQUIRE(Bits(fast) == Bits(expected));
		}
	}
}

TEST_CASE("Doubles the fast path refuses go to the full parser", "[formats]") {
	for (const string text :
	     {"", "-", ".5", "5.", "+1.5", "1e5", "1.5e-3", " 1.5", "1.5 ", "1,5", "1234567890123456", "1_000.5", "inf"}) {
		double fast = 0;
		INFO(text);
		REQUIRE(!ParseFast(text, fast));
	}
	double out = 0;
	REQUIRE(ParseFull("1_000.5", out));
	REQUIRE(out == 1000.5);
	REQUIRE(ParseFull(" 2.5 ", out));
	REQUIRE(out == 2.5);
	REQUIRE(ParseFull("1.5e-3", out));
	REQUIRE(out == 1.5e-3);
	REQUIRE(!ParseFull("1__0", out));
	REQUIRE(!ParseFull("_1", out));
	REQUIRE(!ParseFull("1.5 _", out));
	REQUIRE(!ParseFull("1.5x", out));
}

TEST_CASE("An ISO date parses to the date DuckDB's calendar gives", "[formats]") {
	idx_t fast_dates = 0;
	for (int32_t year = 1; year <= 9999; year++) {
		for (int32_t month = 1; month <= 12; month++) {
			for (int32_t day = 1; day <= 31; day++) {
				char buffer[16] = {};
				snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
				date_t expected;
				const bool valid = Date::TryFromDate(year, month, day, expected);
				date_t fast;
				INFO(buffer);
				REQUIRE(FastDate(buffer, 10, fast) == valid);
				if (valid) {
					REQUIRE(fast == expected);
					fast_dates++;
				}
			}
		}
	}
	REQUIRE(fast_dates == 3652059);
	for (const string text : {"0000-01-01", "2024-00-10", "2024-13-01", "2024-01-00", "2023-02-29", "2100-02-29",
	                          "2024/01/01", "2024-1-01", "20x4-01-01", "2024-01-011"}) {
		char buffer[16] = {};
		memcpy(buffer, text.data(), text.size());
		date_t fast;
		INFO(text);
		REQUIRE(!FastDate(buffer, text.size(), fast));
	}
}
