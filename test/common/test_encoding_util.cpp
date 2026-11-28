#include "catch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/encoding_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"

#include <random>

using namespace duckdb;

namespace {

vector<uint8_t> BigEndian(hugeint_t value, idx_t size) {
	vector<uint8_t> bytes(size);
	auto upper = static_cast<uint64_t>(value.upper);
	auto lower = value.lower;
	for (idx_t i = 0; i < size; i++) {
		bytes[size - 1 - i] = static_cast<uint8_t>(lower);
		lower = (lower >> 8) | (upper << 56);
		upper = static_cast<uint64_t>(static_cast<int64_t>(upper) >> 8);
	}
	return bytes;
}

idx_t MinimalSize(hugeint_t value) {
	for (idx_t size = 1; size < 16; size++) {
		const auto bytes = BigEndian(value, size);
		hugeint_t decoded;
		if (Decimal::TryDecodeBigEndian(bytes.data(), size, decoded) && decoded == value) {
			return size;
		}
	}
	return 16;
}

template <class T>
void CheckDecimal(std::mt19937_64 &random, T minimum, T maximum) {
	for (idx_t i = 0; i < 20000; i++) {
		hugeint_t value;
		value.upper = static_cast<int64_t>(random());
		value.lower = random();
		value = value >> static_cast<int>(random() % 127);
		const bool fits = value >= hugeint_t(minimum) && value <= hugeint_t(maximum);
		for (idx_t size = MinimalSize(value); size <= 20; size += 1 + random() % 4) {
			const auto bytes = BigEndian(value, size);
			T decoded;
			INFO(value.ToString() << " in " << size << " bytes");
			REQUIRE(Decimal::TryDecodeBigEndian(bytes.data(), size, decoded) == fits);
			if (fits) {
				REQUIRE(hugeint_t(decoded) == value);
			}
		}
	}
}

} // namespace

TEST_CASE("LEB128 decoding takes exactly the bytes of an encoding and refuses one too long or cut short", "[formats]") {
	std::mt19937_64 random(7);
	uint8_t bytes[16];
	for (idx_t i = 0; i < 100000; i++) {
		const uint64_t value = random() >> (random() % 64);
		const auto size = EncodingUtil::EncodeUnsignedLEB128(bytes, value);
		uint64_t decoded;
		REQUIRE(EncodingUtil::TryDecodeUnsignedLEB128(bytes, size, decoded) == size);
		REQUIRE(decoded == value);
		REQUIRE(EncodingUtil::TryDecodeUnsignedLEB128(bytes, size - 1, decoded) == 0);
		REQUIRE(EncodingUtil::DecodeUnsignedLEB128(bytes, decoded) == size);
		REQUIRE(decoded == value);
	}
	memset(bytes, 0x80, sizeof(bytes));
	uint64_t decoded;
	REQUIRE(EncodingUtil::TryDecodeUnsignedLEB128(bytes, sizeof(bytes), decoded) == 0);
	REQUIRE_THROWS_AS(EncodingUtil::DecodeUnsignedLEB128(bytes, decoded), IOException);
	bytes[1] = 0x01;
	uint8_t small;
	REQUIRE(EncodingUtil::TryDecodeUnsignedLEB128(bytes, sizeof(bytes), small) == 2);
	REQUIRE(small == 128);
	bytes[1] = 0x80;
	REQUIRE(EncodingUtil::TryDecodeUnsignedLEB128(bytes, sizeof(bytes), small) == 0);
}

TEST_CASE("ZigZag encoding maps signed integers to the unsigned ones LEB128 writes and back", "[formats]") {
	REQUIRE(EncodingUtil::ZigZagEncode(int64_t(0)) == 0);
	REQUIRE(EncodingUtil::ZigZagEncode(int64_t(-1)) == 1);
	REQUIRE(EncodingUtil::ZigZagEncode(int64_t(1)) == 2);
	REQUIRE(EncodingUtil::ZigZagEncode(int64_t(-2)) == 3);
	REQUIRE(EncodingUtil::ZigZagEncode(NumericLimits<int64_t>::Maximum()) == NumericLimits<uint64_t>::Maximum() - 1);
	REQUIRE(EncodingUtil::ZigZagEncode(NumericLimits<int64_t>::Minimum()) == NumericLimits<uint64_t>::Maximum());
	std::mt19937_64 random(11);
	for (idx_t i = 0; i < 100000; i++) {
		const auto value = static_cast<int64_t>(random());
		REQUIRE(EncodingUtil::ZigZagDecode(EncodingUtil::ZigZagEncode(value)) == value);
		const auto narrow = static_cast<int32_t>(value);
		REQUIRE(EncodingUtil::ZigZagDecode(EncodingUtil::ZigZagEncode(narrow)) == narrow);
		REQUIRE(EncodingUtil::ZigZagEncode(narrow) == EncodingUtil::ZigZagEncode(int64_t(narrow)));
	}
}

TEST_CASE("Big-endian two's complement decimals decode into the physical types they fit", "[formats]") {
	std::mt19937_64 random(3);
	CheckDecimal<int16_t>(random, NumericLimits<int16_t>::Minimum(), NumericLimits<int16_t>::Maximum());
	CheckDecimal<int32_t>(random, NumericLimits<int32_t>::Minimum(), NumericLimits<int32_t>::Maximum());
	CheckDecimal<int64_t>(random, NumericLimits<int64_t>::Minimum(), NumericLimits<int64_t>::Maximum());
	hugeint_t int128_min;
	int128_min.upper = NumericLimits<int64_t>::Minimum();
	int128_min.lower = 0;
	hugeint_t int128_max;
	int128_max.upper = NumericLimits<int64_t>::Maximum();
	int128_max.lower = NumericLimits<uint64_t>::Maximum();
	CheckDecimal<hugeint_t>(random, int128_min, int128_max);

	int32_t empty = 1;
	REQUIRE(Decimal::TryDecodeBigEndian(nullptr, 0, empty));
	REQUIRE(empty == 0);
	const uint8_t wrong_sign[] = {0x00, 0xFF, 0xFF};
	int16_t narrow;
	REQUIRE_FALSE(Decimal::TryDecodeBigEndian(wrong_sign, sizeof(wrong_sign), narrow));
	uint8_t long_wrong_sign[17] = {};
	long_wrong_sign[1] = 0x80;
	hugeint_t wide;
	REQUIRE_FALSE(Decimal::TryDecodeBigEndian(long_wrong_sign, sizeof(long_wrong_sign), wide));
	long_wrong_sign[0] = 0xFF;
	REQUIRE(Decimal::TryDecodeBigEndian(long_wrong_sign, sizeof(long_wrong_sign), wide));
	REQUIRE(wide == int128_min);
}
