//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/hugeint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//! The Hugeint class contains static operations for the INT128 type
class Hugeint {
public:
	constexpr static const char *HUGEINT_MINIMUM_STRING = "-170141183460469231731687303715884105728";

	//! Convert a hugeint object to a string
	static string ToString(hugeint_t input);

	template <class T>
	DUCKDB_API static bool TryCast(hugeint_t input, T &result);

	template <class T>
	static T Cast(hugeint_t input) {
		T result = 0;
		TryCast(input, result);
		return result;
	}

	template <class T>
	static bool TryConvert(T value, hugeint_t &result);

	template <class T>
	static hugeint_t Convert(T value) {
		hugeint_t result;
		if (!TryConvert(value, result)) { // LCOV_EXCL_START
			throw OutOfRangeException(double(value), GetTypeId<T>(), GetTypeId<hugeint_t>());
		} // LCOV_EXCL_STOP
		return result;
	}

	static bool TryNegate(hugeint_t input, hugeint_t &result) {
		if (input.upper == NumericLimits<int64_t>::Minimum() && input.lower == 0) {
			return false;
		}
		result = -input;
		return true;
	}

	template <bool CHECK_OVERFLOW = true>
	inline static void NegateInPlace(hugeint_t &input) {
		if (!TryNegate(input, input)) {
			throw OutOfRangeException("Negation of HUGEINT is out of range!");
		}
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Negate(hugeint_t input) {
		NegateInPlace<CHECK_OVERFLOW>(input);
		return input;
	}

	static bool TryMultiply(hugeint_t lhs, hugeint_t rhs, hugeint_t &result) {
		if (lhs.FitsInt64() && rhs.FitsInt64()) [[likely]] {
			result =
			    hugeint_t::FromInt128(absl::int128(static_cast<int64_t>(lhs.lower)) * static_cast<int64_t>(rhs.lower));
			return true;
		}
		const auto lhs_sign = hugeint_t(lhs.upper >> 63, static_cast<uint64_t>(lhs.upper >> 63));
		const auto rhs_sign = hugeint_t(rhs.upper >> 63, static_cast<uint64_t>(rhs.upper >> 63));
		const auto left = ((lhs ^ lhs_sign) - lhs_sign).ToUint128();
		const auto right = ((rhs ^ rhs_sign) - rhs_sign).ToUint128();
		const auto left_high = absl::Uint128High64(left);
		const auto right_high = absl::Uint128High64(right);
		if (left_high != 0 && right_high != 0) {
			return false;
		}
		const auto narrow = left_high != 0 ? absl::Uint128Low64(right) : absl::Uint128Low64(left);
		const auto wide_low = left_high != 0 ? absl::Uint128Low64(left) : absl::Uint128Low64(right);
		const auto low = absl::uint128(wide_low) * narrow;
		const auto high = absl::uint128(left_high | right_high) * narrow;
#if (__GNUC__ >= 5) || defined(__clang__)
		uint64_t top;
		if (absl::Uint128High64(high) != 0 ||
		    __builtin_add_overflow(absl::Uint128High64(low), absl::Uint128Low64(high), &top)) {
			return false;
		}
#else
		const auto top = absl::Uint128High64(low) + absl::Uint128Low64(high);
		if (absl::Uint128High64(high) != 0 || top < absl::Uint128High64(low)) {
			return false;
		}
#endif
		const auto sign = lhs_sign ^ rhs_sign;
		const auto product = absl::MakeUint128(top, absl::Uint128Low64(low));
		if (top >> 63 != 0 && (sign.upper == 0 || product != absl::uint128(1) << 127)) {
			return false;
		}
		result = (hugeint_t::FromUint128(product) ^ sign) - sign;
		return true;
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Multiply(hugeint_t lhs, hugeint_t rhs) {
		hugeint_t result;
		if (!TryMultiply(lhs, rhs, result)) {
			throw OutOfRangeException("Overflow in HUGEINT multiplication: %s * %s", lhs.ToString(), rhs.ToString());
		}
		return result;
	}

	static bool TryDivMod(hugeint_t lhs, hugeint_t rhs, hugeint_t &result, hugeint_t &remainder) {
		if (rhs == 0) {
			return false;
		}
		if (lhs == NumericLimits<hugeint_t>::Minimum() && rhs == -1) {
			return false;
		}
		result = DivMod(lhs, rhs, remainder);
		return true;
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Divide(hugeint_t lhs, hugeint_t rhs) {
		// No division by zero
		if (rhs == 0) {
			throw OutOfRangeException("Division of HUGEINT by zero: %s / %s", lhs.ToString(), rhs.ToString());
		}

		// division only has one reason to overflow: MINIMUM / -1
		if (lhs == NumericLimits<hugeint_t>::Minimum() && rhs == -1) {
			throw OutOfRangeException("Overflow in HUGEINT division: %s / %s", lhs.ToString(), rhs.ToString());
		}
		return Divide<false>(lhs, rhs);
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Modulo(hugeint_t lhs, hugeint_t rhs) {
		// No division by zero
		if (rhs == 0) {
			throw OutOfRangeException("Modulo of HUGEINT by zero: %s %% %s", lhs.ToString(), rhs.ToString());
		}

		// division only has one reason to overflow: MINIMUM / -1
		if (lhs == NumericLimits<hugeint_t>::Minimum() && rhs == -1) {
			throw OutOfRangeException("Overflow in HUGEINT modulo: %s %% %s", lhs.ToString(), rhs.ToString());
		}
		return Modulo<false>(lhs, rhs);
	}

	static bool TryAddInPlace(hugeint_t &lhs, hugeint_t rhs) {
#ifdef ABSL_HAVE_INTRINSIC_INT128
		__int128 sum;
		if (__builtin_add_overflow(static_cast<__int128>(lhs.ToInt128()), static_cast<__int128>(rhs.ToInt128()),
		                           &sum)) {
			return false;
		}
		lhs = hugeint_t::FromInt128(absl::int128(sum));
		return true;
#else
		auto result = lhs + rhs;
		if (((lhs.upper ^ result.upper) & (rhs.upper ^ result.upper)) < 0) {
			return false;
		}
		lhs = result;
		return true;
#endif
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Add(hugeint_t lhs, hugeint_t rhs) {
		if (!TryAddInPlace(lhs, rhs)) {
			throw OutOfRangeException("Overflow in HUGEINT addition: %s + %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	static bool TrySubtractInPlace(hugeint_t &lhs, hugeint_t rhs) {
#ifdef ABSL_HAVE_INTRINSIC_INT128
		__int128 difference;
		if (__builtin_sub_overflow(static_cast<__int128>(lhs.ToInt128()), static_cast<__int128>(rhs.ToInt128()),
		                           &difference)) {
			return false;
		}
		lhs = hugeint_t::FromInt128(absl::int128(difference));
		return true;
#else
		auto result = lhs - rhs;
		if (((lhs.upper ^ rhs.upper) & (lhs.upper ^ result.upper)) < 0) {
			return false;
		}
		lhs = result;
		return true;
#endif
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Subtract(hugeint_t lhs, hugeint_t rhs) {
		if (!TrySubtractInPlace(lhs, rhs)) {
			throw OutOfRangeException("Underflow in HUGEINT subtraction: %s - %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	// DivMod -> returns the result of the division (lhs / rhs), and fills up the remainder
	static hugeint_t DivMod(hugeint_t lhs, hugeint_t rhs, hugeint_t &remainder) {
		return hugeint_t::DivMod(lhs, rhs, remainder);
	}
	// DivMod but lhs MUST be positive, and rhs is a uint64_t
	static hugeint_t DivModPositive(hugeint_t lhs, uint64_t rhs, uint64_t &remainder) {
		D_ASSERT(lhs.upper >= 0);
		if (rhs == 0) {
			remainder = 0;
			return hugeint_t(0);
		}
		absl::uint128 rest;
		const auto quotient = uhugeint_t::DivModUint128(lhs.ToUint128(), rhs, rest);
		remainder = absl::Uint128Low64(rest);
		return hugeint_t::FromUint128(quotient);
	}

	static int Sign(hugeint_t n);
	static hugeint_t Abs(hugeint_t n) {
		if (n < 0) {
			return Negate(n);
		}
		return n;
	}
	// comparison operators
	static bool Equals(const hugeint_t &lhs, const hugeint_t &rhs) {
		return lhs == rhs;
	}

	static bool NotEquals(const hugeint_t &lhs, const hugeint_t &rhs) {
		return lhs != rhs;
	}

	static bool GreaterThan(const hugeint_t &lhs, const hugeint_t &rhs) {
		return lhs > rhs;
	}

	static bool GreaterThanEquals(const hugeint_t &lhs, const hugeint_t &rhs) {
		return lhs >= rhs;
	}

	static bool LessThan(const hugeint_t &lhs, const hugeint_t &rhs) {
		return lhs < rhs;
	}

	static bool LessThanEquals(const hugeint_t &lhs, const hugeint_t &rhs) {
		return lhs <= rhs;
	}

	static constexpr uint8_t CACHED_POWERS_OF_TEN = 39;
	static const hugeint_t POWERS_OF_TEN[CACHED_POWERS_OF_TEN];
};

template <>
inline void Hugeint::NegateInPlace<false>(hugeint_t &input) {
	input = -input;
}

template <>
inline hugeint_t Hugeint::Multiply<false>(hugeint_t lhs, hugeint_t rhs) {
	return lhs * rhs;
}

template <>
inline hugeint_t Hugeint::Divide<false>(hugeint_t lhs, hugeint_t rhs) {
	return lhs / rhs;
}

template <>
inline hugeint_t Hugeint::Modulo<false>(hugeint_t lhs, hugeint_t rhs) {
	return lhs % rhs;
}

template <>
inline hugeint_t Hugeint::Add<false>(hugeint_t lhs, hugeint_t rhs) {
	return lhs + rhs;
}

template <>
inline hugeint_t Hugeint::Subtract<false>(hugeint_t lhs, hugeint_t rhs) {
	return lhs - rhs;
}

template <class DST>
inline bool HugeintTryCastInteger(hugeint_t input, DST &result) {
	if constexpr (std::is_same_v<DST, uint64_t>) {
		if (input.upper != 0) {
			return false;
		}
	} else {
		const auto value = static_cast<int64_t>(input.lower);
		if (!input.FitsInt64() || value < static_cast<int64_t>(NumericLimits<DST>::Minimum()) ||
		    value > static_cast<int64_t>(NumericLimits<DST>::Maximum())) {
			return false;
		}
	}
	result = static_cast<DST>(input.lower);
	return true;
}

template <>
inline bool Hugeint::TryCast(hugeint_t input, int8_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, int16_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, int32_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, int64_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, uint8_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, uint16_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, uint32_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, uint64_t &result) {
	return HugeintTryCastInteger(input, result);
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, hugeint_t &result) {
	result = input;
	return true;
}
template <>
inline bool Hugeint::TryCast(hugeint_t input, uhugeint_t &result) {
	if (input.upper < 0) {
		return false;
	}
	result = uhugeint_t(static_cast<uint64_t>(input.upper), input.lower);
	return true;
}
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, float &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, double &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, long double &result);

template <class SRC>
inline bool HugeintTryConvertInteger(SRC value, hugeint_t &result) {
	if constexpr (std::is_signed_v<SRC>) {
		result = hugeint_t(static_cast<int64_t>(value));
	} else {
		result = hugeint_t(0, static_cast<uint64_t>(value));
	}
	return true;
}

template <>
inline bool Hugeint::TryConvert(int8_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(int16_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(int32_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(int64_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(uint8_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(uint16_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(uint32_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(uint64_t value, hugeint_t &result) {
	return HugeintTryConvertInteger(value, result);
}
template <>
inline bool Hugeint::TryConvert(hugeint_t value, hugeint_t &result) {
	result = value;
	return true;
}
template <>
bool Hugeint::TryConvert(float value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(double value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(long double value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(const char *value, hugeint_t &result);

} // namespace duckdb
