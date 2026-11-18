#include "catch.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <limits>

using namespace duckdb;

namespace {

bool HasMinMaxByValues(const BaseStatistics &stats) {
	return NumericStats::HasMin(stats) && NumericStats::HasMax(stats) &&
	       NumericStats::Min(stats) <= NumericStats::Max(stats);
}

void CheckBounds(const LogicalType &type, const Value &low, const Value &high) {
	INFO(type.ToString());
	auto empty = NumericStats::CreateEmpty(type);
	REQUIRE_FALSE(NumericStats::HasMinMax(empty));
	REQUIRE(NumericStats::HasMinMax(empty) == HasMinMaxByValues(empty));

	auto unknown = NumericStats::CreateUnknown(type);
	REQUIRE_FALSE(NumericStats::HasMinMax(unknown));

	for (const auto &bounds : {std::make_pair(low, high), std::make_pair(low, low), std::make_pair(high, low)}) {
		auto stats = NumericStats::CreateEmpty(type);
		NumericStats::SetMin(stats, bounds.first);
		NumericStats::SetMax(stats, bounds.second);
		REQUIRE(NumericStats::HasMinMax(stats) == HasMinMaxByValues(stats));
	}
	auto ordered = NumericStats::CreateEmpty(type);
	NumericStats::SetMin(ordered, low);
	NumericStats::SetMax(ordered, high);
	REQUIRE(NumericStats::HasMinMax(ordered));
	auto inverted = NumericStats::CreateEmpty(type);
	NumericStats::SetMin(inverted, high);
	NumericStats::SetMax(inverted, low);
	REQUIRE_FALSE(NumericStats::HasMinMax(inverted));
}

} // namespace

TEST_CASE("NumericStats::HasMinMax orders the bounds of every numeric type", "[statistics]") {
	CheckBounds(LogicalType::BOOLEAN, Value::BOOLEAN(false), Value::BOOLEAN(true));
	CheckBounds(LogicalType::TINYINT, Value::TINYINT(-5), Value::TINYINT(7));
	CheckBounds(LogicalType::SMALLINT, Value::SMALLINT(-300), Value::SMALLINT(300));
	CheckBounds(LogicalType::INTEGER, Value::INTEGER(-70000), Value::INTEGER(70000));
	CheckBounds(LogicalType::BIGINT, Value::BIGINT(std::numeric_limits<int64_t>::min()), Value::BIGINT(1));
	CheckBounds(LogicalType::UTINYINT, Value::UTINYINT(1), Value::UTINYINT(200));
	CheckBounds(LogicalType::USMALLINT, Value::USMALLINT(1), Value::USMALLINT(60000));
	CheckBounds(LogicalType::UINTEGER, Value::UINTEGER(1), Value::UINTEGER(4000000000U));
	CheckBounds(LogicalType::UBIGINT, Value::UBIGINT(1), Value::UBIGINT(std::numeric_limits<uint64_t>::max()));
	CheckBounds(LogicalType::HUGEINT, Value::HUGEINT(hugeint_t(-1)), Value::HUGEINT(hugeint_t(1, 0)));
	CheckBounds(LogicalType::UHUGEINT, Value::UHUGEINT(uhugeint_t(1)), Value::UHUGEINT(uhugeint_t(1, 0)));
	CheckBounds(LogicalType::FLOAT, Value::FLOAT(-1.5f), Value::FLOAT(std::numeric_limits<float>::quiet_NaN()));
	CheckBounds(LogicalType::DOUBLE, Value::DOUBLE(-1.5), Value::DOUBLE(std::numeric_limits<double>::quiet_NaN()));
	CheckBounds(LogicalType::DATE, Value::DATE(date_t(10)), Value::DATE(date_t(20)));
	CheckBounds(LogicalType::TIMESTAMP, Value::TIMESTAMP(timestamp_t(10)), Value::TIMESTAMP(timestamp_t(20)));
	CheckBounds(LogicalType::DECIMAL(9, 2), Value::DECIMAL(int32_t(-100), 9, 2), Value::DECIMAL(int32_t(100), 9, 2));
	CheckBounds(LogicalType::DECIMAL(30, 2), Value::DECIMAL(hugeint_t(-100), 30, 2),
	            Value::DECIMAL(hugeint_t(100), 30, 2));
	CheckBounds(LogicalType::INTERVAL, Value::INTERVAL(0, 1, 0), Value::INTERVAL(1, 0, 0));
}
