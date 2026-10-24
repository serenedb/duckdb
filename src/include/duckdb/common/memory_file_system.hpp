//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/memory_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"

#include <absl/container/btree_map.h>
#include <absl/container/btree_set.h>
#include <string_view>

namespace duckdb {

struct MemoryFile;

//! Files that exist only in memory, under memory:// paths, written once from start to end. The instance a database
//! registers keeps them in unbacked blocks of its block cache, which spill instead of being dropped, so they can grow
//! past the cache, until Compact moves a file into one buffer-managed allocation that maps in place; a private
//! instance keeps each file in one heap allocation, which maps in place.
class MemoryFileSystem : public FileSystem {
public:
	static constexpr const char *PREFIX = "memory://";

	//! Files in blocks of the block cache of [db]
	explicit MemoryFileSystem(DatabaseInstance &db);
	//! Files on the heap, for an instance no virtual filesystem knows
	MemoryFileSystem();
	~MemoryFileSystem() override;

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	//! Maps a heap or compacted file, which stays pinned while mapped; a file in blocks is read with GetBlockFile
	unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options,
	                                           optional_ptr<FileOpener> opener = nullptr) override;
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	void Truncate(FileHandle &handle, int64_t new_size) override;
	void FileSync(FileHandle &handle) override;

	FileExistence TryDirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr,
	                                 optional_ptr<ErrorData> error = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;
	FileExistence TryFileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr,
	                            optional_ptr<ErrorData> error = nullptr) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;

	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	bool CanSeek() override;
	bool OnDiskFile(FileHandle &handle) override;
	bool CanHandleFile(const string &fpath) override;
	string GetName() const override;
	string PathSeparator(const string &path) override;

	//! The block cache's view of a file of a registered instance, whose blocks a reader pins; null for a heap or
	//! compacted file
	shared_ptr<BlockCacheFile> GetBlockFile(const string &path);
	//! Copies a closed file of a registered instance from its blocks into one buffer-managed allocation, which spills
	//! only while nothing maps it; readers that pinned its blocks keep them until they close
	void Compact(const string &path);
	//! Removes every file; the database calls it before its block cache goes
	void Clear();

private:
	shared_ptr<MemoryFile> Find(std::string_view path);
	//! The file at [path]; when there is none, null if [null_if_missing], else an error that [verb] found none
	shared_ptr<MemoryFile> FindExisting(const string &path, const char *verb, bool null_if_missing);
	shared_ptr<MemoryFile> Create(std::string_view path);
	static std::string_view Normalize(std::string_view path);

	optional_ptr<DatabaseInstance> db;
	annotated_mutex lock;
	absl::btree_map<string, shared_ptr<MemoryFile>> files DUCKDB_GUARDED_BY(lock);
	absl::btree_set<string> directories DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
