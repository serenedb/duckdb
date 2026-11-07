#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/common/string.hpp"
#include <stdint.h>

#include <absl/numeric/int128.h>
#include <bit>

#if defined(_MSC_VER)
#define DUCKDB_UHUGEINT_OUT_OF_LINE __declspec(noinline)
#elif defined(__GNUC__)
#define DUCKDB_UHUGEINT_OUT_OF_LINE __attribute__((noinline, const))
#else
#define DUCKDB_UHUGEINT_OUT_OF_LINE
#endif

namespace duckdb {

// Forward declaration to allow conversion between hugeint and uhugeint
struct hugeint_t; // NOLINT

struct uhugeint_t { // NOLINT
public:
	uint64_t lower;
	uint64_t upper;

public:
	uhugeint_t() = default;
	constexpr uhugeint_t(uint64_t value) // NOLINT: Allow implicit conversion from `uint64_t`
	    : lower(value), upper(0) {
	}
	constexpr uhugeint_t(uint64_t upper, uint64_t lower) : lower(lower), upper(upper) {
	}
	constexpr uhugeint_t(const uhugeint_t &rhs) = default;
	constexpr uhugeint_t(uhugeint_t &&rhs) = default;
	uhugeint_t &operator=(const uhugeint_t &rhs) = default;
	uhugeint_t &operator=(uhugeint_t &&rhs) = default;

	static constexpr uhugeint_t FromUint128(absl::uint128 value) {
		return uhugeint_t(absl::Uint128High64(value), absl::Uint128Low64(value));
	}
	constexpr absl::uint128 ToUint128() const {
		return absl::MakeUint128(upper, lower);
	}

	static uhugeint_t DivMod(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &remainder) {
		absl::uint128 rest;
		const auto quotient = DivModUint128(lhs.ToUint128(), rhs.ToUint128(), rest);
		remainder = FromUint128(rest);
		return FromUint128(quotient);
	}

	static absl::uint128 DivModUint128(absl::uint128 dividend, absl::uint128 divisor, absl::uint128 &remainder) {
		const auto dividend_high = absl::Uint128High64(dividend);
		const auto dividend_low = absl::Uint128Low64(dividend);
		const auto divisor_high = absl::Uint128High64(divisor);
		const auto divisor_low = absl::Uint128Low64(divisor);
		if (divisor_high == 0) {
			if ((divisor_low & (divisor_low - 1)) == 0) {
				if (divisor_low == 0) {
					remainder = dividend;
					return 0;
				}
				const auto shift = std::countr_zero(divisor_low);
				remainder = dividend_low & (divisor_low - 1);
				return absl::MakeUint128(dividend_high >> shift,
				                         (dividend_low >> shift) | (dividend_high << 1 << (63 - shift)));
			}
			if (dividend_high == 0) {
				remainder = dividend_low % divisor_low;
				return dividend_low / divisor_low;
			}
			uint64_t low_remainder;
			if (dividend_high < divisor_low) {
				const auto quotient = Divide128By64(dividend_high, dividend_low, divisor_low, low_remainder);
				remainder = low_remainder;
				return quotient;
			}
			uint64_t high_remainder;
			const auto quotient_high = Divide128By64(0, dividend_high, divisor_low, high_remainder);
			const auto quotient_low = Divide128By64(high_remainder, dividend_low, divisor_low, low_remainder);
			remainder = low_remainder;
			return absl::MakeUint128(quotient_high, quotient_low);
		}
		if (divisor_low == 0) [[unlikely]] {
			if ((divisor_high & (divisor_high - 1)) == 0) {
				remainder = absl::MakeUint128(dividend_high & (divisor_high - 1), dividend_low);
				return dividend_high >> std::countr_zero(divisor_high);
			}
			remainder = absl::MakeUint128(dividend_high % divisor_high, dividend_low);
			return dividend_high / divisor_high;
		}
		const auto quotient = WideQuotient(dividend, divisor);
		remainder = dividend - absl::uint128(quotient) * divisor;
		return quotient;
	}

	DUCKDB_API string ToString() const;

	// comparison operators
	bool operator==(const uhugeint_t &rhs) const {
		return ToUint128() == rhs.ToUint128();
	}
	bool operator!=(const uhugeint_t &rhs) const {
		return ToUint128() != rhs.ToUint128();
	}
	bool operator<=(const uhugeint_t &rhs) const {
		return ToUint128() <= rhs.ToUint128();
	}
	bool operator<(const uhugeint_t &rhs) const {
		return ToUint128() < rhs.ToUint128();
	}
	bool operator>(const uhugeint_t &rhs) const {
		return ToUint128() > rhs.ToUint128();
	}
	bool operator>=(const uhugeint_t &rhs) const {
		return ToUint128() >= rhs.ToUint128();
	}

	// arithmetic operators
	uhugeint_t operator+(const uhugeint_t &rhs) const {
#if defined(__clang__)
		unsigned long long carry;
		const auto low = __builtin_addcll(lower, rhs.lower, 0, &carry);
		return uhugeint_t(__builtin_addcll(upper, rhs.upper, carry, &carry), low);
#else
		const auto low = lower + rhs.lower;
		return uhugeint_t(upper + rhs.upper + (low < lower), low);
#endif
	}
	uhugeint_t operator-(const uhugeint_t &rhs) const {
		return uhugeint_t(upper - rhs.upper - (lower < rhs.lower), lower - rhs.lower);
	}
	uhugeint_t operator*(const uhugeint_t &rhs) const {
		return FromUint128(ToUint128() * rhs.ToUint128());
	}
	uhugeint_t operator/(const uhugeint_t &rhs) const {
		absl::uint128 remainder;
		return FromUint128(DivModUint128(ToUint128(), rhs.ToUint128(), remainder));
	}
	uhugeint_t operator%(const uhugeint_t &rhs) const {
		absl::uint128 remainder;
		DivModUint128(ToUint128(), rhs.ToUint128(), remainder);
		return FromUint128(remainder);
	}
	uhugeint_t operator-() const {
		return uhugeint_t(uint64_t(0) - upper - (lower != 0), uint64_t(0) - lower);
	}

	// bitwise operators
	uhugeint_t operator>>(const uhugeint_t &rhs) const {
		if ((rhs.upper | (rhs.lower >> 7)) != 0) {
			return uhugeint_t(0);
		}
		const auto shift = rhs.lower;
		const auto high = upper >> (shift & 63);
		const auto low = (lower >> (shift & 63)) | (upper << 1 << (~shift & 63));
		return shift & 64 ? uhugeint_t(0, high) : uhugeint_t(high, low);
	}
	uhugeint_t operator<<(const uhugeint_t &rhs) const {
		if ((rhs.upper | (rhs.lower >> 7)) != 0) {
			return uhugeint_t(0);
		}
		const auto shift = rhs.lower;
		const auto low = lower << (shift & 63);
		const auto high = (upper << (shift & 63)) | (lower >> 1 >> (~shift & 63));
		return shift & 64 ? uhugeint_t(low, 0) : uhugeint_t(high, low);
	}
	uhugeint_t operator&(const uhugeint_t &rhs) const {
		return uhugeint_t(upper & rhs.upper, lower & rhs.lower);
	}
	uhugeint_t operator|(const uhugeint_t &rhs) const {
		return uhugeint_t(upper | rhs.upper, lower | rhs.lower);
	}
	uhugeint_t operator^(const uhugeint_t &rhs) const {
		return uhugeint_t(upper ^ rhs.upper, lower ^ rhs.lower);
	}
	uhugeint_t operator~() const {
		return uhugeint_t(~upper, ~lower);
	}

	// in-place operators
	uhugeint_t &operator+=(const uhugeint_t &rhs) {
		return *this = *this + rhs;
	}
	uhugeint_t &operator-=(const uhugeint_t &rhs) {
		return *this = *this - rhs;
	}
	uhugeint_t &operator*=(const uhugeint_t &rhs) {
		return *this = *this * rhs;
	}
	uhugeint_t &operator/=(const uhugeint_t &rhs) {
		return *this = *this / rhs;
	}
	uhugeint_t &operator%=(const uhugeint_t &rhs) {
		return *this = *this % rhs;
	}
	uhugeint_t &operator>>=(const uhugeint_t &rhs) {
		return *this = *this >> rhs;
	}
	uhugeint_t &operator<<=(const uhugeint_t &rhs) {
		return *this = *this << rhs;
	}
	uhugeint_t &operator&=(const uhugeint_t &rhs) {
		return *this = *this & rhs;
	}
	uhugeint_t &operator|=(const uhugeint_t &rhs) {
		return *this = *this | rhs;
	}
	uhugeint_t &operator^=(const uhugeint_t &rhs) {
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
	DUCKDB_API operator hugeint_t() const; // NOLINT: Allow implicit conversion from `uhugeint_t`

private:
	DUCKDB_UHUGEINT_OUT_OF_LINE static uint64_t WideQuotient(absl::uint128 dividend, absl::uint128 divisor) {
		if (divisor > dividend) {
			return 0;
		}
		const auto shift = std::countl_zero(absl::Uint128High64(divisor));
		const auto quotient_bits = shift - std::countl_zero(absl::Uint128High64(dividend));
		if (quotient_bits < 4) {
			auto shifted = divisor << quotient_bits;
			uint64_t quotient = 0;
			auto rest = dividend;
			for (int bit = quotient_bits; bit >= 0; bit--) {
				const auto difference = rest - shifted;
				const bool fits = rest >= shifted;
				quotient = (quotient << 1) | fits;
				rest = absl::MakeUint128(fits ? absl::Uint128High64(difference) : absl::Uint128High64(rest),
				                         fits ? absl::Uint128Low64(difference) : absl::Uint128Low64(rest));
				shifted >>= 1;
			}
			return quotient;
		}
		const auto halved = dividend >> 1;
		uint64_t estimate_remainder;
		const auto estimate = Divide128By64(absl::Uint128High64(halved), absl::Uint128Low64(halved),
		                                    absl::Uint128High64(divisor << shift), estimate_remainder);
		auto quotient = absl::Uint128Low64((absl::uint128(estimate) << shift) >> 63);
		if (quotient != 0) {
			quotient--;
		}
		if (dividend - absl::uint128(quotient) * divisor >= divisor) {
			quotient++;
		}
		return quotient;
	}
	static uint64_t Divide128By64(uint64_t high, uint64_t low, uint64_t divisor, uint64_t &remainder) {
#if defined(__x86_64__)
		uint64_t quotient;
		__asm__("divq %[divisor]" : "=a"(quotient), "=d"(remainder) : [divisor] "r"(divisor), "a"(low), "d"(high));
		return quotient;
#else
		const auto dividend = absl::MakeUint128(high, low);
		remainder = absl::Uint128Low64(dividend % divisor);
		return absl::Uint128Low64(dividend / divisor);
#endif
	}
};

} // namespace duckdb

#undef DUCKDB_UHUGEINT_OUT_OF_LINE

namespace std {
template <>
struct hash<duckdb::uhugeint_t> {
	size_t operator()(const duckdb::uhugeint_t &val) const {
		using std::hash;
		return hash<uint64_t> {}(val.upper) ^ hash<uint64_t> {}(val.lower);
	}
};
} // namespace std
