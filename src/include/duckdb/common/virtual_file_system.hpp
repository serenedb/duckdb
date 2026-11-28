//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/virtual_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

// bunch of wrappers to allow registering protocol handlers
class VirtualFileSystem : public FileSystem {
public:
	static constexpr idx_t MAX_SUB_SYSTEMS = 64;
	static constexpr idx_t MAX_COMPRESSION_SYSTEMS = 16;

	VirtualFileSystem();
	explicit VirtualFileSystem(unique_ptr<FileSystem> &&inner_file_system);
	~VirtualFileSystem() override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	void ReadScattered(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location) override;
	void ReadStreamed(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location,
	                  FileReadProgress &progress) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options,
	                                           optional_ptr<FileOpener> opener = nullptr) override;
	shared_ptr<InputFile> OpenBlockFile(const string &path) override;
	unique_ptr<FileWriter> OpenFileWriter(const string &path, optional_ptr<FileOpener> opener = nullptr) override;
	void Compact(const vector<string> &paths) override;

	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	FileMetadata Stats(FileHandle &handle) override;
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file,
	                                        optional_ptr<FileOpener> opener = nullptr) override;

	void Truncate(FileHandle &handle, int64_t new_size) override;

	void FileSync(FileHandle &handle) override;
	void HintWriteback(FileHandle &handle, idx_t location, idx_t nr_bytes) override;
	void SyncDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void SyncFiles(const vector<string> &paths, optional_ptr<FileOpener> opener = nullptr) override;

	// need to look up correct fs for this
	FileExistence TryDirectoryExists(const string &directory, optional_ptr<FileOpener> opener,
	                                 optional_ptr<ErrorData> error) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) override;
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener = nullptr) override;

	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) override;
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;

	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;
	void CopyFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;

	FileExistence TryFileExists(const string &filename, optional_ptr<FileOpener> opener,
	                            optional_ptr<ErrorData> error) override;

	bool IsPipe(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) override;

	void RegisterSubSystem(unique_ptr<FileSystem> fs) override;
	void RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) override;
	void UnregisterSubSystem(const string &name) override;
	unique_ptr<FileSystem> ExtractSubSystem(const string &name) override;

	vector<string> ListSubSystems() override;

	FileSystem &GetDefaultFileSystem();

	std::string GetName() const override;

	void SetDisabledFileSystems(const vector<string> &names) override;
	bool SubSystemIsDisabled(const string &name) override;
	bool IsDisabledForPath(const string &path) override;

	string PathSeparator(const string &path) override;

	string CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) override;

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;
	bool SupportsOpenFileExtended() const override {
		return true;
	}

	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener, const ListFilesOptions &options) override;

	bool SupportsListFilesExtended() const override {
		return true;
	}

	unique_ptr<MultiFileList> GlobFilesExtended(const string &path, const FileGlobInput &input,
	                                            optional_ptr<FileOpener> opener) override;
	bool SupportsGlobExtended() const override {
		return true;
	}

private:
	struct SubSystem {
		atomic<FileSystem *> file_system {nullptr};
		atomic<bool> disabled {false};
		string name;
		unique_ptr<FileSystem> owner;
	};

	struct CompressionSystem {
		atomic<CompressedFileSystem *> file_system {nullptr};
		string compression;
		unique_ptr<CompressedFileSystem> owner;
	};

	struct SubSystemMatch {
		reference<SubSystem> sub_system;
		reference<FileSystem> file_system;
	};

	FileSystem &FindFileSystem(const string &path, optional_ptr<FileOpener> file_opener);
	SubSystemMatch FindSubSystem(const string &path);
	optional_ptr<SubSystem> FindRegistered(const string &name);
	optional_ptr<SubSystem> FindRemovable(const string &name) DUCKDB_REQUIRES(registry_lock);
	// Return nullptr if compression is not involved, throw exception if compression is requested but no usable
	// filesystem gets registered.
	optional_ptr<FileSystem> FindCompressionFileSystem(const FileCompressionType &compression, const string &path);

private:
	annotated_mutex registry_lock;
	unordered_set<string> disabled_file_systems DUCKDB_GUARDED_BY(registry_lock);
	atomic<bool> any_disabled {false};
	SubSystem default_file_system;
	array<SubSystem, MAX_SUB_SYSTEMS> sub_systems;
	atomic<idx_t> sub_system_count {0};
	array<CompressionSystem, MAX_COMPRESSION_SYSTEMS> compression_systems;
	atomic<idx_t> compression_system_count {0};
};

} // namespace duckdb
