//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/caching_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/storage/external_file_cache/cache_validation.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"
#include "duckdb/storage/external_file_cache/file_buffer_handle_group.hpp"

namespace duckdb {

class Allocator;
class ClientContext;
class DatabaseInstance;
class CachingFileSystem;

struct ReadThroughputEstimator {
	//! Record a timed read.
	void AddSample(double seconds, idx_t bytes);
	//! Try to produce an estimate. Needs reads of at least two different sizes to separate latency from bandwidth.
	bool TryEstimate(NetworkThroughputEstimate &result) const;

private:
	mutable mutex lock;
	idx_t sample_count = 0;
	double sum_bytes = 0;
	double sum_seconds = 0;
	//! sum of bytes squared
	double sum_bytes_sq = 0;
	//! sum of bytes times seconds
	double sum_bytes_seconds = 0;
};

struct CachingFileHandle {
public:
	DUCKDB_API CachingFileHandle(QueryContext context, CachingFileSystem &caching_file_system, const OpenFileInfo &path,
	                             FileOpenFlags flags, optional_ptr<FileOpener> opener);
	DUCKDB_API ~CachingFileHandle();

public:
	//! Get shared ownership of the underlying FileHandle
	DUCKDB_API shared_ptr<FileHandle> GetFileHandle();
	//! Get the buffer-manager-backed Allocator.
	DUCKDB_API Allocator &GetBufferAllocator() const;
	//! Read [nr_bytes] bytes at the requested [location].
	//! Returns a buffer handle group that keeps the data pinned in memory.
	DUCKDB_API FileBufferHandleGroup Read(idx_t nr_bytes, idx_t location);
	//! Read [nr_bytes] bytes and sets [nr_bytes] to the actually read bytes.
	DUCKDB_API FileBufferHandleGroup Read(idx_t &nr_bytes);
	//! Get some properties of the file
	DUCKDB_API string GetPath() const;
	DUCKDB_API idx_t GetFileSize();
	DUCKDB_API timestamp_t GetLastModifiedTime();
	DUCKDB_API string GetVersionTag();
	DUCKDB_API bool Validate() const;
	DUCKDB_API bool CanSeek();
	DUCKDB_API bool IsRemoteFile() const;
	DUCKDB_API bool OnDiskFile();
	DUCKDB_API bool TryGetNetworkThroughput(NetworkThroughputEstimate &result);
	DUCKDB_API idx_t SeekPosition();
	DUCKDB_API void Seek(idx_t location);

private:
	//! Remove the 'force_full_download' option from the file handle if present, and return whether it was present
	bool StripForceFullDownloadIfPresent();
	//! Whether the settings let this file's reads go through the external file cache
	bool CacheAllowed() const;
	//! The external file cache's view of the file, registered on first use; null when reads must not use the cache
	shared_ptr<ExternalFileCacheFile> GetCachedFile();
	FileBufferHandleGroup ReadUncached(idx_t nr_bytes, idx_t location);
	//! Record a timed read of a local file into the throughput estimate
	void RecordReadThroughput(double total_seconds, idx_t bytes);

private:
	QueryContext context;
	//! The client caching file system that was used to create this CachingFileHandle
	CachingFileSystem &caching_file_system;
	//! For opening the file (possibly with extra info)
	OpenFileInfo path;
	//! Flags used to open the file
	FileOpenFlags flags;
	//! File opener, which contains file open context.
	optional_ptr<FileOpener> opener;
	//! Cache validation mode for this file
	CacheValidationMode validate;

	//! The metadata of the file: from the cache when it knew the file at the open, otherwise from opening it
	ExternalFileCacheFileInfo info;
	annotated_mutex file_handle_mutex;
	//! File handle for the internal filesystem.
	shared_ptr<FileHandle> file_handle DUCKDB_GUARDED_BY(file_handle_mutex);
	//! The freshness deadline of the metadata, which the responses to this handle's reads tighten
	optional<timestamp_t> cache_valid_until DUCKDB_GUARDED_BY(file_handle_mutex);
	shared_ptr<ExternalFileCacheFile> cached_file DUCKDB_GUARDED_BY(file_handle_mutex);

	//! Current position (if non-seeking reads)
	idx_t position;

	//! Throughput fitted from this handle's own uncached reads, used for local files (remote files measure their
	//! own).
	ReadThroughputEstimator throughput_estimator;
};

class CachingFileSystem {
private:
	friend struct CachingFileHandle;

public:
	// Notice, the provided [file_system] should be a raw, non-caching filesystem.
	DUCKDB_API CachingFileSystem(FileSystem &file_system, DatabaseInstance &db);
	DUCKDB_API ~CachingFileSystem();

public:
	DUCKDB_API static CachingFileSystem Get(ClientContext &context);

	DUCKDB_API unique_ptr<CachingFileHandle> OpenFile(const OpenFileInfo &path, const FileOpenFlags &flags,
	                                                  optional_ptr<FileOpener> opener = nullptr);
	DUCKDB_API unique_ptr<CachingFileHandle> OpenFile(QueryContext context, const OpenFileInfo &path,
	                                                  const FileOpenFlags &flags,
	                                                  optional_ptr<FileOpener> opener = nullptr);

private:
	//! Whether reads of [path] go through the external file cache: remote files, and local ones when
	//! cache_local_files is set
	bool ShouldCacheFile(const string &path) const;

	//! The Client FileSystem (needs to be client-specific so we can do, e.g., HTTPFS profiling)
	FileSystem &file_system;
	//! The DatabaseInstance.
	DatabaseInstance &db;
};

} // namespace duckdb
