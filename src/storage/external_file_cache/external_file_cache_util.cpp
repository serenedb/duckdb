//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache_util.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/storage/external_file_cache/external_file_cache_util.hpp"

#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

CacheValidationInfo ExternalFileCacheUtil::GetValidationInfo(FileHandle &handle) {
	auto stats = handle.Stats();
	CacheValidationInfo result;
	result.file_size = NumericCast<idx_t>(stats.file_size);
	result.last_modified = stats.last_modification_time;
	result.cache_valid_until = stats.cache_valid_until;
	result.version_tag = std::move(stats.version_tag);
	return result;
}

CacheValidationMode ExternalFileCacheUtil::GetCacheValidationMode(const OpenFileInfo &info,
                                                                  optional_ptr<ClientContext> client_context,
                                                                  DatabaseInstance &db) {
	// First check if explicitly set in OpenFileInfo options (per-file override).
	bool validate_cache;
	if (info.extended_info != nullptr &&
	    info.extended_info->TryGetOption("validate_external_file_cache", validate_cache)) {
		return validate_cache ? CacheValidationMode::VALIDATE_ALL : CacheValidationMode::NO_VALIDATION;
	}

	// If client context is available, check client-local settings first, then fall back to database config.
	if (client_context) {
		return Settings::Get<ValidateExternalFileCacheSetting>(*client_context);
	}

	// No client context, fall back to database config.
	return Settings::Get<ValidateExternalFileCacheSetting>(db);
}

} // namespace duckdb
