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

	template <class... BYTES>
	uint64_t EqualAny(BYTES... bytes) const {
		const auto hits = [&](__m256i half) {
			__m256i any = _mm256_setzero_si256();
			((any = _mm256_or_si256(any, _mm256_cmpeq_epi8(half, _mm256_set1_epi8(bytes)))), ...);
			return static_cast<uint32_t>(_mm256_movemask_epi8(any));
		};
		return hits(low) | (static_cast<uint64_t>(hits(high)) << 32);
	}

	template <class... BYTES>
	bool ContainsAny(BYTES... bytes) const {
		__m256i any = _mm256_setzero_si256();
		((any = _mm256_or_si256(any, _mm256_or_si256(_mm256_cmpeq_epi8(low, _mm256_set1_epi8(bytes)),
		                                             _mm256_cmpeq_epi8(high, _mm256_set1_epi8(bytes))))),
		 ...);
		return !_mm256_testz_si256(any, any);
	}

	uint64_t NonAscii() const {
		const auto low_bits = static_cast<uint32_t>(_mm256_movemask_epi8(low));
		const auto high_bits = static_cast<uint32_t>(_mm256_movemask_epi8(high));
		return low_bits | (static_cast<uint64_t>(high_bits) << 32);
	}

	bool HasNonAscii() const {
		return !_mm256_testz_si256(_mm256_or_si256(low, high), _mm256_set1_epi8(static_cast<char>(0x80)));
	}

	uint64_t Lookup(const uint8_t *low_nibbles, const uint8_t *high_nibbles) const {
		const auto low_table =
		    _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i *>(low_nibbles)));
		const auto high_table =
		    _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i *>(high_nibbles)));
		const auto nibble = _mm256_set1_epi8(0x0F);
		const auto hits = [&](__m256i half) {
			const auto by_low = _mm256_shuffle_epi8(low_table, _mm256_and_si256(half, nibble));
			const auto by_high = _mm256_shuffle_epi8(high_table, _mm256_and_si256(_mm256_srli_epi16(half, 4), nibble));
			const auto none = _mm256_cmpeq_epi8(_mm256_and_si256(by_low, by_high), _mm256_setzero_si256());
			return ~static_cast<uint32_t>(_mm256_movemask_epi8(none));
		};
		return hits(low) | (static_cast<uint64_t>(hits(high)) << 32);
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
		const auto pattern = vdupq_n_u8(static_cast<uint8_t>(byte));
		return Bits(vceqq_u8(parts[0], pattern), vceqq_u8(parts[1], pattern), vceqq_u8(parts[2], pattern),
		            vceqq_u8(parts[3], pattern));
	}

	template <class... BYTES>
	uint64_t EqualAny(BYTES... bytes) const {
		const auto hits = [&](uint8x16_t part) {
			uint8x16_t any = vdupq_n_u8(0);
			((any = vorrq_u8(any, vceqq_u8(part, vdupq_n_u8(static_cast<uint8_t>(bytes))))), ...);
			return any;
		};
		return Bits(hits(parts[0]), hits(parts[1]), hits(parts[2]), hits(parts[3]));
	}

	template <class... BYTES>
	bool ContainsAny(BYTES... bytes) const {
		return EqualAny(bytes...) != 0;
	}

	uint64_t NonAscii() const {
		const auto any = vorrq_u8(vorrq_u8(parts[0], parts[1]), vorrq_u8(parts[2], parts[3]));
		return vmaxvq_u8(any) >= 0x80 ? ~uint64_t(0) : 0;
	}

	bool HasNonAscii() const {
		return NonAscii() != 0;
	}

	uint64_t Lookup(const uint8_t *low_nibbles, const uint8_t *high_nibbles) const {
		const auto low_table = vld1q_u8(low_nibbles);
		const auto high_table = vld1q_u8(high_nibbles);
		const auto nibble = vdupq_n_u8(0x0F);
		const auto hits = [&](uint8x16_t part) {
			return vtstq_u8(vqtbl1q_u8(low_table, vandq_u8(part, nibble)), vqtbl1q_u8(high_table, vshrq_n_u8(part, 4)));
		};
		return Bits(hits(parts[0]), hits(parts[1]), hits(parts[2]), hits(parts[3]));
	}

	static uint64_t Bits(uint8x16_t first, uint8x16_t second, uint8x16_t third, uint8x16_t fourth) {
		const uint8x16_t bits = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
		                         0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
		auto sum0 = vpaddq_u8(vandq_u8(first, bits), vandq_u8(second, bits));
		const auto sum1 = vpaddq_u8(vandq_u8(third, bits), vandq_u8(fourth, bits));
		sum0 = vpaddq_u8(sum0, sum1);
		sum0 = vpaddq_u8(sum0, sum0);
		return vgetq_lane_u64(vreinterpretq_u64_u8(sum0), 0);
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

	template <class... BYTES>
	uint64_t EqualAny(BYTES... needles) const {
		uint64_t result = 0;
		for (idx_t i = 0; i < CSV_BLOCK_BYTES; i++) {
			result |= static_cast<uint64_t>(((bytes[i] == needles) || ...)) << i;
		}
		return result;
	}

	template <class... BYTES>
	bool ContainsAny(BYTES... needles) const {
		return EqualAny(needles...) != 0;
	}

	uint64_t NonAscii() const {
		uint64_t result = 0;
		for (idx_t i = 0; i < CSV_BLOCK_BYTES; i++) {
			result |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i]) >> 7) << i;
		}
		return result;
	}

	bool HasNonAscii() const {
		return NonAscii() != 0;
	}

	uint64_t Lookup(const uint8_t *low_nibbles, const uint8_t *high_nibbles) const {
		uint64_t result = 0;
		for (idx_t i = 0; i < CSV_BLOCK_BYTES; i++) {
			const auto value = static_cast<uint8_t>(bytes[i]);
			result |= static_cast<uint64_t>((low_nibbles[value & 0x0F] & high_nibbles[value >> 4]) != 0) << i;
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

inline uint64_t CsvFindEscaped(uint64_t escapes, uint64_t &carry) {
	if (!escapes) {
		const uint64_t escaped = carry;
		carry = 0;
		return escaped;
	}
	constexpr uint64_t ODD_BITS = 0xAAAAAAAAAAAAAAAAULL;
	const uint64_t potential = escapes & ~carry;
	const uint64_t codes = (((potential << 1) | ODD_BITS) - potential) ^ ODD_BITS;
	const uint64_t escaped = codes ^ (escapes | carry);
	carry = (codes & escapes) >> 63;
	return escaped;
}

} // namespace duckdb
