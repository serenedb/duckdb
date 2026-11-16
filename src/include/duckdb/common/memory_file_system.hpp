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
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

#include <absl/container/flat_hash_map.h>
#include <string_view>

namespace duckdb {

struct MemoryFile;

//! Files that exist only in memory, under memory:// paths, each written once from start to end through
//! OpenFileWriter and never changed after. The instance a database registers keeps them in unbacked blocks of its
//! external file cache, which spill instead of being dropped, so they can grow past the cache, until Compact replaces
//! a file by one buffer-managed allocation that maps in place; a private instance keeps each file in one heap
//! allocation, which maps in place once written. AddFile makes a file of memory its caller keeps for as long as the
//! file system. A directory is a path prefix: it exists while a file is under it, and listing or removing it goes
//! through every file, which only a dropped index and tests do.
class MemoryFileSystem : public FileSystem {
public:
	static constexpr const char *PREFIX = "memory://";

	//! Files in blocks of the external file cache of [db]
	explicit MemoryFileSystem(DatabaseInstance &db);
	//! Files on the heap, for an instance no virtual filesystem knows
	MemoryFileSystem();
	~MemoryFileSystem() override;

	//! Opens a file for reading
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	//! Maps a heap or compacted file, which stays pinned while mapped; a file in blocks is read with OpenBlockFile
	unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options,
	                                           optional_ptr<FileOpener> opener = nullptr) override;
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;

	FileExistence TryDirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr,
	                                 optional_ptr<ErrorData> error = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override;
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

	//! Null for a heap or compacted file
	shared_ptr<InputFile> OpenBlockFile(const string &path) override;
	unique_ptr<FileWriter> OpenFileWriter(const string &path, optional_ptr<FileOpener> opener = nullptr) override;
	void Compact(const vector<string> &paths) override;
	void AddFile(const string &path, data_ptr_t data, idx_t size);
	//! Removes every file; the database calls it before its external file cache goes
	void Clear();

private:
	friend class MemoryFileWriter;

	shared_ptr<MemoryFile> Find(std::string_view path);
	//! The file at [path]; when there is none, null if [null_if_missing], else an error that [verb] found none
	shared_ptr<MemoryFile> FindExisting(const string &path, const char *verb, bool null_if_missing);
	//! Puts [file] at its path
	void Install(shared_ptr<MemoryFile> file);
	//! Removes [file] from its path unless another file replaced it there
	void Unlink(const MemoryFile &file);
	static std::string_view Normalize(std::string_view path);

	optional_ptr<DatabaseInstance> db;
	annotated_mutex lock;
	absl::flat_hash_map<string, shared_ptr<MemoryFile>> files DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
