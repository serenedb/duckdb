#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/common/string.hpp"
#include <stdint.h>
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/uhugeint.hpp"

#include <absl/numeric/int128.h>
#include <bit>

namespace duckdb {

// Forward declaration to allow conversion between hugeint and uhugeint
struct uhugeint_t; // NOLINT: use numeric casing

struct hugeint_t { // NOLINT: use numeric casing
public:
	uint64_t lower;
	int64_t upper;

public:
	hugeint_t() = default;
	constexpr hugeint_t(int64_t value) // NOLINT: Allow implicit conversion from `int64_t`
	    : lower(static_cast<uint64_t>(value)), upper(value < 0 ? -1 : 0) {
	}
	constexpr hugeint_t(int64_t upper, uint64_t lower) : lower(lower), upper(upper) {
	}
	constexpr hugeint_t(const hugeint_t &rhs) = default;
	constexpr hugeint_t(hugeint_t &&rhs) = default;
	hugeint_t &operator=(const hugeint_t &rhs) = default;
	hugeint_t &operator=(hugeint_t &&rhs) = default;

	static constexpr hugeint_t FromInt128(absl::int128 value) {
		return hugeint_t(absl::Int128High64(value), absl::Int128Low64(value));
	}
	static constexpr hugeint_t FromUint128(absl::uint128 value) {
		return hugeint_t(static_cast<int64_t>(absl::Uint128High64(value)), absl::Uint128Low64(value));
	}
	constexpr absl::int128 ToInt128() const {
		return absl::MakeInt128(upper, lower);
	}
	constexpr absl::uint128 ToUint128() const {
		return absl::MakeUint128(static_cast<uint64_t>(upper), lower);
	}

	static hugeint_t DivMod(hugeint_t lhs, hugeint_t rhs, hugeint_t &remainder) {
		if (lhs.FitsInt64() && rhs.FitsInt64() && rhs.lower != 0 && rhs.lower != ~uint64_t(0)) {
			const auto left = static_cast<int64_t>(lhs.lower);
			const auto right = static_cast<int64_t>(rhs.lower);
			remainder = hugeint_t(left % right);
			return hugeint_t(left / right);
		}
		const auto lhs_sign = lhs.SignMask();
		const auto rhs_sign = rhs.SignMask();
		absl::uint128 rest;
		const auto quotient = uhugeint_t::DivModUint128(WithSign(lhs.ToUint128(), lhs_sign).ToUint128(),
		                                                WithSign(rhs.ToUint128(), rhs_sign).ToUint128(), rest);
		remainder = WithSign(rest, lhs_sign);
		return WithSign(quotient, lhs_sign ^ rhs_sign);
	}

	DUCKDB_API string ToString() const;

	// comparison operators
	bool operator==(const hugeint_t &rhs) const {
		return ToUint128() == rhs.ToUint128();
	}
	bool operator!=(const hugeint_t &rhs) const {
		return ToUint128() != rhs.ToUint128();
	}
	bool operator<=(const hugeint_t &rhs) const {
		return ToInt128() <= rhs.ToInt128();
	}
	bool operator<(const hugeint_t &rhs) const {
		return ToInt128() < rhs.ToInt128();
	}
	bool operator>(const hugeint_t &rhs) const {
		return ToInt128() > rhs.ToInt128();
	}
	bool operator>=(const hugeint_t &rhs) const {
		return ToInt128() >= rhs.ToInt128();
	}

	// arithmetic operators
	hugeint_t operator+(const hugeint_t &rhs) const {
#if defined(__clang__)
		unsigned long long carry;
		const auto low = __builtin_addcll(lower, rhs.lower, 0, &carry);
		const auto high =
		    __builtin_addcll(static_cast<uint64_t>(upper), static_cast<uint64_t>(rhs.upper), carry, &carry);
#else
		const auto low = lower + rhs.lower;
		const auto high = static_cast<uint64_t>(upper) + static_cast<uint64_t>(rhs.upper) + (low < lower);
#endif
		return hugeint_t(static_cast<int64_t>(high), low);
	}
	hugeint_t operator-(const hugeint_t &rhs) const {
		const auto low = lower - rhs.lower;
		const auto high = static_cast<uint64_t>(upper) - static_cast<uint64_t>(rhs.upper) - (lower < rhs.lower);
		return hugeint_t(static_cast<int64_t>(high), low);
	}
	hugeint_t operator*(const hugeint_t &rhs) const {
		return FromUint128(ToUint128() * rhs.ToUint128());
	}
	hugeint_t operator/(const hugeint_t &rhs) const {
		hugeint_t remainder;
		return DivMod(*this, rhs, remainder);
	}
	hugeint_t operator%(const hugeint_t &rhs) const {
		hugeint_t remainder;
		DivMod(*this, rhs, remainder);
		return remainder;
	}
	hugeint_t operator-() const {
		const auto low = uint64_t(0) - lower;
		const auto high = uint64_t(0) - static_cast<uint64_t>(upper) - (lower != 0);
		return hugeint_t(static_cast<int64_t>(high), low);
	}

	// bitwise operators
	hugeint_t operator>>(const hugeint_t &rhs) const {
		const auto shift = rhs.lower;
		const auto keep = uint64_t(0) - uint64_t((static_cast<uint64_t>(rhs.upper) | (shift >> 7)) == 0);
		const auto high = static_cast<uint64_t>(upper >> (shift & 63));
		const auto low = (lower >> (shift & 63)) | (static_cast<uint64_t>(upper) << 1 << (~shift & 63));
		const auto top = shift & 64 ? static_cast<uint64_t>(upper >> 63) : high;
		return hugeint_t(static_cast<int64_t>(top & keep), (shift & 64 ? high : low) & keep);
	}
	hugeint_t operator<<(const hugeint_t &rhs) const {
		const auto shift = rhs.lower;
		const auto keep =
		    uint64_t(0) - uint64_t(((static_cast<uint64_t>(rhs.upper) | (shift >> 7)) == 0) & (upper >= 0));
		const auto low = lower << (shift & 63);
		const auto high = (static_cast<uint64_t>(upper) << (shift & 63)) | (lower >> 1 >> (~shift & 63));
		const auto top =
		    (shift & 64 ? low : high) & (std::numeric_limits<int64_t>::max() | (uint64_t(shift == 64) << 63));
		return hugeint_t(static_cast<int64_t>(top & keep), shift & 64 ? 0 : low & keep);
	}
	hugeint_t operator&(const hugeint_t &rhs) const {
		return hugeint_t(upper & rhs.upper, lower & rhs.lower);
	}
	hugeint_t operator|(const hugeint_t &rhs) const {
		return hugeint_t(upper | rhs.upper, lower | rhs.lower);
	}
	hugeint_t operator^(const hugeint_t &rhs) const {
		return hugeint_t(upper ^ rhs.upper, lower ^ rhs.lower);
	}
	hugeint_t operator~() const {
		return hugeint_t(~upper, ~lower);
	}

	// in-place operators
	hugeint_t &operator+=(const hugeint_t &rhs) {
		return *this = *this + rhs;
	}
	hugeint_t &operator-=(const hugeint_t &rhs) {
		return *this = *this - rhs;
	}
	hugeint_t &operator*=(const hugeint_t &rhs) {
		return *this = *this * rhs;
	}
	hugeint_t &operator/=(const hugeint_t &rhs) {
		return *this = *this / rhs;
	}
	hugeint_t &operator%=(const hugeint_t &rhs) {
		return *this = *this % rhs;
	}
	hugeint_t &operator>>=(const hugeint_t &rhs) {
		return *this = *this >> rhs;
	}
	hugeint_t &operator<<=(const hugeint_t &rhs) {
		return *this = *this << rhs;
	}
	hugeint_t &operator&=(const hugeint_t &rhs) {
		return *this = *this & rhs;
	}
	hugeint_t &operator|=(const hugeint_t &rhs) {
		return *this = *this | rhs;
	}
	hugeint_t &operator^=(const hugeint_t &rhs) {
		return *this = *this ^ rhs;
	}

	// boolean operators
	explicit operator bool() const {
		return upper != 0 || lower != 0;
	}
	bool operator!() const {
		return upper == 0 && lower == 0;
	}

	// cast operators -- doesn't check bounds/overflow/underflow
	explicit operator uint8_t() const {
		return static_cast<uint8_t>(lower);
	}
	explicit operator uint16_t() const {
		return static_cast<uint16_t>(lower);
	}
	explicit operator uint32_t() const {
		return static_cast<uint32_t>(lower);
	}
	explicit operator uint64_t() const {
		return lower;
	}
	explicit operator int8_t() const {
		return static_cast<int8_t>(lower);
	}
	explicit operator int16_t() const {
		return static_cast<int16_t>(lower);
	}
	explicit operator int32_t() const {
		return static_cast<int32_t>(lower);
	}
	explicit operator int64_t() const {
		return static_cast<int64_t>(lower);
	}
	DUCKDB_API explicit operator uhugeint_t() const;

	constexpr bool FitsInt64() const {
		return upper == (static_cast<int64_t>(lower) >> 63);
	}

private:
	constexpr uint64_t SignMask() const {
		return static_cast<uint64_t>(upper >> 63);
	}
	static constexpr hugeint_t WithSign(absl::uint128 value, uint64_t sign) {
		const auto flipped_low = absl::Uint128Low64(value) ^ sign;
		const auto high = (absl::Uint128High64(value) ^ sign) - sign - (flipped_low < sign);
		return hugeint_t(static_cast<int64_t>(high), flipped_low - sign);
	}
};

} // namespace duckdb

namespace std {
template <>
struct hash<duckdb::hugeint_t> {
	size_t operator()(const duckdb::hugeint_t &val) const {
		using std::hash;
		return hash<int64_t> {}(val.upper) ^ hash<uint64_t> {}(val.lower);
	}
};
} // namespace std
