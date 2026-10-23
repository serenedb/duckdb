//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/cache_validation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

struct CacheValidationInfo {
	//! Whether the backend explicitly prohibits reuse. Validators cannot make such an entry reusable.
	bool IsCacheReuseProhibited() const;
	//! Whether the freshness deadline has passed, including explicitly stale entries.
	//! An ordinarily expired entry can still be revalidated using validators.
	bool IsExpired() const;

	//! Version tag (e.g., HTTP ETag). Empty if the storage backend does not provide one.
	string version_tag;
	//! Last modified time. Zero/non-finite if the storage backend does not provide one.
	timestamp_t last_modified = timestamp_t(0);
	//! Freshness deadline for files without validators (e.g., HTTP Cache-Control).
	//! The deadline is exclusive: cached data may be served only while the current time is before it.
	//! Unset means the storage backend does not provide expiry information.
	//! Positive/negative infinity mean always valid/invalid.
	optional<timestamp_t> cache_valid_until;
	idx_t file_size = 0;
};

struct CacheValidation {
	DUCKDB_API static bool IsValid(bool validate, const string &cached_version_tag, timestamp_t cached_last_modified,
	                               const string &current_version_tag, timestamp_t current_last_modified);
	//! Variant that can also validate files without validators using the freshness deadline and file size
	DUCKDB_API static bool IsValid(bool validate, const CacheValidationInfo &cached,
	                               const CacheValidationInfo &current);
	//! Whether the version tag/last modified time provide any cache validation metadata
	DUCKDB_API static bool HasValidationMetadata(const CacheValidationInfo &info);
};

} // namespace duckdb
