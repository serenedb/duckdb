//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_simd.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"

#include <cstring>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace duckdb {

static constexpr idx_t CSV_BLOCK_BYTES = 64;

inline uint64_t CsvSpread(uint64_t bit) {
	return uint64_t(0) - bit;
}

#if defined(__x86_64__)

struct CsvVector {
	explicit CsvVector(const char *block)
	    : low(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(block))),
	      high(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(block + 32))) {
	}

	uint64_t Equal(char byte) const {
		const auto pattern = _mm256_set1_epi8(byte);
		const auto low_bits = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(low, pattern)));
		const auto high_bits = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(high, pattern)));
		return low_bits | (static_cast<uint64_t>(high_bits) << 32);
	}

	uint64_t NonAscii() const {
		const auto low_bits = static_cast<uint32_t>(_mm256_movemask_epi8(low));
		const auto high_bits = static_cast<uint32_t>(_mm256_movemask_epi8(high));
		return low_bits | (static_cast<uint64_t>(high_bits) << 32);
	}

	__m256i low;
	__m256i high;
};

inline uint64_t CsvPrefixXor(uint64_t bits) {
	return static_cast<uint64_t>(
	    _mm_cvtsi128_si64(_mm_clmulepi64_si128(_mm_set_epi64x(0, static_cast<int64_t>(bits)), _mm_set1_epi8(-1), 0)));
}

#elif defined(__aarch64__)

struct CsvVector {
	explicit CsvVector(const char *block) {
		const auto bytes = reinterpret_cast<const uint8_t *>(block);
		for (idx_t i = 0; i < 4; i++) {
			parts[i] = vld1q_u8(bytes + i * 16);
		}
	}

	uint64_t Equal(char byte) const {
		const uint8x16_t bits = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
		                         0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
		const auto pattern = vdupq_n_u8(static_cast<uint8_t>(byte));
		auto sum0 = vpaddq_u8(vandq_u8(vceqq_u8(parts[0], pattern), bits), vandq_u8(vceqq_u8(parts[1], pattern), bits));
		const auto sum1 =
		    vpaddq_u8(vandq_u8(vceqq_u8(parts[2], pattern), bits), vandq_u8(vceqq_u8(parts[3], pattern), bits));
		sum0 = vpaddq_u8(sum0, sum1);
		sum0 = vpaddq_u8(sum0, sum0);
		return vgetq_lane_u64(vreinterpretq_u64_u8(sum0), 0);
	}

	uint64_t NonAscii() const {
		const auto any = vorrq_u8(vorrq_u8(parts[0], parts[1]), vorrq_u8(parts[2], parts[3]));
		return vmaxvq_u8(any) >= 0x80 ? ~uint64_t(0) : 0;
	}

	uint8x16_t parts[4];
};

inline uint64_t CsvPrefixXor(uint64_t bits) {
	return vgetq_lane_u64(vreinterpretq_u64_p128(vmull_p64(bits, ~uint64_t(0))), 0);
}

#else

struct CsvVector {
	explicit CsvVector(const char *block) {
		memcpy(bytes, block, CSV_BLOCK_BYTES);
	}

	uint64_t Equal(char byte) const {
		uint64_t result = 0;
		for (idx_t i = 0; i < CSV_BLOCK_BYTES; i++) {
			result |= static_cast<uint64_t>(bytes[i] == byte) << i;
		}
		return result;
	}

	uint64_t NonAscii() const {
		uint64_t result = 0;
		for (idx_t i = 0; i < CSV_BLOCK_BYTES; i++) {
			result |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i]) >> 7) << i;
		}
		return result;
	}

	char bytes[CSV_BLOCK_BYTES];
};

inline uint64_t CsvPrefixXor(uint64_t bits) {
	bits ^= bits << 1;
	bits ^= bits << 2;
	bits ^= bits << 4;
	bits ^= bits << 8;
	bits ^= bits << 16;
	bits ^= bits << 32;
	return bits;
}

#endif

} // namespace duckdb
