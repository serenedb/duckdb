#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <algorithm>
#include <bit>
#include <cstring>

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#endif
#endif

namespace duckdb {

namespace {

void UnpoisonPastEnd(data_ptr_t data, idx_t size) {
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
	__msan_unpoison(data, size);
#endif
#endif
}

constexpr idx_t MAX_IN_FLIGHT = 16 * ExternalFileCache::MAX_FILE_IN_FLIGHT;

//! Whether the block holds no data: its buffer is gone and no temporary file keeps a copy of it
bool IsDropped(const BlockMemory &memory) {
	return memory.IsUnloaded() && !memory.HasTemporaryFile();
}

//! Whether a block whose slot holds [memory] has to be fetched: it holds no data and no one is loading it
bool IsMissing(const BlockMemory *memory) {
	return !memory || (IsDropped(*memory) && !memory->IsLoading());
}

//! Gives back the claims in [claimed], clearing them
void ReleaseClaims(std::span<optional_ptr<BlockMemory>> claimed) {
	for (auto &memory : claimed) {
		if (memory) {
			memory->FinishLoad();
			memory = nullptr;
		}
	}
}

idx_t BlockBytesOf(const ExternalFileCacheBlocks &blocks, idx_t file_size, idx_t block) {
	const auto start = blocks.BlockStart(block);
	return start >= file_size ? 0 : MinValue(blocks.BlockSize(), file_size - start);
}

struct BlockRange {
	idx_t first;
	idx_t end;
};

//! The blocks [first, end) that hold the bytes [offset, offset + size) of a file of [file_size] bytes, as far as they
//! lie inside it
BlockRange BlocksOf(const ExternalFileCacheBlocks &blocks, idx_t file_size, idx_t offset, idx_t size) {
	if (size == 0 || offset >= file_size) {
		return {0, 0};
	}
	return {blocks.BlockOf(offset), blocks.BlockOf(MinValue(offset + size, file_size) - 1) + 1};
}

//! Narrows [pins] of the blocks that hold [offset, end) of a file to those bytes
void NarrowPins(const ExternalFileCacheBlocks &blocks, vector<FilePin> &pins, idx_t offset, idx_t end) {
	if (pins.empty()) {
		return;
	}
	auto &last = pins.back();
	last.Narrow(0, end - blocks.BlockStart(blocks.BlockOf(offset) + pins.size() - 1));
	auto &head = pins.front();
	head.Narrow(blocks.OffsetInBlock(offset), head.Size() - blocks.OffsetInBlock(offset));
}

//! Calls [callback] with the offset and the block count of each piece of a run of [count] blocks: equal pieces of at
//! most [max_blocks]
template <class FUNC>
void ForEachPiece(idx_t count, idx_t max_blocks, FUNC &&callback) {
	if (count == 0) {
		return;
	}
	const auto pieces = (count + max_blocks - 1) / max_blocks;
	const auto piece_blocks = (count + pieces - 1) / pieces;
	for (idx_t offset = 0; offset < count; offset += piece_blocks) {
		callback(offset, MinValue(piece_blocks, count - offset));
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// ExternalFileCacheBlocks
//===--------------------------------------------------------------------===//

template <class FUNC>
void ExternalFileCacheBlocks::ForEach(FUNC &&callback) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto directory = current.load(std::memory_order_relaxed);
	for (idx_t i = 0; i < directory->size; i++) {
		auto chunk = directory->chunks[i].load(std::memory_order_relaxed);
		if (!chunk) {
			continue;
		}
		for (idx_t j = 0; j < CHUNK; j++) {
			callback(i * CHUNK + j, chunk->slots[j]);
		}
	}
}

ExternalFileCacheBlocks::ExternalFileCacheBlocks(idx_t block_size, idx_t file_size)
    : block_shift(NumericCast<uint8_t>(std::countr_zero(block_size))) {
	D_ASSERT(IsPowerOfTwo(block_size));
	auto directory = make_uniq<Directory>(MaxValue<idx_t>(1, (BlockCountOf(file_size) + CHUNK - 1) / CHUNK));
	current.store(directory.get(), std::memory_order_release);
	annotated_lock_guard<annotated_mutex> guard(lock);
	directories.push_back(std::move(directory));
}

ExternalFileCacheSlot &ExternalFileCacheBlocks::Get(idx_t block) {
	auto found = Find(block);
	if (found) {
		return *found;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto directory = current.load(std::memory_order_relaxed);
	const auto index = block / CHUNK;
	if (index >= directory->size) {
		auto grown = make_uniq<Directory>(MaxValue(index + 1, 2 * directory->size));
		for (idx_t i = 0; i < directory->size; i++) {
			grown->chunks[i].store(directory->chunks[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
		directory = grown.get();
		current.store(directory, std::memory_order_release);
		directories.push_back(std::move(grown));
	}
	auto chunk = directory->chunks[index].load(std::memory_order_relaxed);
	if (!chunk) {
		auto created = make_uniq<Chunk>();
		chunk = created.get();
		directory->chunks[index].store(chunk, std::memory_order_release);
		chunks.push_back(std::move(created));
	}
	return chunk->slots[block % CHUNK];
}

//===--------------------------------------------------------------------===//
// InputFile
//===--------------------------------------------------------------------===//

InputFile::InputFile(ExternalFileCache &cache_p, string path_p, bool unbacked_p, ExternalFileCacheFileInfo info_p,
                     shared_ptr<FileHandle> source_p, shared_ptr<ExternalFileCacheBlocks> blocks_p)
    : cache(&cache_p), path(std::move(path_p)), size(info_p.validation.file_size), unbacked(unbacked_p),
      info(std::move(info_p)), source(std::move(source_p)), blocks(std::move(blocks_p)) {
}

InputFile::InputFile(unique_ptr<MemoryMappedFile> mapping_p)
    : path(mapping_p->GetPath()), size(mapping_p->Size()), unbacked(false), mapping(std::move(mapping_p)),
      paged(mapping->OnDiskFile()) {
	const auto length = mapping->Size();
	if (length != 0) {
		data = mapping->GetData(0, length);
		if (paged) {
			residency.Reset((length + PageResidency::PAGE_SIZE - 1) / PageResidency::PAGE_SIZE);
		}
	}
}

void InputFile::ThrowPastEnd(optional_ptr<const InputFile> file, idx_t offset, idx_t size) {
	if (file) {
		throw DataCorruptionException("Read past the end of \"%s\" at offset %llu of %llu", file->path, offset,
		                              file->GetSize());
	}
	throw DataCorruptionException("Read past the end of a buffer at offset %llu of %llu", offset, size);
}

FilePin InputFile::Pin(idx_t offset, idx_t nr_bytes) const {
	const auto length = GetSize();
	if (offset > length || nr_bytes > length - offset) {
		ThrowPastEnd(this, offset, length);
	}
	if (mapping) {
		return FilePin(data + offset, nr_bytes);
	}
	if (nr_bytes == 0) {
		return FilePin();
	}
	return GetCache().PinBytes(Mutable(), offset, nr_bytes);
}

FilePin InputFile::PinCopy(idx_t offset, idx_t nr_bytes) const {
	D_ASSERT(!mapping);
	const auto length = GetSize();
	if (offset > length || nr_bytes > length - offset) {
		ThrowPastEnd(this, offset, length);
	}
	if (nr_bytes == 0) {
		return FilePin();
	}
	return GetCache().PinCopy(Mutable(), offset, nr_bytes);
}

FilePin InputFile::PinShared(idx_t offset, idx_t nr_bytes) const {
	if (mapping || nr_bytes < MIN_SHARED || blocks->BlockOf(offset) == blocks->BlockOf(offset + nr_bytes - 1)) {
		return Pin(offset, nr_bytes);
	}
	return PinCopy(offset, nr_bytes);
}

void InputFile::ReadOnce(idx_t offset, data_ptr_t target, idx_t nr_bytes) const {
	const auto length = GetSize();
	if (offset > length || nr_bytes > length - offset) {
		ThrowPastEnd(this, offset, length);
	}
	if (mapping) {
		// the kernel reads the next windows while this one is copied, and drops each once it is
		constexpr idx_t WINDOW = 64ULL * 1024 * 1024;
		constexpr idx_t AHEAD = 2 * WINDOW;
		Prefetch(offset, MinValue(nr_bytes, AHEAD));
		for (idx_t done = 0; done < nr_bytes;) {
			const auto window = MinValue(nr_bytes - done, WINDOW);
			if (done + AHEAD < nr_bytes) {
				Prefetch(offset + done + AHEAD, MinValue(nr_bytes - done - AHEAD, WINDOW));
			}
			memcpy(target + done, data + offset + done, window);
			Evict(offset + done, window);
			done += window;
		}
		return;
	}
	auto &file = Mutable();
	auto &owner = GetCache();
	if (nr_bytes < BlockSize()) {
		// less than a block comes through the blocks, which the bytes next to it share
		idx_t done = 0;
		for (auto &pin : owner.PinRange(file, offset, nr_bytes)) {
			memcpy(target + done, pin.Ptr(), pin.Size());
			done += pin.Size();
		}
	} else {
		owner.ReadInto(file, offset, target, nr_bytes);
	}
	owner.MarkUnused(file, offset, nr_bytes);
}

idx_t InputFile::Hint(std::span<FileRange> ranges, idx_t budget) const noexcept {
	if (mapping) {
		for (idx_t i = 0; i < ranges.size() && budget != 0;) {
			const auto from = ranges[i].offset;
			auto to = from + ranges[i].size;
			for (i++; i < ranges.size() && ranges[i].offset <= to + PageResidency::PAGE_SIZE; i++) {
				D_ASSERT(ranges[i].offset >= from);
				to = MaxValue(to, ranges[i].offset + ranges[i].size);
			}
			const auto nr_bytes = MinValue(to - from, budget);
			budget -= nr_bytes;
			Hint(from, nr_bytes);
		}
		return budget;
	}
	idx_t kept = 0;
	for (; kept < ranges.size() && budget != 0; kept++) {
		auto &range = ranges[kept];
		range.size = MinValue(range.size, budget);
		budget -= range.size;
	}
	if (kept != 0) {
		try {
			GetCache().Prefetch(Mutable(), ranges.first(kept), ExternalFileCachePrefetch::HINT);
		} catch (...) { // NOLINT: the read of the bytes reports what failed
		}
	}
	return budget;
}

void InputFile::Prefetch(idx_t offset, idx_t nr_bytes) const noexcept {
	if (mapping) {
		if (paged) {
			mapping->Prefetch(offset, nr_bytes);
		}
		return;
	}
	try {
		GetCache().Prefetch(Mutable(), offset, nr_bytes);
	} catch (...) { // NOLINT: the read of the bytes reports what failed
	}
}

void InputFile::Prefetch(std::span<FileRange> ranges) const noexcept {
	if (mapping) {
		for (auto &range : ranges) {
			Hint(range.offset, range.size);
		}
		return;
	}
	try {
		GetCache().Prefetch(Mutable(), ranges, ExternalFileCachePrefetch::NEED);
	} catch (...) { // NOLINT: the read of the bytes reports what failed
	}
}

idx_t InputFile::ReadAhead(idx_t offset, idx_t nr_bytes, idx_t needed) const noexcept {
	if (mapping) {
		Hint(offset, nr_bytes);
		return offset + nr_bytes;
	}
	try {
		return GetCache().ReadAhead(Mutable(), offset, nr_bytes, needed);
	} catch (...) { // NOLINT: the read of the bytes reports what failed
		return offset + nr_bytes;
	}
}

void InputFile::Evict(idx_t offset, idx_t nr_bytes) const noexcept {
	if (!mapping) {
		GetCache().MarkUnused(*this, offset, nr_bytes);
	} else if (paged) {
		mapping->Evict(offset, nr_bytes);
	}
}

idx_t InputFile::Warm(idx_t offset, idx_t nr_bytes) const noexcept {
	if (!mapping) {
		return GetCache().IsHeld(*this, offset, nr_bytes) ? nr_bytes : 0;
	}
	if (!paged) {
		return nr_bytes;
	}
	return nr_bytes == 0 ? 0 : WarmPaged(offset, nr_bytes);
}

void InputFile::MarkWarm(idx_t offset, idx_t nr_bytes) const noexcept {
	const auto length = size.load(std::memory_order_relaxed);
	if (!paged || nr_bytes == 0 || offset >= length) {
		return;
	}
	const auto last = (MinValue(offset + nr_bytes, length) - 1) / PageResidency::PAGE_SIZE;
	const auto epoch = PageResidency::Epoch();
	if (residency.Valid(epoch) || residency.Adopt(epoch)) {
		residency.Set(offset / PageResidency::PAGE_SIZE, last);
	}
}

bool InputFile::EvictedSlow(uint32_t &epoch) noexcept {
	const auto current = PageResidency::Epoch();
	if (current == epoch) {
		return false;
	}
	epoch = current;
	return true;
}

bool InputFile::IsResidentSlow(idx_t offset, idx_t nr_bytes) const noexcept {
	if (!mapping) {
		return GetCache().IsHeld(*this, offset, nr_bytes);
	}
	const auto length = size.load(std::memory_order_relaxed);
	if (offset >= length) {
		return false;
	}
	nr_bytes = MinValue(nr_bytes, length - offset);
	if (nr_bytes == 0) {
		return false;
	}
	if (!paged) {
		return true;
	}
	if (!PageResidency::IsResident(data + offset, nr_bytes)) {
		return false;
	}
	MarkWarm(offset, nr_bytes);
	return true;
}

bool InputFile::HintSlow(idx_t offset, idx_t nr_bytes) const noexcept {
	if (!mapping) {
		try {
			return GetCache().Prefetch(Mutable(), offset, nr_bytes);
		} catch (...) { // NOLINT: the read of the bytes reports what failed
			return true;
		}
	}
	if (!paged) {
		return false;
	}
	auto warm = Warm(offset, nr_bytes);
	D_ASSERT(warm <= nr_bytes);
	if (warm != 0 && PageResidency::Poll(warm)) {
		warm = 0;
	}
	if (warm == nr_bytes) {
		return false;
	}
	offset += warm;
	nr_bytes -= warm;
	const bool fetch = nr_bytes <= HINT_CHUNK || !IsResidentSlow(offset, nr_bytes);
	if (fetch) {
		mapping->Prefetch(offset, nr_bytes);
		MarkWarm(offset, nr_bytes);
	}
	return fetch;
}

bool InputFile::HasSource() const {
	annotated_lock_guard<annotated_mutex> guard(source_lock);
	return source != nullptr;
}

void InputFile::SetSource(shared_ptr<FileHandle> handle) {
	annotated_lock_guard<annotated_mutex> guard(source_lock);
	if (!source) {
		source = std::move(handle);
	}
}

shared_ptr<FileHandle> InputFile::Source() const {
	annotated_lock_guard<annotated_mutex> guard(source_lock);
	if (!source) {
		throw InternalException("The external file cache has no source to read \"%s\" from", path);
	}
	return source;
}

idx_t InputFile::BlockBytes(idx_t block) const {
	return BlockBytesOf(*blocks, GetSize(), block);
}

//===--------------------------------------------------------------------===//
// ExternalFileCacheAppender
//===--------------------------------------------------------------------===//

ExternalFileCacheAppender::ExternalFileCacheAppender(shared_ptr<InputFile> file_p) : file(std::move(file_p)) {
}

void ExternalFileCacheAppender::Append(const_data_ptr_t data, idx_t count) {
	while (count > 0) {
		const auto window = Window();
		const auto chunk = MinValue(count, window.size);
		memcpy(window.data, data, chunk);
		Commit(chunk);
		data += chunk;
		count -= chunk;
	}
}

FileWriteWindow ExternalFileCacheAppender::Window() {
	const auto &blocks = file->Blocks();
	const auto offset = blocks.OffsetInBlock(size);
	if (!current.IsValid()) {
		current = file->GetCache().CreateBlock(*file, blocks.BlockOf(size));
	}
	return {current.GetDataMutable() + offset, blocks.BlockSize() - offset};
}

void ExternalFileCacheAppender::Commit(idx_t count) {
	const auto &blocks = file->Blocks();
	const auto offset = blocks.OffsetInBlock(size);
	D_ASSERT(current.IsValid() || count == 0);
	D_ASSERT(offset + count <= blocks.BlockSize());
	if (count == 0) {
		return;
	}
	size += count;
	if (file->IsUnbacked()) {
		file->GetCache().SetUnbackedSize(*file, size);
	}
	if (offset + count == blocks.BlockSize()) {
		current.Reset();
	}
}

FilePin ExternalFileCacheAppender::ShareWindow(idx_t count) {
	return current.Share(file->Blocks().OffsetInBlock(size), count);
}

void ExternalFileCacheAppender::Finish(const string &version_tag, timestamp_t last_modified) {
	if (current.IsValid()) {
		const auto &blocks = file->Blocks();
		const auto end = blocks.OffsetInBlock(size);
		UnpoisonPastEnd(current.GetDataMutable() + end, blocks.BlockSize() - end);
		current.Reset();
	}
	if (!file->IsUnbacked()) {
		CacheValidationInfo written;
		written.file_size = size;
		written.version_tag = version_tag;
		written.last_modified = last_modified;
		file->GetCache().PublishWrittenFile(*file, written);
	}
}

void ExternalFileCacheAppender::Abandon() {
	D_ASSERT(!file->IsUnbacked());
	current.Reset();
	file->GetCache().ForbidReuse(*file);
}

//===--------------------------------------------------------------------===//
// ExternalFileCacheFetchTask
//===--------------------------------------------------------------------===//

struct ExternalFileCache::PendingFetch {
	PendingFetch(shared_ptr<InputFile> file_p, idx_t first_block_p, vector<optional_ptr<BlockMemory>> claimed_p,
	             idx_t bytes_p, bool awaited_p)
	    : file(std::move(file_p)), first_block(first_block_p), claimed(std::move(claimed_p)), bytes(bytes_p),
	      awaited(awaited_p) {
		file->in_flight.fetch_add(bytes, std::memory_order_relaxed);
		file->GetCache().in_flight.fetch_add(bytes, std::memory_order_relaxed);
	}

	~PendingFetch() {
		ReleaseClaims(claimed);
		Settle();
	}

	void Run() {
		try {
			file->GetCache().Fetch(*file, first_block, claimed, {}, awaited);
		} catch (std::exception &) { // NOLINT
		}
		Settle();
	}

	void Settle() {
		if (std::exchange(settled, true)) {
			return;
		}
		file->in_flight.fetch_sub(bytes, std::memory_order_relaxed);
		file->GetCache().in_flight.fetch_sub(bytes, std::memory_order_relaxed);
	}

	shared_ptr<InputFile> file;
	idx_t first_block;
	vector<optional_ptr<BlockMemory>> claimed;
	idx_t bytes;
	bool awaited;
	bool taken = false;
	bool settled = false;
};

class ExternalFileCacheFetchTask : public BaseExecutorTask {
public:
	ExternalFileCacheFetchTask(TaskExecutor &executor, shared_ptr<ExternalFileCache::PendingFetch> fetch_p,
	                           bool taken_p = false)
	    : BaseExecutorTask(executor), fetch(std::move(fetch_p)), taken(taken_p) {
	}

	void ExecuteTask() override {
		if (taken || fetch->file->GetCache().TakeFetch(*fetch)) {
			fetch->Run();
		}
	}

	string TaskType() const override {
		return "ExternalFileCacheFetchTask";
	}

private:
	shared_ptr<ExternalFileCache::PendingFetch> fetch;
	bool taken;
};

//===--------------------------------------------------------------------===//
// ExternalFileCache
//===--------------------------------------------------------------------===//

ExternalFileCache::ExternalFileCache(DatabaseInstance &db_p)
    : db(db_p), buffer_manager(BufferManager::GetBufferManager(db_p)) {
}

ExternalFileCache::~ExternalFileCache() {
	StopFetching();
}

ExternalFileCache &ExternalFileCache::Get(DatabaseInstance &db) {
	return db.GetExternalFileCache();
}

ExternalFileCache &ExternalFileCache::Get(ClientContext &context) {
	return context.db->GetExternalFileCache();
}

FilePin ExternalFileCache::AllocateUncached(DatabaseInstance &db, idx_t size) {
	auto buffer = BufferManager::GetBufferManager(db).Allocate(MemoryTag::EXTERNAL_FILE_CACHE, size);
	buffer.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	const auto data = buffer.GetFileBuffer().InternalBuffer();
	return FilePin(std::move(buffer), data, size);
}

void ExternalFileCache::DropBlocks(ExternalFileCacheBlocks &blocks) {
	vector<reference<BlockMemory>> dropped;
	blocks.ForEach([&](idx_t, ExternalFileCacheSlot &slot) {
		if (auto memory = slot.exchange(nullptr, std::memory_order_seq_cst)) {
			dropped.push_back(*memory);
		}
	});
	vector<shared_ptr<BlockHandle>> copies;
	{
		annotated_lock_guard<annotated_mutex> guard(blocks.lock);
		copies.reserve(blocks.copies.size());
		for (auto &copy : blocks.copies) {
			dropped.push_back(copy.second.handle->GetMemory());
			copies.push_back(std::move(copy.second.handle));
		}
		blocks.copies.clear();
		blocks.copy_count.store(0, std::memory_order_relaxed);
	}
	for (auto &entry : dropped) {
		auto &memory = entry.get();
		while (!memory.TryClaim()) {
			WaitForLoad(memory);
		}
		absl::Cleanup finish = [&] {
			memory.FinishLoad();
		};
		buffer_manager.Discard(memory);
	}
}

optional<ExternalFileCache::FileRecord> ExternalFileCache::NewVersion(FileRecord &record,
                                                                      const ExternalFileCacheFileInfo &info,
                                                                      idx_t block_size, bool &sweep) {
	optional<FileRecord> replaced;
	if (record.blocks) {
		replaced = std::move(record);
	} else if (files.size() >= sweep_at) {
		sweep = true;
		sweep_at = NumericLimits<idx_t>::Maximum();
	}
	record = FileRecord {info, make_shared_ptr<ExternalFileCacheBlocks>(block_size, info.validation.file_size)};
	return replaced;
}

void ExternalFileCache::RetireRecord(optional<FileRecord> replaced, bool sweep) {
	if (replaced) {
		DropBlocks(*replaced->blocks);
	}
	if (sweep) {
		SweepFiles();
	}
}

shared_ptr<InputFile> ExternalFileCache::OpenFile(const string &path, const ExternalFileCacheFileInfo &current,
                                                  bool validate, shared_ptr<FileHandle> source) {
	FileRecord opened;
	optional<FileRecord> replaced;
	bool sweep = false;
	const auto block_size = NewBlockSize();
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto &record = files[path];
		if (!record.blocks || (validate ? !CacheValidation::IsValid(record.info.validation, current.validation)
		                                : record.info.validation.IsExpired())) {
			replaced = NewVersion(record, current, block_size, sweep);
		} else if (validate) {
			const auto preserved = record.info.validation.cache_valid_until;
			record.info = current;
			if (!CacheValidation::HasValidationMetadata(current.validation)) {
				record.info.validation.cache_valid_until = preserved;
			}
		}
		opened = record;
	}
	RetireRecord(std::move(replaced), sweep);
	return make_shared_ptr<InputFile>(*this, path, false, std::move(opened.info), std::move(source),
	                                  std::move(opened.blocks));
}

void ExternalFileCache::SweepFiles() {
	const auto holds_data = [&](ExternalFileCacheBlocks &blocks) {
		bool result = false;
		blocks.ForEach([&](idx_t, ExternalFileCacheSlot &slot) {
			result = result || !IsMissing(slot.load(std::memory_order_acquire));
		});
		return result;
	};
	vector<FileRecord> forgotten;
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	for (auto entry = files.begin(); entry != files.end();) {
		if (entry->second.blocks.use_count() == 1 && !holds_data(*entry->second.blocks)) {
			forgotten.push_back(std::move(entry->second));
			files.erase(entry++);
		} else {
			++entry;
		}
	}
	sweep_at = MaxValue<idx_t>(MIN_SWEEP_RECORDS, 2 * files.size());
}

bool ExternalFileCache::TryGetFile(const string &path, ExternalFileCacheFileInfo &result) {
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	auto entry = files.find(path);
	if (entry == files.end() || entry->second.info.validation.IsExpired()) {
		return false;
	}
	result = entry->second.info;
	return true;
}

void ExternalFileCache::ForbidReuse(InputFile &file) {
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto entry = files.find(file.path);
		if (entry != files.end() && entry->second.blocks == file.blocks) {
			files.erase(entry);
		}
	}
	DropBlocks(*file.blocks);
}

shared_ptr<InputFile> ExternalFileCache::CreateUnbackedFile(const string &path) {
	return make_shared_ptr<InputFile>(*this, path, true, ExternalFileCacheFileInfo(), nullptr,
	                                  make_shared_ptr<ExternalFileCacheBlocks>(DEFAULT_BLOCK_SIZE, 0));
}

shared_ptr<InputFile> ExternalFileCache::CreateWrittenFile(const string &path) {
	const ExternalFileCacheFileInfo info;
	FileRecord created;
	optional<FileRecord> replaced;
	bool sweep = false;
	const auto block_size = NewBlockSize();
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto &record = files[path];
		replaced = NewVersion(record, info, block_size, sweep);
		created = record;
	}
	RetireRecord(std::move(replaced), sweep);
	return make_shared_ptr<InputFile>(*this, path, false, info, nullptr, std::move(created.blocks));
}

void ExternalFileCache::PublishWrittenFile(InputFile &file, const CacheValidationInfo &written) {
	D_ASSERT(!file.IsUnbacked());
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	auto entry = files.find(file.path);
	if (entry != files.end() && entry->second.blocks == file.blocks) {
		entry->second.info.validation = written;
	}
}

void ExternalFileCache::ForgetFile(const string &path) {
	optional<FileRecord> forgotten;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto entry = files.find(path);
		if (entry == files.end()) {
			return;
		}
		forgotten = std::move(entry->second);
		files.erase(entry);
	}
	RetireRecord(std::move(forgotten), false);
}

void ExternalFileCache::ForgetDirectory(const string &directory) {
	const auto prefix = directory.ends_with('/') ? directory : directory + '/';
	vector<FileRecord> forgotten;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		for (auto entry = files.begin(); entry != files.end();) {
			if (entry->first.starts_with(prefix)) {
				forgotten.push_back(std::move(entry->second));
				files.erase(entry++);
			} else {
				++entry;
			}
		}
	}
	for (auto &record : forgotten) {
		DropBlocks(*record.blocks);
	}
}

[[gnu::always_inline]] bool ExternalFileCache::PinLoaded(ExternalFileCacheSlot &slot, idx_t size,
                                                         FilePin &result) const {
	const auto memory = slot.load(std::memory_order_acquire);
	if (!memory || !memory->TryPinLoaded()) {
		return false;
	}
	if (slot.load(std::memory_order_acquire) != memory) {
		buffer_manager.Unpin(*memory);
		return false;
	}
	result = FilePin(BufferHandle(*memory, memory->GetLoadedBuffer()), memory->GetLoadedData(), size);
	return true;
}

BlockMemory &ExternalFileCache::MemoryOf(const InputFile &file, ExternalFileCacheSlot &slot, idx_t size) {
	auto memory = slot.load(std::memory_order_acquire);
	if (memory) {
		return *memory;
	}
	const auto destroy_buffer_upon = file.IsUnbacked()     ? DestroyBufferUpon::BLOCK
	                                 : file.Info().on_disk ? DestroyBufferUpon::EVICTION
	                                                       : DestroyBufferUpon::KEEP_COPY;
	auto handle = buffer_manager.RegisterUnloadedMemory(size, destroy_buffer_upon);
	auto &created = handle->GetMemory();
	if (!slot.compare_exchange_strong(memory, &created, std::memory_order_seq_cst)) {
		return *memory;
	}
	annotated_lock_guard<annotated_mutex> guard(file.blocks->lock);
	file.blocks->handles.push_back(std::move(handle));
	return created;
}

optional_ptr<BlockMemory> ExternalFileCache::ClaimMissing(InputFile &file, idx_t block) {
	auto &slot = file.blocks->Get(block);
	auto &memory = MemoryOf(file, slot, file.BlockBytes(block));
	if (!memory.TryClaim()) {
		return nullptr;
	}
	if (slot.load(std::memory_order_acquire) != &memory || !IsDropped(memory)) {
		memory.FinishLoad();
		return nullptr;
	}
	return &memory;
}

bool ExternalFileCache::TakeFetch(PendingFetch &fetch) {
	annotated_lock_guard<annotated_mutex> guard(fetch_lock);
	if (fetch.taken) {
		return false;
	}
	TakeFetchLocked(fetch);
	return true;
}

void ExternalFileCache::TakeFetchLocked(PendingFetch &fetch) {
	fetch.taken = true;
	for (auto memory : fetch.claimed) {
		if (memory) {
			pending_fetches.erase(memory.get());
		}
	}
}

void ExternalFileCache::WaitForLoad(BlockMemory &memory) {
	shared_ptr<PendingFetch> fetch;
	{
		annotated_lock_guard<annotated_mutex> guard(fetch_lock);
		const auto entry = pending_fetches.find(&memory);
		if (entry != pending_fetches.end()) {
			fetch = entry->second;
			TakeFetchLocked(*fetch);
			unique_ptr<BaseExecutorTask> task = make_uniq<ExternalFileCacheFetchTask>(*fetch_tasks, fetch, true);
			if (fetch_tasks->StartTask(task)) {
				fetch.reset();
			}
		}
	}
	if (fetch) {
		fetch->Run();
	}
	auto &scheduler = TaskScheduler::GetScheduler(db);
	while (!scheduler.HasWorkers() && memory.IsLoading()) {
		shared_ptr<Task> task;
		if (!TakeFetchTask(task)) {
			break;
		}
		task->Execute(TaskExecutionMode::PROCESS_ALL);
	}
	memory.WaitForLoad();
}

bool ExternalFileCache::IsCached(const InputFile &file, idx_t offset, idx_t size) const {
	const auto range = BlocksOf(*file.blocks, file.GetSize(), offset, size);
	for (auto block = range.first; block < range.end; block++) {
		if (IsMissing(file.blocks->Load(block))) {
			return false;
		}
	}
	return true;
}

bool ExternalFileCache::HoldsCopy(const InputFile &file, idx_t offset, idx_t size) const {
	auto &blocks = *file.blocks;
	if (blocks.copy_count.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	const absl::ReaderMutexLock guard(&blocks.lock);
	const auto entry = blocks.copies.find(offset);
	return entry != blocks.copies.end() && entry->second.size == size &&
	       entry->second.handle->GetMemory().GetState() == BlockState::BLOCK_LOADED;
}

void ExternalFileCache::TightenDeadline(const InputFile &file, timestamp_t deadline) {
	auto &valid_until = file.blocks->valid_until;
	auto current = valid_until.load(std::memory_order_relaxed);
	do {
		if (deadline.value >= current) {
			return;
		}
	} while (!valid_until.compare_exchange_weak(current, deadline.value, std::memory_order_relaxed));
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	const auto entry = files.find(file.path);
	if (entry != files.end() && entry->second.blocks == file.blocks) {
		CacheValidation::TightenDeadline(entry->second.info.validation.cache_valid_until, deadline);
	}
}

bool ExternalFileCache::IsHeld(const InputFile &file, idx_t offset, idx_t size) const {
	return IsCached(file, offset, size) || HoldsCopy(file, offset, size);
}

FilePin ExternalFileCache::PinCopy(InputFile &file, idx_t offset, idx_t size) {
	D_ASSERT(size != 0 && offset + size <= file.GetSize());
	// the bytes come through their blocks, which the bytes next to them share; the blocks they fill are then the first
	// to leave
	const auto read = [&](data_ptr_t target) {
		idx_t done = 0;
		for (auto &pin : PinRange(file, offset, size)) {
			memcpy(target + done, pin.Ptr(), pin.Size());
			done += pin.Size();
		}
		MarkUnused(file, offset, size);
	};
	if (file.ReuseForbidden()) {
		auto result = AllocateUncached(db, size);
		read(result.GetDataMutable());
		return result;
	}
	auto &blocks = *file.blocks;
	shared_ptr<BlockHandle> handle;
	{
		// every reader of the bytes after the first finds the copy: they look it up together
		const absl::ReaderMutexLock guard(&blocks.lock);
		const auto entry = blocks.copies.find(offset);
		if (entry != blocks.copies.end()) {
			D_ASSERT(entry->second.size == size);
			handle = entry->second.handle;
		}
	}
	if (!handle) {
		annotated_lock_guard<annotated_mutex> guard(blocks.lock);
		auto entry = blocks.copies.find(offset);
		if (entry == blocks.copies.end()) {
			auto created = buffer_manager.RegisterUnloadedMemory(size, DestroyBufferUpon::EVICTION);
			entry = blocks.copies.emplace(offset, ExternalFileCacheBlocks::Copy {size, std::move(created)}).first;
			blocks.copy_count.fetch_add(1, std::memory_order_relaxed);
		}
		D_ASSERT(entry->second.size == size);
		handle = entry->second.handle;
	}
	auto &memory = handle->GetMemory();
	while (true) {
		if (memory.TryPinLoaded()) {
			return FilePin(BufferHandle(memory, memory.GetLoadedBuffer()), memory.GetLoadedData(), size);
		}
		if (!memory.TryClaim()) {
			WaitForLoad(memory);
			continue;
		}
		absl::Cleanup finish = [&] {
			memory.FinishLoad();
		};
		auto buffer = buffer_manager.LoadClaimed(QueryContext(), handle);
		if (!buffer.IsValid()) {
			auto load = buffer_manager.PrepareLoad(memory, size);
			read(load.buffer->GetDataMutable());
			if (file.ReuseForbidden()) {
				// a response that brought the bytes forbade keeping them
				auto unshared = buffer_manager.RegisterUnloadedMemory(size, DestroyBufferUpon::UNPIN);
				return FilePin(buffer_manager.CompleteLoad(unshared->GetMemory(), std::move(load)), size);
			}
			buffer = buffer_manager.CompleteLoad(memory, std::move(load));
		}
		return FilePin(std::move(buffer), size);
	}
}

void ExternalFileCache::Fetch(InputFile &file, idx_t first_block, std::span<optional_ptr<BlockMemory>> claimed,
                              std::span<FilePin> pins, bool awaited) {
	D_ASSERT(pins.empty() || pins.size() == claimed.size());
	D_ASSERT(!file.IsUnbacked());
	absl::Cleanup finish = [&] {
		ReleaseClaims(claimed);
	};
	idx_t first = claimed.size();
	idx_t last = 0;
	for (idx_t i = 0; i < claimed.size(); i++) {
		if (claimed[i]) {
			first = MinValue(first, i);
			last = i;
		}
	}
	if (first == claimed.size()) {
		return;
	}
	TaskScheduler::BlockingScope blocking;
	const auto &blocks = *file.blocks;
	vector<optional<BlockLoad>> loads(claimed.size());
	for (idx_t i = first; i <= last; i++) {
		if (claimed[i]) {
			loads[i].emplace(buffer_manager.PrepareLoad(*claimed[i], claimed[i]->GetMemoryUsage()));
		}
	}
	const auto start = blocks.BlockStart(first_block + first);
	const auto end = MinValue(file.GetSize(), blocks.BlockStart(first_block + last + 1));
	auto source = file.Source();
	vector<FileReadTarget> targets;
	targets.reserve(last - first + 1);
	for (idx_t i = first; i <= last; i++) {
		const auto block_bytes = file.BlockBytes(first_block + i);
		if (!loads[i]) {
			targets.push_back({nullptr, block_bytes});
			continue;
		}
		auto data = loads[i]->buffer->GetDataMutable();
		UnpoisonPastEnd(data + block_bytes, claimed[i]->GetMemoryUsage() - block_bytes);
		targets.push_back({data, block_bytes});
	}
	// hands out the first [count] blocks of the request, whose bytes are in
	idx_t arrived = 0;
	const auto hand_out = [&](idx_t count) {
		if (arrived >= count) {
			return;
		}
		const auto valid_until = source->file_system.GetCacheValidUntil(*source);
		const bool shareable = !valid_until || *valid_until != timestamp_t::ninfinity();
		if (!shareable) {
			file.reuse_forbidden.store(true, std::memory_order_relaxed);
		} else if (valid_until && valid_until->IsFinite()) {
			TightenDeadline(file, *valid_until);
		}
		for (; arrived < count; arrived++) {
			const auto i = first + arrived;
			if (!loads[i]) {
				continue;
			}
			shared_ptr<BlockHandle> unshared;
			if (!shareable) {
				unshared =
				    buffer_manager.RegisterUnloadedMemory(claimed[i]->GetMemoryUsage(), DestroyBufferUpon::UNPIN);
			}
			auto buffer =
			    buffer_manager.CompleteLoad(unshared ? unshared->GetMemory() : *claimed[i], std::move(*loads[i]));
			claimed[i]->FinishLoad();
			claimed[i] = nullptr;
			if (!pins.empty()) {
				pins[i] = FilePin(std::move(buffer), file.BlockBytes(first_block + i));
			}
		}
	};
	struct Arrival final : public FileReadProgress {
		explicit Arrival(decltype(hand_out) &hand_out_p) : hand_out(hand_out_p) {
		}
		void Filled(idx_t count) override {
			hand_out(count);
		}
		decltype(hand_out) &hand_out;
	} arrival(hand_out);
	ReadSource(file, *source, targets, start, pins.empty() && !awaited, &arrival);
	hand_out(targets.size());
	fetches.fetch_add(1, std::memory_order_relaxed);
	fetched_bytes.fetch_add(end - start, std::memory_order_relaxed);
}

FilePin ExternalFileCache::Pin(InputFile &file, idx_t block) {
	const auto size = file.BlockBytes(block);
	if (size == 0) {
		throw InternalException("Block %llu of \"%s\" lies past its end", block, file.path);
	}
	auto &slot = file.blocks->Get(block);
	while (true) {
		FilePin result;
		if (PinLoaded(slot, size, result)) {
			return result;
		}
		auto &memory = MemoryOf(file, slot, size);
		if (!memory.TryClaim()) {
			WaitForLoad(memory);
			continue;
		}
		optional_ptr<BlockMemory> claimed = &memory;
		absl::Cleanup finish = [&] {
			if (claimed) {
				claimed->FinishLoad();
			}
		};
		if (slot.load(std::memory_order_acquire) != &memory) {
			continue;
		}
		auto owner = memory.GetOwner();
		auto buffer = buffer_manager.LoadClaimed(QueryContext(), owner);
		if (buffer.IsValid()) {
			result = FilePin(std::move(buffer), size);
			return result;
		}
		if (file.IsUnbacked()) {
			throw InternalException("A block of the in-memory file \"%s\" is missing from the external file cache",
			                        file.path);
		}
		misses.fetch_add(1, std::memory_order_relaxed);
		Fetch(file, block, std::span(&claimed, 1), std::span(&result, 1));
		return result;
	}
}

template <class ADMIT>
ExternalFileCache::RunPlan ExternalFileCache::PlanRun(InputFile &file, idx_t block, idx_t end, ADMIT &&admit) {
	const auto &blocks = *file.blocks;
	const auto piece = PieceBlocks(file);
	const auto pieces = [&](idx_t count) {
		return (count + piece - 1) / piece;
	};
	RunPlan plan {block, {}, block, false};
	plan.claimed.reserve(blocks.BlockOf(MAX_REQUEST));
	vector<optional_ptr<BlockMemory>> more;
	absl::Cleanup abandon = [&] {
		ReleaseClaims(plan.claimed);
		ReleaseClaims(more);
	};
	idx_t bytes = 0;
	// claims the missing blocks from the plan's next one on, for a request that holds [before] blocks already
	const auto claim = [&](vector<optional_ptr<BlockMemory>> &run, idx_t before) {
		for (; plan.next < end && blocks.BlockStart(before + run.size() + 1) <= MAX_REQUEST; plan.next++) {
			if (!IsMissing(file.blocks->Load(plan.next))) {
				return;
			}
			const auto block_bytes = file.BlockBytes(plan.next);
			if (!admit(plan.next, bytes + block_bytes)) {
				plan.exhausted = true;
				return;
			}
			const auto memory = ClaimMissing(file, plan.next);
			if (!memory) {
				return;
			}
			run.push_back(memory);
			bytes += block_bytes;
		}
	};
	claim(plan.claimed, 0);
	if (plan.claimed.empty() && !plan.exhausted) {
		// another reader fetches the block
		plan.next = block + 1;
	}
	while (!plan.claimed.empty() && !plan.exhausted) {
		idx_t gap = 0;
		while (plan.next + gap < end && blocks.BlockStart(gap + 1) <= MERGE_GAP &&
		       !IsMissing(file.blocks->Load(plan.next + gap))) {
			gap++;
		}
		const auto behind = plan.next + gap;
		if (gap == 0 || behind >= end || !IsMissing(file.blocks->Load(behind))) {
			break;
		}
		// the run behind the gap joins this one when one request for both takes fewer pieces than a request for
		// each: the blocks of the gap are read again for it
		const auto held = bytes;
		plan.next = behind;
		claim(more, plan.claimed.size() + gap);
		if (more.empty() ||
		    pieces(plan.claimed.size() + gap + more.size()) >= pieces(plan.claimed.size()) + pieces(more.size())) {
			ReleaseClaims(more);
			more.clear();
			bytes = held;
			plan.next = behind;
			break;
		}
		plan.claimed.resize(plan.claimed.size() + gap);
		plan.claimed.insert(plan.claimed.end(), more.begin(), more.end());
		more.clear();
	}
	idx_t fetched = 0;
	for (auto &memory : plan.claimed) {
		fetched += memory ? 1 : 0;
	}
	misses.fetch_add(fetched, std::memory_order_relaxed);
	std::move(abandon).Cancel();
	return plan;
}

vector<FilePin> ExternalFileCache::PinRange(InputFile &file, idx_t offset, idx_t size) {
	vector<FilePin> result;
	const auto range = BlocksOf(*file.blocks, file.GetSize(), offset, size);
	if (range.first == range.end) {
		return result;
	}
	result.resize(range.end - range.first);
	bool complete = true;
	for (auto block = range.first; block < range.end; block++) {
		auto &slot = file.blocks->Get(block);
		complete = PinLoaded(slot, file.BlockBytes(block), result[block - range.first]) && complete;
	}
	if (!complete && !file.IsUnbacked()) {
		vector<RunPlan> plans;
		absl::Cleanup abandon = [&] {
			for (auto &plan : plans) {
				ReleaseClaims(plan.claimed);
			}
		};
		for (auto block = range.first; block < range.end;) {
			if (!IsMissing(file.blocks->Load(block))) {
				block++;
				continue;
			}
			auto plan = PlanRun(file, block, range.end, [](idx_t, idx_t) { return true; });
			block = plan.next;
			if (!plan.claimed.empty()) {
				plans.push_back(std::move(plan));
			}
		}
		FetchConcurrently(file, plans, range.first, result);
	}
	for (auto block = range.first; block < range.end; block++) {
		if (!result[block - range.first].IsValid()) {
			result[block - range.first] = Pin(file, block);
		}
	}
	NarrowPins(*file.blocks, result, offset, MinValue(offset + size, file.GetSize()));
	return result;
}

vector<FilePin> ExternalFileCache::PinRangeOfWhole(InputFile &file, idx_t offset, idx_t size) {
	auto pins = TryPinRange(file, offset, size);
	if (!pins.empty()) {
		return pins;
	}
	const auto file_size = file.GetSize();
	if (file.IsUnbacked() || IsCached(file, offset, size) || file.blocks->BlockCountOf(file_size) > PieceBlocks(file)) {
		return PinRange(file, offset, size);
	}
	auto whole = PinRange(file, 0, file_size);
	const auto range = BlocksOf(*file.blocks, file_size, offset, size);
	pins.reserve(range.end - range.first);
	for (auto block = range.first; block < range.end; block++) {
		pins.push_back(std::move(whole[block]));
	}
	NarrowPins(*file.blocks, pins, offset, MinValue(offset + size, file_size));
	return pins;
}

FilePin ExternalFileCache::PinBytes(InputFile &file, idx_t offset, idx_t size) {
	D_ASSERT(size != 0 && offset + size <= file.GetSize());
	const auto &blocks = *file.blocks;
	if (blocks.BlockOf(offset + size - 1) == blocks.BlockOf(offset)) {
		auto pin = Pin(file, blocks.BlockOf(offset));
		pin.Narrow(blocks.OffsetInBlock(offset), size);
		return pin;
	}
	auto pins = PinRange(file, offset, size);
	auto result = AllocateUncached(db, size);
	idx_t done = 0;
	for (auto &pin : pins) {
		memcpy(result.GetDataMutable() + done, pin.Ptr(), pin.Size());
		done += pin.Size();
	}
	return result;
}

void ExternalFileCache::FetchConcurrently(InputFile &file, vector<RunPlan> &plans, idx_t first, vector<FilePin> &pins) {
	const auto piece_blocks = PieceBlocks(file);
	const auto for_each_piece = [&](auto &&callback) {
		for (auto &plan : plans) {
			ForEachPiece(plan.claimed.size(), piece_blocks, [&](idx_t offset, idx_t count) {
				const auto piece_first = plan.first_block + offset;
				callback(piece_first, std::span(plan.claimed).subspan(offset, count),
				         std::span(pins).subspan(piece_first - first, count));
			});
		}
	};
	idx_t pieces = 0;
	for_each_piece([&](idx_t, std::span<optional_ptr<BlockMemory>>, std::span<FilePin>) { pieces++; });
	if (pieces == 0) {
		return;
	}
	if (pieces == 1) {
		for_each_piece([&](idx_t piece_first, std::span<optional_ptr<BlockMemory>> claimed, std::span<FilePin> out) {
			Fetch(file, piece_first, claimed, out);
		});
		return;
	}
	TaskExecutor executor(TaskScheduler::GetScheduler(db), TaskSchedulerType::ASYNC);
	for_each_piece([&](idx_t piece_first, std::span<optional_ptr<BlockMemory>> claimed, std::span<FilePin> out) {
		executor.ScheduleTask([this, &file, piece_first, claimed, out]() { Fetch(file, piece_first, claimed, out); },
		                      "ExternalFileCacheFetchTask");
	});
	executor.WorkOnTasks();
}

vector<FilePin> ExternalFileCache::TryPinRange(InputFile &file, idx_t offset, idx_t size) {
	vector<FilePin> result;
	const auto range = BlocksOf(*file.blocks, file.GetSize(), offset, size);
	result.reserve(range.end - range.first);
	for (auto block = range.first; block < range.end; block++) {
		const auto slot = file.blocks->Find(block);
		FilePin pin;
		if (!slot || !PinLoaded(*slot, file.BlockBytes(block), pin)) {
			result.clear();
			return result;
		}
		result.push_back(std::move(pin));
	}
	NarrowPins(*file.blocks, result, offset, MinValue(offset + size, file.GetSize()));
	return result;
}

bool ExternalFileCache::TakeFetchTask(shared_ptr<Task> &task) {
	annotated_lock_guard<annotated_mutex> guard(fetch_lock);
	return fetch_tasks && fetch_tasks->GetTask(task);
}

void ExternalFileCache::StopFetching() {
	unique_ptr<TaskExecutor> stopping;
	{
		annotated_lock_guard<annotated_mutex> guard(fetch_lock);
		stopped = true;
		stopping = std::move(fetch_tasks);
		pending_fetches.clear();
	}
	if (stopping) {
		stopping->CancelAndDrain();
	}
}

bool ExternalFileCache::Prefetch(InputFile &file, idx_t offset, idx_t size) {
	if (IsHeld(file, offset, size)) {
		return false;
	}
	PrefetchBlocks(file, offset, size, ExternalFileCachePrefetch::HINT);
	return true;
}

idx_t ExternalFileCache::ReadAhead(InputFile &file, idx_t offset, idx_t size, idx_t needed) {
	if (IsHeld(file, offset, size)) {
		return offset + size;
	}
	return PrefetchBlocks(file, offset, size, ExternalFileCachePrefetch::HINT, needed);
}

idx_t ExternalFileCache::PrefetchBlocks(InputFile &file, idx_t offset, idx_t size, ExternalFileCachePrefetch kind,
                                        idx_t needed) {
	if (file.IsUnbacked()) {
		return offset + size;
	}
	const auto &blocks = *file.blocks;
	const auto range = BlocksOf(blocks, file.GetSize(), offset, size);
	const auto needed_end = needed == 0 ? range.first : BlocksOf(blocks, file.GetSize(), offset, needed).end;
	const auto admit = [&](idx_t block, idx_t bytes) {
		return kind == ExternalFileCachePrefetch::NEED || block < needed_end ||
		       (file.in_flight.load(std::memory_order_relaxed) + bytes <= MAX_FILE_IN_FLIGHT &&
		        in_flight.load(std::memory_order_relaxed) + bytes <= MAX_IN_FLIGHT);
	};
	for (auto block = range.first; block < range.end;) {
		if (!IsMissing(blocks.Load(block))) {
			block++;
			continue;
		}
		auto plan = PlanRun(file, block, range.end, admit);
		if (!plan.claimed.empty()) {
			ScheduleFetch(file, plan.first_block, std::move(plan.claimed),
			              needed != 0 && plan.first_block == range.first);
		}
		if (plan.exhausted) {
			return blocks.BlockStart(plan.next);
		}
		block = plan.next;
	}
	return offset + size;
}

void ExternalFileCache::Prefetch(InputFile &file, std::span<FileRange> ranges, ExternalFileCachePrefetch kind) {
	// a range whose blocks are all in memory needs nothing: every range of a query over cached data
	idx_t missing = 0;
	for (auto &range : ranges) {
		if (!IsCached(file, range.offset, range.size)) {
			ranges[missing++] = range;
		}
	}
	ranges = ranges.first(missing);
	if (ranges.empty()) {
		return;
	}
	auto &blocks = *file.blocks;
	if (blocks.copy_count.load(std::memory_order_relaxed) != 0) {
		// bytes a copy of which is in memory are not fetched
		const absl::ReaderMutexLock guard(&blocks.lock);
		idx_t kept = 0;
		for (auto &range : ranges) {
			const auto entry = blocks.copies.find(range.offset);
			if (entry == blocks.copies.end() || entry->second.size != range.size ||
			    entry->second.handle->GetMemory().GetState() != BlockState::BLOCK_LOADED) {
				ranges[kept++] = range;
			}
		}
		ranges = ranges.first(kept);
	}
	std::sort(ranges.begin(), ranges.end(), [](const FileRange &l, const FileRange &r) { return l.offset < r.offset; });
	const auto piece = PieceBlocks(file);
	const auto pieces = [&](idx_t first, idx_t last) {
		return (last - first + piece) / piece;
	};
	idx_t merged = 0;
	for (auto &range : ranges) {
		if (merged != 0 && range.offset <= ranges[merged - 1].offset + ranges[merged - 1].size + MERGE_GAP) {
			auto &last = ranges[merged - 1];
			const auto size = MaxValue(last.size, range.offset + range.size - last.offset);
			const auto first = blocks.BlockOf(last.offset);
			const auto last_block = blocks.BlockOf(last.offset + MaxValue<idx_t>(last.size, 1) - 1);
			const auto next_block = blocks.BlockOf(range.offset);
			const auto end_block = MaxValue(last_block, blocks.BlockOf(last.offset + MaxValue<idx_t>(size, 1) - 1));
			if (next_block <= last_block + 1 ||
			    pieces(first, end_block) < pieces(first, last_block) + pieces(next_block, end_block)) {
				last.size = size;
				continue;
			}
		}
		ranges[merged++] = range;
	}
	for (auto &range : ranges.first(merged)) {
		PrefetchBlocks(file, range.offset, range.size, kind);
	}
}

void ExternalFileCache::MarkUnused(const InputFile &file, idx_t offset, idx_t size) const {
	const auto file_size = file.GetSize();
	if (size == 0 || offset >= file_size) {
		return;
	}
	const auto &blocks = *file.blocks;
	const auto end = MinValue(offset + size, file_size);
	const auto last = end == file_size ? blocks.BlockOf(end - 1) + 1 : blocks.BlockOf(end);
	for (auto block = blocks.BlockCountOf(offset); block < last; block++) {
		if (const auto memory = file.blocks->Load(block)) {
			memory->TakeReferenced();
		}
	}
}

void ExternalFileCache::ReadInto(InputFile &file, idx_t offset, data_ptr_t target, idx_t size) {
	D_ASSERT(size != 0 && offset <= file.GetSize() && size <= file.GetSize() - offset);
	const auto &blocks = *file.blocks;
	const auto read_piece = [&](idx_t piece_offset, idx_t piece_size) {
		const auto piece_target = target + (piece_offset - offset);
		if (file.IsUnbacked() || IsCached(file, piece_offset, piece_size)) {
			for (idx_t done = 0; done < piece_size;) {
				const auto in_block = blocks.OffsetInBlock(piece_offset + done);
				const auto pin = Pin(file, blocks.BlockOf(piece_offset + done));
				const auto bytes = MinValue(piece_size - done, pin.Size() - in_block);
				memcpy(piece_target + done, pin.Ptr() + in_block, bytes);
				done += bytes;
			}
			return;
		}
		TaskScheduler::BlockingScope blocking;
		const FileReadTarget piece {piece_target, piece_size};
		ReadSource(file, *file.Source(), std::span(&piece, 1), piece_offset, false, nullptr);
		fetches.fetch_add(1, std::memory_order_relaxed);
		fetched_bytes.fetch_add(piece_size, std::memory_order_relaxed);
	};
	const auto piece_bytes = blocks.BlockStart(PieceBlocks(file));
	if (size <= piece_bytes) {
		read_piece(offset, size);
		return;
	}
	TaskExecutor executor(TaskScheduler::GetScheduler(db), TaskSchedulerType::ASYNC);
	for (idx_t done = 0; done < size; done += piece_bytes) {
		executor.ScheduleTask(
		    [&read_piece, piece_offset = offset + done, piece_size = MinValue(piece_bytes, size - done)]() {
			    read_piece(piece_offset, piece_size);
		    },
		    "ExternalFileCacheReadTask");
	}
	executor.WorkOnTasks();
}

void ExternalFileCache::ScheduleFetch(InputFile &file, idx_t first_block, vector<optional_ptr<BlockMemory>> claimed,
                                      bool awaited) {
	idx_t handed = 0;
	absl::Cleanup abandon = [&] {
		ReleaseClaims(std::span(claimed).subspan(handed));
	};
	annotated_lock_guard<annotated_mutex> guard(fetch_lock);
	if (stopped) {
		return;
	}
	if (!fetch_tasks) {
		fetch_tasks = make_uniq<TaskExecutor>(TaskScheduler::GetScheduler(db), TaskSchedulerType::ASYNC);
	}
	// the tasks keep the file open
	const auto held = file.shared_from_this();
	vector<shared_ptr<PendingFetch>> fetches;
	ForEachPiece(claimed.size(), PieceBlocks(file), [&](idx_t offset, idx_t count) {
		const auto piece_first = first_block + offset;
		const auto range = std::span(claimed).subspan(offset, count);
		vector<optional_ptr<BlockMemory>> piece_claimed(range.begin(), range.end());
		idx_t bytes = 0;
		for (idx_t i = 0; i < count; i++) {
			if (piece_claimed[i]) {
				bytes += file.BlockBytes(piece_first + i);
			}
		}
		fetches.push_back(
		    make_shared_ptr<PendingFetch>(held, piece_first, std::move(piece_claimed), bytes, awaited && offset == 0));
		handed = offset + count;
	});
	for (auto &fetch : fetches) {
		ScheduleFetchLocked(std::move(fetch));
	}
}

void ExternalFileCache::ScheduleFetchLocked(shared_ptr<PendingFetch> fetch) {
	if (stopped || !fetch_tasks) {
		return;
	}
	for (auto memory : fetch->claimed) {
		if (memory) {
			pending_fetches[memory.get()] = fetch;
		}
	}
	fetch_tasks->ScheduleTask(make_uniq<ExternalFileCacheFetchTask>(*fetch_tasks, std::move(fetch)));
}

FilePin ExternalFileCache::CreateBlock(InputFile &file, idx_t block) {
	D_ASSERT(file.IsUnbacked() || !file.HasSource());
	auto &slot = file.blocks->Get(block);
	if (slot.load(std::memory_order_acquire)) {
		throw InternalException("Block %llu of \"%s\" is written twice", block, file.path);
	}
	auto &memory = MemoryOf(file, slot, file.BlockSize());
	if (!memory.TryClaim()) {
		throw InternalException("Block %llu of \"%s\" is written twice", block, file.path);
	}
	absl::Cleanup finish = [&] {
		memory.FinishLoad();
	};
	auto buffer = buffer_manager.CompleteLoad(memory, buffer_manager.PrepareLoad(memory, file.BlockSize()));
	return FilePin(std::move(buffer), file.BlockSize());
}

void ExternalFileCache::SetUnbackedSize(InputFile &file, idx_t size) {
	D_ASSERT(file.IsUnbacked());
	file.size.store(size, std::memory_order_release);
}

void ExternalFileCache::SetEnabled(bool enabled_p) {
	if (!enabled.exchange(enabled_p) || enabled_p) {
		return;
	}
	vector<shared_ptr<ExternalFileCacheBlocks>> versions;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		for (auto &file : files) {
			versions.push_back(file.second.blocks);
		}
	}
	for (auto &blocks : versions) {
		DropBlocks(*blocks);
	}
}

ExternalFileCacheStatistics ExternalFileCache::GetStatistics() const {
	ExternalFileCacheStatistics result;
	for (auto &info : buffer_manager.GetMemoryUsageInfo()) {
		if (info.tag == MemoryTag::EXTERNAL_FILE_CACHE) {
			result.used = info.size;
			result.evictions = info.evictions;
			result.spilled = info.temporary_writes;
			result.spill_reads = info.temporary_reads;
		}
	}
	result.misses = misses.load(std::memory_order_relaxed);
	result.fetched_bytes = fetched_bytes.load(std::memory_order_relaxed);
	result.fetches = fetches.load(std::memory_order_relaxed);
	return result;
}

vector<CachedFileInformation> ExternalFileCache::GetCachedFileInformation() const {
	struct Named {
		string path;
		idx_t size;
		shared_ptr<ExternalFileCacheBlocks> blocks;
	};
	vector<Named> named;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		for (auto &file : files) {
			named.push_back({file.first, file.second.info.validation.file_size, file.second.blocks});
		}
	}
	vector<CachedFileInformation> result;
	for (auto &file : named) {
		file.blocks->ForEach([&](idx_t block, ExternalFileCacheSlot &slot) {
			const auto memory = slot.load(std::memory_order_acquire);
			const bool loaded = memory && memory->GetState() == BlockState::BLOCK_LOADED;
			const bool spilled = memory && memory->HasTemporaryFile();
			if (loaded || spilled) {
				result.push_back({file.path, BlockBytesOf(*file.blocks, file.size, block),
				                  file.blocks->BlockStart(block), loaded, spilled});
			}
		});
	}
	return result;
}

idx_t ExternalFileCache::NewBlockSize() const {
	return Settings::Get<ExternalFileCacheRemoteMinBlockSizeSetting>(db);
}

void ExternalFileCache::ReadSource(InputFile &file, FileHandle &source, std::span<const FileReadTarget> targets,
                                   idx_t offset, bool background, optional_ptr<FileReadProgress> progress) {
	if (file.Info().on_disk) {
		source.file_system.ReadScattered(source, targets, offset);
		return;
	}
	const auto bytes = FileReadTarget::TotalSize(targets);
	{
		annotated_unique_lock<annotated_mutex> guard(read_lock);
		const auto has_room = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
			return reading == 0 || reading + bytes <= MAX_BURST;
		};
		if (background) {
			const auto turn = read_turns++;
			const auto admitted = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
				return turn == read_turn && awaited_reads == 0 && has_room();
			};
			guard.mutex()->Await(absl::Condition(&admitted));
			read_turn++;
		} else {
			awaited_reads++;
			guard.mutex()->Await(absl::Condition(&has_room));
			awaited_reads--;
		}
		reading += bytes;
	}
	absl::Cleanup done = [&] {
		annotated_lock_guard<annotated_mutex> guard(read_lock);
		reading -= bytes;
	};
	if (progress) {
		source.file_system.ReadStreamed(source, targets, offset, *progress);
	} else {
		source.file_system.ReadScattered(source, targets, offset);
	}
}

idx_t ExternalFileCache::PieceBlocks(const InputFile &file) const {
	const idx_t piece = file.Info().on_disk ? Settings::Get<ExternalFileCacheLocalMaxBlockSizeSetting>(db)
	                                        : Settings::Get<ExternalFileCacheRemoteMaxBlockSizeSetting>(db);
	return MaxValue<idx_t>(1, file.Blocks().BlockOf(MinValue(piece, MAX_REQUEST)));
}

} // namespace duckdb
