//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/external_file_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/page_residency.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer/temporary_file_information.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/cache_validation.hpp"

#include <span>

namespace duckdb {

class ClientContext;
class ExternalFileCache;

//! What the cache knows of a file without opening it
struct ExternalFileCacheFileInfo {
	CacheValidationInfo validation;
	bool can_seek = true;
	//! A file on local disk, read again as cheaply as a temporary file, so its evicted blocks are dropped
	bool on_disk = false;
};

//! A block held in the cache, or bytes read into a buffer the pin owns: they stay valid and unchanged while the pin
//! lives. Bytes that are valid without a pin, such as a mapped file's, travel in one that holds nothing
class FilePin {
public:
	FilePin() = default;
	//! The [size_p] bytes at [data_p], which outlive the pin
	FilePin(const_data_ptr_t data_p, idx_t size_p)
	    : data(const_cast<data_ptr_t>(data_p)), size(size_p) { // NOLINT: bytes that are only read
	}
	FilePin(BufferHandle handle_p, idx_t size_p)
	    : handle(std::move(handle_p)), data(handle.GetDataMutable()), size(size_p) {
	}
	[[gnu::always_inline]] FilePin(BufferHandle handle_p, data_ptr_t data_p, idx_t size_p)
	    : handle(std::move(handle_p)), data(data_p), size(size_p) {
	}
	[[gnu::always_inline]] FilePin(FilePin &&other) noexcept
	    : handle(std::move(other.handle)), data(other.data), size(other.size) {
		other.data = nullptr;
		other.size = 0;
	}
	[[gnu::always_inline]] FilePin &operator=(FilePin &&other) noexcept {
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
	FilePin(const FilePin &) = delete;
	FilePin &operator=(const FilePin &) = delete;

	//! Whether the pin holds a block or a buffer, which the bytes need to stay valid
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
	FilePin Share(idx_t offset, idx_t size_p) {
		if (!handle.IsValid()) {
			return {};
		}
		D_ASSERT(offset <= size && size_p <= size - offset);
		return FilePin(handle.Share(), data + offset, size_p);
	}
	//! Narrows this pin to the [size] bytes at [offset] of its bytes
	void Narrow(idx_t offset, idx_t size_p) {
		D_ASSERT(offset <= size && size_p <= size - offset);
		data += offset;
		size = size_p;
	}
	void Reset() {
		BufferHandle released = std::move(handle);
		data = nullptr;
		size = 0;
	}

private:
	BufferHandle handle;
	data_ptr_t data = nullptr;
	idx_t size = 0;
};

//! The slot of a block of a file: the memory of its buffer-managed block, which the buffer pool may evict, or null
//! while the block has none
using ExternalFileCacheSlot = atomic<BlockMemory *>;

//! The slots of the blocks of one version of a file, and the blocks, which live as long as the slots
class ExternalFileCacheBlocks {
public:
	ExternalFileCacheBlocks(idx_t block_size, idx_t file_size);
	ExternalFileCacheBlocks(const ExternalFileCacheBlocks &) = delete;
	ExternalFileCacheBlocks &operator=(const ExternalFileCacheBlocks &) = delete;

	idx_t BlockSize() const {
		return idx_t(1) << block_shift;
	}
	idx_t BlockOf(idx_t offset) const {
		return offset >> block_shift;
	}
	idx_t BlockStart(idx_t block) const {
		return block << block_shift;
	}
	idx_t OffsetInBlock(idx_t offset) const {
		return offset & (BlockSize() - 1);
	}
	idx_t BlockCountOf(idx_t size) const {
		return (size + BlockSize() - 1) >> block_shift;
	}
	//! Whether a freshness deadline that a response to a fetch of the blocks gave has passed
	bool Expired() const {
		const auto deadline = valid_until.load(std::memory_order_relaxed);
		return deadline != NO_DEADLINE && Timestamp::GetCurrentTimestamp().value >= deadline;
	}

private:
	friend class ExternalFileCache;

	static constexpr idx_t CHUNK = 512;
	static constexpr int64_t NO_DEADLINE = NumericLimits<int64_t>::Maximum();
	struct Chunk {
		ExternalFileCacheSlot slots[CHUNK] {};
	};
	struct Directory {
		explicit Directory(idx_t size_p) : size(size_p), chunks(new atomic<Chunk *>[size_p] {}) {
		}
		const idx_t size;
		const unique_ptr<atomic<Chunk *>[]> chunks;
	};

	//! A copy of bytes of the file that a reader cannot read in place, made once for every reader of them
	struct Copy {
		idx_t size = 0;
		shared_ptr<BlockHandle> handle;
	};

	//! The slot of block [block], or null when it has none yet
	ExternalFileCacheSlot *Find(idx_t block) const {
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
	ExternalFileCacheSlot &Get(idx_t block);
	//! Calls [callback] with every slot and its block number
	template <class FUNC>
	void ForEach(FUNC &&callback);

	const uint8_t block_shift;
	atomic<Directory *> current;
	annotated_mutex lock;
	//! Every directory the slots had: readers may still look at an earlier one
	vector<unique_ptr<Directory>> directories DUCKDB_GUARDED_BY(lock);
	vector<unique_ptr<Chunk>> chunks DUCKDB_GUARDED_BY(lock);
	//! The blocks slots named, including the ones a slot let go: a reader may still pin those
	vector<shared_ptr<BlockHandle>> handles DUCKDB_GUARDED_BY(lock);
	//! The copies the file's readers share, by the offset of their bytes
	unordered_map<idx_t, Copy> copies DUCKDB_GUARDED_BY(lock);
	//! How many copies there are: a reader of a file without any looks for none
	atomic<idx_t> copy_count {0};
	//! The earliest freshness deadline the responses to fetches of the blocks gave, in microseconds
	atomic<int64_t> valid_until {NO_DEADLINE};
};

//! Whether a prefetch is a guess at what a reader may read next or the blocks it will read
enum class ExternalFileCachePrefetch : uint8_t { HINT, NEED };

struct FileRange {
	idx_t offset = 0;
	idx_t size = 0;
};

//! An open file that readers read from memory. A remote or an in-memory file lies in blocks the cache holds, which
//! opens of the same path and version share. A file on local disk is mapped: its bytes lie in one piece that the
//! kernel pages in, and the file remembers which pages it saw in memory, so that it asks the kernel once
class InputFile : public enable_shared_from_this<InputFile> {
public:
	//! A hint of at most this many bytes of a mapped file is given without asking the kernel what is in memory
	static constexpr idx_t HINT_CHUNK = 128ULL * 1024;
	//! The most a reader of a mapped file hints ahead of where it reads
	static constexpr idx_t MAX_HINT = 2ULL * 1024 * 1024;
	//! The most bytes of several ranges that are hinted together
	static constexpr idx_t MAX_HINTED = 16ULL * 1024 * 1024;
	//! The fewest bytes in several blocks that their readers share a copy of: a smaller copy costs less to make than
	//! to keep
	static constexpr idx_t MIN_SHARED = 4096;

	InputFile(ExternalFileCache &cache, string path, bool unbacked, ExternalFileCacheFileInfo info,
	          shared_ptr<FileHandle> source, shared_ptr<ExternalFileCacheBlocks> blocks);
	//! A file read in place from [mapping]
	DUCKDB_API explicit InputFile(unique_ptr<MemoryMappedFile> mapping);

	//! The cache that holds the blocks of a file that lies in blocks
	ExternalFileCache &GetCache() const {
		D_ASSERT(cache);
		return *cache.get_mutable();
	}
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
	const ExternalFileCacheFileInfo &Info() const {
		return info;
	}
	const ExternalFileCacheBlocks &Blocks() const {
		return *blocks;
	}
	idx_t BlockSize() const {
		return blocks->BlockSize();
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

	//! Whether the file is mapped: its bytes lie in memory in one piece
	bool IsMapped() const {
		return mapping != nullptr;
	}
	//! The bytes of a mapped file; null when it has none or lies in blocks
	const_data_ptr_t Data() const {
		return data;
	}
	//! The mapping of a mapped file that has bytes; null otherwise
	optional_ptr<MemoryMappedFile> Mapping() const {
		return data ? mapping.get() : nullptr;
	}
	//! Whether the kernel pages the mapped file in from disk
	bool IsPaged() const {
		return paged;
	}

public:
	//! What a reader does with the bytes of the file, however they lie. A read past its end throws; hints never
	//! fail

	//! The [nr_bytes] bytes at [offset] in one piece, valid while the pin lives: in place when they lie in the
	//! mapping or in one block, otherwise in a copy the pin owns
	DUCKDB_API FilePin Pin(idx_t offset, idx_t nr_bytes) const;
	//! A copy of the [nr_bytes] bytes at [offset] of a file in blocks, for a reader that reads them again and again
	//! and cannot read them in place: it starts where a block does and is readable up to its next sector boundary.
	//! It is made once for every reader of the same bytes and stays while the cache has room
	DUCKDB_API FilePin PinCopy(idx_t offset, idx_t nr_bytes) const;
	//! Pin for bytes that are read again and again: in place when they lie in the mapping or in one block, and the
	//! copy their readers share (PinCopy) when they lie in several; less than MIN_SHARED bytes are the pin's own copy
	DUCKDB_API FilePin PinShared(idx_t offset, idx_t nr_bytes) const;
	//! Reads the [nr_bytes] bytes at [offset] into [target] for a reader that keeps its own copy and reads them
	//! once: bytes of at least a block come straight from the source, and none stay in memory on its account
	DUCKDB_API void ReadOnce(idx_t offset, data_ptr_t target, idx_t nr_bytes) const;
	//! Says the bytes are read soon: starts reading the ones that are not in memory. Returns whether it did
	[[gnu::always_inline]] bool Hint(idx_t offset, idx_t nr_bytes) const noexcept {
		if (Trusted(offset, nr_bytes)) [[likely]] {
			return false;
		}
		return HintSlow(offset, nr_bytes);
	}
	//! Says the bytes of [ranges], which come in the order of their offsets, are read soon, [budget] bytes of them
	//! at most: a mapped file hints them, ranges closer than a page as one, and a file in blocks hands them to the
	//! cache as they are, which leaves out the ones a copy of which is in memory and makes requests of the rest.
	//! Changes [ranges] in place; returns what is left of the budget
	DUCKDB_API idx_t Hint(std::span<FileRange> ranges, idx_t budget = MAX_HINTED) const noexcept;
	//! Starts reading the bytes without asking which are in memory
	DUCKDB_API void Prefetch(idx_t offset, idx_t nr_bytes) const noexcept;
	//! Starts reading the ones of [ranges] that are not in memory, all of which the reader is going to read; sorts
	//! and merges them in place
	DUCKDB_API void Prefetch(std::span<FileRange> ranges) const noexcept;
	//! Hints the [nr_bytes] at [offset] that a reader reads in order: the first [needed] of them, which it reads next,
	//! are asked for whatever the bytes in flight, and the rest only within the budget of a hint. When [needed] is not
	//! 0 the reader waits for the block at [offset]: a request that holds it is read like a caller's. Returns where the
	//! bytes asked for end
	DUCKDB_API idx_t ReadAhead(idx_t offset, idx_t nr_bytes, idx_t needed) const noexcept;
	//! Says the reader is done with the bytes, which makes them the first to leave memory
	DUCKDB_API void Evict(idx_t offset, idx_t nr_bytes) const noexcept;
	//! Whether the bytes are in memory
	[[gnu::always_inline]] bool IsResident(idx_t offset, idx_t nr_bytes) const noexcept {
		if (Trusted(offset, nr_bytes)) [[likely]] {
			return true;
		}
		return IsResidentSlow(offset, nr_bytes);
	}
	//! How many of the bytes, from [offset] on, the file knows to be in memory
	DUCKDB_API idx_t Warm(idx_t offset, idx_t nr_bytes) const noexcept;
	//! Remembers the bytes of a mapped file as in memory
	DUCKDB_API void MarkWarm(idx_t offset, idx_t nr_bytes) const noexcept;
	//! Whether pages may have left memory since [epoch] while a reader trusted the bytes; moves [epoch] on
	[[gnu::always_inline]] bool Evicted(idx_t offset, idx_t nr_bytes, uint32_t &epoch) const noexcept {
		if (!paged || nr_bytes == 0 || Trusted(offset, nr_bytes)) [[likely]] {
			return false;
		}
		return EvictedSlow(epoch);
	}
	//! Fails a read at [offset] past the end of [file], or of [size] bytes that are no file's
	[[noreturn]] DUCKDB_API static void ThrowPastEnd(optional_ptr<const InputFile> file, idx_t offset, idx_t size);

private:
	friend class ExternalFileCache;
	friend class ExternalFileCacheFetchTask;
	friend class InputFileReader;

	shared_ptr<FileHandle> Source() const;
	//! The file for the cache, which changes what it knows of the blocks on behalf of readers
	InputFile &Mutable() const {
		return const_cast<InputFile &>(*this); // NOLINT: readers hold the file const
	}
	//! How many bytes of a paged file, from [offset] on, the residency map has
	[[gnu::always_inline]] idx_t WarmPaged(idx_t offset, idx_t nr_bytes) const noexcept {
		D_ASSERT(paged && nr_bytes != 0);
		const auto length = size.load(std::memory_order_relaxed);
		if (offset >= length || !residency.Valid(PageResidency::Epoch())) {
			return 0;
		}
		const auto first = offset / PageResidency::PAGE_SIZE;
		const auto last = (MinValue(offset + nr_bytes, length) - 1) / PageResidency::PAGE_SIZE;
		const auto pages = residency.Test(first, last);
		if (pages > last - first) {
			return nr_bytes;
		}
		const auto warm = (first + pages) * PageResidency::PAGE_SIZE;
		return warm - MinValue(warm, offset);
	}
	//! Whether the residency map of a paged file has every one of the bytes and the calling thread still trusts it
	[[gnu::always_inline]] bool Trusted(idx_t offset, idx_t nr_bytes) const noexcept {
		return paged && nr_bytes != 0 && WarmPaged(offset, nr_bytes) == nr_bytes && !PageResidency::Poll(nr_bytes);
	}
	DUCKDB_API bool HintSlow(idx_t offset, idx_t nr_bytes) const noexcept;
	DUCKDB_API bool IsResidentSlow(idx_t offset, idx_t nr_bytes) const noexcept;
	DUCKDB_API static bool EvictedSlow(uint32_t &epoch) noexcept;

	const optional_ptr<ExternalFileCache> cache;
	const string path;
	atomic<idx_t> size;
	const bool unbacked;
	const ExternalFileCacheFileInfo info;
	mutable annotated_mutex source_lock;
	shared_ptr<FileHandle> source DUCKDB_GUARDED_BY(source_lock);
	const shared_ptr<ExternalFileCacheBlocks> blocks;
	//! Bytes the background fetches of this file are fetching right now
	atomic<idx_t> in_flight {0};
	atomic<bool> reuse_forbidden {false};

	const unique_ptr<MemoryMappedFile> mapping;
	const_data_ptr_t data = nullptr;
	bool paged = false;
	PageResidencyMap residency;
};

//! Writes an unbacked or written file from start to end: it creates each block as the write reaches it and pins the
//! block it fills until the block is full or the write ends
class ExternalFileCacheAppender {
public:
	explicit ExternalFileCacheAppender(shared_ptr<InputFile> file);

	//! Appends [count] bytes of [data]; readers of an unbacked file see them once they are appended
	void Append(const_data_ptr_t data, idx_t count);
	//! The rest of the block the write has reached, created if needed, to fill in place and append with Commit
	FileWriteWindow Window();
	//! Appends the first [count] bytes of the last window
	void Commit(idx_t count);
	FilePin ShareWindow(idx_t count);
	//! The bytes appended
	idx_t Size() const {
		return size;
	}
	//! Ends the write; a written file is published at the size appended, as the version [version_tag] of its source,
	//! last modified at [last_modified]
	void Finish(const string &version_tag = string(), timestamp_t last_modified = timestamp_t(0));
	//! Ends the write of a written file and drops its blocks
	void Abandon();
	void FundFrom(BufferPoolReservation &source_p) {
		source = &source_p;
	}

private:
	shared_ptr<InputFile> file;
	FilePin current;
	idx_t size = 0;
	optional_ptr<BufferPoolReservation> source;
};

struct ExternalFileCacheStatistics {
	//! Memory the buffer pool holds for the cache's blocks and for the buffers of reads it does not keep
	idx_t used = 0;
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
class ExternalFileCache {
public:
	static constexpr idx_t DEFAULT_BLOCK_SIZE = 128ULL * 1024ULL;
	static constexpr idx_t MIN_BLOCK_SIZE = 32ULL * 1024ULL;
	static constexpr idx_t MAX_BLOCK_SIZE = 256ULL * 1024ULL;
	//! Missing runs closer than this many bytes are fetched by one request
	static constexpr idx_t MERGE_GAP = 512ULL * 1024ULL;
	//! The largest run of blocks fetched together
	static constexpr idx_t MAX_REQUEST = 16ULL * 1024ULL * 1024ULL;
	//! The bytes the cache reads from remote sources at once; a read that would pass them waits its turn. Connections
	//! that start together and each carry a long response overrun the network path, and a handshake or the tail of a
	//! response then waits out a timer
	static constexpr idx_t MAX_BURST = 64ULL * 1024ULL * 1024ULL;
	//! Bytes one file may have in flight, which bounds hints tuned for the page cache
	static constexpr idx_t MAX_FILE_IN_FLIGHT = 64ULL * 1024ULL * 1024ULL;
	//! The most bytes one read-ahead fetches
	static constexpr idx_t MAX_READ_AHEAD = 8ULL * 1024ULL * 1024ULL;

public:
	explicit ExternalFileCache(DatabaseInstance &db);
	~ExternalFileCache();

	static ExternalFileCache &Get(DatabaseInstance &db);
	static ExternalFileCache &Get(ClientContext &context);
	//! A pin owning a new buffer of [size] bytes for a read the cache does not keep
	static FilePin AllocateUncached(DatabaseInstance &db, idx_t size);

	//! Registers an open of [path] whose metadata is [current], reading missing blocks from [source], which may be
	//! set later. It shares the blocks of an earlier open that [current] validates; without [validate] it shares them
	//! while they are fresh, and [current] may then be the earlier open's metadata.
	shared_ptr<InputFile> OpenFile(const string &path, const ExternalFileCacheFileInfo &current, bool validate,
	                               shared_ptr<FileHandle> source);
	//! The metadata of the fresh blocks of an earlier open of [path], when there are any
	bool TryGetFile(const string &path, ExternalFileCacheFileInfo &result);
	//! Stops sharing the blocks of [file] and drops them, once a response forbids reusing them or their writer abandons
	//! them
	void ForbidReuse(InputFile &file);
	//! A file whose blocks exist only in the cache and its temporary files
	shared_ptr<InputFile> CreateUnbackedFile(const string &path);
	//! A file the caller writes under [path], replacing earlier opens of [path]: its writer creates the blocks, and
	//! once its ExternalFileCacheAppender finishes, opens of [path] share them like fetched blocks
	shared_ptr<InputFile> CreateWrittenFile(const string &path);
	//! Drops the blocks of every open of [path]; current pins keep their bytes
	void ForgetFile(const string &path);
	//! Drops the blocks of every open of a file below [directory]; current pins keep their bytes
	void ForgetDirectory(const string &directory);

	//! Pins block [block] of [file], fetching it when missing; throws when the fetch fails
	FilePin Pin(InputFile &file, idx_t block);
	//! Pins the bytes of [offset, offset + size) of [file] that lie inside it, a pin per block they span, fetching the
	//! missing blocks together
	vector<FilePin> PinRange(InputFile &file, idx_t offset, idx_t size);
	//! PinRange for a reader that comes back for other bytes of [file] and cannot tell which: a miss in a file that
	//! fits in one request fetches all of it, and its later reads find it in memory
	vector<FilePin> PinRangeOfWhole(InputFile &file, idx_t offset, idx_t size);
	//! Pins the [size] bytes at [offset] of [file] in one piece: in their block when they lie in one, otherwise in a
	//! copy the pin owns; Ptr() and Size() are the bytes either way
	FilePin PinBytes(InputFile &file, idx_t offset, idx_t size);
	//! Pins the copy of the [size] bytes at [offset] of [file] that its readers share, making it when it is missing
	//! or was evicted. A file whose bytes the cache may not keep gets a copy the pin owns
	FilePin PinCopy(InputFile &file, idx_t offset, idx_t size);
	//! Pins the bytes of [offset, offset + size) of [file] like PinRange when all of their blocks are in memory; empty
	//! otherwise
	vector<FilePin> TryPinRange(InputFile &file, idx_t offset, idx_t size);
	//! Whether every block of [offset, offset + size) is in memory, in a temporary file or loading
	bool IsCached(const InputFile &file, idx_t offset, idx_t size) const;
	//! Whether a reader of [offset, offset + size) has nothing to fetch: the blocks are cached, or a copy of exactly
	//! these bytes is in memory
	bool IsHeld(const InputFile &file, idx_t offset, idx_t size) const;
	//! Starts fetching the missing blocks of [offset, offset + size), up to the bytes a file and the cache may have in
	//! flight, and returns whether the bytes were not held. Bytes a copy of which is in memory are not fetched
	bool Prefetch(InputFile &file, idx_t offset, idx_t size);
	//! Starts fetching the missing blocks of [ranges], where ranges closer than MERGE_GAP make one when that takes
	//! fewer requests; sorts and merges [ranges] in place. A HINT stops at the bytes a file and the cache may have in
	//! flight. Bytes a copy of which is in memory are not fetched
	void Prefetch(InputFile &file, std::span<FileRange> ranges, ExternalFileCachePrefetch kind);
	//! InputFile::ReadAhead
	idx_t ReadAhead(InputFile &file, idx_t offset, idx_t size, idx_t needed);
	void MarkUnused(const InputFile &file, idx_t offset, idx_t size) const;
	//! Reads [offset, offset + size) of [file] into [target] for a reader that keeps its own copy and reads the bytes
	//! once: pieces the cache holds are copied from it, the others come from the source straight into [target],
	//! concurrently, and never become cache blocks
	void ReadInto(InputFile &file, idx_t offset, data_ptr_t target, idx_t size);

	//! Whether external file reads go through the cache; turning it off drops the blocks of backed files, while current
	//! pins keep their bytes
	void SetEnabled(bool enabled);
	bool IsEnabled() const {
		return enabled.load(std::memory_order_relaxed);
	}

	ExternalFileCacheStatistics GetStatistics() const;
	//! The cached blocks of files the cache can name, in memory or only in temporary files
	vector<CachedFileInformation> GetCachedFileInformation() const;

	//! Waits for the fetch tasks in flight and refuses new ones; the database calls it before its scheduler stops
	void StopFetching();

private:
	friend class ExternalFileCacheAppender;
	friend class ExternalFileCacheFetchTask;

	//! Ends the write of [file], which wrote the version [written] describes: its blocks stay, and TryGetFile reports
	//! [written] for it
	void PublishWrittenFile(InputFile &file, const CacheValidationInfo &written);
	//! Pins a new block [block] of the unbacked or written [file] for its writer, holding whatever its buffer held
	//! before; ExternalFileCacheAppender fills it up to the end of the file
	FilePin CreateBlock(InputFile &file, idx_t block, optional_ptr<BufferPoolReservation> source);
	//! Extends the unbacked [file] to [size] bytes once its blocks are written
	void SetUnbackedSize(InputFile &file, idx_t size);

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
		ExternalFileCacheFileInfo info;
		//! The blocks of this version of the file, which its open files share
		shared_ptr<ExternalFileCacheBlocks> blocks;
	};

	//! Pins the block in [slot] when it is in memory
	bool PinLoaded(ExternalFileCacheSlot &slot, idx_t size, FilePin &result) const;
	//! The memory in [slot] of a block of [file], registered for a buffer of [size] bytes when the slot has none: the
	//! bytes of the block that lie inside a file that is fetched, a whole block of a file that is written
	BlockMemory &MemoryOf(const InputFile &file, ExternalFileCacheSlot &slot, idx_t size);
	optional_ptr<BlockMemory> ClaimMissing(InputFile &file, idx_t block);
	//! Lets the slots of [blocks] go, destroying their blocks and the copies of them at once or with their last pin;
	//! the copies its readers share go the same way
	void DropBlocks(ExternalFileCacheBlocks &blocks);
	//! Whether the copy of exactly [offset, offset + size) of [file] that its readers share is in memory
	bool HoldsCopy(const InputFile &file, idx_t offset, idx_t size) const;
	//! Moves the freshness deadline of the blocks of [file] to [deadline], which a response to a fetch of them gave,
	//! when it is earlier: its open files and later opens of its path stop reusing the blocks then
	void TightenDeadline(const InputFile &file, timestamp_t deadline);
	//! Prefetch of a range no copy of which was looked for, the first [needed] bytes of which a reader reads next
	//! (InputFile::ReadAhead); returns where the bytes asked for end
	idx_t PrefetchBlocks(InputFile &file, idx_t offset, idx_t size, ExternalFileCachePrefetch kind, idx_t needed = 0);

	//! Gives [record] new blocks of [block_size] bytes for [info], returning the version it replaces; sets [sweep] when
	//! the record is new and the records are due a sweep
	optional<FileRecord> NewVersion(FileRecord &record, const ExternalFileCacheFileInfo &info, idx_t block_size,
	                                bool &sweep) DUCKDB_REQUIRES(file_lock);
	//! Drops the blocks of [replaced], a version of a file the records no longer name, and sweeps the records when
	//! [sweep]
	void RetireRecord(optional<FileRecord> replaced, bool sweep);
	//! Forgets the files no one has open and the cache holds no block of
	void SweepFiles();

	//! Plans a run of [file] from [block] up to [end]: a claimed block for each missing block while [admit] accepts
	//! the block and the run's bytes with it. A gap of up to MERGE_GAP of blocks it does not fetch is bridged when one
	//! request for the blocks on both sides takes fewer pieces than a request for each side, the rule of Prefetch for
	//! ranges; the run takes at most MAX_REQUEST and never ends in a gap
	template <class ADMIT>
	RunPlan PlanRun(InputFile &file, idx_t block, idx_t end, ADMIT &&admit);
	//! Reads the [claimed] blocks of the run of [file] from [first_block] with one request and gives back their
	//! claims, clearing each entry, also when it throws; when [pins] is not empty, pins each block read there. Null
	//! entries are blocks the run does not fetch. A block is handed out once its bytes are in, where the source tells
	//! of them: its readers do not wait for the rest of the request. A fetch without [pins] reads in the background
	//! unless a reader [awaited] it
	void Fetch(InputFile &file, idx_t first_block, std::span<optional_ptr<BlockMemory>> claimed,
	           std::span<FilePin> pins, bool awaited = false);
	//! Fetches the runs of [plans] of [file] as concurrent pieces of at most PieceBlocks, pinning each block read in
	//! [pins], which start at block [first]; throws the first error, and leaves the claims of pieces it did not fetch
	void FetchConcurrently(InputFile &file, vector<RunPlan> &plans, idx_t first, vector<FilePin> &pins);
	//! Waits while [memory] loads. A fetch of [memory] that no worker took yet starts on a thread of its own, so the
	//! wait depends on no worker and still ends with the first block of the fetch; it runs here when the scheduler has
	//! no thread for it. When the scheduler has no workers to run queued fetches, runs them until [memory] loads
	void WaitForLoad(BlockMemory &memory);
	struct PendingFetch;
	bool TakeFetch(PendingFetch &fetch);
	void TakeFetchLocked(PendingFetch &fetch) DUCKDB_REQUIRES(fetch_lock);
	void ScheduleFetchLocked(shared_ptr<PendingFetch> fetch) DUCKDB_REQUIRES(fetch_lock);

	//! Fetches the [claimed] blocks of the run of [file] from [first_block] in the background, a task per piece of at
	//! most PieceBlocks; the piece of its first block is read like a caller's when a reader [awaited] that block
	void ScheduleFetch(InputFile &file, idx_t first_block, vector<optional_ptr<BlockMemory>> claimed, bool awaited);
	idx_t PieceBlocks(const InputFile &file) const;
	idx_t NewBlockSize() const;
	//! Takes a queued fetch task, which a waiter runs itself
	bool TakeFetchTask(shared_ptr<Task> &task);
	//! Reads [targets] of [file] at [offset] from [source], its source. A read of a remote file first waits for room in
	//! MAX_BURST: a [background] one in arrival order and behind every read a caller is waiting for; it tells
	//! [progress], when given one, of the targets its bytes fill
	void ReadSource(InputFile &file, FileHandle &source, std::span<const FileReadTarget> targets, idx_t offset,
	                bool background, optional_ptr<FileReadProgress> progress);

private:
	DatabaseInstance &db;
	BufferManager &buffer_manager;

	mutable annotated_mutex file_lock;
	unordered_map<string, FileRecord> files DUCKDB_GUARDED_BY(file_lock);
	static constexpr idx_t MIN_SWEEP_RECORDS = 1024;
	//! The record count at which the next open sweeps the records
	idx_t sweep_at DUCKDB_GUARDED_BY(file_lock) = MIN_SWEEP_RECORDS;
	atomic<bool> enabled {true};

	//! Background fetches: their executor is made by the first of them and goes when fetching stops, since the cache
	//! outlives the scheduler
	annotated_mutex fetch_lock;
	unique_ptr<TaskExecutor> fetch_tasks DUCKDB_GUARDED_BY(fetch_lock);
	unordered_map<const BlockMemory *, shared_ptr<PendingFetch>> pending_fetches DUCKDB_GUARDED_BY(fetch_lock);
	bool stopped DUCKDB_GUARDED_BY(fetch_lock) = false;
	atomic<idx_t> in_flight {0};

	//! The reads of remote sources: the bytes being read, the reads callers wait for, and the turns handed out to
	//! background reads and taken by them
	annotated_mutex read_lock;
	idx_t reading DUCKDB_GUARDED_BY(read_lock) = 0;
	idx_t awaited_reads DUCKDB_GUARDED_BY(read_lock) = 0;
	idx_t read_turns DUCKDB_GUARDED_BY(read_lock) = 0;
	idx_t read_turn DUCKDB_GUARDED_BY(read_lock) = 0;

	atomic<idx_t> misses {0};
	atomic<idx_t> fetched_bytes {0};
	atomic<idx_t> fetches {0};
};

} // namespace duckdb
