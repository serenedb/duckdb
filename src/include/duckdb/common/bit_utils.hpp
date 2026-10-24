//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/bit_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <bit>

namespace duckdb {

template <class T>
struct CountZeros {};

template <>
struct CountZeros<uint64_t> {
	inline static idx_t Leading(uint64_t value) {
		return static_cast<idx_t>(std::countl_zero(value));
	}
	inline static idx_t Trailing(uint64_t value) {
		return static_cast<idx_t>(std::countr_zero(value));
	}
};

template <>
struct CountZeros<uint32_t> {
	inline static idx_t Leading(uint32_t value) {
		return static_cast<idx_t>(std::countl_zero(value));
	}
	inline static idx_t Trailing(uint32_t value) {
		return static_cast<idx_t>(std::countr_zero(value));
	}
};

template <>
struct CountZeros<hugeint_t> {
	inline static idx_t Leading(hugeint_t value) {
		return static_cast<idx_t>(std::countl_zero(static_cast<unsigned __int128>(value.ToUint128())));
	}
	inline static idx_t Trailing(hugeint_t value) {
		return static_cast<idx_t>(std::countr_zero(static_cast<unsigned __int128>(value.ToUint128())));
	}
};

template <>
struct CountZeros<uhugeint_t> {
	inline static idx_t Leading(uhugeint_t value) {
		return static_cast<idx_t>(std::countl_zero(static_cast<unsigned __int128>(value.ToUint128())));
	}
	inline static idx_t Trailing(uhugeint_t value) {
		return static_cast<idx_t>(std::countr_zero(static_cast<unsigned __int128>(value.ToUint128())));
	}
};

} // namespace duckdb
