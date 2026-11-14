//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/bswap.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace duckdb {

#ifndef DUCKDB_IS_BIG_ENDIAN
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define DUCKDB_IS_BIG_ENDIAN 1
#else
#define DUCKDB_IS_BIG_ENDIAN 0
#endif
#endif

template <class T>
inline T BSwap(T x) {
	if constexpr (std::is_floating_point_v<T>) {
		using Bits = std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t, uint64_t>;
		return std::bit_cast<T>(std::byteswap(std::bit_cast<Bits>(x)));
	} else {
		return std::byteswap(x);
	}
}

template <class T>
inline T BSwapIfLE(T x) {
	if constexpr (std::endian::native == std::endian::little) {
		return BSwap(x);
	} else {
		return x;
	}
}

template <class T>
inline T BSwapIfBE(T x) {
	if constexpr (std::endian::native == std::endian::big) {
		return BSwap(x);
	} else {
		return x;
	}
}

} // namespace duckdb
