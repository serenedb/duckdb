#include "duckdb/common/memory_file_system.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cerrno>
#include <cstring>

namespace duckdb {

//! A memory file. Its writer is the only one to change it, until it closes it; after, the file never changes, and
//! Compact replaces it by another one at its path.
struct MemoryFile {
	MemoryFile(string path_p, timestamp_t created_p, shared_ptr<InputFile> blocks_p, shared_ptr<BlockHandle> span_p)
	    : path(std::move(path_p)), created(created_p), blocks(std::move(blocks_p)), span(std::move(span_p)) {
	}

	const string path;
	const timestamp_t created;
	//! A file of a registered instance, in unbacked blocks of the external file cache; readable while it is written
	const shared_ptr<InputFile> blocks;
	//! A compacted file of a registered instance, in one buffer-managed allocation
	const shared_ptr<BlockHandle> span;
	//! A heap file, grown by its writer without zeroing; nothing reads it before it is closed
	unsafe_unique_array<data_t> heap;
	idx_t heap_capacity = 0;
	//! The bytes of a closed heap file, or the bytes AddFile borrows
	data_ptr_t data = nullptr;
	atomic<idx_t> size {0};
	atomic<bool> closed {false};
};

namespace {

string DirectoryPrefix(std::string_view directory) {
	string prefix(directory);
	if (!prefix.ends_with('/')) {
		prefix += '/';
	}
	return prefix;
}

unordered_map<string, string> ErrnoInfo(int error) {
	return {{"errno", std::to_string(error)}};
}

[[noreturn]] void ThrowMissing(const char *verb, const string &path) {
	throw IOException(ErrnoInfo(ENOENT), "Cannot %s file \"%s\": No such file or directory", verb, path);
}

void RequireClosed(const MemoryFile &file, const char *verb) {
	if (!file.closed.load(std::memory_order_acquire)) {
		throw IOException("Cannot %s file \"%s\": it is still open for writing", verb, file.path);
	}
}

constexpr idx_t MIN_HEAP_CAPACITY = 4096;

void CopyRange(InputFile &file, idx_t offset, idx_t count, data_ptr_t target) {
	const auto &blocks = file.Blocks();
	for (idx_t done = 0; done < count;) {
		const auto at = offset + done;
		auto pin = file.GetCache().Pin(file, blocks.BlockOf(at));
		const auto in_block = blocks.OffsetInBlock(at);
		const auto chunk = MinValue(count - done, blocks.BlockSize() - in_block);
		memcpy(target + done, pin.Ptr() + in_block, chunk);
		done += chunk;
	}
}

} // namespace

//! Fills a memory file: in its cache blocks for the instance a database registers, on the heap otherwise
class MemoryFileWriter final : public FileWriter {
public:
	MemoryFileWriter(MemoryFileSystem &fs_p, shared_ptr<MemoryFile> file_p) : fs(fs_p), file(std::move(file_p)) {
		if (file->blocks) {
			appender.emplace(file->blocks);
		}
	}
	~MemoryFileWriter() override {
		MemoryFileWriter::Abort();
	}

	FileWriteWindow Window() override {
		if (appender) {
			return appender->Window();
		}
		const auto size = file->size.load(std::memory_order_relaxed);
		GrowHeap(size + 1);
		return {file->heap.get() + size, file->heap_capacity - size};
	}

	void Commit(idx_t nr_bytes, uint32_t crc32c) override {
		if (appender) {
			appender->Commit(nr_bytes);
			file->size.store(appender->Size(), std::memory_order_release);
			return;
		}
		const auto size = file->size.load(std::memory_order_relaxed);
		D_ASSERT(size + nr_bytes <= file->heap_capacity);
		file->size.store(size + nr_bytes, std::memory_order_release);
	}

	bool Append(const_data_ptr_t, idx_t) override {
		return false;
	}

	void Close() override {
		if (!open) {
			return;
		}
		open = false;
		if (appender) {
			appender->Finish();
			appender.reset();
		} else {
			file->data = file->heap.get();
		}
		file->closed.store(true, std::memory_order_release);
	}

	void Abort() noexcept override {
		if (!open) {
			return;
		}
		open = false;
		appender.reset();
		fs.Unlink(*file);
	}

private:
	void GrowHeap(idx_t needed) {
		if (needed <= file->heap_capacity) {
			return;
		}
		const auto capacity = MaxValue(needed, MaxValue(2 * file->heap_capacity, MIN_HEAP_CAPACITY));
		auto grown = make_unsafe_uniq_array_uninitialized<data_t>(capacity);
		const auto size = file->size.load(std::memory_order_relaxed);
		if (size != 0) {
			memcpy(grown.get(), file->heap.get(), size);
		}
		file->heap = std::move(grown);
		file->heap_capacity = capacity;
	}

	MemoryFileSystem &fs;
	shared_ptr<MemoryFile> file;
	//! Writes a file in cache blocks
	optional<ExternalFileCacheAppender> appender;
	bool open = true;
};

namespace {

class MemoryFileHandle : public FileHandle {
public:
	MemoryFileHandle(MemoryFileSystem &fs, shared_ptr<MemoryFile> file_p, FileOpenFlags flags)
	    : FileHandle(fs, file_p->path, flags), file(std::move(file_p)) {
	}

	void Close() override {
	}

	shared_ptr<MemoryFile> file;
	idx_t position = 0;
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

void MemoryFileSystem::Install(shared_ptr<MemoryFile> file) {
	shared_ptr<MemoryFile> replaced;
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto &slot = files[file->path];
	replaced = std::exchange(slot, std::move(file));
}

void MemoryFileSystem::Unlink(const MemoryFile &file) {
	shared_ptr<MemoryFile> removed;
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = files.find(file.path);
	if (entry != files.end() && entry->second.get() == &file) {
		removed = std::move(entry->second);
		files.erase(entry);
	}
}

void MemoryFileSystem::AddFile(const string &path, data_ptr_t data, idx_t size) {
	auto file =
	    make_shared_ptr<MemoryFile>(string(Normalize(path)), Timestamp::GetCurrentTimestamp(), nullptr, nullptr);
	file->data = data;
	file->size.store(size, std::memory_order_relaxed);
	file->closed.store(true, std::memory_order_release);
	Install(std::move(file));
}

unique_ptr<FileHandle> MemoryFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                  optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw NotImplementedException("Files under %s are written through OpenFileWriter: \"%s\"", PREFIX, path);
	}
	auto file = FindExisting(path, "open", flags.ReturnNullIfNotExists());
	if (!file) {
		return nullptr;
	}
	return make_uniq<MemoryFileHandle>(*this, std::move(file), flags);
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
	if (file->blocks) {
		throw NotImplementedException("\"%s\" is held in external file cache blocks, which are read with OpenBlockFile",
		                              path.path);
	}
	RequireClosed(*file, "map");
	const auto size = file->size.load(std::memory_order_relaxed);
	if (!file->span) {
		auto result = make_uniq<MemoryFileMapping>(file->path, flags, file->data, size);
		result->file = std::move(file);
		return std::move(result);
	}
	auto span = file->span;
	auto pin = BufferManager::GetBufferManager(*db).Pin(span);
	auto result = make_uniq<MemoryFileMapping>(file->path, flags, pin.GetDataMutable(), size);
	result->file = std::move(file);
	result->pin = std::move(pin);
	return std::move(result);
}

shared_ptr<InputFile> MemoryFileSystem::OpenBlockFile(const string &path) {
	return FindExisting(path, "open", false)->blocks;
}

unique_ptr<FileWriter> MemoryFileSystem::OpenFileWriter(const string &path, optional_ptr<FileOpener> opener) {
	const auto name = string(Normalize(path));
	auto file =
	    make_shared_ptr<MemoryFile>(name, Timestamp::GetCurrentTimestamp(),
	                                db ? ExternalFileCache::Get(*db).CreateUnbackedFile(name) : nullptr, nullptr);
	Install(file);
	return make_uniq<MemoryFileWriter>(*this, std::move(file));
}

void MemoryFileSystem::Compact(const vector<string> &paths) {
	vector<shared_ptr<MemoryFile>> sources;
	for (auto &path : paths) {
		auto file = FindExisting(path, "compact", false);
		RequireClosed(*file, "compact");
		if (file->blocks) {
			sources.push_back(std::move(file));
		}
	}
	if (sources.empty()) {
		return;
	}
	auto &buffer_manager = BufferManager::GetBufferManager(*db);
	vector<shared_ptr<MemoryFile>> compacted;
	compacted.reserve(sources.size());
	for (auto &file : sources) {
		const auto size = file->size.load(std::memory_order_relaxed);
		auto span = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, MaxValue<idx_t>(size, 1), false);
		CopyRange(*file->blocks, 0, size, span.GetDataMutable());
		auto &packed = compacted.emplace_back(
		    make_shared_ptr<MemoryFile>(file->path, file->created, nullptr, span.GetBlockHandle()));
		packed->size.store(size, std::memory_order_relaxed);
		packed->closed.store(true, std::memory_order_release);
	}
	vector<shared_ptr<MemoryFile>> replaced;
	annotated_lock_guard<annotated_mutex> guard(lock);
	for (idx_t i = 0; i < compacted.size(); i++) {
		auto entry = files.find(compacted[i]->path);
		if (entry != files.end() && entry->second == sources[i]) {
			replaced.push_back(std::exchange(entry->second, std::move(compacted[i])));
		}
	}
}

void MemoryFileSystem::Clear() {
	decltype(files) removed;
	annotated_lock_guard<annotated_mutex> guard(lock);
	removed.swap(files);
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
	if (file.blocks) {
		CopyRange(*file.blocks, location, bytes, target);
		return;
	}
	RequireClosed(file, "read");
	if (file.span) {
		auto span = file.span;
		auto pin = BufferManager::GetBufferManager(*db).Pin(span);
		memcpy(target, pin.Ptr() + location, bytes);
		return;
	}
	memcpy(target, file.data + location, bytes);
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

FileExistence MemoryFileSystem::TryDirectoryExists(const string &directory, optional_ptr<FileOpener> opener,
                                                   optional_ptr<ErrorData> error) {
	const auto prefix = DirectoryPrefix(Normalize(directory));
	annotated_lock_guard<annotated_mutex> guard(lock);
	for (const auto &entry : files) {
		if (entry.first.starts_with(prefix)) {
			return FileExistence::EXISTS;
		}
	}
	return FileExistence::MISSING;
}

void MemoryFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
}

void MemoryFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	const auto prefix = DirectoryPrefix(Normalize(directory));
	vector<shared_ptr<MemoryFile>> removed;
	annotated_lock_guard<annotated_mutex> guard(lock);
	absl::erase_if(files, [&](auto &entry) {
		if (!entry.first.starts_with(prefix)) {
			return false;
		}
		removed.push_back(std::move(entry.second));
		return true;
	});
}

bool MemoryFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                                 FileOpener *opener) {
	const auto prefix = DirectoryPrefix(Normalize(directory));
	vector<pair<string, bool>> entries;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		unordered_set<string> subdirectories;
		for (const auto &file : files) {
			if (!file.first.starts_with(prefix)) {
				continue;
			}
			auto rest = file.first.substr(prefix.size());
			const auto separator = rest.find('/');
			if (separator == string::npos) {
				entries.emplace_back(std::move(rest), false);
			} else if (subdirectories.insert(rest.substr(0, separator)).second) {
				entries.emplace_back(rest.substr(0, separator), true);
			}
		}
	}
	for (auto &entry : entries) {
		callback(entry.first, entry.second);
	}
	return !entries.empty();
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
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = files.find(Normalize(filename));
	if (entry == files.end()) {
		return false;
	}
	removed = std::move(entry->second);
	files.erase(entry);
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
