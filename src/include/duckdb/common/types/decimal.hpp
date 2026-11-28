//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/decimal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/limits.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"

#include <type_traits>

namespace duckdb {

template <class PHYSICAL_TYPE>
struct DecimalWidth {};

template <>
struct DecimalWidth<int16_t> {
	static constexpr uint8_t max = 4;
};

template <>
struct DecimalWidth<int32_t> {
	static constexpr uint8_t max = 9;
};

template <>
struct DecimalWidth<int64_t> {
	static constexpr uint8_t max = 18;
};

template <>
struct DecimalWidth<hugeint_t> {
	static constexpr uint8_t max = 38;
};

//! The Decimal class is a static class that holds helper functions for the Decimal type
class Decimal {
public:
	static constexpr uint8_t MAX_WIDTH_INT16 = DecimalWidth<int16_t>::max;
	static constexpr uint8_t MAX_WIDTH_INT32 = DecimalWidth<int32_t>::max;
	static constexpr uint8_t MAX_WIDTH_INT64 = DecimalWidth<int64_t>::max;
	static constexpr uint8_t MAX_WIDTH_INT128 = DecimalWidth<hugeint_t>::max;
	static constexpr uint8_t MAX_WIDTH_DECIMAL = MAX_WIDTH_INT128;

public:
	//! Whether width/scale form a valid DECIMAL type: width in [1, MAX_WIDTH_DECIMAL] and scale not exceeding width.
	static bool IsValidWidthScale(uint8_t width, uint8_t scale) {
		return width >= 1 && width <= MAX_WIDTH_DECIMAL && scale <= width;
	}
	static string ToString(int16_t value, uint8_t width, uint8_t scale);
	static string ToString(int32_t value, uint8_t width, uint8_t scale);
	static string ToString(int64_t value, uint8_t width, uint8_t scale);
	static string ToString(hugeint_t value, uint8_t width, uint8_t scale);

	template <class T>
	static bool TryDecodeBigEndian(const_data_ptr_t bytes, idx_t size, T &result) {
		const bool negative = size != 0 && (bytes[0] & 0x80) != 0;
		if (size <= sizeof(int64_t)) {
			uint64_t value = negative ? NumericLimits<uint64_t>::Maximum() : 0;
			for (idx_t i = 0; i < size; i++) {
				value = (value << 8) | bytes[i];
			}
			const auto signed_value = static_cast<int64_t>(value);
			if constexpr (std::is_same<T, hugeint_t>::value) {
				result = hugeint_t(signed_value);
			} else {
				if (signed_value < NumericLimits<T>::Minimum() || signed_value > NumericLimits<T>::Maximum()) {
					return false;
				}
				result = static_cast<T>(signed_value);
			}
			return true;
		}
		const uint8_t sign = negative ? 0xFF : 0x00;
		const idx_t skip = size > sizeof(hugeint_t) ? size - sizeof(hugeint_t) : 0;
		for (idx_t i = 0; i < skip; i++) {
			if (bytes[i] != sign) {
				return false;
			}
		}
		if (skip && ((bytes[skip] & 0x80) != 0) != negative) {
			return false;
		}
		uint64_t upper = negative ? NumericLimits<uint64_t>::Maximum() : 0;
		uint64_t lower = upper;
		for (idx_t i = skip; i < size; i++) {
			upper = (upper << 8) | (lower >> 56);
			lower = (lower << 8) | bytes[i];
		}
		hugeint_t value;
		value.upper = static_cast<int64_t>(upper);
		value.lower = lower;
		if constexpr (std::is_same<T, hugeint_t>::value) {
			result = value;
			return true;
		} else {
			return Hugeint::TryCast(value, result);
		}
	}
};
} // namespace duckdb
