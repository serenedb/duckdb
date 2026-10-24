#include "duckdb/common/memory_file_system.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <cerrno>
#include <cstring>

namespace duckdb {

struct MemoryFile {
	explicit MemoryFile(string path_p) : path(std::move(path_p)), created(Timestamp::GetCurrentTimestamp()) {
	}

	const string path;
	const timestamp_t created;
	atomic<idx_t> size {0};

	annotated_mutex lock;
	//! A file of a registered instance, in unbacked block cache blocks
	shared_ptr<BlockCacheFile> blocks DUCKDB_GUARDED_BY(lock);
	//! A compacted file of a registered instance, in one buffer-managed allocation
	shared_ptr<BlockHandle> span DUCKDB_GUARDED_BY(lock);
	//! A heap file
	vector<data_t> heap DUCKDB_GUARDED_BY(lock);
	//! Open write handles; a heap file moves while it grows, so it maps only once they are closed
	idx_t writers DUCKDB_GUARDED_BY(lock) = 0;
};

namespace {

string DirectoryPrefix(std::string_view directory) {
	string prefix(directory);
	prefix += '/';
	return prefix;
}

unordered_map<string, string> ErrnoInfo(int error) {
	return {{"errno", std::to_string(error)}};
}

[[noreturn]] void ThrowMissing(const char *verb, const string &path) {
	throw IOException(ErrnoInfo(ENOENT), "Cannot %s file \"%s\": No such file or directory", verb, path);
}

void CopyRange(BlockCache &cache, const shared_ptr<BlockCacheFile> &file, idx_t offset, idx_t count,
               data_ptr_t target) {
	for (idx_t done = 0; done < count;) {
		const auto at = offset + done;
		auto pin = cache.Pin(file, at / BlockCache::BLOCK_SIZE);
		const auto in_block = at % BlockCache::BLOCK_SIZE;
		const auto chunk = MinValue(count - done, BlockCache::BLOCK_SIZE - in_block);
		memcpy(target + done, pin.Ptr() + in_block, chunk);
		done += chunk;
	}
}

class MemoryFileHandle : public FileHandle {
public:
	MemoryFileHandle(MemoryFileSystem &fs, shared_ptr<MemoryFile> file_p, FileOpenFlags flags,
	                 optional_ptr<BlockCache> cache)
	    : FileHandle(fs, file_p->path, flags), file(std::move(file_p)), writing(flags.OpenForWriting()) {
		if (writing) {
			annotated_lock_guard<annotated_mutex> guard(file->lock);
			file->writers++;
			if (file->blocks) {
				appender.emplace(*cache, file->blocks);
			}
		}
	}
	~MemoryFileHandle() override {
		MemoryFileHandle::Close();
	}

	void Close() override {
		if (appender) {
			appender->Finish();
			appender.reset();
		}
		if (writing) {
			annotated_lock_guard<annotated_mutex> guard(file->lock);
			file->writers--;
			writing = false;
		}
	}

	shared_ptr<MemoryFile> file;
	//! Writes a file in block cache blocks
	optional<BlockCacheAppender> appender;
	idx_t position = 0;
	bool writing;
};

class MemoryFileMapping : public MemoryMappedFile {
public:
	MemoryFileMapping(string path, FileOpenFlags flags, data_ptr_t data, idx_t size)
	    : MemoryMappedFile(std::move(path), std::move(flags), data, size) {
	}

	void Sync() override {
	}
	bool Trim(idx_t offset, idx_t length) override {
		return false;
	}
	bool OnDiskFile() const override {
		return false;
	}
	void Close() override {
		pin.Destroy();
		file.reset();
		data = nullptr;
		size = 0;
	}

	shared_ptr<MemoryFile> file;
	//! Keeps a compacted file in memory while it is mapped
	BufferHandle pin;
};

} // namespace

MemoryFileSystem::MemoryFileSystem(DatabaseInstance &db_p) : db(&db_p) {
}

MemoryFileSystem::MemoryFileSystem() {
}

MemoryFileSystem::~MemoryFileSystem() {
}

std::string_view MemoryFileSystem::Normalize(std::string_view path) {
	const auto prefix = strlen(PREFIX);
	while (path.size() > prefix && path.back() == '/') {
		path.remove_suffix(1);
	}
	return path;
}

bool MemoryFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(fpath, PREFIX);
}

string MemoryFileSystem::GetName() const {
	return "MemoryFileSystem";
}

string MemoryFileSystem::PathSeparator(const string &path) {
	return "/";
}

shared_ptr<MemoryFile> MemoryFileSystem::Find(std::string_view path) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = files.find(Normalize(path));
	return entry == files.end() ? nullptr : entry->second;
}

shared_ptr<MemoryFile> MemoryFileSystem::FindExisting(const string &path, const char *verb, bool null_if_missing) {
	auto file = Find(path);
	if (!file && !null_if_missing) {
		ThrowMissing(verb, path);
	}
	return file;
}

shared_ptr<MemoryFile> MemoryFileSystem::Create(std::string_view path) {
	auto file = make_shared_ptr<MemoryFile>(string(path));
	if (db) {
		annotated_lock_guard<annotated_mutex> file_guard(file->lock);
		file->blocks = BlockCache::Get(*db).CreateUnbackedFile(file->path);
	}
	shared_ptr<MemoryFile> replaced;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &slot = files[file->path];
		replaced = std::move(slot);
		slot = file;
	}
	return file;
}

unique_ptr<FileHandle> MemoryFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                  optional_ptr<FileOpener> opener) {
	const auto name = Normalize(path);
	if (flags.OpenForWriting()) {
		auto existing = Find(name);
		if (existing && flags.ReturnNullIfExists()) {
			return nullptr;
		}
		if (existing && !flags.OverwriteExistingFile()) {
			if (flags.ExclusiveCreate()) {
				throw IOException(ErrnoInfo(EEXIST), "Cannot create file \"%s\": it already exists", path);
			}
			throw NotImplementedException("Files under %s are written once; \"%s\" exists", PREFIX, path);
		}
		if (!existing && !flags.CreateFileIfNotExists() && !flags.OverwriteExistingFile()) {
			if (flags.ReturnNullIfNotExists()) {
				return nullptr;
			}
			ThrowMissing("open", path);
		}
		return make_uniq<MemoryFileHandle>(*this, Create(name), flags, db ? &BlockCache::Get(*db) : nullptr);
	}
	auto file = FindExisting(path, "open", flags.ReturnNullIfNotExists());
	if (!file) {
		return nullptr;
	}
	return make_uniq<MemoryFileHandle>(*this, std::move(file), flags, nullptr);
}

unique_ptr<MemoryMappedFile> MemoryFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                             const MMapOptions &options,
                                                             optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw NotImplementedException("Files under %s are mapped read-only", PREFIX);
	}
	auto file = FindExisting(path.path, "open", flags.ReturnNullIfNotExists());
	if (!file) {
		return nullptr;
	}
	shared_ptr<BlockHandle> span;
	{
		annotated_lock_guard<annotated_mutex> guard(file->lock);
		if (file->blocks) {
			throw NotImplementedException("\"%s\" is held in block cache blocks, which are read with GetBlockFile",
			                              path.path);
		}
		if (file->writers != 0) {
			throw IOException("Cannot map file \"%s\": it is still open for writing", path.path);
		}
		if (!file->span) {
			const auto size = file->size.load(std::memory_order_acquire);
			auto result = make_uniq<MemoryFileMapping>(file->path, flags, file->heap.data(), size);
			result->file = file;
			return std::move(result);
		}
		span = file->span;
	}
	auto pin = BufferManager::GetBufferManager(*db).Pin(span);
	auto result = make_uniq<MemoryFileMapping>(file->path, flags, pin.GetDataMutable(),
	                                           file->size.load(std::memory_order_acquire));
	result->file = std::move(file);
	result->pin = std::move(pin);
	return std::move(result);
}

shared_ptr<BlockCacheFile> MemoryFileSystem::GetBlockFile(const string &path) {
	auto file = FindExisting(path, "open", false);
	annotated_lock_guard<annotated_mutex> guard(file->lock);
	return file->blocks;
}

void MemoryFileSystem::Compact(const string &path) {
	auto file = FindExisting(path, "compact", false);
	shared_ptr<BlockCacheFile> blocks;
	{
		annotated_lock_guard<annotated_mutex> guard(file->lock);
		if (file->writers != 0) {
			throw IOException("Cannot compact file \"%s\": it is still open for writing", path);
		}
		blocks = file->blocks;
	}
	if (!blocks) {
		return;
	}
	const auto size = file->size.load(std::memory_order_acquire);
	auto span =
	    BufferManager::GetBufferManager(*db).Allocate(MemoryTag::IN_MEMORY_TABLE, MaxValue<idx_t>(size, 1), false);
	Allocator::AdviseHugePages(span.GetDataMutable(), size);
	CopyRange(BlockCache::Get(*db), blocks, 0, size, span.GetDataMutable());
	annotated_lock_guard<annotated_mutex> guard(file->lock);
	if (file->blocks == blocks) {
		file->span = span.GetBlockHandle();
		file->blocks.reset();
	}
}

void MemoryFileSystem::Clear() {
	decltype(files) removed;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		removed = std::move(files);
		files.clear();
		directories.clear();
	}
}

int64_t MemoryFileSystem::Write(FileHandle &handle_p, void *buffer, int64_t nr_bytes) {
	auto &handle = handle_p.Cast<MemoryFileHandle>();
	auto &file = *handle.file;
	auto source = const_data_ptr_cast(buffer);
	idx_t remaining = NumericCast<idx_t>(nr_bytes);
	if (handle.appender) {
		absl::Cleanup publish = [&] {
			file.size.store(handle.appender->Size(), std::memory_order_release);
		};
		handle.appender->Append(source, remaining);
	} else {
		if (db) {
			throw IOException("Cannot write to \"%s\": it is not open for writing", handle.path);
		}
		annotated_lock_guard<annotated_mutex> guard(file.lock);
		file.heap.insert(file.heap.end(), source, source + remaining);
		file.size.store(file.heap.size(), std::memory_order_release);
	}
	handle.position += NumericCast<idx_t>(nr_bytes);
	return nr_bytes;
}

void MemoryFileSystem::Write(FileHandle &handle_p, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &handle = handle_p.Cast<MemoryFileHandle>();
	if (location != handle.file->size.load(std::memory_order_acquire)) {
		throw NotImplementedException("Files under %s are written from start to end; \"%s\" has %llu bytes, not %llu",
		                              PREFIX, handle.path, handle.file->size.load(), location);
	}
	Write(handle_p, buffer, nr_bytes);
}

void MemoryFileSystem::Read(FileHandle &handle_p, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &handle = handle_p.Cast<MemoryFileHandle>();
	auto &file = *handle.file;
	const auto size = file.size.load(std::memory_order_acquire);
	const auto bytes = NumericCast<idx_t>(nr_bytes);
	if (location + bytes > size) {
		throw IOException("Could not read %llu bytes at %llu from \"%s\": it has %llu bytes", bytes, location,
		                  handle.path, size);
	}
	if (bytes == 0) {
		return;
	}
	auto target = data_ptr_cast(buffer);
	shared_ptr<BlockCacheFile> blocks;
	shared_ptr<BlockHandle> span;
	{
		annotated_lock_guard<annotated_mutex> guard(file.lock);
		if (!file.heap.empty()) {
			memcpy(target, file.heap.data() + location, bytes);
			return;
		}
		blocks = file.blocks;
		span = file.span;
	}
	if (span) {
		auto pin = BufferManager::GetBufferManager(*db).Pin(span);
		memcpy(target, pin.Ptr() + location, bytes);
		return;
	}
	CopyRange(BlockCache::Get(*db), blocks, location, bytes, target);
}

int64_t MemoryFileSystem::Read(FileHandle &handle_p, void *buffer, int64_t nr_bytes) {
	auto &handle = handle_p.Cast<MemoryFileHandle>();
	const auto size = handle.file->size.load(std::memory_order_acquire);
	const auto bytes = handle.position >= size ? 0 : MinValue(NumericCast<idx_t>(nr_bytes), size - handle.position);
	if (bytes > 0) {
		Read(handle_p, buffer, NumericCast<int64_t>(bytes), handle.position);
	}
	handle.position += bytes;
	return NumericCast<int64_t>(bytes);
}

int64_t MemoryFileSystem::GetFileSize(FileHandle &handle) {
	return NumericCast<int64_t>(handle.Cast<MemoryFileHandle>().file->size.load(std::memory_order_acquire));
}

timestamp_t MemoryFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return handle.Cast<MemoryFileHandle>().file->created;
}

FileType MemoryFileSystem::GetFileType(FileHandle &handle) {
	return FileType::FILE_TYPE_REGULAR;
}

void MemoryFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	throw NotImplementedException("Files under %s cannot be truncated", PREFIX);
}

void MemoryFileSystem::FileSync(FileHandle &handle) {
}

FileExistence MemoryFileSystem::TryDirectoryExists(const string &directory, optional_ptr<FileOpener> opener,
                                                   optional_ptr<ErrorData> error) {
	const auto name = Normalize(directory);
	const auto prefix = DirectoryPrefix(name);
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (directories.contains(name)) {
		return FileExistence::EXISTS;
	}
	const auto first = files.lower_bound(prefix);
	return first != files.end() && first->first.starts_with(prefix) ? FileExistence::EXISTS : FileExistence::MISSING;
}

void MemoryFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	directories.emplace(Normalize(directory));
}

void MemoryFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	const auto name = Normalize(directory);
	const auto prefix = DirectoryPrefix(name);
	vector<shared_ptr<MemoryFile>> removed;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		for (auto entry = files.lower_bound(prefix); entry != files.end() && entry->first.starts_with(prefix);) {
			removed.push_back(std::move(entry->second));
			entry = files.erase(entry);
		}
		directories.erase(name);
		for (auto entry = directories.lower_bound(prefix); entry != directories.end() && entry->starts_with(prefix);) {
			entry = directories.erase(entry);
		}
	}
}

bool MemoryFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                                 FileOpener *opener) {
	const auto name = Normalize(directory);
	const auto prefix = DirectoryPrefix(name);
	vector<pair<string, bool>> entries;
	bool exists = false;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		exists = directories.contains(name);
		unordered_set<string> subdirectories;
		for (auto file = files.lower_bound(prefix); file != files.end() && file->first.starts_with(prefix); ++file) {
			exists = true;
			auto rest = file->first.substr(prefix.size());
			const auto separator = rest.find('/');
			if (separator == string::npos) {
				entries.emplace_back(std::move(rest), false);
			} else if (subdirectories.insert(rest.substr(0, separator)).second) {
				entries.emplace_back(rest.substr(0, separator), true);
			}
		}
		for (auto it = directories.lower_bound(prefix); it != directories.end() && it->starts_with(prefix); ++it) {
			const auto &child = *it;
			auto rest = child.substr(prefix.size());
			const auto separator = rest.find('/');
			auto child_name = separator == string::npos ? rest : rest.substr(0, separator);
			if (subdirectories.insert(child_name).second) {
				entries.emplace_back(std::move(child_name), true);
			}
		}
	}
	for (auto &entry : entries) {
		callback(entry.first, entry.second);
	}
	return exists;
}

void MemoryFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	shared_ptr<MemoryFile> replaced;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto entry = files.find(Normalize(source));
		if (entry == files.end()) {
			throw IOException(ErrnoInfo(ENOENT), "Could not move \"%s\" to \"%s\": No such file or directory", source,
			                  target);
		}
		auto file = std::move(entry->second);
		files.erase(entry);
		auto &slot = files[string(Normalize(target))];
		replaced = std::move(slot);
		slot = std::move(file);
	}
}

FileExistence MemoryFileSystem::TryFileExists(const string &filename, optional_ptr<FileOpener> opener,
                                              optional_ptr<ErrorData> error) {
	return Find(filename) ? FileExistence::EXISTS : FileExistence::MISSING;
}

void MemoryFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	if (!TryRemoveFile(filename, opener)) {
		throw IOException(ErrnoInfo(ENOENT), "Could not remove file \"%s\": No such file or directory", filename);
	}
}

bool MemoryFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	shared_ptr<MemoryFile> removed;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto entry = files.find(Normalize(filename));
		if (entry == files.end()) {
			return false;
		}
		removed = std::move(entry->second);
		files.erase(entry);
	}
	return true;
}

void MemoryFileSystem::Seek(FileHandle &handle, idx_t location) {
	handle.Cast<MemoryFileHandle>().position = location;
}

idx_t MemoryFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<MemoryFileHandle>().position;
}

bool MemoryFileSystem::CanSeek() {
	return true;
}

bool MemoryFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

} // namespace duckdb
