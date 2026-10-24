//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/block_cache/block_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/block_cache/block_cache_spill.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer/temporary_file_information.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/cache_validation.hpp"

#include <span>

namespace duckdb {

class BlockCache;

//! What the cache knows of a file without opening it
struct BlockCacheFileInfo {
	CacheValidationInfo validation;
	bool can_seek = true;
	//! A file on local disk, read again as cheaply as a spilled block, so its blocks never spill
	bool on_disk = false;
};

//! One slot of the translation table: the key of a cached block and the header of the frame holding it
struct BlockCacheEntry {
	//! Version (high 32 bits), flags (10 bits) and pin count (low 22 bits)
	atomic<uint64_t> state {0};
	//! File number (high 32 bits) and block number (low 32 bits); zero when the slot is free
	atomic<uint64_t> key {0};
	//! The frame holding the block
	atomic<uint32_t> frame {0};
	//! The next entry of the bucket's chain: its overflow pool index plus one, zero at the end
	atomic<uint32_t> next {0};
};

//! A bucket of the translation table: a version that is odd while the chain changes, and the chain's first entry
struct alignas(64) BlockCacheBucket {
	atomic<uint64_t> version {0};
	BlockCacheEntry first;
};

//! A block held in the cache, or bytes read into a buffer the pin owns: they stay valid and unchanged while the pin
//! lives
class BlockCachePin {
public:
	BlockCachePin() = default;
	BlockCachePin(BlockCache &cache_p, BlockCacheEntry &entry_p, data_ptr_t data_p, idx_t size_p)
	    : cache(&cache_p), entry(&entry_p), data(data_p), size(size_p) {
	}
	~BlockCachePin() {
		Reset();
	}
	BlockCachePin(BlockCachePin &&other) noexcept
	    : cache(other.cache), entry(other.entry), data(other.data), size(other.size), owned(std::move(other.owned)) {
		other.entry = nullptr;
		other.data = nullptr;
		other.size = 0;
	}
	BlockCachePin &operator=(BlockCachePin &&other) noexcept {
		if (this != &other) {
			Reset();
			cache = other.cache;
			entry = other.entry;
			data = other.data;
			size = other.size;
			owned = std::move(other.owned);
			other.entry = nullptr;
			other.data = nullptr;
			other.size = 0;
		}
		return *this;
	}
	BlockCachePin(const BlockCachePin &) = delete;
	BlockCachePin &operator=(const BlockCachePin &) = delete;

	bool IsValid() const {
		return entry || owned.IsValid();
	}
	const_data_ptr_t Ptr() const {
		return data;
	}
	//! Writable bytes of a block its writer created, which only that writer pins
	data_ptr_t GetDataMutable() const {
		return data;
	}
	idx_t Size() const {
		return size;
	}
	void Reset() {
		if (entry) {
			Unpin();
		}
		owned.Destroy();
		data = nullptr;
		size = 0;
	}

private:
	friend class BlockCache;

	void Unpin();

	optional_ptr<BlockCache> cache;
	optional_ptr<BlockCacheEntry> entry;
	data_ptr_t data = nullptr;
	idx_t size = 0;
	BufferHandle owned;
};

//! An open file whose blocks the cache holds; opens of the same path and version share one file number
class BlockCacheFile {
public:
	BlockCacheFile(BlockCache &cache, string path, uint32_t number, bool unbacked, BlockCacheFileInfo info,
	               shared_ptr<FileHandle> source);
	//! The blocks of an unbacked file go with its last reference
	~BlockCacheFile();

	const string &GetPath() const {
		return path;
	}
	idx_t GetSize() const {
		return size.load(std::memory_order_acquire);
	}
	bool IsUnbacked() const {
		return unbacked;
	}
	//! The metadata the blocks belong to, as of this open
	const BlockCacheFileInfo &Info() const {
		return info;
	}
	idx_t BlockCount() const;
	//! Bytes of block [block] that lie inside the file
	idx_t BlockBytes(idx_t block) const;
	//! Whether missing blocks can be fetched; a file opened without a source gets one before its first miss
	bool HasSource() const;
	//! Sets where missing blocks are read from, unless the file has a source already
	void SetSource(shared_ptr<FileHandle> handle);

private:
	friend class BlockCache;
	friend class BlockCacheFetchTask;

	shared_ptr<FileHandle> Source() const;

	BlockCache &cache;
	const string path;
	const uint32_t number;
	atomic<idx_t> size;
	const bool unbacked;
	const BlockCacheFileInfo info;
	mutable annotated_mutex source_lock;
	shared_ptr<FileHandle> source DUCKDB_GUARDED_BY(source_lock);
	//! Bytes this file is fetching right now
	atomic<idx_t> in_flight {0};
};

//! Writes an unbacked or written file from start to end: it creates each block as the write reaches it and pins the
//! block it fills until the block is full or the write ends
class BlockCacheAppender {
public:
	BlockCacheAppender(BlockCache &cache, shared_ptr<BlockCacheFile> file);

	//! Appends [count] bytes of [data]; readers of an unbacked file see them once they are appended
	void Append(const_data_ptr_t data, idx_t count);
	//! The bytes appended
	idx_t Size() const {
		return size;
	}
	//! Ends the write, zeroing the rest of the last block; a written file is published at the size appended
	void Finish();
	//! Ends the write of a written file and drops its blocks
	void Abandon();

private:
	BlockCache &cache;
	shared_ptr<BlockCacheFile> file;
	BlockCachePin current;
	idx_t size = 0;
};

struct BlockCacheStatistics {
	idx_t capacity = 0;
	idx_t used = 0;
	idx_t hits = 0;
	idx_t misses = 0;
	idx_t fetched_bytes = 0;
	idx_t fetches = 0;
	idx_t evictions = 0;
	idx_t spilled = 0;
	idx_t spill_reads = 0;
};

//! Fixed-size blocks of remote and in-memory files, shared by every reader of a database instance. A lookup writes
//! nothing and a pin is one compare-and-swap; frames come from a region on huge pages whose memory is reserved from
//! the buffer pool, which can take it back; eviction samples frames and keeps blocks read twice over blocks read once.
class BlockCache {
public:
	static constexpr idx_t BLOCK_SIZE = 256ULL * 1024ULL;
	//! Missing runs closer than this are fetched by one request
	static constexpr idx_t MERGE_GAP = 1024ULL * 1024ULL;
	//! The largest request
	static constexpr idx_t MAX_REQUEST = 16ULL * 1024ULL * 1024ULL;
	//! Bytes one file may have in flight, which bounds hints tuned for the page cache
	static constexpr idx_t MAX_FILE_IN_FLIGHT = 64ULL * 1024ULL * 1024ULL;

	static idx_t BlockCountOf(idx_t size) {
		return (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	}
	//! The key of block [block] of the file numbered [file]; the keys of a file are one range, in block order
	static uint64_t MakeKey(uint32_t file, idx_t block);
	static uint32_t FileOf(uint64_t key) {
		return static_cast<uint32_t>(key >> 32);
	}
	static idx_t BlockOf(uint64_t key) {
		return key & 0xFFFFFFFFULL;
	}

public:
	BlockCache(DatabaseInstance &db, idx_t capacity);
	~BlockCache();

	static BlockCache &Get(DatabaseInstance &db);
	//! The capacity the settings ask for: external_file_cache_size, or a quarter of the memory limit
	static idx_t ConfiguredCapacity(DatabaseInstance &db);
	//! A pin owning a new buffer of [size] bytes for a read the cache does not keep, aligned for direct I/O such as a
	//! read from the spill tier
	static BlockCachePin AllocateUncached(DatabaseInstance &db, idx_t size);

	//! Registers an open of [path] whose metadata is [current], reading missing blocks from [source], which may be
	//! set later. It shares the blocks of an earlier open that [current] validates; without [validate] it shares them
	//! while they are fresh, and [current] may then be the earlier open's metadata.
	shared_ptr<BlockCacheFile> OpenFile(const string &path, const BlockCacheFileInfo &current, bool validate,
	                                    shared_ptr<FileHandle> source);
	//! The metadata of the fresh blocks of an earlier open of [path], when there are any
	bool TryGetFile(const string &path, BlockCacheFileInfo &result);
	//! Stops sharing the blocks of [file] once a response forbids reusing them
	void ForbidReuse(BlockCacheFile &file);
	//! A file whose blocks exist only in the cache and its spill tier
	shared_ptr<BlockCacheFile> CreateUnbackedFile(const string &path);
	//! A file the caller writes under [path], replacing earlier opens of [path]: its writer creates the blocks, and
	//! once FinishWrittenFile publishes it, opens of [path] share them like fetched blocks
	shared_ptr<BlockCacheFile> CreateWrittenFile(const string &path);
	//! Ends the write of [file] at [size] bytes; a published file keeps its blocks, an abandoned one drops them
	void FinishWrittenFile(BlockCacheFile &file, idx_t size, bool published);
	//! Drops the blocks of every open of [path]; current pins keep their bytes
	void ForgetFile(const string &path);

	//! Pins block [block] of [file], fetching it when missing; throws when the fetch fails
	BlockCachePin Pin(const shared_ptr<BlockCacheFile> &file, idx_t block);
	//! Pins the blocks of [offset, offset + size) of [file], fetching the missing ones together
	vector<BlockCachePin> PinRange(const shared_ptr<BlockCacheFile> &file, idx_t offset, idx_t size);
	//! Pins the blocks of [offset, offset + size) of [file] when all of them are cached; empty otherwise
	vector<BlockCachePin> TryPinRange(BlockCacheFile &file, idx_t offset, idx_t size);
	//! Whether every block of [offset, offset + size) is cached or loading
	bool IsCached(BlockCacheFile &file, idx_t offset, idx_t size) const;
	//! Starts fetching the missing blocks of [offset, offset + size) and returns
	void Prefetch(const shared_ptr<BlockCacheFile> &file, idx_t offset, idx_t size);
	//! Pins a new block [block] of the unbacked or written [file] for its writer, holding whatever its frame held
	//! before; BlockCacheAppender fills it and zeroes what lies past the end of the file
	BlockCachePin CreateBlock(BlockCacheFile &file, idx_t block);
	//! Extends the unbacked [file] to [size] bytes once its blocks are written
	void SetUnbackedSize(BlockCacheFile &file, idx_t size);

	//! Frees at least [target] bytes for the buffer pool when it can; returns the bytes freed
	idx_t EvictToReduceMemory(idx_t target);
	//! Sets the memory the cache may use
	void SetCapacity(idx_t capacity);
	//! Drops every block of a backed file and the spilled copies of such blocks; current pins keep their bytes
	void Clear();
	//! Whether external file reads go through the cache; turning it off drops the blocks of backed files
	void SetEnabled(bool enabled);
	bool IsEnabled() const {
		return enabled.load(std::memory_order_relaxed);
	}
	//! Configures the spill tier; an empty path or a zero size turns spilling of backed blocks off
	void SetSpill(const string &path, idx_t size);

	BlockCacheStatistics GetStatistics() const;
	//! The cached blocks of files the cache can name, in memory or only in the spill tier
	vector<CachedFileInformation> GetBlockInformation() const;

private:
	friend class BlockCachePin;
	friend class BlockCacheFile;
	friend class BlockCacheFetchTask;

	struct Run {
		idx_t first_block;
		idx_t block_count;
	};
	struct RunPlan {
		Run run;
		//! A loading entry, pinned for the fetch, per block to fetch; null for the cached blocks between them
		vector<optional_ptr<BlockCacheEntry>> entries;
		//! The bytes of the blocks to fetch
		idx_t bytes;
		//! The first block the run did not take
		idx_t next;
		//! Whether the budget or the frames ran out
		bool exhausted;
	};
	struct FileRecord {
		uint32_t number = 0;
		BlockCacheFileInfo info;
		//! Opens of this version of the file that are still live
		idx_t opens = 0;
	};

	static uint32_t WaitStripe(const BlockCacheEntry &entry);
	BlockCacheBucket &BucketFor(uint64_t key) const;
	data_ptr_t FrameData(uint32_t frame) const;
	//! Calls [callback] with the entry of every frame that holds one
	template <class FUNC>
	void ForEachEntry(FUNC &&callback) const;

	//! Finds block [block] of [file] and pins it, waiting while it loads when [wait_for_load]; false when it is not
	//! cached
	bool Lookup(const BlockCacheFile &file, idx_t block, BlockCachePin &result, bool wait_for_load);
	//! Pins block [block] of [file] when it is cached or loading; [result] holds no bytes until the block has loaded
	bool PinPresent(const BlockCacheFile &file, idx_t block, BlockCachePin &result);
	//! Waits until the entry [pin] holds has loaded and gives [pin] its bytes; resets [pin] when the load failed
	bool AwaitLoad(BlockCachePin &pin);
	//! The entry of [key] that lookups can find, or null
	BlockCacheEntry *FindEntry(uint64_t key) const;
	BlockCacheEntry *FindEntry(uint64_t key, uint64_t &state) const;
	//! Inserts a loading entry for [key], pinned once for the caller who fills it; null when [key] is already present
	optional_ptr<BlockCacheEntry> InsertLoading(uint64_t key, uint32_t frame, uint64_t flags);
	//! The entry flags of blocks of [file]
	static uint64_t FileFlags(const BlockCacheFile &file);
	void Unpin(BlockCacheEntry &entry);
	void Unlink(BlockCacheEntry &entry);
	//! Removes [entry] of [key] from lookups: an unpinned block goes now, adding its frame to [released], a pinned or
	//! loading one with its last pin; a block of an unbacked file only when [drop_unbacked]
	void DropEntry(BlockCacheEntry &entry, uint64_t key, bool drop_unbacked, vector<uint32_t> &released);

	//! A frame for a new block: a free one while the cache is below its capacity and the buffer pool has room for it
	//! once it evicts unpinned buffers, else a victim's, whose frame is used as it is
	bool AcquireFrame(uint32_t &frame);
	//! A free frame whose memory the buffer pool grants, evicting the caches for it only when [evict_caches]
	bool TakeFreeFrame(uint32_t &frame, bool evict_caches);
	void ReleaseFrame(uint32_t frame);
	//! Gives [frames] back to the system and the buffer pool, a run of adjacent frames at once, and frees them to be
	//! handed out lowest first
	void ReleaseFrames(std::span<uint32_t> frames);
	//! Evicts one unpinned block chosen by sampling and returns its frame
	bool EvictOne(uint32_t &frame);
	//! Evicts [entry] when it is unpinned and still in [state]; [keep_copy] spills it first where the tier allows, and
	//! a reader that misses the block then finds its copy or waits for it
	bool TryEvict(BlockCacheEntry &entry, uint64_t state, uint32_t &frame, bool keep_copy);
	//! Drops the blocks of [file], which no one reads anymore
	void DropFile(BlockCacheFile &file);
	void DropBlocks(uint32_t number, idx_t block_count);
	void ReleaseUnbacked(uint32_t number, idx_t block_count);
	bool IsLiveUnbacked(uint32_t number) const;
	//! Gives [record] a new number for [info] with [opens] opens, returning the version it replaces; sets [sweep] when
	//! the record is new and the records are due a sweep
	optional<FileRecord> Renumber(FileRecord &record, const BlockCacheFileInfo &info, idx_t opens, bool &sweep)
	    DUCKDB_REQUIRES(file_lock);
	//! Drops the blocks of [replaced], a version of a file no open shares anymore, and sweeps the records when [sweep]
	void Retire(const optional<FileRecord> &replaced, bool sweep);
	//! Counts the end of an open of a backed file
	void CloseFile(const BlockCacheFile &file);
	//! Forgets the files no one has open and the cache holds no block of
	void SweepFiles();

	//! Plans a run of [file] from [block] up to [end]: a loading entry flagged [flags] for each block [missing] reports
	//! while [admit] accepts the run's bytes with it, bridging cached gaps of up to MERGE_GAP; the run takes at most
	//! MAX_REQUEST and never ends in a gap
	template <class MISSING, class ADMIT>
	RunPlan PlanRun(BlockCacheFile &file, idx_t block, idx_t end, uint64_t flags, MISSING &&missing, ADMIT &&admit);
	//! Reads the blocks of [run] of [file] into the loading entries the caller inserted; null entries are cached gaps
	void Fetch(BlockCacheFile &file, const Run &run, vector<optional_ptr<BlockCacheEntry>> &entries);
	//! Reads block [block] of [file] into a buffer of its own, for a miss that found no frame
	BlockCachePin ReadPrivate(BlockCacheFile &file, idx_t block);
	//! Ends the load of [entry]: its bytes are shared when [shared]; otherwise the load failed or its bytes serve only
	//! the loader, and every other reader fetches the block again
	void FinishLoad(BlockCacheEntry &entry, bool shared);
	void WaitForLoad(BlockCacheEntry &entry);
	//! Fills [entry] from the spill tier when it holds [key]
	bool ReadSpilled(uint64_t key, BlockCacheEntry &entry);
	//! Whether blocks of unbacked files can spill, configuring a file in the temporary directory when none is set
	bool CanSpillUnbacked();

	void ScheduleFetch(const shared_ptr<BlockCacheFile> &file, const Run &run,
	                   vector<optional_ptr<BlockCacheEntry>> entries, idx_t bytes);
	ProducerToken &Producer();
	void FinishFetchTask(idx_t bytes);
	//! Takes a queued fetch task, which a waiter runs itself and shutdown abandons
	bool TakeFetchTask(shared_ptr<Task> &task);

public:
	//! Waits for the fetch tasks in flight and refuses new ones; the database calls it before its scheduler stops
	void StopFetching();
#ifdef DEBUG
	//! Runs once, the next time an eviction is about to write the copy of a block of a live unbacked file
	std::function<void()> debug_before_keep_copy;
#endif

private:
	DatabaseInstance &db;
	BufferManager &buffer_manager;

	//! Frames: one region of BLOCK_SIZE slots on huge pages
	data_ptr_t region = nullptr;
	idx_t region_size = 0;
	uint32_t frame_count = 0;
	//! The entry owning each frame, for the eviction sampler
	unique_ptr<atomic<BlockCacheEntry *>[]> frame_owner;
	//! One past the highest frame ever handed out, the range the sampler draws from
	atomic<uint32_t> high_water {0};

	//! The translation table: twice as many buckets as frames, plus a type-stable overflow pool whose entry [F] is the
	//! overflow entry of the block in frame F, free once a frame is given up, since that unlinks its entry first
	unique_ptr<BlockCacheBucket[]> buckets;
	uint32_t bucket_mask = 0;
	unique_ptr<BlockCacheEntry[]> pool;

	mutable annotated_mutex frame_lock;
	//! Frames holding no block
	vector<uint32_t> free_frames DUCKDB_GUARDED_BY(frame_lock);
	//! Frames holding a block or about to, all of them reserved from the buffer pool
	atomic<idx_t> used_frames {0};
	//! Frames the cache may use
	atomic<idx_t> capacity_frames {0};
	//! State of the eviction sampler
	atomic<uint64_t> sampler_seed {0};

	//! Waiters on loading blocks
	static constexpr idx_t WAIT_STRIPES = 64;
	mutable mutex wait_locks[WAIT_STRIPES];

	mutable annotated_mutex file_lock;
	unordered_map<string, FileRecord> files DUCKDB_GUARDED_BY(file_lock);
	uint32_t next_file_number DUCKDB_GUARDED_BY(file_lock) = 1;
	static constexpr idx_t MIN_SWEEP_RECORDS = 1024;
	//! The record count at which the next open sweeps the records
	idx_t sweep_at DUCKDB_GUARDED_BY(file_lock) = MIN_SWEEP_RECORDS;
	//! Unbacked files someone still references; blocks of the others are dropped, never spilled
	unordered_set<uint32_t> live_unbacked DUCKDB_GUARDED_BY(file_lock);
	atomic<bool> enabled {true};

	//! Background fetches
	annotated_mutex fetch_lock;
	unique_ptr<ProducerToken> producer DUCKDB_GUARDED_BY(fetch_lock);
	idx_t pending_tasks DUCKDB_GUARDED_BY(fetch_lock) = 0;
	bool stopped DUCKDB_GUARDED_BY(fetch_lock) = false;
	atomic<idx_t> in_flight {0};

	BlockCacheSpill spill;
	atomic<bool> unbacked_spill_ready {false};

	//! Hits, counted per shard so that threads pinning different blocks do not share a cache line
	struct alignas(64) CounterShard {
		atomic<idx_t> value {0};
	};
	static constexpr idx_t COUNTER_SHARDS = 32;
	CounterShard hits[COUNTER_SHARDS];
	void CountHit(const BlockCacheEntry &entry);
	atomic<idx_t> misses {0};
	atomic<idx_t> fetched_bytes {0};
	atomic<idx_t> fetches {0};
	atomic<idx_t> evictions {0};
	atomic<idx_t> spilled {0};
	atomic<idx_t> spill_reads {0};
};

} // namespace duckdb
