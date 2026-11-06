#include "duckdb/storage/block_cache/block_cache.hpp"

#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <algorithm>
#include <cstring>

namespace duckdb {

namespace {

constexpr idx_t MAX_IN_FLIGHT = 16 * BlockCache::MAX_FILE_IN_FLIGHT;

//! Whether the block holds no data: its buffer is gone and no temporary file keeps a copy of it
bool IsDropped(const BlockMemory &memory) {
	return memory.IsUnloaded() && !memory.HasTemporaryFile();
}

//! Whether a block whose slot holds [memory] has to be fetched: it holds no data and no one is loading it
bool IsMissing(const BlockMemory *memory) {
	return !memory || (!memory->IsLoading() && IsDropped(*memory));
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

idx_t BlockBytesOf(idx_t file_size, idx_t block) {
	const auto start = block * BlockCache::BLOCK_SIZE;
	return start >= file_size ? 0 : MinValue(BlockCache::BLOCK_SIZE, file_size - start);
}

struct BlockRange {
	idx_t first;
	idx_t end;
};

//! The blocks [first, end) that hold the bytes [offset, offset + size) of a file of [file_size] bytes, as far as they
//! lie inside it
BlockRange BlocksOf(idx_t file_size, idx_t offset, idx_t size) {
	if (size == 0 || offset >= file_size) {
		return {0, 0};
	}
	return {offset / BlockCache::BLOCK_SIZE, (MinValue(offset + size, file_size) - 1) / BlockCache::BLOCK_SIZE + 1};
}

//! Narrows [pins] of the blocks that hold [offset, end) of a file to those bytes
void NarrowPins(vector<BlockCachePin> &pins, idx_t offset, idx_t end) {
	if (pins.empty()) {
		return;
	}
	auto &last = pins.back();
	last.Narrow(0, end - (offset / BlockCache::BLOCK_SIZE + pins.size() - 1) * BlockCache::BLOCK_SIZE);
	auto &head = pins.front();
	head.Narrow(offset % BlockCache::BLOCK_SIZE, head.Size() - offset % BlockCache::BLOCK_SIZE);
}

//! Calls [callback] with the offset and the block count of each piece of a run of [count] blocks: equal pieces of at
//! most PIECE_SIZE
template <class FUNC>
void ForEachPiece(idx_t count, FUNC &&callback) {
	if (count == 0) {
		return;
	}
	constexpr idx_t MAX_BLOCKS = BlockCache::PIECE_SIZE / BlockCache::BLOCK_SIZE;
	const auto pieces = (count + MAX_BLOCKS - 1) / MAX_BLOCKS;
	const auto piece_blocks = (count + pieces - 1) / pieces;
	for (idx_t offset = 0; offset < count; offset += piece_blocks) {
		callback(offset, MinValue(piece_blocks, count - offset));
	}
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
	return BlockCachePin(handle.Share(), data + offset, size_p);
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

BlockCacheBlocks::BlockCacheBlocks(idx_t block_count) {
	auto directory = make_uniq<Directory>(MaxValue<idx_t>(1, (block_count + CHUNK - 1) / CHUNK));
	current.store(directory.get(), std::memory_order_release);
	annotated_lock_guard<annotated_mutex> guard(lock);
	directories.push_back(std::move(directory));
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

BlockCacheFile::BlockCacheFile(string path_p, bool unbacked_p, BlockCacheFileInfo info_p,
                               shared_ptr<FileHandle> source_p, shared_ptr<BlockCacheBlocks> blocks_p)
    : path(std::move(path_p)), size(info_p.validation.file_size), unbacked(unbacked_p), info(std::move(info_p)),
      source(std::move(source_p)), blocks(std::move(blocks_p)) {
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

idx_t BlockCacheFile::BlockBytes(idx_t block) const {
	return BlockBytesOf(GetSize(), block);
}

//===--------------------------------------------------------------------===//
// BlockCacheAppender
//===--------------------------------------------------------------------===//

BlockCacheAppender::BlockCacheAppender(shared_ptr<BlockCacheFile> file_p) : file(std::move(file_p)) {
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
		current = file->GetCache().CreateBlock(*file, size / BlockCache::BLOCK_SIZE);
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
		file->GetCache().SetUnbackedSize(*file, size);
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
		cache.PublishWrittenFile(*file, written);
	}
}

void BlockCacheAppender::Abandon() {
	D_ASSERT(!file->IsUnbacked());
	current.Reset();
	cache.ForbidReuse(*file);
}

//===--------------------------------------------------------------------===//
// BlockCacheFetchTask
//===--------------------------------------------------------------------===//

class BlockCacheFetchTask : public Task {
public:
	BlockCacheFetchTask(BlockCache &cache_p, shared_ptr<BlockCacheFile> file_p, idx_t first_block_p,
	                    vector<optional_ptr<BlockMemory>> claimed_p, idx_t bytes_p)
	    : cache(cache_p), file(std::move(file_p)), first_block(first_block_p), claimed(std::move(claimed_p)),
	      bytes(bytes_p) {
	}

	~BlockCacheFetchTask() override {
		ReleaseClaims(claimed);
		file->in_flight.fetch_sub(bytes, std::memory_order_relaxed);
		file.reset();
		cache.FinishFetchTask(bytes);
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		try {
			cache.Fetch(*file, first_block, claimed, {});
		} catch (std::exception &) { // NOLINT
		}
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "BlockCacheFetchTask";
	}

private:
	shared_ptr<BlockCacheFile> file;
	idx_t first_block;
	vector<optional_ptr<BlockMemory>> claimed;
	idx_t bytes;
};

//===--------------------------------------------------------------------===//
// BlockCache
//===--------------------------------------------------------------------===//

BlockCache::BlockCache(DatabaseInstance &db_p) : db(db_p), buffer_manager(BufferManager::GetBufferManager(db_p)) {
}

BlockCache::~BlockCache() {
	StopFetching();
}

BlockCache &BlockCache::Get(DatabaseInstance &db) {
	return db.GetBlockCache();
}

BlockCachePin BlockCache::AllocateUncached(DatabaseInstance &db, idx_t size) {
	auto buffer = BufferManager::GetBufferManager(db).Allocate(MemoryTag::EXTERNAL_FILE_CACHE, size);
	buffer.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	const auto data = buffer.GetFileBuffer().InternalBuffer();
	return BlockCachePin(std::move(buffer), data, size);
}

void BlockCache::DropBlocks(BlockCacheBlocks &blocks) {
	vector<reference<BlockMemory>> dropped;
	blocks.ForEach([&](idx_t, BlockCacheSlot &slot) {
		if (auto memory = slot.exchange(nullptr, std::memory_order_seq_cst)) {
			dropped.push_back(*memory);
		}
	});
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

optional<BlockCache::FileRecord> BlockCache::NewVersion(FileRecord &record, const BlockCacheFileInfo &info,
                                                        bool &sweep) {
	optional<FileRecord> replaced;
	if (record.blocks) {
		replaced = std::move(record);
	} else if (files.size() >= sweep_at) {
		sweep = true;
		sweep_at = NumericLimits<idx_t>::Maximum();
	}
	record = FileRecord {info, make_shared_ptr<BlockCacheBlocks>(BlockCountOf(info.validation.file_size))};
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
		if (!record.blocks || (validate ? !CacheValidation::IsValid(record.info.validation, current.validation)
		                                : record.info.validation.IsExpired())) {
			replaced = NewVersion(record, current, sweep);
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
	return make_shared_ptr<BlockCacheFile>(path, false, std::move(opened.info), std::move(source),
	                                       std::move(opened.blocks));
}

void BlockCache::SweepFiles() {
	const auto holds_data = [&](BlockCacheBlocks &blocks) {
		bool result = false;
		blocks.ForEach(
		    [&](idx_t, BlockCacheSlot &slot) { result = result || !IsMissing(slot.load(std::memory_order_acquire)); });
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
		if (entry != files.end() && entry->second.blocks == file.blocks) {
			files.erase(entry);
		}
	}
	DropBlocks(*file.blocks);
}

shared_ptr<BlockCacheFile> BlockCache::CreateUnbackedFile(const string &path) {
	return make_shared_ptr<BlockCacheFile>(path, true, BlockCacheFileInfo(), nullptr,
	                                       make_shared_ptr<BlockCacheBlocks>(0));
}

shared_ptr<BlockCacheFile> BlockCache::CreateWrittenFile(const string &path) {
	const BlockCacheFileInfo info;
	FileRecord created;
	optional<FileRecord> replaced;
	bool sweep = false;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto &record = files[path];
		replaced = NewVersion(record, info, sweep);
		created = record;
	}
	RetireRecord(std::move(replaced), sweep);
	return make_shared_ptr<BlockCacheFile>(path, false, info, nullptr, std::move(created.blocks));
}

void BlockCache::PublishWrittenFile(BlockCacheFile &file, const CacheValidationInfo &written) {
	D_ASSERT(!file.IsUnbacked());
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	auto entry = files.find(file.path);
	if (entry != files.end() && entry->second.blocks == file.blocks) {
		entry->second.info.validation = written;
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

[[gnu::always_inline]] bool BlockCache::PinLoaded(BlockCacheSlot &slot, idx_t size, BlockCachePin &result) const {
	const auto memory = slot.load(std::memory_order_acquire);
	if (!memory || !memory->TryPinLoaded()) {
		return false;
	}
	if (slot.load(std::memory_order_acquire) != memory) {
		buffer_manager.Unpin(*memory);
		return false;
	}
	result = BlockCachePin(BufferHandle(*memory, memory->GetLoadedBuffer()), memory->GetLoadedData(), size);
	hits[TaskScheduler::GetEstimatedCPUId() % HIT_COUNTERS].value.fetch_add(1, std::memory_order_relaxed);
	return true;
}

BlockMemory &BlockCache::MemoryOf(const BlockCacheFile &file, BlockCacheSlot &slot) {
	auto memory = slot.load(std::memory_order_acquire);
	if (memory) {
		return *memory;
	}
	const auto destroy_buffer_upon = file.IsUnbacked()     ? DestroyBufferUpon::BLOCK
	                                 : file.Info().on_disk ? DestroyBufferUpon::EVICTION
	                                                       : DestroyBufferUpon::KEEP_COPY;
	auto handle =
	    buffer_manager.RegisterUnloadedMemory(MemoryTag::EXTERNAL_FILE_CACHE, BLOCK_SIZE, 0, destroy_buffer_upon);
	auto &created = handle->GetMemory();
	if (!slot.compare_exchange_strong(memory, &created, std::memory_order_seq_cst)) {
		return *memory;
	}
	annotated_lock_guard<annotated_mutex> guard(file.blocks->lock);
	file.blocks->handles.push_back(std::move(handle));
	return created;
}

optional_ptr<BlockMemory> BlockCache::ClaimMissing(BlockCacheFile &file, idx_t block) {
	auto &slot = file.blocks->Get(block);
	auto &memory = MemoryOf(file, slot);
	if (!memory.TryClaim()) {
		return nullptr;
	}
	if (slot.load(std::memory_order_acquire) != &memory || !IsDropped(memory)) {
		memory.FinishLoad();
		return nullptr;
	}
	return &memory;
}

void BlockCache::WaitForLoad(BlockMemory &memory) {
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

bool BlockCache::IsCached(BlockCacheFile &file, idx_t offset, idx_t size) const {
	const auto range = BlocksOf(file.GetSize(), offset, size);
	for (auto block = range.first; block < range.end; block++) {
		if (IsMissing(file.blocks->Load(block))) {
			return false;
		}
	}
	return true;
}

void BlockCache::Fetch(BlockCacheFile &file, idx_t first_block, std::span<optional_ptr<BlockMemory>> claimed,
                       std::span<BlockCachePin> pins) {
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
	vector<optional<BlockLoad>> loads(claimed.size());
	for (idx_t i = first; i <= last; i++) {
		if (claimed[i]) {
			loads[i].emplace(buffer_manager.PrepareLoad(*claimed[i], BLOCK_SIZE, 0));
		}
	}
	const auto start = (first_block + first) * BLOCK_SIZE;
	const auto end = MinValue(file.GetSize(), (first_block + last + 1) * BLOCK_SIZE);
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
		memset(data + block_bytes, 0, BLOCK_SIZE - block_bytes);
		targets.push_back({data, block_bytes});
	}
	source->file_system.ReadScattered(*source, targets, start);
	const auto valid_until = source->file_system.GetCacheValidUntil(*source);
	const bool shareable = !valid_until || *valid_until != timestamp_t::ninfinity();
	if (!shareable) {
		file.reuse_forbidden.store(true, std::memory_order_relaxed);
	}
	fetches.fetch_add(1, std::memory_order_relaxed);
	fetched_bytes.fetch_add(end - start, std::memory_order_relaxed);
	for (idx_t i = first; i <= last; i++) {
		if (!loads[i]) {
			continue;
		}
		shared_ptr<BlockHandle> unshared;
		if (!shareable) {
			unshared = buffer_manager.RegisterUnloadedMemory(MemoryTag::EXTERNAL_FILE_CACHE, BLOCK_SIZE, 0,
			                                                 DestroyBufferUpon::UNPIN);
		}
		auto buffer = buffer_manager.CompleteLoad(unshared ? unshared->GetMemory() : *claimed[i], std::move(*loads[i]));
		claimed[i]->FinishLoad();
		claimed[i] = nullptr;
		if (!pins.empty()) {
			pins[i] = BlockCachePin(std::move(buffer), file.BlockBytes(first_block + i));
		}
	}
}

BlockCachePin BlockCache::Pin(BlockCacheFile &file, idx_t block) {
	const auto size = file.BlockBytes(block);
	if (size == 0) {
		throw InternalException("Block %llu of \"%s\" lies past its end", block, file.path);
	}
	auto &slot = file.blocks->Get(block);
	while (true) {
		BlockCachePin result;
		if (PinLoaded(slot, size, result)) {
			return result;
		}
		auto &memory = MemoryOf(file, slot);
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
			result = BlockCachePin(std::move(buffer), size);
			return result;
		}
		if (file.IsUnbacked()) {
			throw InternalException("A block of the in-memory file \"%s\" is missing from the block cache", file.path);
		}
		misses.fetch_add(1, std::memory_order_relaxed);
		Fetch(file, block, std::span(&claimed, 1), std::span(&result, 1));
		return result;
	}
}

BlockCachePin BlockCache::PinInOrder(const shared_ptr<BlockCacheFile> &file, idx_t block,
                                     BlockCacheReadAhead &read_ahead, bool ahead) {
	if (file->IsUnbacked()) {
		return Pin(*file, block);
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
	return Pin(*file, block);
}

template <class ADMIT>
BlockCache::RunPlan BlockCache::PlanRun(BlockCacheFile &file, idx_t block, idx_t end, ADMIT &&admit) {
	RunPlan plan {block, {}, block, false};
	plan.claimed.reserve(MAX_REQUEST / BLOCK_SIZE);
	absl::Cleanup abandon = [&] {
		ReleaseClaims(plan.claimed);
	};
	idx_t bytes = 0;
	idx_t gap = 0;
	for (; plan.next < end && (plan.claimed.size() + 1) * BLOCK_SIZE <= MAX_REQUEST; plan.next++) {
		if (IsMissing(file.blocks->Load(plan.next))) {
			const auto block_bytes = file.BlockBytes(plan.next);
			if (!admit(bytes + block_bytes)) {
				plan.exhausted = true;
				break;
			}
			if (const auto memory = ClaimMissing(file, plan.next)) {
				plan.claimed.push_back(memory);
				bytes += block_bytes;
				gap = 0;
				misses.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
		}
		if ((gap + 1) * BLOCK_SIZE > MERGE_GAP) {
			break;
		}
		gap++;
		plan.claimed.push_back(nullptr);
	}
	while (!plan.claimed.empty() && !plan.claimed.back()) {
		plan.claimed.pop_back();
	}
	std::move(abandon).Cancel();
	return plan;
}

vector<BlockCachePin> BlockCache::PinRange(BlockCacheFile &file, idx_t offset, idx_t size) {
	vector<BlockCachePin> result;
	const auto range = BlocksOf(file.GetSize(), offset, size);
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
			auto plan = PlanRun(file, block, range.end, [](idx_t) { return true; });
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
	NarrowPins(result, offset, MinValue(offset + size, file.GetSize()));
	return result;
}

BlockCachePin BlockCache::PinBytes(BlockCacheFile &file, idx_t offset, idx_t size) {
	D_ASSERT(size != 0 && offset + size <= file.GetSize());
	if ((offset + size - 1) / BLOCK_SIZE == offset / BLOCK_SIZE) {
		auto pin = Pin(file, offset / BLOCK_SIZE);
		pin.Narrow(offset % BLOCK_SIZE, size);
		return pin;
	}
	auto result = AllocateUncached(db, size);
	idx_t done = 0;
	for (auto &pin : PinRange(file, offset, size)) {
		memcpy(result.GetDataMutable() + done, pin.Ptr(), pin.Size());
		done += pin.Size();
	}
	return result;
}

void BlockCache::FetchConcurrently(BlockCacheFile &file, vector<RunPlan> &plans, idx_t first,
                                   vector<BlockCachePin> &pins) {
	const auto for_each_piece = [&](auto &&callback) {
		for (auto &plan : plans) {
			ForEachPiece(plan.claimed.size(), [&](idx_t offset, idx_t count) {
				const auto piece_first = plan.first_block + offset;
				callback(piece_first, std::span(plan.claimed).subspan(offset, count),
				         std::span(pins).subspan(piece_first - first, count));
			});
		}
	};
	idx_t pieces = 0;
	for_each_piece([&](idx_t, std::span<optional_ptr<BlockMemory>>, std::span<BlockCachePin>) { pieces++; });
	if (pieces == 0) {
		return;
	}
	if (pieces == 1) {
		for_each_piece([&](idx_t piece_first, std::span<optional_ptr<BlockMemory>> claimed,
		                   std::span<BlockCachePin> out) { Fetch(file, piece_first, claimed, out); });
		return;
	}
	TaskExecutor executor(TaskScheduler::GetScheduler(db), TaskSchedulerType::ASYNC);
	for_each_piece([&](idx_t piece_first, std::span<optional_ptr<BlockMemory>> claimed, std::span<BlockCachePin> out) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor, [this, &file, piece_first, claimed, out]() { Fetch(file, piece_first, claimed, out); },
		    "BlockCacheFetchTask"));
	});
	executor.WorkOnTasks();
}

vector<BlockCachePin> BlockCache::TryPinRange(BlockCacheFile &file, idx_t offset, idx_t size) {
	vector<BlockCachePin> result;
	const auto range = BlocksOf(file.GetSize(), offset, size);
	result.reserve(range.end - range.first);
	for (auto block = range.first; block < range.end; block++) {
		const auto slot = file.blocks->Find(block);
		BlockCachePin pin;
		if (!slot || !PinLoaded(*slot, file.BlockBytes(block), pin)) {
			result.clear();
			return result;
		}
		result.push_back(std::move(pin));
	}
	NarrowPins(result, offset, MinValue(offset + size, file.GetSize()));
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
	if (file.IsUnbacked()) {
		return;
	}
	const auto range = BlocksOf(file.GetSize(), offset, size);
	const auto admit = [&](idx_t bytes) {
		return kind == BlockCachePrefetch::NEED ||
		       (file.in_flight.load(std::memory_order_relaxed) + bytes <= MAX_FILE_IN_FLIGHT &&
		        in_flight.load(std::memory_order_relaxed) + bytes <= MAX_IN_FLIGHT);
	};
	for (auto block = range.first; block < range.end;) {
		if (!IsMissing(file.blocks->Load(block))) {
			block++;
			continue;
		}
		auto plan = PlanRun(file, block, range.end, admit);
		if (!plan.claimed.empty()) {
			ScheduleFetch(file_p, plan.first_block, std::move(plan.claimed));
		}
		if (plan.exhausted) {
			return;
		}
		block = plan.next;
	}
}

void BlockCache::Prefetch(const shared_ptr<BlockCacheFile> &file, std::span<BlockCacheRange> ranges,
                          BlockCachePrefetch kind) {
	std::sort(ranges.begin(), ranges.end(),
	          [](const BlockCacheRange &l, const BlockCacheRange &r) { return l.offset < r.offset; });
	idx_t merged = 0;
	for (auto &range : ranges) {
		if (merged != 0 && range.offset <= ranges[merged - 1].offset + ranges[merged - 1].size + MERGE_GAP) {
			auto &last = ranges[merged - 1];
			last.size = MaxValue(last.size, range.offset + range.size - last.offset);
		} else {
			ranges[merged++] = range;
		}
	}
	for (auto &range : ranges.first(merged)) {
		Prefetch(file, range.offset, range.size, kind);
	}
}

void BlockCache::MarkUnused(const BlockCacheFile &file, idx_t offset, idx_t size) const {
	const auto file_size = file.GetSize();
	if (size == 0 || offset >= file_size) {
		return;
	}
	const auto end = MinValue(offset + size, file_size);
	const auto last = end == file_size ? (end - 1) / BLOCK_SIZE + 1 : end / BLOCK_SIZE;
	for (auto block = (offset + BLOCK_SIZE - 1) / BLOCK_SIZE; block < last; block++) {
		if (const auto memory = file.blocks->Load(block)) {
			memory->TakeReferenced();
		}
	}
}

void BlockCache::ScheduleFetch(const shared_ptr<BlockCacheFile> &file, idx_t first_block,
                               vector<optional_ptr<BlockMemory>> claimed) {
	idx_t handed = 0;
	absl::Cleanup abandon = [&] {
		ReleaseClaims(std::span(claimed).subspan(handed));
	};
	auto &scheduler = TaskScheduler::GetScheduler(db);
	ForEachPiece(claimed.size(), [&](idx_t offset, idx_t count) {
		const auto piece_first = first_block + offset;
		const auto range = std::span(claimed).subspan(offset, count);
		vector<optional_ptr<BlockMemory>> piece_claimed(range.begin(), range.end());
		idx_t bytes = 0;
		for (idx_t i = 0; i < count; i++) {
			if (piece_claimed[i]) {
				bytes += file->BlockBytes(piece_first + i);
			}
		}
		shared_ptr<Task> task =
		    make_shared_ptr<BlockCacheFetchTask>(*this, file, piece_first, std::move(piece_claimed), bytes);
		handed = offset + count;
		file->in_flight.fetch_add(bytes, std::memory_order_relaxed);
		in_flight.fetch_add(bytes, std::memory_order_relaxed);
		{
			annotated_lock_guard<annotated_mutex> guard(fetch_lock);
			pending_tasks++;
			if (stopped) {
				return;
			}
		}
		scheduler.ScheduleTask(Producer(), std::move(task), scheduler.AsyncPool());
	});
}

BlockCachePin BlockCache::CreateBlock(BlockCacheFile &file, idx_t block) {
	D_ASSERT(file.IsUnbacked() || !file.HasSource());
	auto &slot = file.blocks->Get(block);
	if (slot.load(std::memory_order_acquire)) {
		throw InternalException("Block %llu of \"%s\" is written twice", block, file.path);
	}
	auto &memory = MemoryOf(file, slot);
	if (!memory.TryClaim()) {
		throw InternalException("Block %llu of \"%s\" is written twice", block, file.path);
	}
	absl::Cleanup finish = [&] {
		memory.FinishLoad();
	};
	auto buffer = buffer_manager.CompleteLoad(memory, buffer_manager.PrepareLoad(memory, BLOCK_SIZE, 0));
	return BlockCachePin(std::move(buffer), BLOCK_SIZE);
}

void BlockCache::SetUnbackedSize(BlockCacheFile &file, idx_t size) {
	D_ASSERT(file.IsUnbacked());
	file.size.store(size, std::memory_order_release);
}

void BlockCache::SetEnabled(bool enabled_p) {
	if (!enabled.exchange(enabled_p) || enabled_p) {
		return;
	}
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

BlockCacheStatistics BlockCache::GetStatistics() const {
	BlockCacheStatistics result;
	for (auto &info : buffer_manager.GetMemoryUsageInfo()) {
		if (info.tag == MemoryTag::EXTERNAL_FILE_CACHE) {
			result.used = info.size;
			result.evictions = info.evictions;
			result.spilled = info.temporary_writes;
			result.spill_reads = info.temporary_reads;
		}
	}
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
		file.blocks->ForEach([&](idx_t block, BlockCacheSlot &slot) {
			const auto memory = slot.load(std::memory_order_acquire);
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
