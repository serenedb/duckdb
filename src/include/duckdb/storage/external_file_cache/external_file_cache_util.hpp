//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/external_file_cache_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/storage/external_file_cache/cache_validation.hpp"

namespace duckdb {

struct OpenFileInfo;
struct FileHandle;
class DatabaseInstance;
class ClientContext;

struct ExternalFileCacheUtil {
	//! What tells the versions of an open file apart, from one stat of its handle: every way into the cache records
	//! it, so that a later open of the same version asks the store nothing
	DUCKDB_API static CacheValidationInfo GetValidationInfo(FileHandle &handle);

	//! Get the cache validation mode in order of file open info, client context (for client-local settings), or
	//! database config.
	DUCKDB_API static CacheValidationMode
	GetCacheValidationMode(const OpenFileInfo &info, optional_ptr<ClientContext> client_context, DatabaseInstance &db);
};

} // namespace duckdb
