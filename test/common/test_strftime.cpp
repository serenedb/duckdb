#include "catch.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"

#include <vector>

using namespace duckdb;

TEST_CASE("Test that strftime format works", "[strftime]") {
	auto string = StrfTimeFormat::Format(Timestamp::FromDatetime(Date::FromDate(1992, 1, 1), dtime_t(0)), "%Y%m%d");
	REQUIRE(string == "19920101");
}

TEST_CASE("Test that strptime parses a time by a format", "[strftime]") {
	StrpTimeFormat format;
	REQUIRE(StrTimeFormat::ParseFormatSpecifier("%H:%M:%S", format).empty());
	dtime_t result;
	string error_message;
	REQUIRE(format.TryParseTime(string_t("12:34:56"), result, error_message));
	REQUIRE(result == Time::FromTime(12, 34, 56));
	REQUIRE(!format.TryParseTime(string_t("12:34"), result, error_message));
	REQUIRE(!error_message.empty());
}
