#include "catch.hpp"
#include "duckdb/common/bit_utils.hpp"

using namespace duckdb;

TEST_CASE("CountZeros on 32 and 64 bits", "[bit_utils]") {
	REQUIRE(CountZeros<uint64_t>::Leading(0) == 64);
	REQUIRE(CountZeros<uint64_t>::Trailing(0) == 64);
	REQUIRE(CountZeros<uint32_t>::Leading(0) == 32);
	REQUIRE(CountZeros<uint32_t>::Trailing(0) == 32);
	REQUIRE(CountZeros<uint64_t>::Leading(~uint64_t(0)) == 0);
	REQUIRE(CountZeros<uint32_t>::Trailing(~uint32_t(0)) == 0);
	for (idx_t bit = 0; bit < 64; bit++) {
		const uint64_t one = uint64_t(1) << bit;
		REQUIRE(CountZeros<uint64_t>::Leading(one) == 63 - bit);
		REQUIRE(CountZeros<uint64_t>::Trailing(one) == bit);
		REQUIRE(CountZeros<uint64_t>::Leading(one | 1) == 63 - bit);
		REQUIRE(CountZeros<uint64_t>::Trailing(one | (uint64_t(1) << 63)) == bit);
	}
	for (idx_t bit = 0; bit < 32; bit++) {
		const uint32_t one = uint32_t(1) << bit;
		REQUIRE(CountZeros<uint32_t>::Leading(one) == 31 - bit);
		REQUIRE(CountZeros<uint32_t>::Trailing(one) == bit);
	}
}

TEST_CASE("CountZeros on 128 bits", "[bit_utils]") {
	REQUIRE(CountZeros<hugeint_t>::Leading(hugeint_t(0)) == 128);
	REQUIRE(CountZeros<hugeint_t>::Trailing(hugeint_t(0)) == 128);
	REQUIRE(CountZeros<uhugeint_t>::Leading(uhugeint_t(0)) == 128);
	REQUIRE(CountZeros<uhugeint_t>::Trailing(uhugeint_t(0)) == 128);
	REQUIRE(CountZeros<hugeint_t>::Leading(hugeint_t(-1)) == 0);
	REQUIRE(CountZeros<hugeint_t>::Trailing(hugeint_t(-1)) == 0);
	for (idx_t bit = 0; bit < 128; bit++) {
		const uint64_t upper = bit >= 64 ? uint64_t(1) << (bit - 64) : 0;
		const uint64_t lower = bit < 64 ? uint64_t(1) << bit : 0;
		const uhugeint_t unsigned_one(upper, lower);
		const hugeint_t signed_one(static_cast<int64_t>(upper), lower);
		REQUIRE(CountZeros<uhugeint_t>::Leading(unsigned_one) == 127 - bit);
		REQUIRE(CountZeros<uhugeint_t>::Trailing(unsigned_one) == bit);
		REQUIRE(CountZeros<hugeint_t>::Leading(signed_one) == 127 - bit);
		REQUIRE(CountZeros<hugeint_t>::Trailing(signed_one) == bit);
		REQUIRE(CountZeros<uhugeint_t>::Leading(uhugeint_t(upper, lower | 1)) == 127 - bit);
		REQUIRE(CountZeros<uhugeint_t>::Trailing(uhugeint_t(upper | (uint64_t(1) << 63), lower)) == bit);
	}
}
