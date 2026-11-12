//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/uhugeint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/uhugeint.hpp"

namespace duckdb {

//! The uhugeint class contains static operations for the UINT128 type
class Uhugeint {
public:
	//! Convert a uhugeint object to a string
	static string ToString(uhugeint_t input);

	template <class T>
	DUCKDB_API static bool TryCast(uhugeint_t input, T &result);

	template <class T>
	static T Cast(uhugeint_t input) {
		T result = 0;
		TryCast(input, result);
		return result;
	}

	template <class T>
	static bool TryConvert(T value, uhugeint_t &result);

	template <class T>
	static uhugeint_t Convert(T value) {
		uhugeint_t result;
		if (!TryConvert(value, result)) { // LCOV_EXCL_START
			throw OutOfRangeException(double(value), GetTypeId<T>(), GetTypeId<uhugeint_t>());
		} // LCOV_EXCL_STOP
		return result;
	}

	static bool TryNegate(uhugeint_t input, uhugeint_t &result) {
		result = -input;
		return true;
	}

	// "The negative of an unsigned quantity is computed by subtracting its value from 2^n, where n is the number of
	// bits in the promoted operand."
	template <bool CHECK_OVERFLOW = true>
	inline static void NegateInPlace(uhugeint_t &input) {
		if (!TryNegate(input, input)) {
			throw OutOfRangeException("UHUGEINT is out of range");
		}
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Negate(uhugeint_t input) {
		NegateInPlace<CHECK_OVERFLOW>(input);
		return input;
	}

	static bool TryMultiply(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &result) {
		if ((lhs.upper | rhs.upper) == 0) [[likely]] {
			result = uhugeint_t::FromUint128(absl::uint128(lhs.lower) * rhs.lower);
			return true;
		}
		if (lhs.upper != 0 && rhs.upper != 0) {
			return false;
		}
		const auto low = absl::uint128(lhs.lower) * rhs.lower;
		const auto factor = lhs.upper != 0 ? rhs.lower : lhs.lower;
#if (__GNUC__ >= 5) || defined(__clang__)
		uint64_t cross;
		if (__builtin_mul_overflow(lhs.upper | rhs.upper, factor, &cross)) {
			return false;
		}
		uint64_t upper;
		if (__builtin_add_overflow(absl::Uint128High64(low), cross, &upper)) {
			return false;
		}
#else
		const auto cross = absl::uint128(lhs.upper | rhs.upper) * factor;
		const auto upper = absl::Uint128High64(low) + absl::Uint128Low64(cross);
		if (absl::Uint128High64(cross) != 0 || upper < absl::Uint128High64(low)) {
			return false;
		}
#endif
		result = uhugeint_t(upper, absl::Uint128Low64(low));
		return true;
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Multiply(uhugeint_t lhs, uhugeint_t rhs) {
		uhugeint_t result;
		if (!TryMultiply(lhs, rhs, result)) {
			throw OutOfRangeException("Overflow in UHUGEINT multiplication: %s * %s", lhs.ToString(), rhs.ToString());
		}
		return result;
	}

	static bool TryDivMod(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &result, uhugeint_t &remainder) {
		if (rhs == 0) {
			return false;
		}
		result = DivMod(lhs, rhs, remainder);
		return true;
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Divide(uhugeint_t lhs, uhugeint_t rhs) {
		// division between two same-size unsigned integers can only go wrong with division by zero
		if (rhs == 0) {
			throw OutOfRangeException("Division of UHUGEINT by zero: %s / %s", lhs.ToString(), rhs.ToString());
		}
		return Divide<false>(lhs, rhs);
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Modulo(uhugeint_t lhs, uhugeint_t rhs) {
		if (rhs == 0) {
			throw OutOfRangeException("Modulo of UHUGEINT by zero: %s %% %s", lhs.ToString(), rhs.ToString());
		}
		return Modulo<false>(lhs, rhs);
	}

	static bool TryAddInPlace(uhugeint_t &lhs, uhugeint_t rhs) {
#if (__GNUC__ >= 5) || defined(__clang__)
		uint64_t upper;
		auto carry = __builtin_add_overflow(lhs.lower, rhs.lower, &lhs.lower);
		auto overflow = __builtin_add_overflow(lhs.upper, rhs.upper, &upper);
		overflow |= __builtin_add_overflow(upper, uint64_t(carry), &lhs.upper);
		return !overflow;
#else
		uint64_t new_upper = lhs.upper + rhs.upper;
		bool no_overflow = !(new_upper < lhs.upper || new_upper < rhs.upper);
		new_upper += (lhs.lower + rhs.lower) < lhs.lower;
		if (new_upper < lhs.upper || new_upper < rhs.upper) {
			no_overflow = false;
		}
		lhs.upper = new_upper;
		lhs.lower += rhs.lower;
		return no_overflow;
#endif
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Add(uhugeint_t lhs, uhugeint_t rhs) {
		if (!TryAddInPlace(lhs, rhs)) {
			throw OutOfRangeException("Overflow in UHUGEINT addition: %s + %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	static bool TrySubtractInPlace(uhugeint_t &lhs, uhugeint_t rhs) {
#if (__GNUC__ >= 5) || defined(__clang__)
		uint64_t upper;
		auto borrow = __builtin_sub_overflow(lhs.lower, rhs.lower, &lhs.lower);
		auto overflow = __builtin_sub_overflow(lhs.upper, rhs.upper, &upper);
		overflow |= __builtin_sub_overflow(upper, uint64_t(borrow), &lhs.upper);
		return !overflow;
#else
		uint64_t new_upper = lhs.upper - rhs.upper - ((lhs.lower - rhs.lower) > lhs.lower);
		bool no_overflow = !(new_upper > lhs.upper);
		lhs.lower -= rhs.lower;
		lhs.upper = new_upper;
		return no_overflow;
#endif
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Subtract(uhugeint_t lhs, uhugeint_t rhs) {
		if (!TrySubtractInPlace(lhs, rhs)) {
			throw OutOfRangeException("Underflow in HUGEINT subtraction: %s - %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	// DivMod -> returns the result of the division (lhs / rhs), and fills up the remainder
	static uhugeint_t DivMod(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &remainder) {
		return uhugeint_t::DivMod(lhs, rhs, remainder);
	}

	static int Sign(hugeint_t n);
	static hugeint_t Abs(hugeint_t n);

	// comparison operators
	static bool Equals(uhugeint_t lhs, uhugeint_t rhs) {
		return lhs == rhs;
	}

	static bool NotEquals(uhugeint_t lhs, uhugeint_t rhs) {
		return lhs != rhs;
	}

	static bool GreaterThan(uhugeint_t lhs, uhugeint_t rhs) {
		return lhs > rhs;
	}

	static bool GreaterThanEquals(uhugeint_t lhs, uhugeint_t rhs) {
		return lhs >= rhs;
	}

	static bool LessThan(uhugeint_t lhs, uhugeint_t rhs) {
		return lhs < rhs;
	}

	static bool LessThanEquals(uhugeint_t lhs, uhugeint_t rhs) {
		return lhs <= rhs;
	}

	static constexpr uint8_t CACHED_POWERS_OF_TEN = 39;
	static const uhugeint_t POWERS_OF_TEN[CACHED_POWERS_OF_TEN];
};

template <>
inline void Uhugeint::NegateInPlace<false>(uhugeint_t &input) {
	input = -input;
}

template <>
inline uhugeint_t Uhugeint::Multiply<false>(uhugeint_t lhs, uhugeint_t rhs) {
	return lhs * rhs;
}

template <>
inline uhugeint_t Uhugeint::Divide<false>(uhugeint_t lhs, uhugeint_t rhs) {
	return lhs / rhs;
}

template <>
inline uhugeint_t Uhugeint::Modulo<false>(uhugeint_t lhs, uhugeint_t rhs) {
	return lhs % rhs;
}

template <>
inline uhugeint_t Uhugeint::Add<false>(uhugeint_t lhs, uhugeint_t rhs) {
	return lhs + rhs;
}

template <>
inline uhugeint_t Uhugeint::Subtract<false>(uhugeint_t lhs, uhugeint_t rhs) {
	return lhs - rhs;
}

template <class DST>
inline bool UhugeintTryCastInteger(uhugeint_t input, DST &result) {
	if (input.upper != 0 || input.lower > uint64_t(NumericLimits<DST>::Maximum())) {
		return false;
	}
	result = static_cast<DST>(input.lower);
	return true;
}

template <>
inline bool Uhugeint::TryCast(uhugeint_t input, int8_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, int16_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, int32_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, int64_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, uint8_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, uint16_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, uint32_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, uint64_t &result) {
	return UhugeintTryCastInteger(input, result);
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, hugeint_t &result) {
	if (input.upper > uint64_t(NumericLimits<int64_t>::Maximum())) {
		return false;
	}
	result = hugeint_t(static_cast<int64_t>(input.upper), input.lower);
	return true;
}
template <>
inline bool Uhugeint::TryCast(uhugeint_t input, uhugeint_t &result) {
	result = input;
	return true;
}
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, float &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, double &result);

template <class SRC>
inline bool UhugeintTryConvertInteger(SRC value, uhugeint_t &result) {
	if constexpr (std::is_signed_v<SRC>) {
		if (value < 0) {
			return false;
		}
	}
	result = uhugeint_t(static_cast<uint64_t>(value));
	return true;
}

template <>
inline bool Uhugeint::TryConvert(int8_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(int16_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(int32_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(int64_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(uint8_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(uint16_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(uint32_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(uint64_t value, uhugeint_t &result) {
	return UhugeintTryConvertInteger(value, result);
}
template <>
inline bool Uhugeint::TryConvert(uhugeint_t value, uhugeint_t &result) {
	result = value;
	return true;
}
template <>
bool Uhugeint::TryConvert(float value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(double value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(long double value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(const char *value, uhugeint_t &result);

} // namespace duckdb
