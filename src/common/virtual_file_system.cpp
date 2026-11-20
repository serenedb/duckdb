#include "duckdb/common/virtual_file_system.hpp"

#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/pipe_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

VirtualFileSystem::VirtualFileSystem() : VirtualFileSystem(FileSystem::CreateLocal()) {
}

VirtualFileSystem::VirtualFileSystem(unique_ptr<FileSystem> &&inner) {
	default_file_system.name = inner->GetName();
	default_file_system.file_system = inner.get();
	default_file_system.owner = std::move(inner);
	VirtualFileSystem::RegisterCompressionFilesystem(make_uniq<GZipFileSystem>());
}

VirtualFileSystem::~VirtualFileSystem() {
}

unique_ptr<MemoryMappedFile> VirtualFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                              const MMapOptions &options,
                                                              optional_ptr<FileOpener> opener) {
	return FindFileSystem(path.path, opener).MemoryMapFile(path, flags, options, opener);
}

shared_ptr<InputFile> VirtualFileSystem::OpenBlockFile(const string &path) {
	return FindFileSystem(path, nullptr).OpenBlockFile(path);
}

unique_ptr<FileWriter> VirtualFileSystem::OpenFileWriter(const string &path, optional_ptr<FileOpener> opener) {
	return FindFileSystem(path, opener).OpenFileWriter(path, opener);
}

void VirtualFileSystem::Compact(const vector<string> &paths) {
	D_ASSERT(!paths.empty());
	FindFileSystem(paths[0], nullptr).Compact(paths);
}

FileSystem &VirtualFileSystem::GetDefaultFileSystem() {
	auto &fs = *default_file_system.file_system.load();
	if (default_file_system.disabled) {
		throw PermissionException("File system %s has been disabled by configuration", fs.GetName());
	}
	return fs;
}

optional_ptr<FileSystem> VirtualFileSystem::FindCompressionFileSystem(const FileCompressionType &compression,
                                                                      const string &filepath) {
	const idx_t count = compression_system_count;
	auto resolved = compression;
	// For auto-detection, check whether any registered compression filesystem can handle this file.
	if (resolved.IsAutoDetect()) {
		auto lower_path = StringUtil::Lower(filepath);
		if (StringUtil::EndsWith(lower_path, ".tmp")) {
			// strip .tmp
			lower_path = lower_path.substr(0, lower_path.length() - 4);
		}
		for (idx_t i = 0; i < count; i++) {
			auto file_system = compression_systems[i].file_system.load();
			if (file_system && file_system->CanHandleFile(lower_path)) {
				return file_system;
			}
		}
		if (!IsFileCompressed(lower_path, FileCompressionType::ZSTD)) {
			// no applicable compression filesystem was found - consider the file uncompressed
			return nullptr;
		}
		// the file looks zstd-compressed but zstd is not registered - fall through to raise an error below
		resolved = FileCompressionType::ZSTD;
	}
	if (resolved.IsUncompressed()) {
		return nullptr;
	}

	// An explicit compression type was requested - look it up in the registry.
	for (idx_t i = 0; i < count; i++) {
		auto &compression_system = compression_systems[i];
		auto file_system = compression_system.file_system.load();
		if (file_system && compression_system.compression == resolved.ToString()) {
			return file_system;
		}
	}
	// zstd support is provided by the parquet extension - hint at loading it
	string hint;
	if (resolved == FileCompressionType::ZSTD) {
		hint = "\nConsider explicitly \"INSTALL parquet; LOAD parquet;\" to support this compression scheme";
	}
	throw NotImplementedException(
	    "Attempting to open a compressed file, but the compression type is not supported (compression type \"%s\")%s",
	    resolved.ToString(), hint);
}

unique_ptr<FileHandle> VirtualFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                           optional_ptr<FileOpener> opener) {
	const FileCompressionType compression = flags.Compression();
	// open the base file handle in UNCOMPRESSED mode
	flags.SetCompression(FileCompressionType::UNCOMPRESSED);

	auto &internal_filesystem = FindFileSystem(file.path, opener);

	// File handle gets created.
	unique_ptr<FileHandle> file_handle = nullptr;

	// Handle caching logic.
	if (flags.GetCachingMode() != CachingMode::NO_CACHING) {
		auto caching_filesystem =
		    make_shared_ptr<CachingFileSystemWrapper>(internal_filesystem, opener, flags.GetCachingMode());
		// caching filesystem's lifecycle is extended inside of caching file handle.
		file_handle = caching_filesystem->OpenFile(file, flags, opener);
	} else {
		file_handle = internal_filesystem.OpenFile(file, flags, opener);
	}
	if (!file_handle) {
		return nullptr;
	}

	try {
		// Evaluate and apply compression option then.
		const auto context = !flags.MultiClientAccess() ? FileOpener::TryGetClientContext(opener) : QueryContext();
		if (file_handle->GetType() == FileType::FILE_TYPE_FIFO) {
			file_handle = PipeFileSystem::OpenPipe(context, std::move(file_handle));
		} else {
			auto compression_filesystem = FindCompressionFileSystem(compression, file.path);
			if (compression_filesystem) {
				file_handle =
				    compression_filesystem->OpenCompressedFile(context, std::move(file_handle), flags.OpenForWriting());
			}
		}
	} catch (...) {
		auto error = std::current_exception();
		if (flags.OpenForWriting() && file_handle) {
			try {
				file_handle->AbortWrite();
			} catch (...) { // NOLINT
			}
		}
		std::rethrow_exception(error);
	}
	return file_handle;
}

void VirtualFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	handle.file_system.Read(handle, buffer, nr_bytes, location);
}

void VirtualFileSystem::ReadScattered(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location) {
	handle.file_system.ReadScattered(handle, targets, location);
}

void VirtualFileSystem::ReadStreamed(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location,
                                     FileReadProgress &progress) {
	handle.file_system.ReadStreamed(handle, targets, location, progress);
}

void VirtualFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	handle.file_system.Write(handle, buffer, nr_bytes, location);
}

int64_t VirtualFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	return handle.file_system.Read(handle, buffer, nr_bytes);
}

int64_t VirtualFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	return handle.file_system.Write(handle, buffer, nr_bytes);
}

int64_t VirtualFileSystem::GetFileSize(FileHandle &handle) {
	return handle.file_system.GetFileSize(handle);
}
timestamp_t VirtualFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return handle.file_system.GetLastModifiedTime(handle);
}
string VirtualFileSystem::GetVersionTag(FileHandle &handle) {
	return handle.file_system.GetVersionTag(handle);
}
FileType VirtualFileSystem::GetFileType(FileHandle &handle) {
	return handle.file_system.GetFileType(handle);
}
FileMetadata VirtualFileSystem::Stats(FileHandle &handle) {
	return handle.file_system.Stats(handle);
}

optional<FileMetadata> VirtualFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	return FindFileSystem(file.path, opener).GetStatsIfExists(file, opener);
}

void VirtualFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	handle.file_system.Truncate(handle, new_size);
}

void VirtualFileSystem::FileSync(FileHandle &handle) {
	handle.file_system.FileSync(handle);
}

void VirtualFileSystem::HintWriteback(FileHandle &handle, idx_t location, idx_t nr_bytes) {
	handle.file_system.HintWriteback(handle, location, nr_bytes);
}

void VirtualFileSystem::SyncDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	FindFileSystem(directory, opener).SyncDirectory(directory, opener);
}

void VirtualFileSystem::SyncFiles(const vector<string> &paths, optional_ptr<FileOpener> opener) {
	using Batch = pair<reference<FileSystem>, vector<string>>;
	vector<Batch> batches;
	for (auto &path : paths) {
		auto &fs = FindFileSystem(path, opener);
		auto batch = std::find_if(batches.begin(), batches.end(),
		                          [&](const Batch &entry) { return RefersToSameObject(entry.first.get(), fs); });
		if (batch == batches.end()) {
			batch = batches.emplace(batches.end(), fs, vector<string>());
		}
		batch->second.push_back(path);
	}
	for (auto &batch : batches) {
		batch.first.get().SyncFiles(batch.second, opener);
	}
}

// need to look up correct fs for this
FileExistence VirtualFileSystem::TryDirectoryExists(const string &directory, optional_ptr<FileOpener> opener,
                                                    optional_ptr<ErrorData> error) {
	try {
		return FindFileSystem(directory, opener).TryDirectoryExists(directory, opener, error);
	} catch (std::exception &ex) {
		return ClassifyExistenceFailure(ex, error);
	}
}
void VirtualFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}, opener);
}

bool VirtualFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	return FindFileSystem(directory, opener).CreateDirectoryExtended(directory, options, opener);
}

void VirtualFileSystem::CreateDirectoriesRecursive(const string &path, optional_ptr<FileOpener> opener) {
	CreateDirectoryExtended(path, {CreateDirectoryMode::RECURSIVE}, opener);
}

void VirtualFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	RemoveDirectoryExtended(directory, {RemoveDirectoryMode::RECURSIVE}, opener);
}

bool VirtualFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	return FindFileSystem(directory, opener).RemoveDirectoryExtended(directory, options, opener);
}

bool VirtualFileSystem::ListFilesExtended(const string &directory,
                                          const std::function<void(OpenFileInfo &info)> &callback,
                                          optional_ptr<FileOpener> opener, const ListFilesOptions &options) {
	return FindFileSystem(directory, opener).ListFiles(directory, callback, opener, options);
}

void VirtualFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	FindFileSystem(source, opener).MoveFile(source, target, opener);
}

FileExistence VirtualFileSystem::TryFileExists(const string &filename, optional_ptr<FileOpener> opener,
                                               optional_ptr<ErrorData> error) {
	try {
		return FindFileSystem(filename, opener).TryFileExists(filename, opener, error);
	} catch (std::exception &ex) {
		return ClassifyExistenceFailure(ex, error);
	}
}

bool VirtualFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	return FindFileSystem(filename, opener).IsPipe(filename, opener);
}

void VirtualFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	FindFileSystem(filename, opener).RemoveFile(filename, opener);
}

bool VirtualFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	return FindFileSystem(filename, opener).TryRemoveFile(filename, opener);
}

void VirtualFileSystem::RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) {
	reference_map_t<FileSystem, vector<string>> files_by_fs;
	for (const auto &filename : filenames) {
		auto &fs = FindFileSystem(filename, opener);
		files_by_fs[fs].push_back(filename);
	}
	for (auto &entry : files_by_fs) {
		entry.first.get().RemoveFiles(entry.second, opener);
	}
}

string VirtualFileSystem::PathSeparator(const string &path) {
	return FindFileSystem(path, nullptr).PathSeparator(path);
}

string VirtualFileSystem::CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) {
	return FindFileSystem(path_p, opener).CanonicalizePath(path_p, opener);
}

unique_ptr<MultiFileList> VirtualFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                               optional_ptr<FileOpener> opener) {
	return FindFileSystem(path, opener).Glob(path, input, opener);
}

void VirtualFileSystem::RegisterSubSystem(unique_ptr<FileSystem> fs) {
	annotated_lock_guard<annotated_mutex> guard(registry_lock);
	auto name = fs->GetName();
	if (FindRegistered(name)) {
		throw InvalidInputException("Filesystem with name %s has already been registered, cannot re-register!", name);
	}
	const idx_t count = sub_system_count;
	if (count == MAX_SUB_SYSTEMS) {
		throw InvalidInputException("Filesystem %s cannot be registered: at most %d filesystems can be registered",
		                            name, MAX_SUB_SYSTEMS);
	}
	auto &sub_system = sub_systems[count];
	sub_system.disabled = disabled_file_systems.contains(name);
	sub_system.name = std::move(name);
	sub_system.file_system = fs.get();
	sub_system.owner = std::move(fs);
	sub_system_count = count + 1;
}

void VirtualFileSystem::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	annotated_lock_guard<annotated_mutex> guard(registry_lock);
	auto compression = fs->GetCompressionType().ToString();
	const idx_t count = compression_system_count;
	if (count == MAX_COMPRESSION_SYSTEMS) {
		throw InvalidInputException(
		    "Compression filesystem %s cannot be registered: at most %d compression filesystems can be registered",
		    compression, MAX_COMPRESSION_SYSTEMS);
	}
	auto &compression_system = compression_systems[count];
	compression_system.compression = compression;
	compression_system.file_system = fs.get();
	compression_system.owner = std::move(fs);
	compression_system_count = count + 1;
	for (idx_t i = 0; i < count; i++) {
		if (compression_systems[i].compression == compression) {
			compression_systems[i].file_system = nullptr;
		}
	}
}

optional_ptr<VirtualFileSystem::SubSystem> VirtualFileSystem::FindRegistered(const string &name) {
	const idx_t count = sub_system_count;
	for (idx_t i = 0; i < count; i++) {
		auto &sub_system = sub_systems[i];
		if (sub_system.file_system.load() && sub_system.name == name) {
			return sub_system;
		}
	}
	return nullptr;
}

optional_ptr<VirtualFileSystem::SubSystem> VirtualFileSystem::FindRemovable(const string &name) {
	if (disabled_file_systems.contains(name)) {
		return nullptr;
	}
	return FindRegistered(name);
}

void VirtualFileSystem::UnregisterSubSystem(const string &name) {
	annotated_lock_guard<annotated_mutex> guard(registry_lock);
	auto sub_system = FindRemovable(name);
	if (sub_system) {
		sub_system->file_system = nullptr;
	}
}

unique_ptr<FileSystem> VirtualFileSystem::ExtractSubSystem(const string &name) {
	annotated_lock_guard<annotated_mutex> guard(registry_lock);
	auto sub_system = FindRemovable(name);
	if (!sub_system) {
		return nullptr;
	}
	sub_system->file_system = nullptr;
	return std::move(sub_system->owner);
}

void VirtualFileSystem::SetDisabledFileSystems(const vector<string> &names) {
	annotated_lock_guard<annotated_mutex> guard(registry_lock);
	unordered_set<string> new_disabled_file_systems;
	for (auto &name : names) {
		if (name.empty()) {
			continue;
		}
		if (!new_disabled_file_systems.insert(name).second) {
			throw InvalidInputException("Duplicate disabled file system \"%s\"", name);
		}
	}
	for (auto &disabled_fs : disabled_file_systems) {
		if (!new_disabled_file_systems.contains(disabled_fs)) {
			throw InvalidInputException("File system \"%s\" has been disabled previously, it cannot be re-enabled",
			                            disabled_fs);
		}
	}
	disabled_file_systems = std::move(new_disabled_file_systems);
	default_file_system.disabled = disabled_file_systems.contains(default_file_system.name);
	const idx_t count = sub_system_count;
	for (idx_t i = 0; i < count; i++) {
		auto &sub_system = sub_systems[i];
		sub_system.disabled = disabled_file_systems.contains(sub_system.name);
	}
	any_disabled = !disabled_file_systems.empty();
}

vector<string> VirtualFileSystem::ListSubSystems() {
	vector<string> names;
	const idx_t count = sub_system_count;
	for (idx_t i = 0; i < count; i++) {
		auto &sub_system = sub_systems[i];
		if (sub_system.file_system.load()) {
			names.push_back(sub_system.name);
		}
	}
	return names;
}

std::string VirtualFileSystem::GetName() const {
	return "VirtualFileSystem";
}

bool VirtualFileSystem::SubSystemIsDisabled(const string &name) {
	if (!any_disabled) {
		return false;
	}
	if (name == default_file_system.name) {
		return default_file_system.disabled;
	}
	auto sub_system = FindRegistered(name);
	if (sub_system) {
		return sub_system->disabled;
	}
	annotated_lock_guard<annotated_mutex> guard(registry_lock);
	return disabled_file_systems.contains(name);
}

bool VirtualFileSystem::IsDisabledForPath(const string &path) {
	if (!any_disabled) {
		return false;
	}
	return FindSubSystem(path).sub_system.get().disabled;
}

FileSystem &VirtualFileSystem::FindFileSystem(const string &path, optional_ptr<FileOpener> opener) {
	auto match = FindSubSystem(path);
	if (RefersToSameObject(match.sub_system.get(), default_file_system)) {
		auto db_instance = FileOpener::TryGetDatabase(opener);
		if (db_instance) {
			ExtensionHelper::AutoLoadExtensionForPath(*db_instance, path);
			match = FindSubSystem(path);
		}
	}
	auto &fs = match.file_system.get();
	if (match.sub_system.get().disabled) {
		throw PermissionException("File system %s has been disabled by configuration", fs.GetName());
	}
	return fs;
}

VirtualFileSystem::SubSystemMatch VirtualFileSystem::FindSubSystem(const string &path) {
	SubSystemMatch match {default_file_system, *default_file_system.file_system.load()};
	const idx_t count = sub_system_count;
	for (idx_t i = 0; i < count; i++) {
		auto &sub_system = sub_systems[i];
		auto fs = sub_system.file_system.load();
		if (!fs || !fs->CanHandleFile(path)) {
			continue;
		}
		match = {sub_system, *fs};
		if (fs->IsManuallySet()) {
			break;
		}
	}
	return match;
}

} // namespace duckdb
