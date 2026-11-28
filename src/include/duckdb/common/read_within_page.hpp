//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/read_within_page.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"

#include <cstdint>
#include <cstring>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define DUCKDB_READS_PAST_OBJECTS 0
#endif
#endif
#if !defined(DUCKDB_READS_PAST_OBJECTS) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define DUCKDB_READS_PAST_OBJECTS 0
#endif
#ifndef DUCKDB_READS_PAST_OBJECTS
#define DUCKDB_READS_PAST_OBJECTS 1
#endif

namespace duckdb {

static constexpr idx_t READ_WITHIN_PAGE_BYTES = 4096;

inline bool ReadableWithinPage(const void *data, idx_t count) {
	return DUCKDB_READS_PAST_OBJECTS && count <= READ_WITHIN_PAGE_BYTES &&
	       (reinterpret_cast<uintptr_t>(data) & (READ_WITHIN_PAGE_BYTES - 1)) <= READ_WITHIN_PAGE_BYTES - count;
}

template <idx_t SLACK>
inline void CopyPaddedWithSlack(void *out, const void *data, idx_t size) {
	static constexpr idx_t BLOCK = 16;
	static_assert(SLACK % BLOCK == 0, "the slack is a whole number of blocks");
	if (size <= SLACK) {
		for (idx_t i = 0; i < SLACK; i += BLOCK) {
			memcpy(static_cast<char *>(out) + i, static_cast<const char *>(data) + i, BLOCK);
		}
	} else {
		memcpy(out, data, size);
	}
}

template <idx_t SLACK>
inline void CopyWithSlack(void *out, const void *data, idx_t size) {
	if (ReadableWithinPage(data, SLACK)) {
		CopyPaddedWithSlack<SLACK>(out, data, size);
	} else {
		memcpy(out, data, size);
	}
}

} // namespace duckdb
