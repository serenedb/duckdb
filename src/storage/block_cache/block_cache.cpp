#include "duckdb/storage/block_cache/block_cache.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <cstring>

#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace duckdb {

namespace {

constexpr uint64_t PIN_MASK = (1ULL << 22) - 1;
constexpr uint64_t FLAG_PREFETCHED = 1ULL << 22;
constexpr uint64_t FLAG_LOADING = 1ULL << 23;
constexpr uint64_t FLAG_LOADED = 1ULL << 24;
constexpr uint64_t FLAG_FAILED = 1ULL << 25;
constexpr uint64_t FLAG_EVICTING = 1ULL << 26;
constexpr uint64_t FLAG_PROBATION = 1ULL << 27;
constexpr uint64_t FLAG_HOT = 1ULL << 28;
constexpr uint64_t FLAG_UNBACKED = 1ULL << 29;
constexpr uint64_t FLAG_SPILLED = 1ULL << 30;
constexpr uint64_t FLAG_LOCAL = 1ULL << 31;
constexpr uint64_t VERSION_ONE = 1ULL << 32;
constexpr uint64_t VERSION_MASK = ~(VERSION_ONE - 1);
constexpr uint64_t UNPINNABLE = FLAG_EVICTING | FLAG_FAILED;

constexpr idx_t SAMPLE_SIZE = 8;
constexpr idx_t EVICTION_ATTEMPTS = 64;
constexpr idx_t ACQUIRE_ATTEMPTS = 4;
constexpr idx_t HUGE_PAGE_SIZE = 2ULL * 1024ULL * 1024ULL;
constexpr idx_t MAX_IN_FLIGHT = 16 * BlockCache::MAX_FILE_IN_FLIGHT;
constexpr uintptr_t CACHE_LINE = 64;

uint64_t Pins(uint64_t state) {
	return state & PIN_MASK;
}

//! Whether a block in [state] can be evicted: loaded, unpinned and neither failed nor being evicted already
bool Evictable(uint64_t state) {
	D_ASSERT(!(state & FLAG_LOADED) || !(state & FLAG_LOADING));
	return Pins(state) == 0 && (state & FLAG_LOADED) && !(state & UNPINNABLE);
}

idx_t BlockBytesOf(idx_t file_size, idx_t block) {
	const auto start = block * BlockCache::BLOCK_SIZE;
	return start >= file_size ? 0 : MinValue(BlockCache::BLOCK_SIZE, file_size - start);
}

uint64_t Referenced(uint64_t state) {
	if (state & FLAG_PREFETCHED) {
		return state & ~FLAG_PREFETCHED;
	}
	return (state & ~FLAG_PROBATION) | FLAG_HOT;
}

void Pause() {
#if defined(__x86_64__) || defined(_M_X64)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	asm volatile("yield");
#endif
}

void LockBucket(BlockCacheBucket &bucket) {
	auto version = bucket.version.load(std::memory_order_relaxed);
	while (true) {
		if (!(version & 1) && bucket.version.compare_exchange_weak(version, version + 1, std::memory_order_acquire,
		                                                           std::memory_order_relaxed)) {
			return;
		}
		Pause();
		version = bucket.version.load(std::memory_order_relaxed);
	}
}

void UnlockBucket(BlockCacheBucket &bucket) {
	bucket.version.fetch_add(1, std::memory_order_release);
}

data_ptr_t MapRegion(idx_t size) {
#ifdef _WIN32
	return Allocator::DefaultAllocator().AllocateData(size);
#else
	const auto mapped = mmap(nullptr, size + HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE,
	                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (mapped == MAP_FAILED) {
		throw OutOfMemoryException("Could not reserve %s of address space for the block cache",
		                           StringUtil::BytesToHumanReadableString(size));
	}
	const auto base = reinterpret_cast<uintptr_t>(mapped);
	const auto aligned = AlignValue<uintptr_t, HUGE_PAGE_SIZE>(base);
	if (aligned > base) {
		munmap(mapped, aligned - base);
	}
	const auto end = base + size + HUGE_PAGE_SIZE;
	if (end > aligned + size) {
		munmap(reinterpret_cast<void *>(aligned + size), end - aligned - size);
	}
#ifdef MADV_HUGEPAGE
	madvise(reinterpret_cast<void *>(aligned), size, MADV_HUGEPAGE);
#endif
	return reinterpret_cast<data_ptr_t>(aligned);
#endif
}

void UnmapRegion(data_ptr_t region, idx_t size) {
#ifdef _WIN32
	Allocator::DefaultAllocator().FreeData(region, size);
#else
	munmap(region, size);
#endif
}

void DiscardFrame(data_ptr_t frame, idx_t size) {
#ifndef _WIN32
	madvise(frame, size, MADV_DONTNEED);
#endif
}

} // namespace

//===--------------------------------------------------------------------===//
// BlockCachePin
//===--------------------------------------------------------------------===//

void BlockCachePin::Unpin() {
	cache->Unpin(*entry);
	entry = nullptr;
}

//===--------------------------------------------------------------------===//
// BlockCacheFile
//===--------------------------------------------------------------------===//

BlockCacheFile::BlockCacheFile(BlockCache &cache_p, string path_p, uint32_t number_p, bool unbacked_p,
                               BlockCacheFileInfo info_p, shared_ptr<FileHandle> source_p)
    : cache(cache_p), path(std::move(path_p)), number(number_p), size(info_p.validation.file_size),
      unbacked(unbacked_p), info(std::move(info_p)), source(std::move(source_p)) {
}

BlockCacheFile::~BlockCacheFile() {
	if (unbacked) {
		cache.ReleaseUnbacked(number, BlockCount());
	} else {
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
		const auto offset = size % BlockCache::BLOCK_SIZE;
		if (!current.IsValid()) {
			current = cache.CreateBlock(*file, size / BlockCache::BLOCK_SIZE);
		}
		const auto chunk = MinValue(count, BlockCache::BLOCK_SIZE - offset);
		memcpy(current.GetDataMutable() + offset, data, chunk);
		data += chunk;
		count -= chunk;
		size += chunk;
		cache.SetUnbackedSize(*file, size);
		if (offset + chunk == BlockCache::BLOCK_SIZE) {
			current.Reset();
		}
	}
}

void BlockCacheAppender::Finish() {
	current.Reset();
}

//===--------------------------------------------------------------------===//
// BlockCacheFetchTask
//===--------------------------------------------------------------------===//

class BlockCacheFetchTask : public Task {
public:
	BlockCacheFetchTask(BlockCache &cache_p, shared_ptr<BlockCacheFile> file_p, BlockCache::Run run_p,
	                    vector<optional_ptr<BlockCacheEntry>> entries_p, idx_t bytes_p)
	    : cache(cache_p), file(std::move(file_p)), run(run_p), entries(std::move(entries_p)), bytes(bytes_p) {
	}

	~BlockCacheFetchTask() override {
		if (!done) {
			for (auto &entry : entries) {
				if (entry) {
					cache.FinishLoad(*entry, false);
				}
			}
			Finish();
		}
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		try {
			cache.Fetch(*file, run, entries);
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
		for (auto &entry : entries) {
			if (entry) {
				cache.Unpin(*entry);
			}
		}
		file->in_flight.fetch_sub(bytes, std::memory_order_relaxed);
		cache.FinishFetchTask(bytes);
	}

	BlockCache &cache;
	shared_ptr<BlockCacheFile> file;
	BlockCache::Run run;
	vector<optional_ptr<BlockCacheEntry>> entries;
	idx_t bytes;
	bool done = false;
};

//===--------------------------------------------------------------------===//
// BlockCache
//===--------------------------------------------------------------------===//

BlockCache::BlockCache(DatabaseInstance &db_p, idx_t capacity)
    : db(db_p), buffer_manager(BufferManager::GetBufferManager(db_p)) {
	frame_count = NumericCast<uint32_t>(MaxValue<idx_t>(1, capacity / BLOCK_SIZE));
	region_size = frame_count * BLOCK_SIZE;
	region = MapRegion(region_size);
	frame_owner = unique_ptr<atomic<BlockCacheEntry *>[]>(new atomic<BlockCacheEntry *>[frame_count]);
	for (idx_t i = 0; i < frame_count; i++) {
		frame_owner[i].store(nullptr, std::memory_order_relaxed);
	}
	const idx_t bucket_count = NextPowerOfTwo(2 * idx_t(frame_count));
	buckets = unique_ptr<BlockCacheBucket[]>(new BlockCacheBucket[bucket_count]);
	bucket_mask = NumericCast<uint32_t>(bucket_count - 1);
	pool = unique_ptr<BlockCacheEntry[]>(new BlockCacheEntry[frame_count]);
	free_frames.reserve(frame_count);
	for (uint32_t i = frame_count; i > 0; i--) {
		free_frames.push_back(i - 1);
	}
	capacity_frames = frame_count;
}

BlockCache::~BlockCache() {
	StopFetching();
	buffer_manager.FreeReservedMemory(MemoryTag::EXTERNAL_FILE_CACHE,
	                                  used_frames.load(std::memory_order_relaxed) * BLOCK_SIZE);
	UnmapRegion(region, region_size);
}

BlockCache &BlockCache::Get(DatabaseInstance &db) {
	return db.GetBlockCache();
}

idx_t BlockCache::ConfiguredCapacity(DatabaseInstance &db) {
	const auto configured = db.config.options.external_file_cache_size;
	if (configured != DConstants::INVALID_INDEX) {
		return configured;
	}
	return BufferManager::GetBufferManager(db).GetMaxMemory() / 4;
}

BlockCachePin BlockCache::AllocateUncached(DatabaseInstance &db, idx_t size) {
	BlockCachePin result;
	result.owned = BufferManager::GetBufferManager(db).Allocate(MemoryTag::EXTERNAL_FILE_CACHE, size);
	result.owned.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	result.data = result.owned.GetDataMutable();
	result.size = size;
	return result;
}

uint64_t BlockCache::MakeKey(uint32_t file, idx_t block) {
	D_ASSERT(block <= NumericLimits<uint32_t>::Maximum());
	return (uint64_t(file) << 32) | block;
}

BlockCacheBucket &BlockCache::BucketFor(uint64_t key) const {
	return buckets[MurmurHash64(key) & bucket_mask];
}

data_ptr_t BlockCache::FrameData(uint32_t frame) const {
	return region + idx_t(frame) * BLOCK_SIZE;
}

void BlockCache::CountHit(const BlockCacheEntry &entry) {
	hits[(reinterpret_cast<uintptr_t>(&entry) / CACHE_LINE) % COUNTER_SHARDS].value.fetch_add(
	    1, std::memory_order_relaxed);
}

uint32_t BlockCache::WaitStripe(const BlockCacheEntry &entry) {
	return NumericCast<uint32_t>((reinterpret_cast<uintptr_t>(&entry) / CACHE_LINE) % WAIT_STRIPES);
}

template <class FUNC>
void BlockCache::ForEachEntry(FUNC &&callback) const {
	const auto high = high_water.load(std::memory_order_acquire);
	for (idx_t frame = 0; frame < high; frame++) {
		auto entry = frame_owner[frame].load(std::memory_order_acquire);
		if (entry) {
			callback(*entry);
		}
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
	record = FileRecord {next_file_number++, info, opens};
	return replaced;
}

void BlockCache::Retire(const optional<FileRecord> &replaced, bool sweep) {
	if (replaced) {
		DropBlocks(replaced->number, BlockCountOf(replaced->info.validation.file_size));
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
	Retire(replaced, sweep);
	return make_shared_ptr<BlockCacheFile>(*this, path, opened.number, false, std::move(opened.info),
	                                       std::move(source));
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
	unordered_set<uint32_t> cached;
	ForEachEntry([&](BlockCacheEntry &entry) { cached.insert(FileOf(entry.key.load(std::memory_order_relaxed))); });
	for (auto key : spill.Keys()) {
		cached.insert(FileOf(key));
	}
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	for (auto entry = files.begin(); entry != files.end();) {
		if (entry->second.opens == 0 && cached.find(entry->second.number) == cached.end()) {
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
	DropFile(file);
}

shared_ptr<BlockCacheFile> BlockCache::CreateUnbackedFile(const string &path) {
	uint32_t number;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		number = next_file_number++;
		live_unbacked.insert(number);
	}
	return make_shared_ptr<BlockCacheFile>(*this, path, number, true, BlockCacheFileInfo(), nullptr);
}

shared_ptr<BlockCacheFile> BlockCache::CreateWrittenFile(const string &path) {
	const BlockCacheFileInfo info;
	uint32_t number;
	optional<FileRecord> replaced;
	bool sweep = false;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto &record = files[path];
		replaced = Renumber(record, info, 1, sweep);
		number = record.number;
	}
	Retire(replaced, sweep);
	return make_shared_ptr<BlockCacheFile>(*this, path, number, false, info, nullptr);
}

void BlockCache::FinishWrittenFile(BlockCacheFile &file, idx_t size, bool published) {
	D_ASSERT(!file.IsUnbacked());
	file.size.store(size, std::memory_order_release);
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		auto entry = files.find(file.path);
		if (entry != files.end() && entry->second.number == file.number) {
			if (published) {
				entry->second.info.validation.file_size = size;
			} else {
				files.erase(entry);
			}
		}
	}
	if (!published) {
		DropFile(file);
	}
}

void BlockCache::ReleaseUnbacked(uint32_t number, idx_t block_count) {
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		live_unbacked.erase(number);
	}
	DropBlocks(number, block_count);
}

bool BlockCache::IsLiveUnbacked(uint32_t number) const {
	annotated_lock_guard<annotated_mutex> guard(file_lock);
	return live_unbacked.find(number) != live_unbacked.end();
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
	Retire(forgotten, false);
}

void BlockCache::DropFile(BlockCacheFile &file) {
	DropBlocks(file.number, file.BlockCount());
}

void BlockCache::DropBlocks(uint32_t number, idx_t block_count) {
	spill.EraseFile(number);
	if (block_count > high_water.load(std::memory_order_acquire)) {
		ForEachEntry([&](BlockCacheEntry &entry) {
			const auto key = entry.key.load(std::memory_order_relaxed);
			if (FileOf(key) == number) {
				DropEntry(entry, key, true);
			}
		});
		return;
	}
	for (idx_t block = 0; block < block_count; block++) {
		const auto key = MakeKey(number, block);
		auto entry = FindEntry(key);
		if (entry) {
			DropEntry(*entry, key, true);
		}
	}
}

void BlockCache::DropEntry(BlockCacheEntry &entry, uint64_t key, bool drop_unbacked) {
	auto state = entry.state.load(std::memory_order_acquire);
	while ((state & (FLAG_LOADING | FLAG_LOADED)) && !(state & UNPINNABLE) &&
	       entry.key.load(std::memory_order_relaxed) == key && (drop_unbacked || !(state & FLAG_UNBACKED))) {
		if (Pins(state) == 0) {
			uint32_t frame;
			if (TryEvict(entry, state, frame, false)) {
				ReleaseFrame(frame);
				return;
			}
		} else if (entry.state.compare_exchange_weak(state, state | FLAG_FAILED, std::memory_order_acq_rel,
		                                             std::memory_order_acquire)) {
			return;
		}
		state = entry.state.load(std::memory_order_acquire);
	}
}

BlockCacheEntry *BlockCache::FindEntry(uint64_t key) const {
	uint64_t state = 0;
	return FindEntry(key, state);
}

BlockCacheEntry *BlockCache::FindEntry(uint64_t key, uint64_t &state) const {
	auto &bucket = BucketFor(key);
	while (true) {
		const auto version = bucket.version.load(std::memory_order_acquire);
		if (version & 1) {
			Pause();
			continue;
		}
		bool consistent = true;
		auto entry = &bucket.first;
		for (idx_t steps = 0;; steps++) {
			state = entry->state.load(std::memory_order_acquire);
			if (entry->key.load(std::memory_order_relaxed) == key && !(state & UNPINNABLE)) {
				return entry;
			}
			const auto next = entry->next.load(std::memory_order_acquire);
			if (next == 0) {
				break;
			}
			if (steps > frame_count) {
				consistent = false;
				break;
			}
			entry = &pool[next - 1];
		}
		if (!consistent) {
			continue;
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (bucket.version.load(std::memory_order_relaxed) == version) {
			return nullptr;
		}
	}
}

bool BlockCache::PinPresent(const BlockCacheFile &file, idx_t block, BlockCachePin &result) {
	D_ASSERT(!result.IsValid());
	const auto key = MakeKey(file.number, block);
	while (true) {
		uint64_t state = 0;
		auto found = FindEntry(key, state);
		if (!found) {
			return false;
		}
		if (Pins(state) == PIN_MASK) {
			throw InternalException("Too many pins of one block cache block");
		}
		auto desired = state + 1;
		if (state & FLAG_LOADED) {
			desired = Referenced(desired);
		}
		if (!found->state.compare_exchange_weak(state, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			continue;
		}
		result.cache = this;
		result.entry = found;
		result.size = file.BlockBytes(block);
		if (desired & FLAG_LOADED) {
			result.data = FrameData(found->frame.load(std::memory_order_relaxed));
			CountHit(*found);
		}
		return true;
	}
}

bool BlockCache::AwaitLoad(BlockCachePin &pin) {
	auto &entry = *pin.entry;
	WaitForLoad(entry);
	auto state = entry.state.load(std::memory_order_acquire);
	if (!(state & FLAG_LOADED)) {
		pin.Reset();
		return false;
	}
	while (!(state & (FLAG_HOT | UNPINNABLE)) &&
	       !entry.state.compare_exchange_weak(state, Referenced(state), std::memory_order_acq_rel,
	                                          std::memory_order_acquire)) {
	}
	pin.data = FrameData(entry.frame.load(std::memory_order_relaxed));
	CountHit(entry);
	return true;
}

bool BlockCache::Lookup(const BlockCacheFile &file, idx_t block, BlockCachePin &result, bool wait_for_load) {
	while (PinPresent(file, block, result)) {
		if (result.data) {
			return true;
		}
		if (!wait_for_load) {
			result.Reset();
			return false;
		}
		if (AwaitLoad(result)) {
			return true;
		}
	}
	return false;
}

bool BlockCache::IsCached(BlockCacheFile &file, idx_t offset, idx_t size) const {
	const auto file_size = file.GetSize();
	if (size == 0 || offset >= file_size) {
		return true;
	}
	const auto last = (MinValue(offset + size, file_size) - 1) / BLOCK_SIZE;
	for (auto block = offset / BLOCK_SIZE; block <= last; block++) {
		if (!FindEntry(MakeKey(file.number, block))) {
			return false;
		}
	}
	return true;
}

uint64_t BlockCache::FileFlags(const BlockCacheFile &file) {
	if (file.unbacked) {
		return FLAG_UNBACKED;
	}
	return file.info.on_disk ? FLAG_LOCAL : 0;
}

optional_ptr<BlockCacheEntry> BlockCache::InsertLoading(uint64_t key, uint32_t frame, uint64_t flags) {
	auto &bucket = BucketFor(key);
	LockBucket(bucket);
	for (auto entry = &bucket.first;;) {
		if (entry->key.load(std::memory_order_relaxed) == key &&
		    !(entry->state.load(std::memory_order_relaxed) & UNPINNABLE)) {
			UnlockBucket(bucket);
			return nullptr;
		}
		const auto next = entry->next.load(std::memory_order_relaxed);
		if (next == 0) {
			break;
		}
		entry = &pool[next - 1];
	}
	BlockCacheEntry *slot;
	if (bucket.first.key.load(std::memory_order_relaxed) == 0) {
		slot = &bucket.first;
	} else {
		slot = &pool[frame];
		D_ASSERT(slot->key.load(std::memory_order_relaxed) == 0);
		slot->next.store(bucket.first.next.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}
	slot->key.store(key, std::memory_order_relaxed);
	slot->frame.store(frame, std::memory_order_relaxed);
	const auto version = (slot->state.load(std::memory_order_relaxed) & VERSION_MASK) + VERSION_ONE;
	slot->state.store(version | FLAG_LOADING | FLAG_PROBATION | flags | 1, std::memory_order_release);
	if (slot != &bucket.first) {
		bucket.first.next.store(frame + 1, std::memory_order_release);
	}
	frame_owner[frame].store(slot, std::memory_order_release);
	UnlockBucket(bucket);
	return slot;
}

void BlockCache::Unpin(BlockCacheEntry &entry) {
	const auto previous = entry.state.fetch_sub(1, std::memory_order_acq_rel);
	D_ASSERT(Pins(previous) > 0);
	if (Pins(previous) == 1 && (previous & FLAG_FAILED)) {
		const auto frame = entry.frame.load(std::memory_order_relaxed);
		frame_owner[frame].store(nullptr, std::memory_order_relaxed);
		Unlink(entry);
		ReleaseFrame(frame);
	}
}

void BlockCache::Unlink(BlockCacheEntry &entry) {
	auto &bucket = BucketFor(entry.key.load(std::memory_order_relaxed));
	LockBucket(bucket);
	entry.key.store(0, std::memory_order_relaxed);
	if (&entry != &bucket.first) {
		const auto index = NumericCast<uint32_t>(&entry - pool.get());
		D_ASSERT(index == entry.frame.load(std::memory_order_relaxed));
		auto previous = &bucket.first;
		while (previous->next.load(std::memory_order_relaxed) != index + 1) {
			previous = &pool[previous->next.load(std::memory_order_relaxed) - 1];
		}
		previous->next.store(entry.next.load(std::memory_order_relaxed), std::memory_order_release);
	}
	entry.state.store(entry.state.load(std::memory_order_relaxed) & VERSION_MASK, std::memory_order_release);
	UnlockBucket(bucket);
}

bool BlockCache::TakeFreeFrame(uint32_t &frame) {
	{
		annotated_lock_guard<annotated_mutex> guard(frame_lock);
		if (free_frames.empty()) {
			return false;
		}
		frame = free_frames.back();
		free_frames.pop_back();
	}
	used_frames.fetch_add(1, std::memory_order_relaxed);
	absl::Cleanup give_back = [&] {
		used_frames.fetch_sub(1, std::memory_order_relaxed);
		annotated_lock_guard<annotated_mutex> guard(frame_lock);
		free_frames.push_back(frame);
	};
	if (!buffer_manager.TryReserveMemory(MemoryTag::EXTERNAL_FILE_CACHE, BLOCK_SIZE)) {
		return false;
	}
	std::move(give_back).Cancel();
	auto high = high_water.load(std::memory_order_relaxed);
	while (high <= frame && !high_water.compare_exchange_weak(high, frame + 1, std::memory_order_relaxed)) {
	}
	return true;
}

bool BlockCache::AcquireFrame(uint32_t &frame) {
	for (idx_t attempt = 0; attempt < ACQUIRE_ATTEMPTS; attempt++) {
		if (used_frames.load(std::memory_order_relaxed) < capacity_frames.load(std::memory_order_relaxed) &&
		    TakeFreeFrame(frame)) {
			return true;
		}
		if (EvictOne(frame)) {
			return true;
		}
	}
	return TakeFreeFrame(frame);
}

void BlockCache::ReleaseFrame(uint32_t frame) {
	DiscardFrame(FrameData(frame), BLOCK_SIZE);
	{
		annotated_lock_guard<annotated_mutex> guard(frame_lock);
		free_frames.push_back(frame);
	}
	used_frames.fetch_sub(1, std::memory_order_relaxed);
	buffer_manager.FreeReservedMemory(MemoryTag::EXTERNAL_FILE_CACHE, BLOCK_SIZE);
}

bool BlockCache::EvictOne(uint32_t &frame) {
	const auto high = high_water.load(std::memory_order_relaxed);
	if (high == 0) {
		return false;
	}
	auto seed = MurmurHash64(sampler_seed.fetch_add(1, std::memory_order_relaxed) + 1);
	for (idx_t attempt = 0; attempt < EVICTION_ATTEMPTS; attempt++) {
		BlockCacheEntry *victim = nullptr;
		uint64_t victim_state = 0;
		for (idx_t i = 0; i < SAMPLE_SIZE; i++) {
			seed = MurmurHash64(seed + i + 1);
			auto candidate = frame_owner[seed % high].load(std::memory_order_acquire);
			if (!candidate) {
				continue;
			}
			auto state = candidate->state.load(std::memory_order_acquire);
			if (!Evictable(state)) {
				continue;
			}
			if ((state & FLAG_UNBACKED) && !(state & FLAG_SPILLED) && !CanSpillUnbacked() &&
			    IsLiveUnbacked(FileOf(candidate->key.load(std::memory_order_relaxed)))) {
				continue;
			}
			if (state & FLAG_PROBATION) {
				victim = candidate;
				victim_state = state;
				break;
			}
			if (state & FLAG_HOT) {
				candidate->state.compare_exchange_weak(state, state & ~FLAG_HOT, std::memory_order_relaxed);
				continue;
			}
			if (!victim) {
				victim = candidate;
				victim_state = state;
			}
		}
		if (victim && TryEvict(*victim, victim_state, frame, true)) {
			return true;
		}
	}
	return false;
}

bool BlockCache::TryEvict(BlockCacheEntry &entry, uint64_t state, uint32_t &frame, bool keep_copy) {
	if (!Evictable(state)) {
		return false;
	}
	if (!entry.state.compare_exchange_strong(state, state | FLAG_EVICTING, std::memory_order_acq_rel,
	                                         std::memory_order_relaxed)) {
		return false;
	}
	const auto key = entry.key.load(std::memory_order_relaxed);
	frame = entry.frame.load(std::memory_order_relaxed);
	if (keep_copy && (state & FLAG_UNBACKED) && !IsLiveUnbacked(FileOf(key))) {
		keep_copy = false;
		spill.Erase(key);
	}
	if (keep_copy && !(state & (FLAG_SPILLED | FLAG_LOCAL))) {
		const bool unbacked = state & FLAG_UNBACKED;
		const bool written = spill.Write(key, FrameData(frame), unbacked);
		if (unbacked && !written) {
			entry.state.fetch_and(~FLAG_EVICTING, std::memory_order_release);
			return false;
		}
		if (written) {
			spilled.fetch_add(1, std::memory_order_relaxed);
		}
	}
	frame_owner[frame].store(nullptr, std::memory_order_relaxed);
	Unlink(entry);
	evictions.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void BlockCache::FinishLoad(BlockCacheEntry &entry, bool shared) {
	const auto outcome = shared ? FLAG_LOADED : FLAG_FAILED;
	auto state = entry.state.load(std::memory_order_relaxed);
	while (!entry.state.compare_exchange_weak(state, (state & ~FLAG_LOADING) | outcome, std::memory_order_acq_rel,
	                                          std::memory_order_relaxed)) {
	}
	auto &lock = wait_locks[WaitStripe(entry)];
	lock.Lock();
	lock.Unlock();
}

void BlockCache::WaitForLoad(BlockCacheEntry &entry) {
	while (TaskScheduler::GetScheduler(db).NumberOfThreads() == 0 &&
	       entry.state.load(std::memory_order_acquire) & FLAG_LOADING) {
		shared_ptr<Task> task;
		if (!TakeFetchTask(task)) {
			break;
		}
		task->Execute(TaskExecutionMode::PROCESS_ALL);
	}
	auto &lock = wait_locks[WaitStripe(entry)];
	TaskScheduler::BlockingScope blocking;
	lock.LockWhen(absl::Condition(
	    +[](BlockCacheEntry *waited) { return !(waited->state.load(std::memory_order_acquire) & FLAG_LOADING); },
	    &entry));
	lock.Unlock();
}

bool BlockCache::CanSpillUnbacked() {
	if (unbacked_spill_ready.load(std::memory_order_acquire)) {
		return true;
	}
	if (!spill.IsConfigured()) {
		auto &buffer_manager = BufferManager::GetBufferManager(db);
		if (!buffer_manager.HasTemporaryDirectory()) {
			return false;
		}
		auto &fs = FileSystem::GetFileSystem(db);
		const auto &directory = buffer_manager.GetTemporaryDirectory();
		try {
			if (!fs.DirectoryExists(directory)) {
				fs.CreateDirectory(directory);
			}
			spill.ConfigureDefault(fs, fs.JoinPath(directory, "duckdb_block_cache.spill"));
		} catch (std::exception &) { // NOLINT
			return false;
		}
	}
	unbacked_spill_ready.store(true, std::memory_order_release);
	return true;
}

void BlockCache::Clear() {
	spill.EraseBacked();
	ForEachEntry([&](BlockCacheEntry &entry) { DropEntry(entry, entry.key.load(std::memory_order_relaxed), false); });
}

void BlockCache::SetEnabled(bool enabled_p) {
	if (enabled.exchange(enabled_p) && !enabled_p) {
		Clear();
	}
}

bool BlockCache::ReadSpilled(uint64_t key, BlockCacheEntry &entry) {
	if (!spill.Read(key, FrameData(entry.frame.load(std::memory_order_relaxed)))) {
		return false;
	}
	entry.state.fetch_or(FLAG_SPILLED, std::memory_order_relaxed);
	spill_reads.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void BlockCache::Fetch(BlockCacheFile &file, const Run &run, vector<optional_ptr<BlockCacheEntry>> &entries) {
	D_ASSERT(entries.size() == run.block_count);
	vector<bool> pending(entries.size(), false);
	idx_t first_pending = entries.size();
	idx_t last_pending = 0;
	bool shareable = true;
	try {
		TaskScheduler::BlockingScope blocking;
		for (idx_t i = 0; i < entries.size(); i++) {
			if (!entries[i]) {
				continue;
			}
			const auto block = run.first_block + i;
			if (ReadSpilled(MakeKey(file.number, block), *entries[i])) {
				FinishLoad(*entries[i], true);
				continue;
			}
			pending[i] = true;
			first_pending = MinValue(first_pending, i);
			last_pending = i;
		}
		if (first_pending < entries.size()) {
			if (file.IsUnbacked()) {
				throw InternalException("A block of the in-memory file \"%s\" is missing from the block cache",
				                        file.path);
			}
			const auto start = (run.first_block + first_pending) * BLOCK_SIZE;
			const auto end = MinValue(file.GetSize(), (run.first_block + last_pending + 1) * BLOCK_SIZE);
			auto source = file.Source();
			if (first_pending == last_pending) {
				source->Read(FrameData(entries[first_pending]->frame.load(std::memory_order_relaxed)), end - start,
				             start);
			} else {
				auto buffer = Allocator::DefaultAllocator().Allocate(end - start);
				source->Read(buffer.get(), end - start, start);
				for (idx_t i = first_pending; i <= last_pending; i++) {
					if (pending[i]) {
						const auto block_bytes = file.BlockBytes(run.first_block + i);
						memcpy(FrameData(entries[i]->frame.load(std::memory_order_relaxed)),
						       buffer.get() + (i - first_pending) * BLOCK_SIZE, block_bytes);
					}
				}
			}
			const auto valid_until = source->file_system.GetCacheValidUntil(*source);
			shareable = !valid_until || *valid_until != timestamp_t::ninfinity();
			fetches.fetch_add(1, std::memory_order_relaxed);
			fetched_bytes.fetch_add(end - start, std::memory_order_relaxed);
		}
	} catch (...) {
		for (auto &entry : entries) {
			if (entry && (entry->state.load(std::memory_order_relaxed) & FLAG_LOADING)) {
				FinishLoad(*entry, false);
			}
		}
		throw;
	}
	for (idx_t i = 0; i < entries.size(); i++) {
		if (!pending[i]) {
			continue;
		}
		FinishLoad(*entries[i], shareable);
	}
}

BlockCachePin BlockCache::Pin(const shared_ptr<BlockCacheFile> &file_p, idx_t block) {
	auto &file = *file_p;
	if (block >= file.BlockCount()) {
		throw InternalException("Block %llu of \"%s\" lies past its end", block, file.path);
	}
	const auto key = MakeKey(file.number, block);
	while (true) {
		BlockCachePin result;
		if (Lookup(file, block, result, true)) {
			return result;
		}
		uint32_t frame;
		if (!AcquireFrame(frame)) {
			return ReadPrivate(file, block);
		}
		auto entry = InsertLoading(key, frame, FileFlags(file));
		if (!entry) {
			ReleaseFrame(frame);
			continue;
		}
		misses.fetch_add(1, std::memory_order_relaxed);
		vector<optional_ptr<BlockCacheEntry>> entries {entry};
		try {
			Fetch(file, Run {block, 1}, entries);
		} catch (...) {
			Unpin(*entry);
			throw;
		}
		return BlockCachePin(*this, *entry, FrameData(frame), file.BlockBytes(block));
	}
}

BlockCachePin BlockCache::ReadPrivate(BlockCacheFile &file, idx_t block) {
	const auto nr_bytes = file.BlockBytes(block);
	auto result = AllocateUncached(db, BLOCK_SIZE);
	if (!spill.Read(MakeKey(file.number, block), result.GetDataMutable())) {
		if (file.IsUnbacked()) {
			throw InternalException("A block of the in-memory file \"%s\" is missing from the block cache", file.path);
		}
		TaskScheduler::BlockingScope blocking;
		file.Source()->Read(result.GetDataMutable(), nr_bytes, block * BLOCK_SIZE);
		fetches.fetch_add(1, std::memory_order_relaxed);
		fetched_bytes.fetch_add(nr_bytes, std::memory_order_relaxed);
	}
	misses.fetch_add(1, std::memory_order_relaxed);
	result.size = nr_bytes;
	return result;
}

template <class MISSING, class ADMIT>
BlockCache::RunPlan BlockCache::PlanRun(BlockCacheFile &file, idx_t block, idx_t end, uint64_t flags, MISSING &&missing,
                                        ADMIT &&admit) {
	RunPlan plan {Run {block, 0}, {}, 0, block, false};
	plan.entries.reserve(MAX_REQUEST / BLOCK_SIZE);
	absl::Cleanup abandon = [&] {
		for (auto &entry : plan.entries) {
			if (entry) {
				FinishLoad(*entry, false);
				Unpin(*entry);
			}
		}
	};
	idx_t gap = 0;
	for (; plan.next < end && (plan.run.block_count + 1) * BLOCK_SIZE <= MAX_REQUEST; plan.next++) {
		if (missing(plan.next)) {
			const auto block_bytes = file.BlockBytes(plan.next);
			uint32_t frame;
			if (!admit(plan.bytes + block_bytes) || !AcquireFrame(frame)) {
				plan.exhausted = true;
				break;
			}
			auto entry = InsertLoading(MakeKey(file.number, plan.next), frame, flags);
			if (entry) {
				plan.entries.push_back(entry);
				plan.run.block_count++;
				plan.bytes += block_bytes;
				gap = 0;
				misses.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			ReleaseFrame(frame);
		}
		if ((gap + 1) * BLOCK_SIZE > MERGE_GAP) {
			break;
		}
		gap++;
		plan.entries.push_back(nullptr);
		plan.run.block_count++;
	}
	while (!plan.entries.empty() && !plan.entries.back()) {
		plan.entries.pop_back();
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
	for (idx_t i = 0; i < count; i++) {
		PinPresent(file, first + i, result[i]);
	}
	const auto missing = [&](idx_t block) {
		return !result[block - first].IsValid();
	};
	for (auto block = first; block < first + count;) {
		if (!missing(block)) {
			block++;
			continue;
		}
		auto plan = PlanRun(file, block, first + count, FileFlags(file), missing, [](idx_t) { return true; });
		if (plan.entries.empty()) {
			result[block - first] = Pin(file_p, block);
			block++;
			continue;
		}
		for (idx_t i = 0; i < plan.run.block_count; i++) {
			if (plan.entries[i]) {
				result[plan.run.first_block - first + i] =
				    BlockCachePin(*this, *plan.entries[i], nullptr, file.BlockBytes(plan.run.first_block + i));
			}
		}
		Fetch(file, plan.run, plan.entries);
		for (idx_t i = 0; i < plan.run.block_count; i++) {
			if (plan.entries[i]) {
				result[plan.run.first_block - first + i].data =
				    FrameData(plan.entries[i]->frame.load(std::memory_order_relaxed));
			}
		}
		block = plan.next;
	}
	for (idx_t i = 0; i < count; i++) {
		auto &pin = result[i];
		if (!pin.data && !(pin.IsValid() && AwaitLoad(pin))) {
			pin = Pin(file_p, first + i);
		}
	}
	return result;
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
		BlockCachePin pin;
		if (!Lookup(file, block, pin, false)) {
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

void BlockCache::Prefetch(const shared_ptr<BlockCacheFile> &file_p, idx_t offset, idx_t size) {
	auto &file = *file_p;
	const auto file_size = file.GetSize();
	if (file.IsUnbacked() || size == 0 || offset >= file_size) {
		return;
	}
	size = MinValue(size, file_size - offset);
	const auto end = (offset + size - 1) / BLOCK_SIZE + 1;
	const auto missing = [&](idx_t block) {
		return !FindEntry(MakeKey(file.number, block));
	};
	const auto admit = [&](idx_t bytes) {
		return file.in_flight.load(std::memory_order_relaxed) + bytes <= MAX_FILE_IN_FLIGHT &&
		       in_flight.load(std::memory_order_relaxed) + bytes <= MAX_IN_FLIGHT;
	};
	for (auto block = offset / BLOCK_SIZE; block < end;) {
		if (!missing(block)) {
			block++;
			continue;
		}
		auto plan = PlanRun(file, block, end, FileFlags(file) | FLAG_PREFETCHED, missing, admit);
		if (!plan.entries.empty()) {
			ScheduleFetch(file_p, plan.run, std::move(plan.entries), plan.bytes);
		}
		if (plan.exhausted) {
			return;
		}
		block = plan.next;
	}
}

void BlockCache::ScheduleFetch(const shared_ptr<BlockCacheFile> &file, const Run &run,
                               vector<optional_ptr<BlockCacheEntry>> entries, idx_t bytes) {
	shared_ptr<Task> task;
	{
		absl::Cleanup abandon = [&] {
			for (auto &entry : entries) {
				if (entry) {
					FinishLoad(*entry, false);
					Unpin(*entry);
				}
			}
		};
		task = make_shared_ptr<BlockCacheFetchTask>(*this, file, run, std::move(entries), bytes);
		std::move(abandon).Cancel();
	}
	file->in_flight.fetch_add(bytes, std::memory_order_relaxed);
	in_flight.fetch_add(bytes, std::memory_order_relaxed);
	{
		annotated_lock_guard<annotated_mutex> guard(fetch_lock);
		pending_tasks++;
		if (stopped) {
			return;
		}
	}
	auto &scheduler = TaskScheduler::GetScheduler(db);
	scheduler.ScheduleTask(Producer(), std::move(task), scheduler.AsyncPool());
}

BlockCachePin BlockCache::CreateBlock(BlockCacheFile &file, idx_t block) {
	D_ASSERT(file.IsUnbacked() || !file.HasSource());
	const auto key = MakeKey(file.number, block);
	uint32_t frame;
	if (!AcquireFrame(frame)) {
		throw OutOfMemoryException("The block cache has no free frame for a block of \"%s\"", file.path);
	}
	auto entry = InsertLoading(key, frame, FileFlags(file));
	if (!entry) {
		ReleaseFrame(frame);
		throw InternalException("Block %llu of \"%s\" is written twice", block, file.path);
	}
	memset(FrameData(frame), 0, BLOCK_SIZE);
	FinishLoad(*entry, true);
	return BlockCachePin(*this, *entry, FrameData(frame), BLOCK_SIZE);
}

void BlockCache::SetUnbackedSize(BlockCacheFile &file, idx_t size) {
	D_ASSERT(file.IsUnbacked());
	file.size.store(size, std::memory_order_release);
}

idx_t BlockCache::EvictToReduceMemory(idx_t target) {
	idx_t freed = 0;
	while (freed < target) {
		uint32_t frame;
		if (!EvictOne(frame)) {
			break;
		}
		ReleaseFrame(frame);
		freed += BLOCK_SIZE;
	}
	return freed;
}

void BlockCache::SetCapacity(idx_t capacity) {
	const auto frames = MinValue<idx_t>(frame_count, capacity / BLOCK_SIZE);
	capacity_frames.store(frames, std::memory_order_relaxed);
	const auto used = used_frames.load(std::memory_order_relaxed);
	if (used > frames) {
		EvictToReduceMemory((used - frames) * BLOCK_SIZE);
	}
}

void BlockCache::SetSpill(const string &path, idx_t size) {
	spill.Configure(FileSystem::GetFileSystem(db), path, size);
	unbacked_spill_ready.store(spill.IsConfigured(), std::memory_order_release);
}

BlockCacheStatistics BlockCache::GetStatistics() const {
	BlockCacheStatistics result;
	result.capacity = capacity_frames.load(std::memory_order_relaxed) * BLOCK_SIZE;
	result.used = used_frames.load(std::memory_order_relaxed) * BLOCK_SIZE;
	for (auto &shard : hits) {
		result.hits += shard.value.load(std::memory_order_relaxed);
	}
	result.misses = misses.load(std::memory_order_relaxed);
	result.fetched_bytes = fetched_bytes.load(std::memory_order_relaxed);
	result.fetches = fetches.load(std::memory_order_relaxed);
	result.evictions = evictions.load(std::memory_order_relaxed);
	result.spilled = spilled.load(std::memory_order_relaxed);
	result.spill_reads = spill_reads.load(std::memory_order_relaxed);
	return result;
}

vector<CachedFileInformation> BlockCache::GetBlockInformation() const {
	unordered_map<uint32_t, pair<string, idx_t>> paths;
	{
		annotated_lock_guard<annotated_mutex> guard(file_lock);
		for (auto &file : files) {
			paths[file.second.number] = make_pair(file.first, file.second.info.validation.file_size);
		}
	}
	vector<CachedFileInformation> result;
	unordered_set<uint64_t> in_memory;
	ForEachEntry([&](BlockCacheEntry &entry) {
		const auto state = entry.state.load(std::memory_order_acquire);
		const auto key = entry.key.load(std::memory_order_relaxed);
		if (!(state & FLAG_LOADED) || key == 0) {
			return;
		}
		auto path = paths.find(FileOf(key));
		if (path == paths.end()) {
			return;
		}
		in_memory.insert(key);
		result.push_back({path->second.first, BlockBytesOf(path->second.second, BlockOf(key)),
		                  BlockOf(key) * BLOCK_SIZE, true, spill.Contains(key)});
	});
	for (auto key : spill.Keys()) {
		auto path = paths.find(FileOf(key));
		if (path == paths.end() || in_memory.find(key) != in_memory.end()) {
			continue;
		}
		result.push_back({path->second.first, BlockBytesOf(path->second.second, BlockOf(key)),
		                  BlockOf(key) * BLOCK_SIZE, false, true});
	}
	return result;
}

} // namespace duckdb
