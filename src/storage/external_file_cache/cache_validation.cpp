#include "duckdb/storage/external_file_cache/cache_validation.hpp"

#include "duckdb/common/operator/subtract.hpp"

namespace duckdb {

bool CacheValidationInfo::IsCacheReuseProhibited() const {
	return cache_valid_until && *cache_valid_until == timestamp_t::ninfinity();
}

bool CacheValidationInfo::IsExpired() const {
	return cache_valid_until && Timestamp::GetCurrentTimestamp() >= *cache_valid_until;
}

//! Whether the last modified timestamp is usable as a cache validator
static bool HasUsableLastModified(timestamp_t last_modified) {
	return last_modified.IsFinite() && last_modified != timestamp_t(0);
}

bool CacheValidation::IsValid(bool validate, const string &cached_version_tag, timestamp_t cached_last_modified,
                              const string &current_version_tag, timestamp_t current_last_modified) {
	if (!validate) {
		return true; // Assume valid
	}
	if (!current_version_tag.empty() || !cached_version_tag.empty()) {
		return cached_version_tag == current_version_tag; // Validity checked by version tag
	}
	if (cached_last_modified != current_last_modified) {
		return false; // The file has certainly been modified
	}

	// If the modified time is not assigned, we can't validate it.
	if (!current_last_modified.IsFinite() || !cached_last_modified.IsFinite()) {
		return false;
	}

	// The last modified time matches. However, we cannot blindly trust this,
	// because some file systems use a low resolution clock to set the last modified time.
	// So, we will require that the last modified time is more than 10 seconds ago.
	static constexpr int64_t LAST_MODIFIED_THRESHOLD = 10LL * 1000LL * 1000LL;
	const auto access_time = Timestamp::GetCurrentTimestamp();
	if (access_time < current_last_modified) {
		return false; // Last modified in the future?
	}
	int64_t last_modified_time;
	if (!TrySubtractOperator::Operation(access_time, current_last_modified, last_modified_time)) {
		// out of range
		return false;
	}
	return last_modified_time > LAST_MODIFIED_THRESHOLD;
}

bool CacheValidation::HasValidationMetadata(const CacheValidationInfo &info) {
	return !info.version_tag.empty() || HasUsableLastModified(info.last_modified);
}

bool CacheValidation::IsValid(bool validate, const CacheValidationInfo &cached, const CacheValidationInfo &current) {
	if (cached.IsCacheReuseProhibited() || current.IsCacheReuseProhibited()) {
		return false;
	}
	if (!validate) {
		return true; // Assume valid
	}
	if (HasValidationMetadata(cached) || HasValidationMetadata(current)) {
		return IsValid(validate, cached.version_tag, cached.last_modified, current.version_tag, current.last_modified);
	}
	// No validators at all: cached data may be served within the freshness deadline the storage backend granted
	// when the cache entry was created (e.g., HTTP Cache-Control), as long as the file size is unchanged.
	if (cached.file_size != current.file_size) {
		return false; // The file has certainly been modified
	}
	if (!cached.cache_valid_until) {
		return false; // The backend does not provide expiry information, so we cannot validate at all
	}
	return Timestamp::GetCurrentTimestamp() < *cached.cache_valid_until;
}

} // namespace duckdb
