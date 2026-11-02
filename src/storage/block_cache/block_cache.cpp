#include "duckdb/storage/block_cache/block_cache.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <cstring>

namespace duckdb {

namespace {

constexpr uintptr_t SLOT_LOADING = 1;
constexpr idx_t MAX_IN_FLIGHT = 16 * BlockCache::MAX_FILE_IN_FLIGHT;
constexpr uintptr_t CACHE_LINE = 64;

//! The block memory a slot's value names, or null before the slot's first load
BlockMemory *AsMemory(uintptr_t value) {
	return reinterpret_cast<BlockMemory *>(value & ~SLOT_LOADING);
}

//! Whether the buffer pool destroyed the block's buffer without keeping a copy
bool IsDropped(const BlockMemory &memory) {
	return memory.IsUnloaded() && !memory.HasTemporaryFile();
}

idx_t BlockBytesOf(idx_t file_size, idx_t block) {
	const auto start = block * BlockCache::BLOCK_SIZE;
	return start >= file_size ? 0 : MinValue(BlockCache::BLOCK_SIZE, file_size - start);
}

//! The blocks of each piece of a run of [block_count] blocks: equal pieces of at most PIECE_SIZE
idx_t PieceBlocks(idx_t block_count) {
	constexpr idx_t MAX_BLOCKS = BlockCache::PIECE_SIZE / BlockCache::BLOCK_SIZE;
	const auto pieces = (block_count + MAX_BLOCKS - 1) / MAX_BLOCKS;
	return (block_count + pieces - 1) / pieces;
}

} // namespace

//===--------------------------------------------------------------------===//
// BlockCachePin
//===--------------------------------------------------------------------===//

BlockCachePin BlockCachePin::Share(idx_t offset, idx_t size_p) {
	if (!handle.IsValid()) {
		return {};
	}
	D_ASSERT(offset <= size && size_p <= size - offset);
	BlockCachePin result(handle.Share(), size_p);
	result.data = data + offset;
	return result;
}

//===--------------------------------------------------------------------===//
// BlockCacheBlocks
//===--------------------------------------------------------------------===//

template <class FUNC>
void BlockCacheBlocks::ForEach(FUNC &&callback) {
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

BlockCacheBlocks::BlockCacheBlocks(BlockCache &cache_p, idx_t block_count) : cache(cache_p) {
	auto directory = make_uniq<Directory>(MaxValue<idx_t>(1, (block_count + CHUNK - 1) / CHUNK));
	current.store(directory.get(), std::memory_order_release);
	annotated_lock_guard<annotated_mutex> guard(lock);
	directories.push_back(std::move(directory));
}

BlockCacheBlocks::~BlockCacheBlocks() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	cache.block_count.fetch_sub(handles.size(), std::memory_order_relaxed);
}

BlockCacheSlot &BlockCacheBlocks::Get(idx_t block) {
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
// BlockCacheFile
//===--------------------------------------------------------------------===//

BlockCacheFile::BlockCacheFile(BlockCache &cache_p, string path_p, uint32_t number_p, bool unbacked_p,
                               BlockCacheFileInfo info_p, shared_ptr<FileHandle> source_p,
                               shared_ptr<BlockCacheBlocks> blocks_p)
    : cache(cache_p), path(std::move(path_p)), number(number_p), size(info_p.validation.file_size),
      unbacked(unbacked_p), info(std::move(info_p)), source(std::move(source_p)), blocks(std::move(blocks_p)) {
}

BlockCacheFile::~BlockCacheFile() {
	if (!unbacked) {
		cache.CloseFile(*this);
	}
}

bool BlockCacheFile::HasSource() const {
	annotated_lock_guard<annotated_mutex> guard(source_lock);
	return source != nullptr;
}

void BlockCacheFile::SetSource(shared_ptr<FileHandle> handle) {
	annotated_lock_guard<annotated_mutex> guard(source_lock);
	if (!source) {
		source = std::move(handle);
	}
}

shared_ptr<FileHandle> BlockCacheFile::Source() const {
	annotated_lock_guard<annotated_mutex> guard(source_lock);
	if (!source) {
		throw InternalException("The block cache has no source to read \"%s\" from", path);
	}
	return source;
}

idx_t BlockCacheFile::BlockCount() const {
	return BlockCache::BlockCountOf(GetSize());
}

idx_t BlockCacheFile::BlockBytes(idx_t block) const {
	return BlockBytesOf(GetSize(), block);
}

//===--------------------------------------------------------------------===//
// BlockCacheAppender
//===--------------------------------------------------------------------===//

BlockCacheAppender::BlockCacheAppender(BlockCache &cache_p, shared_ptr<BlockCacheFile> file_p)
    : cache(cache_p), file(std::move(file_p)) {
}

void BlockCacheAppender::Append(const_data_ptr_t data, idx_t count) {
	while (count > 0) {
		const auto window = Window();
		const auto chunk = MinValue(count, window.size);
		memcpy(window.data, data, chunk);
		Commit(chunk);
		data += chunk;
		count -= chunk;
	}
}

FileWriteWindow BlockCacheAppender::Window() {
	const auto offset = size % BlockCache::BLOCK_SIZE;
	if (!current.IsValid()) {
		current = cache.CreateBlock(*file, size / BlockCache::BLOCK_SIZE);
	}
	return {current.GetDataMutable() + offset, BlockCache::BLOCK_SIZE - offset};
}

void BlockCacheAppender::Commit(idx_t count) {
	const auto offset = size % BlockCache::BLOCK_SIZE;
	D_ASSERT(current.IsValid() || count == 0);
	D_ASSERT(offset + count <= BlockCache::BLOCK_SIZE);
	if (count == 0) {
		return;
	}
	size += count;
	if (file->IsUnbacked()) {
		cache.SetUnbackedSize(*file, size);
	}
	if (offset + count == BlockCache::BLOCK_SIZE) {
		current.Reset();
	}
}

void BlockCacheAppender::Finish(const string &version_tag, timestamp_t last_modified) {
	if (current.IsValid()) {
		const auto end = size % BlockCache::BLOCK_SIZE;
		memset(current.GetDataMutable() + end, 0, BlockCache::BLOCK_SIZE - end);
		current.Reset();
	}
	if (!file->IsUnbacked()) {
		CacheValidationInfo written;
		written.file_size = size;
		written.version_tag = version_tag;
		written.last_modified = last_modified;
		cache.FinishWrittenFile(*file, written, true);
	}
}

void BlockCacheAppender::Abandon() {
	D_ASSERT(!file->IsUnbacked());
	current.Reset();
	CacheValidationInfo written;
	written.file_size = size;
	cache.FinishWrittenFile(*file, written, false);
}

//===--------------------------------------------------------------------===//
// BlockCacheFetchTask
//===--------------------------------------------------------------------===//

class BlockCacheFetchTask : public Task {
public:
	BlockCacheFetchTask(BlockCache &cache_p, shared_ptr<BlockCacheFile> file_p, BlockCache::Run run_p,
	                    vector<optional_ptr<BlockCache::Slot>> slots_p, idx_t bytes_p)
	    : cache(cache_p), file(std::move(file_p)), run(run_p), slots(std::move(slots_p)), bytes(bytes_p) {
	}

	~BlockCacheFetchTask() override {
		if (!done) {
			for (auto &slot : slots) {
				if (slot) {
					cache.Unclaim(*slot);
				}
			}
			Finish();
		}
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		try {
			cache.Fetch(*file, run, slots, {});
		} catch (std::exception &) { // NOLINT
		}
		Finish();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "BlockCacheFetchTask";
	}

private:
	void Finish() {
		done = true;
		file->in_flight.fetch_sub(bytes, std::memory_order_relaxed);
		cache.FinishFetchTask(bytes);
	}

	BlockCache &cache;
	shared_ptr<BlockCacheFile> file;
	BlockCache::Run run;
	vector<optional_ptr<BlockCache::Slot>> slots;
	idx_t bytes;
	bool done = false;
};

//===--------------------------------------------------------------------===//
// BlockCache
//===--------------------------------------------------------------------===//

BlockCache::BlockCache(DatabaseInstance &db_p)
    : db(db_p), buffer_manager(BufferManager::GetBufferManager(db_p)), block_manager(buffer_manager, BLOCK_SIZE, 0) {
}

BlockCache::~BlockCache() {
	StopFetching();
	unordered_map<string, FileRecord> dropped;
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	std::swap(dropped, files);
}

BlockCache &BlockCache::Get(DatabaseInstance &db) {
	return db.GetBlockCache();
}

BlockCachePin BlockCache::AllocateUncached(DatabaseInstance &db, idx_t size) {
	auto buffer = BufferManager::GetBufferManager(db).Allocate(MemoryTag::EXTERNAL_FILE_CACHE, size);
	buffer.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	BlockCachePin result(std::move(buffer), size);
	result.data = result.handle.GetFileBuffer().InternalBuffer();
	return result;
}

void BlockCache::DropBlocks(BlockCacheBlocks &blocks) {
	const auto drop = [&](Slot &slot) {
		auto value = slot.load(std::memory_order_acquire);
		while (!(value & SLOT_LOADING)) {
			if (!AsMemory(value)) {
				return true;
			}
			if (slot.compare_exchange_weak(value, 0, std::memory_order_seq_cst)) {
				auto owner = AsMemory(value)->GetOwner();
				buffer_manager.Discard(owner);
				return true;
			}
		}
		return false;
	};
	vector<reference<Slot>> loading;
	blocks.ForEach([&](idx_t, Slot &slot) {
		if (!drop(slot)) {
			loading.push_back(slot);
		}
	});
	for (auto &slot : loading) {
		WaitForLoad(slot);
		drop(slot);
	}
}

optional<BlockCache::FileRecord> BlockCache::Renumber(FileRecord &record, const BlockCacheFileInfo &info, idx_t opens,
                                                      bool &sweep) {
	optional<FileRecord> replaced;
	if (record.number != 0) {
		replaced = std::move(record);
	} else if (files.size() >= sweep_at) {
		sweep = true;
		sweep_at = NumericLimits<idx_t>::Maximum();
	}
	record = FileRecord {next_file_number++, info, opens,
	                     make_shared_ptr<BlockCacheBlocks>(*this, BlockCountOf(info.validation.file_size))};
	return replaced;
}

void BlockCache::RetireRecord(optional<FileRecord> replaced, bool sweep) {
	if (replaced) {
		DropBlocks(*replaced->blocks);
	}
	if (sweep) {
		SweepFiles();
	}
}

shared_ptr<BlockCacheFile> BlockCache::OpenFile(const string &path, const BlockCacheFileInfo &current, bool validate,
                                                shared_ptr<FileHandle> source) {
	FileRecord opened;
	optional<FileRecord> replaced;
	bool sweep = false;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto &record = files[path];
		if (record.number == 0 ||
		    (validate ? !CacheValidation::IsValid(validate, record.info.validation, current.validation)
		              : record.info.validation.IsExpired())) {
			replaced = Renumber(record, current, 0, sweep);
		} else if (validate) {
			const auto preserved = record.info.validation.cache_valid_until;
			record.info = current;
			if (!CacheValidation::HasValidationMetadata(current.validation)) {
				record.info.validation.cache_valid_until = preserved;
			}
		}
		record.opens++;
		opened = record;
	}
	RetireRecord(std::move(replaced), sweep);
	return make_shared_ptr<BlockCacheFile>(*this, path, opened.number, false, std::move(opened.info), std::move(source),
	                                       std::move(opened.blocks));
}

void BlockCache::CloseFile(const BlockCacheFile &file) {
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	auto entry = files.find(file.path);
	if (entry != files.end() && entry->second.number == file.number) {
		D_ASSERT(entry->second.opens > 0);
		entry->second.opens--;
	}
}

void BlockCache::SweepFiles() {
	const auto holds_data = [&](BlockCacheBlocks &blocks) {
		bool result = false;
		blocks.ForEach([&](idx_t, Slot &slot) {
			const auto value = slot.load(std::memory_order_acquire);
			const auto memory = AsMemory(value);
			result = result || (value & SLOT_LOADING) || (memory && !IsDropped(*memory));
		});
		return result;
	};
	vector<FileRecord> forgotten;
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	for (auto entry = files.begin(); entry != files.end();) {
		if (entry->second.opens == 0 && !holds_data(*entry->second.blocks)) {
			forgotten.push_back(std::move(entry->second));
			files.erase(entry++);
		} else {
			++entry;
		}
	}
	sweep_at = MaxValue<idx_t>(MIN_SWEEP_RECORDS, 2 * files.size());
}

bool BlockCache::TryGetFile(const string &path, BlockCacheFileInfo &result) {
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	auto entry = files.find(path);
	if (entry == files.end() || entry->second.info.validation.IsExpired()) {
		return false;
	}
	result = entry->second.info;
	return true;
}

void BlockCache::ForbidReuse(BlockCacheFile &file) {
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto entry = files.find(file.path);
		if (entry == files.end() || entry->second.number != file.number) {
			return;
		}
		files.erase(entry);
	}
	DropBlocks(*file.blocks);
}

shared_ptr<BlockCacheFile> BlockCache::CreateUnbackedFile(const string &path) {
	uint32_t number;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		number = next_file_number++;
	}
	return make_shared_ptr<BlockCacheFile>(*this, path, number, true, BlockCacheFileInfo(), nullptr,
	                                       make_shared_ptr<BlockCacheBlocks>(*this, 0));
}

shared_ptr<BlockCacheFile> BlockCache::CreateWrittenFile(const string &path) {
	const BlockCacheFileInfo info;
	FileRecord created;
	optional<FileRecord> replaced;
	bool sweep = false;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto &record = files[path];
		replaced = Renumber(record, info, 1, sweep);
		created = record;
	}
	RetireRecord(std::move(replaced), sweep);
	return make_shared_ptr<BlockCacheFile>(*this, path, created.number, false, info, nullptr,
	                                       std::move(created.blocks));
}

void BlockCache::FinishWrittenFile(BlockCacheFile &file, const CacheValidationInfo &written, bool published) {
	D_ASSERT(!file.IsUnbacked());
	file.size.store(written.file_size, std::memory_order_release);
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto entry = files.find(file.path);
		if (entry != files.end() && entry->second.number == file.number) {
			if (published) {
				entry->second.info.validation = written;
			} else {
				files.erase(entry);
			}
		}
	}
	if (!published) {
		DropBlocks(*file.blocks);
	}
}

void BlockCache::ForgetFile(const string &path) {
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

[[gnu::always_inline]] bool BlockCache::PinLoaded(Slot &slot, uintptr_t value, idx_t size,
                                                  BlockCachePin &result) const {
	const auto memory = AsMemory(value);
	if ((value & SLOT_LOADING) || !memory || !memory->TryPinLoaded()) {
		return false;
	}
	if (slot.load(std::memory_order_acquire) != value) {
		buffer_manager.Unpin(*memory);
		return false;
	}
	result = BlockCachePin(BufferHandle(*memory, memory->GetLoadedBuffer()), memory->GetLoadedData(), size);
	hits[TaskScheduler::GetEstimatedCPUId() % HIT_COUNTERS].value.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool BlockCache::PinPresent(Slot &slot, idx_t size, BlockCachePin &result, optional<uintptr_t> &missing) {
	const auto value = slot.load(std::memory_order_acquire);
	const auto memory = AsMemory(value);
	if (PinLoaded(slot, value, size, result)) {
		return true;
	}
	if ((value & SLOT_LOADING) || (memory && memory->GetState() == BlockState::BLOCK_LOADED)) {
		missing.reset();
		return false;
	}
	missing = value;
	if (!memory) {
		return false;
	}
	if (IsDropped(*memory)) {
		return false;
	}
	auto owner = memory->GetOwner();
	auto buffer = buffer_manager.Pin(owner);
	if (!buffer.IsValid()) {
		return false;
	}
	missing.reset();
	result = BlockCachePin(std::move(buffer), size);
	return true;
}

bool BlockCache::Claim(Slot &slot, uintptr_t expected) {
	return slot.compare_exchange_strong(expected, expected | SLOT_LOADING, std::memory_order_seq_cst);
}

optional<uintptr_t> BlockCache::MissingFrom(const Slot &slot) {
	const auto value = slot.load(std::memory_order_acquire);
	const auto memory = AsMemory(value);
	if ((value & SLOT_LOADING) || (memory && !IsDropped(*memory))) {
		return optional<uintptr_t>();
	}
	return value;
}

BufferHandle BlockCache::AllocateBlock(const BlockCacheFile &file, Slot &slot) {
	const auto memory = AsMemory(slot.load(std::memory_order_acquire));
	if (memory) {
		auto owner = memory->GetOwner();
		return buffer_manager.PinEmpty(owner, block_manager.GetBlockSize(), block_manager.GetBlockHeaderSize());
	}
	auto buffer = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, &block_manager, !file.IsUnbacked());
	if (!file.IsUnbacked() && !file.Info().on_disk) {
		buffer.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::KEEP_COPY);
	}
	return buffer;
}

void BlockCache::Publish(BlockCacheBlocks &blocks, Slot &slot, BufferHandle &buffer, bool shared) {
	const auto value = slot.load(std::memory_order_relaxed);
	D_ASSERT(value & SLOT_LOADING);
	if (AsMemory(value)) {
		slot.store(value & ~SLOT_LOADING, std::memory_order_release);
	} else if (shared) {
		auto handle = buffer.GetBlockHandle();
		const auto memory = reinterpret_cast<uintptr_t>(&handle->GetMemory());
		{
			annotated_lock_guard<annotated_mutex> guard(blocks.lock);
			blocks.handles.push_back(std::move(handle));
		}
		block_count.fetch_add(1, std::memory_order_relaxed);
		slot.store(memory, std::memory_order_release);
	} else {
		slot.store(0, std::memory_order_release);
	}
	Wake(slot);
}

void BlockCache::Unclaim(Slot &slot) {
	slot.fetch_and(~SLOT_LOADING, std::memory_order_seq_cst);
	Wake(slot);
}

uint32_t BlockCache::WaitStripe(const Slot &slot) {
	return NumericCast<uint32_t>((reinterpret_cast<uintptr_t>(&slot) / CACHE_LINE) % WAIT_STRIPES);
}

void BlockCache::Wake(const Slot &slot) {
	auto &lock = wait_locks[WaitStripe(slot)];
	lock.Lock();
	lock.Unlock();
}

void BlockCache::WaitForLoad(Slot &slot) {
	auto &scheduler = TaskScheduler::GetScheduler(db);
	while (!scheduler.HasWorkers() && (slot.load(std::memory_order_acquire) & SLOT_LOADING)) {
		shared_ptr<Task> task;
		if (!TakeFetchTask(task)) {
			break;
		}
		task->Execute(TaskExecutionMode::PROCESS_ALL);
	}
	auto &lock = wait_locks[WaitStripe(slot)];
	TaskScheduler::BlockingScope blocking;
	lock.LockWhen(absl::Condition(
	    +[](Slot *waited) { return !(waited->load(std::memory_order_acquire) & SLOT_LOADING); }, &slot));
	lock.Unlock();
}

bool BlockCache::IsCached(BlockCacheFile &file, idx_t offset, idx_t size) const {
	const auto file_size = file.GetSize();
	if (size == 0 || offset >= file_size) {
		return true;
	}
	const auto last = (MinValue(offset + size, file_size) - 1) / BLOCK_SIZE;
	for (auto block = offset / BLOCK_SIZE; block <= last; block++) {
		auto slot = file.blocks->Find(block);
		if (!slot) {
			return false;
		}
		const auto value = slot->load(std::memory_order_acquire);
		const auto memory = AsMemory(value);
		if (!(value & SLOT_LOADING) && (!memory || IsDropped(*memory))) {
			return false;
		}
	}
	return true;
}

void BlockCache::Fetch(BlockCacheFile &file, const Run &run, std::span<optional_ptr<Slot>> slots,
                       std::span<BlockCachePin> pins) {
	D_ASSERT(slots.size() == run.block_count);
	D_ASSERT(pins.empty() || pins.size() == run.block_count);
	vector<BufferHandle> buffers(slots.size());
	idx_t first = slots.size();
	idx_t last = 0;
	for (idx_t i = 0; i < slots.size(); i++) {
		if (slots[i]) {
			first = MinValue(first, i);
			last = i;
		}
	}
	if (first == slots.size()) {
		return;
	}
	bool shareable = true;
	try {
		if (file.IsUnbacked()) {
			throw InternalException("A block of the in-memory file \"%s\" is missing from the block cache", file.path);
		}
		TaskScheduler::BlockingScope blocking;
		for (idx_t i = first; i <= last; i++) {
			if (slots[i]) {
				buffers[i] = AllocateBlock(file, *slots[i]);
			}
		}
		for (idx_t i = first; i <= last; i++) {
			if (slots[i] && !buffers[i].IsValid()) {
				Unclaim(*slots[i]);
				slots[i] = nullptr;
			}
		}
		while (first <= last && !slots[first]) {
			first++;
		}
		while (last > first && !slots[last]) {
			last--;
		}
		if (first > last) {
			return;
		}
		const auto start = (run.first_block + first) * BLOCK_SIZE;
		const auto end = MinValue(file.GetSize(), (run.first_block + last + 1) * BLOCK_SIZE);
		auto source = file.Source();
		AllocatedData gap;
		vector<FileReadTarget> targets;
		targets.reserve(last - first + 1);
		for (idx_t i = first; i <= last; i++) {
			const auto block_bytes = file.BlockBytes(run.first_block + i);
			if (slots[i]) {
				targets.push_back({buffers[i].GetDataMutable(), block_bytes});
				continue;
			}
			if (!gap.get()) {
				gap = Allocator::DefaultAllocator().Allocate(BLOCK_SIZE);
			}
			targets.push_back({gap.get(), block_bytes});
		}
		source->file_system.ReadScattered(*source, targets, start);
		for (idx_t i = first; i <= last; i++) {
			if (slots[i]) {
				const auto block_bytes = file.BlockBytes(run.first_block + i);
				memset(buffers[i].GetDataMutable() + block_bytes, 0, BLOCK_SIZE - block_bytes);
			}
		}
		const auto valid_until = source->file_system.GetCacheValidUntil(*source);
		shareable = !valid_until || *valid_until != timestamp_t::ninfinity();
		fetches.fetch_add(1, std::memory_order_relaxed);
		fetched_bytes.fetch_add(end - start, std::memory_order_relaxed);
	} catch (...) {
		for (idx_t i = 0; i < slots.size(); i++) {
			if (!slots[i]) {
				continue;
			}
			const auto memory = AsMemory(slots[i]->load(std::memory_order_relaxed));
			if (buffers[i].IsValid() && memory) {
				buffers[i].Destroy();
				auto owner = memory->GetOwner();
				buffer_manager.Discard(owner);
			}
			Unclaim(*slots[i]);
		}
		throw;
	}
	for (idx_t i = first; i <= last; i++) {
		if (!slots[i]) {
			continue;
		}
		Publish(*file.blocks, *slots[i], buffers[i], shareable);
		if (!pins.empty()) {
			pins[i] = BlockCachePin(std::move(buffers[i]), file.BlockBytes(run.first_block + i));
		}
	}
}

BlockCachePin BlockCache::Pin(const shared_ptr<BlockCacheFile> &file_p, idx_t block) {
	auto &file = *file_p;
	if (block >= file.BlockCount()) {
		throw InternalException("Block %llu of \"%s\" lies past its end", block, file.path);
	}
	const auto size = file.BlockBytes(block);
	auto found = file.blocks->Find(block);
	if (found) {
		BlockCachePin result;
		if (PinLoaded(*found, found->load(std::memory_order_acquire), size, result)) {
			return result;
		}
	}
	auto &slot = found ? *found : file.blocks->Get(block);
	while (true) {
		BlockCachePin result;
		optional<uintptr_t> missing;
		if (PinPresent(slot, size, result, missing)) {
			return result;
		}
		if (!missing) {
			WaitForLoad(slot);
			continue;
		}
		D_ASSERT(!file.IsUnbacked() || !AsMemory(*missing));
		if (!Claim(slot, *missing)) {
			continue;
		}
		misses.fetch_add(1, std::memory_order_relaxed);
		optional_ptr<Slot> claimed = &slot;
		Fetch(file, Run {block, 1}, std::span(&claimed, 1), std::span(&result, 1));
		if (result.IsValid()) {
			return result;
		}
	}
}

BlockCachePin BlockCache::PinInOrder(const shared_ptr<BlockCacheFile> &file, idx_t block,
                                     BlockCacheReadAhead &read_ahead, bool ahead) {
	if (file->IsUnbacked()) {
		return Pin(file, block);
	}
	if (block != std::exchange(read_ahead.next_block, block + 1)) {
		read_ahead.blocks = 0;
		read_ahead.end = 0;
	} else if (ahead && read_ahead.end <= block + 1 + read_ahead.blocks / 2) {
		read_ahead.blocks = MinValue<idx_t>(MaxValue<idx_t>(read_ahead.blocks * 2, 2), READ_AHEAD_BLOCKS);
		read_ahead.end = MaxValue(block + 1, read_ahead.end) + read_ahead.blocks;
		try {
			Prefetch(file, block * BLOCK_SIZE, (read_ahead.end - block) * BLOCK_SIZE, BlockCachePrefetch::HINT);
		} catch (...) {
		}
	}
	return Pin(file, block);
}

template <class MISSING, class ADMIT>
BlockCache::RunPlan BlockCache::PlanRun(BlockCacheFile &file, idx_t block, idx_t end, MISSING &&missing,
                                        ADMIT &&admit) {
	RunPlan plan {Run {block, 0}, {}, 0, block, false};
	plan.slots.reserve(MAX_REQUEST / BLOCK_SIZE);
	absl::Cleanup abandon = [&] {
		for (auto &slot : plan.slots) {
			if (slot) {
				Unclaim(*slot);
			}
		}
	};
	idx_t gap = 0;
	for (; plan.next < end && (plan.run.block_count + 1) * BLOCK_SIZE <= MAX_REQUEST; plan.next++) {
		const optional<uintptr_t> from = missing(plan.next);
		if (from) {
			const auto block_bytes = file.BlockBytes(plan.next);
			if (!admit(plan.bytes + block_bytes)) {
				plan.exhausted = true;
				break;
			}
			auto &slot = file.blocks->Get(plan.next);
			if (Claim(slot, *from)) {
				plan.slots.push_back(&slot);
				plan.run.block_count++;
				plan.bytes += block_bytes;
				gap = 0;
				misses.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
		}
		if ((gap + 1) * BLOCK_SIZE > MERGE_GAP) {
			break;
		}
		gap++;
		plan.slots.push_back(nullptr);
		plan.run.block_count++;
	}
	while (!plan.slots.empty() && !plan.slots.back()) {
		plan.slots.pop_back();
		plan.run.block_count--;
	}
	std::move(abandon).Cancel();
	return plan;
}

vector<BlockCachePin> BlockCache::PinRange(const shared_ptr<BlockCacheFile> &file_p, idx_t offset, idx_t size) {
	auto &file = *file_p;
	vector<BlockCachePin> result;
	const auto file_size = file.GetSize();
	if (size == 0 || offset >= file_size) {
		return result;
	}
	const auto end = MinValue(offset + size, file_size);
	const auto first = offset / BLOCK_SIZE;
	const auto count = (end - 1) / BLOCK_SIZE - first + 1;
	result.resize(count);
	vector<optional<uintptr_t>> missing(count);
	bool complete = true;
	for (idx_t i = 0; i < count; i++) {
		if (!PinPresent(file.blocks->Get(first + i), file.BlockBytes(first + i), result[i], missing[i])) {
			complete = complete && !missing[i];
		}
	}
	if (!complete) {
		vector<RunPlan> plans;
		absl::Cleanup abandon = [&] {
			for (auto &plan : plans) {
				for (auto &slot : plan.slots) {
					if (slot) {
						Unclaim(*slot);
					}
				}
			}
		};
		for (auto block = first; block < first + count;) {
			if (!missing[block - first]) {
				block++;
				continue;
			}
			auto plan = PlanRun(
			    file, block, first + count, [&](idx_t next) { return missing[next - first]; },
			    [](idx_t) { return true; });
			if (plan.slots.empty()) {
				block++;
				continue;
			}
			block = plan.next;
			plans.push_back(std::move(plan));
		}
		std::move(abandon).Cancel();
		FetchConcurrently(file, plans, first, result);
	}
	for (idx_t i = 0; i < count; i++) {
		if (!result[i].IsValid()) {
			result[i] = Pin(file_p, first + i);
		}
	}
	return result;
}

BlockCachePin BlockCache::PinBytes(const shared_ptr<BlockCacheFile> &file, idx_t offset, idx_t size) {
	D_ASSERT(size != 0 && offset + size <= file->GetSize());
	const auto first = offset / BLOCK_SIZE;
	if ((offset + size - 1) / BLOCK_SIZE == first) {
		auto pin = Pin(file, first);
		pin.data += offset % BLOCK_SIZE;
		pin.size = size;
		return pin;
	}
	const auto pins = PinRange(file, offset, size);
	auto result = AllocateUncached(db, size);
	for (idx_t done = 0; done < size;) {
		const auto at = offset + done;
		const auto in_block = at % BLOCK_SIZE;
		const auto chunk = MinValue(size - done, BLOCK_SIZE - in_block);
		memcpy(result.data + done, pins[at / BLOCK_SIZE - first].Ptr() + in_block, chunk);
		done += chunk;
	}
	return result;
}

void BlockCache::FetchConcurrently(BlockCacheFile &file, vector<RunPlan> &plans, idx_t first,
                                   vector<BlockCachePin> &pins) {
	const auto for_each_piece = [&](auto &&callback) {
		for (auto &plan : plans) {
			const auto piece_blocks = PieceBlocks(plan.run.block_count);
			for (idx_t offset = 0; offset < plan.run.block_count; offset += piece_blocks) {
				const Run piece {plan.run.first_block + offset, MinValue(piece_blocks, plan.run.block_count - offset)};
				callback(piece, std::span(plan.slots).subspan(offset, piece.block_count),
				         std::span(pins).subspan(piece.first_block - first, piece.block_count));
			}
		}
	};
	idx_t pieces = 0;
	for_each_piece([&](const Run &, std::span<optional_ptr<Slot>>, std::span<BlockCachePin>) { pieces++; });
	if (pieces == 0) {
		return;
	}
	if (pieces == 1) {
		for_each_piece([&](const Run &piece, std::span<optional_ptr<Slot>> slots, std::span<BlockCachePin> out) {
			Fetch(file, piece, slots, out);
		});
		return;
	}
	TaskExecutor executor(TaskScheduler::GetScheduler(db), TaskSchedulerType::ASYNC);
	for_each_piece([&](const Run &piece, std::span<optional_ptr<Slot>> slots, std::span<BlockCachePin> out) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor, [this, &file, piece, slots, out]() { Fetch(file, piece, slots, out); }, "BlockCacheFetchTask"));
	});
	executor.WorkOnTasks();
}

vector<BlockCachePin> BlockCache::TryPinRange(BlockCacheFile &file, idx_t offset, idx_t size) {
	vector<BlockCachePin> result;
	const auto file_size = file.GetSize();
	if (size == 0 || offset >= file_size) {
		return result;
	}
	const auto first = offset / BLOCK_SIZE;
	const auto last = (MinValue(offset + size, file_size) - 1) / BLOCK_SIZE;
	result.reserve(last - first + 1);
	for (auto block = first; block <= last; block++) {
		auto slot = file.blocks->Find(block);
		BlockCachePin pin;
		if (!slot || !PinLoaded(*slot, slot->load(std::memory_order_acquire), file.BlockBytes(block), pin)) {
			result.clear();
			break;
		}
		result.push_back(std::move(pin));
	}
	return result;
}

ProducerToken &BlockCache::Producer() {
	annotated_lock_guard<annotated_mutex> guard(fetch_lock);
	if (!producer) {
		producer = TaskScheduler::GetScheduler(db).CreateProducer();
	}
	return *producer;
}

void BlockCache::FinishFetchTask(idx_t bytes) {
	in_flight.fetch_sub(bytes, std::memory_order_relaxed);
	annotated_lock_guard<annotated_mutex> guard(fetch_lock);
	pending_tasks--;
}

bool BlockCache::TakeFetchTask(shared_ptr<Task> &task) {
	annotated_lock_guard<annotated_mutex> guard(fetch_lock);
	return producer && TaskScheduler::GetScheduler(db).GetTaskFromProducer(*producer, task);
}

void BlockCache::StopFetching() {
	{
		annotated_lock_guard<annotated_mutex> guard(fetch_lock);
		stopped = true;
	}
	while (true) {
		shared_ptr<Task> task;
		if (!TakeFetchTask(task)) {
			break;
		}
	}
	annotated_unique_lock<annotated_mutex> guard(fetch_lock);
	while (pending_tasks != 0) {
		fetch_lock.Await(absl::Condition(
		    +[](BlockCache *cache) DUCKDB_NO_THREAD_SAFETY_ANALYSIS { return cache->pending_tasks == 0; }, this));
	}
	producer.reset();
}

void BlockCache::Prefetch(const shared_ptr<BlockCacheFile> &file_p, idx_t offset, idx_t size, BlockCachePrefetch kind) {
	auto &file = *file_p;
	const auto file_size = file.GetSize();
	if (file.IsUnbacked() || size == 0 || offset >= file_size) {
		return;
	}
	size = MinValue(size, file_size - offset);
	const auto end = (offset + size - 1) / BLOCK_SIZE + 1;
	const auto missing = [&](idx_t block) -> optional<uintptr_t> {
		auto slot = file.blocks->Find(block);
		if (!slot) {
			return uintptr_t(0);
		}
		return MissingFrom(*slot);
	};
	const auto admit = [&](idx_t bytes) {
		return kind == BlockCachePrefetch::NEED ||
		       (file.in_flight.load(std::memory_order_relaxed) + bytes <= MAX_FILE_IN_FLIGHT &&
		        in_flight.load(std::memory_order_relaxed) + bytes <= MAX_IN_FLIGHT);
	};
	for (auto block = offset / BLOCK_SIZE; block < end;) {
		if (!missing(block)) {
			block++;
			continue;
		}
		auto plan = PlanRun(file, block, end, missing, admit);
		if (!plan.slots.empty()) {
			ScheduleFetch(file_p, plan.run, std::move(plan.slots));
		}
		if (plan.exhausted) {
			return;
		}
		block = plan.next;
	}
}

void BlockCache::ScheduleFetch(const shared_ptr<BlockCacheFile> &file, const Run &run,
                               vector<optional_ptr<Slot>> slots) {
	idx_t handed = 0;
	absl::Cleanup abandon = [&] {
		for (idx_t i = handed; i < slots.size(); i++) {
			if (slots[i]) {
				Unclaim(*slots[i]);
			}
		}
	};
	auto &scheduler = TaskScheduler::GetScheduler(db);
	const auto piece_blocks = PieceBlocks(run.block_count);
	for (idx_t first = 0; first < run.block_count; first += piece_blocks) {
		const Run piece {run.first_block + first, MinValue(piece_blocks, run.block_count - first)};
		const auto range = std::span(slots).subspan(first, piece.block_count);
		vector<optional_ptr<Slot>> piece_slots(range.begin(), range.end());
		idx_t bytes = 0;
		for (idx_t i = 0; i < piece.block_count; i++) {
			if (piece_slots[i]) {
				bytes += file->BlockBytes(piece.first_block + i);
			}
		}
		shared_ptr<Task> task = make_shared_ptr<BlockCacheFetchTask>(*this, file, piece, std::move(piece_slots), bytes);
		handed = first + piece.block_count;
		file->in_flight.fetch_add(bytes, std::memory_order_relaxed);
		in_flight.fetch_add(bytes, std::memory_order_relaxed);
		{
			annotated_lock_guard<annotated_mutex> guard(fetch_lock);
			pending_tasks++;
			if (stopped) {
				continue;
			}
		}
		scheduler.ScheduleTask(Producer(), std::move(task), scheduler.AsyncPool());
	}
}

BlockCachePin BlockCache::CreateBlock(BlockCacheFile &file, idx_t block) {
	D_ASSERT(file.IsUnbacked() || !file.HasSource());
	auto &slot = file.blocks->Get(block);
	if (!Claim(slot, 0)) {
		throw InternalException("Block %llu of \"%s\" is written twice", block, file.path);
	}
	BufferHandle buffer;
	try {
		buffer = AllocateBlock(file, slot);
	} catch (...) {
		Unclaim(slot);
		throw;
	}
	Publish(*file.blocks, slot, buffer, true);
	return BlockCachePin(std::move(buffer), BLOCK_SIZE);
}

void BlockCache::SetUnbackedSize(BlockCacheFile &file, idx_t size) {
	D_ASSERT(file.IsUnbacked());
	file.size.store(size, std::memory_order_release);
}

void BlockCache::Clear() {
	vector<shared_ptr<BlockCacheBlocks>> versions;
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

void BlockCache::SetEnabled(bool enabled_p) {
	if (enabled.exchange(enabled_p) && !enabled_p) {
		Clear();
	}
}

BlockCacheStatistics BlockCache::GetStatistics() const {
	BlockCacheStatistics result;
	result.capacity = buffer_manager.GetMaxMemory();
	for (auto &info : buffer_manager.GetMemoryUsageInfo()) {
		if (info.tag == MemoryTag::EXTERNAL_FILE_CACHE) {
			result.used = info.size;
			result.evictions = info.evictions;
			result.spilled = info.temporary_writes;
			result.spill_reads = info.temporary_reads;
		}
	}
	result.blocks = block_count.load(std::memory_order_relaxed);
	for (auto &counter : hits) {
		result.hits += counter.value.load(std::memory_order_relaxed);
	}
	result.misses = misses.load(std::memory_order_relaxed);
	result.fetched_bytes = fetched_bytes.load(std::memory_order_relaxed);
	result.fetches = fetches.load(std::memory_order_relaxed);
	return result;
}

vector<CachedFileInformation> BlockCache::GetBlockInformation() const {
	struct Named {
		string path;
		idx_t size;
		shared_ptr<BlockCacheBlocks> blocks;
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
		file.blocks->ForEach([&](idx_t block, Slot &slot) {
			const auto value = slot.load(std::memory_order_acquire);
			const auto memory = (value & SLOT_LOADING) ? nullptr : AsMemory(value);
			const bool loaded = memory && memory->GetState() == BlockState::BLOCK_LOADED;
			const bool spilled = memory && memory->HasTemporaryFile();
			if (loaded || spilled) {
				result.push_back({file.path, BlockBytesOf(file.size, block), block * BLOCK_SIZE, loaded, spilled});
			}
		});
	}
	return result;
}

} // namespace duckdb
