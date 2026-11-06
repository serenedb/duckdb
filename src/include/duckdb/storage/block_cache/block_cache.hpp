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
#include "duckdb/common/limits.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
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
	//! A file on local disk, read again as cheaply as a temporary file, so its evicted blocks are dropped
	bool on_disk = false;
};

//! A block held in the cache, or bytes read into a buffer the pin owns: they stay valid and unchanged while the pin
//! lives
class BlockCachePin {
public:
	BlockCachePin() = default;
	BlockCachePin(BufferHandle handle_p, idx_t size_p)
	    : handle(std::move(handle_p)), data(handle.GetDataMutable()), size(size_p) {
	}
	[[gnu::always_inline]] BlockCachePin(BufferHandle handle_p, data_ptr_t data_p, idx_t size_p)
	    : handle(std::move(handle_p)), data(data_p), size(size_p) {
	}
	[[gnu::always_inline]] BlockCachePin(BlockCachePin &&other) noexcept
	    : handle(std::move(other.handle)), data(other.data), size(other.size) {
		other.data = nullptr;
		other.size = 0;
	}
	[[gnu::always_inline]] BlockCachePin &operator=(BlockCachePin &&other) noexcept {
		if (this != &other) {
			BufferHandle released = std::move(handle);
			handle = std::move(other.handle);
			data = other.data;
			size = other.size;
			other.data = nullptr;
			other.size = 0;
		}
		return *this;
	}
	BlockCachePin(const BlockCachePin &) = delete;
	BlockCachePin &operator=(const BlockCachePin &) = delete;

	bool IsValid() const {
		return handle.IsValid();
	}
	const_data_ptr_t Ptr() const {
		return data;
	}
	//! Writable bytes of a block its writer created, which only that writer pins, or of a buffer the pin owns
	data_ptr_t GetDataMutable() const {
		return data;
	}
	idx_t Size() const {
		return size;
	}
	//! Another pin of the [size] bytes at [offset] of the pinned bytes
	BlockCachePin Share(idx_t offset, idx_t size);
	//! Narrows this pin to the [size] bytes at [offset] of its bytes
	void Narrow(idx_t offset, idx_t size_p) {
		D_ASSERT(offset <= size && size_p <= size - offset);
		data += offset;
		size = size_p;
	}
	void Reset() {
		handle.Destroy();
		data = nullptr;
		size = 0;
	}

private:
	friend class BlockCache;

	BufferHandle handle;
	data_ptr_t data = nullptr;
	idx_t size = 0;
};

//! The slot of a block of a file: the memory of its buffer-managed block, which the buffer pool may evict, or null
//! while the block has none
using BlockCacheSlot = atomic<BlockMemory *>;

//! The slots of the blocks of one version of a file, and the blocks, which live as long as the slots
class BlockCacheBlocks {
public:
	explicit BlockCacheBlocks(idx_t block_count);
	BlockCacheBlocks(const BlockCacheBlocks &) = delete;
	BlockCacheBlocks &operator=(const BlockCacheBlocks &) = delete;

private:
	friend class BlockCache;

	static constexpr idx_t CHUNK = 512;
	struct Chunk {
		BlockCacheSlot slots[CHUNK] {};
	};
	struct Directory {
		explicit Directory(idx_t size_p) : size(size_p), chunks(new atomic<Chunk *>[size_p] {}) {
		}
		const idx_t size;
		const unique_ptr<atomic<Chunk *>[]> chunks;
	};

	//! The slot of block [block], or null when it has none yet
	BlockCacheSlot *Find(idx_t block) const {
		const auto directory = current.load(std::memory_order_acquire);
		if (block / CHUNK >= directory->size) {
			return nullptr;
		}
		const auto chunk = directory->chunks[block / CHUNK].load(std::memory_order_acquire);
		return chunk ? &chunk->slots[block % CHUNK] : nullptr;
	}
	//! The memory in the slot of block [block], or null
	BlockMemory *Load(idx_t block) const {
		const auto slot = Find(block);
		return slot ? slot->load(std::memory_order_acquire) : nullptr;
	}
	//! The slot of block [block], created when missing
	BlockCacheSlot &Get(idx_t block);
	//! Calls [callback] with every slot and its block number
	template <class FUNC>
	void ForEach(FUNC &&callback);

	atomic<Directory *> current;
	annotated_mutex lock;
	//! Every directory the slots had: readers may still look at an earlier one
	vector<unique_ptr<Directory>> directories DUCKDB_GUARDED_BY(lock);
	vector<unique_ptr<Chunk>> chunks DUCKDB_GUARDED_BY(lock);
	//! The blocks slots named, including the ones a slot let go: a reader may still pin those
	vector<shared_ptr<BlockHandle>> handles DUCKDB_GUARDED_BY(lock);
};

//! An open file whose blocks the cache holds; opens of the same path and version share its blocks
class BlockCacheFile {
public:
	BlockCacheFile(string path, bool unbacked, BlockCacheFileInfo info, shared_ptr<FileHandle> source,
	               shared_ptr<BlockCacheBlocks> blocks);

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
	//! Bytes of block [block] that lie inside the file
	idx_t BlockBytes(idx_t block) const;
	//! Whether missing blocks can be fetched; a file opened without a source gets one before its first miss
	bool HasSource() const;
	//! Sets where missing blocks are read from, unless the file has a source already
	void SetSource(shared_ptr<FileHandle> handle);
	//! Whether a response to a fetch of the file forbade reusing the bytes it returned
	bool ReuseForbidden() const {
		return reuse_forbidden.load(std::memory_order_relaxed);
	}

private:
	friend class BlockCache;
	friend class BlockCacheFetchTask;

	shared_ptr<FileHandle> Source() const;

	const string path;
	atomic<idx_t> size;
	const bool unbacked;
	const BlockCacheFileInfo info;
	mutable annotated_mutex source_lock;
	shared_ptr<FileHandle> source DUCKDB_GUARDED_BY(source_lock);
	const shared_ptr<BlockCacheBlocks> blocks;
	//! Bytes the background fetches of this file are fetching right now
	atomic<idx_t> in_flight {0};
	atomic<bool> reuse_forbidden {false};
};

//! Writes an unbacked or written file from start to end: it creates each block as the write reaches it and pins the
//! block it fills until the block is full or the write ends
class BlockCacheAppender {
public:
	BlockCacheAppender(BlockCache &cache, shared_ptr<BlockCacheFile> file);

	//! Appends [count] bytes of [data]; readers of an unbacked file see them once they are appended
	void Append(const_data_ptr_t data, idx_t count);
	//! The rest of the block the write has reached, created if needed, to fill in place and append with Commit
	FileWriteWindow Window();
	//! Appends the first [count] bytes of the last window
	void Commit(idx_t count);
	//! The bytes appended
	idx_t Size() const {
		return size;
	}
	//! Ends the write, zeroing the rest of the last block; a written file is published at the size appended, as the
	//! version [version_tag] of its source, last modified at [last_modified]
	void Finish(const string &version_tag = string(), timestamp_t last_modified = timestamp_t(0));
	//! Ends the write of a written file and drops its blocks
	void Abandon();

private:
	BlockCache &cache;
	shared_ptr<BlockCacheFile> file;
	BlockCachePin current;
	idx_t size = 0;
};

//! Where one reader of a file is in it, so that the cache reads ahead of a reader that pins the file's blocks in order
struct BlockCacheReadAhead {
	//! The block a reader that keeps reading in order pins next
	idx_t next_block = NumericLimits<idx_t>::Maximum();
	//! The end of the blocks fetched ahead so far
	idx_t end = 0;
	//! The blocks the last read-ahead fetched
	idx_t blocks = 0;
};

//! Whether a prefetch is a guess at what a reader may read next or the blocks it will read
enum class BlockCachePrefetch : uint8_t { HINT, NEED };

struct BlockCacheRange {
	idx_t offset = 0;
	idx_t size = 0;
};

struct BlockCacheStatistics {
	//! Memory the buffer pool holds for the cache's blocks and for the buffers of reads it does not keep
	idx_t used = 0;
	//! Pins that found the block in memory
	idx_t hits = 0;
	idx_t misses = 0;
	idx_t fetched_bytes = 0;
	idx_t fetches = 0;
	//! Blocks the buffer pool evicted
	idx_t evictions = 0;
	//! Evicted blocks written to temporary files, and blocks read back from them
	idx_t spilled = 0;
	idx_t spill_reads = 0;
};

//! Fixed-size blocks of remote and in-memory files, shared by every reader of a database instance. Each block is a
//! buffer-managed block, which the buffer pool evicts like any other: blocks of in-memory files go to temporary files,
//! blocks of remote files keep a copy there while the copies have room, and the cache fetches a dropped block again
//! into the same block. A lookup writes nothing shared and a pin is one atomic add on the block.
class BlockCache {
public:
	static constexpr idx_t BLOCK_SIZE = 128ULL * 1024ULL;
	//! Missing runs closer than this are fetched by one request
	static constexpr idx_t MERGE_GAP = 4 * BLOCK_SIZE;
	//! The largest run of blocks fetched together
	static constexpr idx_t MAX_REQUEST = 16ULL * 1024ULL * 1024ULL;
	//! A longer run is fetched as concurrent requests of at most this size, and the blocks of each are handed out as
	//! soon as it lands
	static constexpr idx_t PIECE_SIZE = 4ULL * 1024ULL * 1024ULL;
	//! Bytes one file may have in flight, which bounds hints tuned for the page cache
	static constexpr idx_t MAX_FILE_IN_FLIGHT = 64ULL * 1024ULL * 1024ULL;
	//! The most blocks one read-ahead fetches
	static constexpr idx_t READ_AHEAD_BLOCKS = 64;

	static constexpr idx_t BlockCountOf(idx_t size) {
		return (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	}

public:
	explicit BlockCache(DatabaseInstance &db);
	~BlockCache();

	static BlockCache &Get(DatabaseInstance &db);
	//! A pin owning a new buffer of [size] bytes for a read the cache does not keep
	static BlockCachePin AllocateUncached(DatabaseInstance &db, idx_t size);

	//! Registers an open of [path] whose metadata is [current], reading missing blocks from [source], which may be
	//! set later. It shares the blocks of an earlier open that [current] validates; without [validate] it shares them
	//! while they are fresh, and [current] may then be the earlier open's metadata.
	shared_ptr<BlockCacheFile> OpenFile(const string &path, const BlockCacheFileInfo &current, bool validate,
	                                    shared_ptr<FileHandle> source);
	//! The metadata of the fresh blocks of an earlier open of [path], when there are any
	bool TryGetFile(const string &path, BlockCacheFileInfo &result);
	//! Stops sharing the blocks of [file] and drops them, once a response forbids reusing them or their writer abandons
	//! them
	void ForbidReuse(BlockCacheFile &file);
	//! A file whose blocks exist only in the cache and its temporary files
	shared_ptr<BlockCacheFile> CreateUnbackedFile(const string &path);
	//! A file the caller writes under [path], replacing earlier opens of [path]: its writer creates the blocks, and
	//! once its BlockCacheAppender finishes, opens of [path] share them like fetched blocks
	shared_ptr<BlockCacheFile> CreateWrittenFile(const string &path);
	//! Drops the blocks of every open of [path]; current pins keep their bytes
	void ForgetFile(const string &path);

	//! Pins block [block] of [file], fetching it when missing; throws when the fetch fails
	BlockCachePin Pin(BlockCacheFile &file, idx_t block);
	//! Pins block [block] of [file] for a reader that may read it in order: when the reader's last pin was of the
	//! block before and [ahead] holds, the blocks from [block] on are fetched ahead, twice as many as the time before
	//! (2 to READ_AHEAD_BLOCKS)
	BlockCachePin PinInOrder(const shared_ptr<BlockCacheFile> &file, idx_t block, BlockCacheReadAhead &read_ahead,
	                         bool ahead);
	//! Pins the bytes of [offset, offset + size) of [file] that lie inside it, a pin per block they span, fetching the
	//! missing blocks together
	vector<BlockCachePin> PinRange(BlockCacheFile &file, idx_t offset, idx_t size);
	//! Pins the [size] bytes at [offset] of [file] in one piece: in their block when they lie in one, otherwise in a
	//! copy the pin owns; Ptr() and Size() are the bytes either way
	BlockCachePin PinBytes(BlockCacheFile &file, idx_t offset, idx_t size);
	//! Pins the bytes of [offset, offset + size) of [file] like PinRange when all of their blocks are in memory; empty
	//! otherwise
	vector<BlockCachePin> TryPinRange(BlockCacheFile &file, idx_t offset, idx_t size);
	//! Whether every block of [offset, offset + size) is in memory, in a temporary file or loading
	bool IsCached(BlockCacheFile &file, idx_t offset, idx_t size) const;
	//! Starts fetching the missing blocks of [offset, offset + size) and returns; a HINT stops at the bytes a file and
	//! the cache may have in flight
	void Prefetch(const shared_ptr<BlockCacheFile> &file, idx_t offset, idx_t size, BlockCachePrefetch kind);
	//! Prefetches [ranges] like one Prefetch per range, where ranges closer than MERGE_GAP make one; sorts and merges
	//! [ranges] in place
	void Prefetch(const shared_ptr<BlockCacheFile> &file, std::span<BlockCacheRange> ranges, BlockCachePrefetch kind);
	void MarkUnused(const BlockCacheFile &file, idx_t offset, idx_t size) const;

	//! Whether external file reads go through the cache; turning it off drops the blocks of backed files, while current
	//! pins keep their bytes
	void SetEnabled(bool enabled);
	bool IsEnabled() const {
		return enabled.load(std::memory_order_relaxed);
	}

	BlockCacheStatistics GetStatistics() const;
	//! The cached blocks of files the cache can name, in memory or only in temporary files
	vector<CachedFileInformation> GetBlockInformation() const;

	//! Waits for the fetch tasks in flight and refuses new ones; the database calls it before its scheduler stops
	void StopFetching();

private:
	friend class BlockCacheAppender;
	friend class BlockCacheFetchTask;

	//! Ends the write of [file], which wrote the version [written] describes: its blocks stay, and TryGetFile reports
	//! [written] for it
	void PublishWrittenFile(BlockCacheFile &file, const CacheValidationInfo &written);
	//! Pins a new block [block] of the unbacked or written [file] for its writer, holding whatever its buffer held
	//! before; BlockCacheAppender fills it and zeroes what lies past the end of the file
	BlockCachePin CreateBlock(BlockCacheFile &file, idx_t block);
	//! Extends the unbacked [file] to [size] bytes once its blocks are written
	void SetUnbackedSize(BlockCacheFile &file, idx_t size);

	struct RunPlan {
		idx_t first_block;
		//! The block claimed for the fetch per block of the run; null for the blocks between them it does not fetch
		vector<optional_ptr<BlockMemory>> claimed;
		//! The first block the run did not take
		idx_t next;
		//! Whether the budget ran out
		bool exhausted;
	};
	struct FileRecord {
		BlockCacheFileInfo info;
		//! The blocks of this version of the file, which its open files share
		shared_ptr<BlockCacheBlocks> blocks;
	};

	//! Hits counted per CPU
	struct alignas(64) HitCounter {
		atomic<idx_t> value {0};
	};
	static constexpr idx_t HIT_COUNTERS = 64;

	//! Pins the block in [slot] when it is in memory
	bool PinLoaded(BlockCacheSlot &slot, idx_t size, BlockCachePin &result) const;
	BlockMemory &MemoryOf(const BlockCacheFile &file, BlockCacheSlot &slot);
	optional_ptr<BlockMemory> ClaimMissing(BlockCacheFile &file, idx_t block);
	//! Lets the slots of [blocks] go, destroying their blocks and the copies of them at once or with their last pin
	void DropBlocks(BlockCacheBlocks &blocks);

	//! Gives [record] new blocks for [info], returning the version it replaces; sets [sweep] when the record is new and
	//! the records are due a sweep
	optional<FileRecord> NewVersion(FileRecord &record, const BlockCacheFileInfo &info, bool &sweep)
	    DUCKDB_REQUIRES(file_lock);
	//! Drops the blocks of [replaced], a version of a file the records no longer name, and sweeps the records when
	//! [sweep]
	void RetireRecord(optional<FileRecord> replaced, bool sweep);
	//! Forgets the files no one has open and the cache holds no block of
	void SweepFiles();

	//! Plans a run of [file] from [block] up to [end]: a claimed block for each missing block while [admit] accepts
	//! the run's bytes with it, bridging gaps of up to MERGE_GAP of blocks it does not fetch; the run takes at most
	//! MAX_REQUEST and never ends in a gap
	template <class ADMIT>
	RunPlan PlanRun(BlockCacheFile &file, idx_t block, idx_t end, ADMIT &&admit);
	//! Reads the [claimed] blocks of the run of [file] from [first_block] with one request and gives back their
	//! claims, clearing each entry, also when it throws; when [pins] is not empty, pins each block read there. Null
	//! entries are blocks the run does not fetch
	void Fetch(BlockCacheFile &file, idx_t first_block, std::span<optional_ptr<BlockMemory>> claimed,
	           std::span<BlockCachePin> pins);
	//! Fetches the runs of [plans] of [file] as concurrent pieces of at most PIECE_SIZE, pinning each block read in
	//! [pins], which start at block [first]; throws the first error, and leaves the claims of pieces it did not fetch
	void FetchConcurrently(BlockCacheFile &file, vector<RunPlan> &plans, idx_t first, vector<BlockCachePin> &pins);
	//! Waits while [memory] loads; when the scheduler has no workers to run queued fetches, runs them until it loads
	void WaitForLoad(BlockMemory &memory);

	//! Fetches the [claimed] blocks of the run of [file] from [first_block] in the background, a task per piece of at
	//! most PIECE_SIZE
	void ScheduleFetch(const shared_ptr<BlockCacheFile> &file, idx_t first_block,
	                   vector<optional_ptr<BlockMemory>> claimed);
	ProducerToken &Producer();
	void FinishFetchTask(idx_t bytes);
	//! Takes a queued fetch task, which a waiter runs itself and shutdown abandons
	bool TakeFetchTask(shared_ptr<Task> &task);

private:
	DatabaseInstance &db;
	BufferManager &buffer_manager;

	mutable HitCounter hits[HIT_COUNTERS];

	mutable annotated_mutex file_lock;
	unordered_map<string, FileRecord> files DUCKDB_GUARDED_BY(file_lock);
	static constexpr idx_t MIN_SWEEP_RECORDS = 1024;
	//! The record count at which the next open sweeps the records
	idx_t sweep_at DUCKDB_GUARDED_BY(file_lock) = MIN_SWEEP_RECORDS;
	atomic<bool> enabled {true};

	//! Background fetches
	annotated_mutex fetch_lock;
	unique_ptr<ProducerToken> producer DUCKDB_GUARDED_BY(fetch_lock);
	idx_t pending_tasks DUCKDB_GUARDED_BY(fetch_lock) = 0;
	bool stopped DUCKDB_GUARDED_BY(fetch_lock) = false;
	atomic<idx_t> in_flight {0};

	atomic<idx_t> misses {0};
	atomic<idx_t> fetched_bytes {0};
	atomic<idx_t> fetches {0};
};

} // namespace duckdb
