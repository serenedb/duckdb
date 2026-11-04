//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_pool.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/temporary_file_information.hpp"

namespace duckdb {

class TemporaryMemoryManager;
class ObjectCache;
struct EvictionQueue;

//! The BufferPool is in charge of handling memory management for one or more databases. It defines memory limits
//! and implements priority eviction among all users of the pool.
class BufferPool {
	friend class BlockMemory;
	friend class BlockHandle;
	friend class BlockManager;
	friend class BufferManager;
	friend class StandardBufferManager;

public:
	BufferPool(BlockAllocator &block_allocator, idx_t maximum_memory, bool track_eviction_timestamps,
	           idx_t allocator_bulk_deallocation_flush_threshold, idx_t external_file_cache_min_memory);
	virtual ~BufferPool();

	//! Set a new memory limit to the buffer pool, throws an exception if the new limit is too low and not enough
	//! blocks can be evicted
	void SetLimit(idx_t limit, const char *exception_postscript);

	//! If bulk deallocation larger than this occurs, flush outstanding allocations
	void SetAllocatorBulkDeallocationFlushThreshold(idx_t threshold);
	idx_t GetAllocatorBulkDeallocationFlushThreshold();

	void SetExternalFileCacheMinMemory(idx_t size);
	idx_t GetExternalFileCacheMinMemory() const;

	void UpdateUsedMemory(MemoryTag tag, int64_t size);

	idx_t GetUsedMemory(bool flush = true) const;

	idx_t GetMaxMemory() const;

	virtual idx_t GetOperatorMemoryLimit() const;

	TemporaryMemoryManager &GetTemporaryMemoryManager();

	vector<EvictionQueueInformation> GetEvictionQueueInfo() const;

	//! Take per-database ObjectCache under buffer pool's memory management.
	//! Notice, object cache should be registered for at most once, otherwise InvalidInput exception is thrown.
	void SetObjectCache(ObjectCache *object_cache_p) {
		if (object_cache != nullptr) {
			throw InvalidInputException("Object cache has already been registered in buffer pool, cannot re-register!");
		}
		object_cache = object_cache_p;
	}

protected:
	//! Evict blocks until the currently used memory + extra_memory fit, returns false if this was not possible
	//! (i.e. not enough blocks could be evicted)
	//! If the "buffer" argument is specified AND the system can find a buffer to re-use for the given allocation size
	//! "buffer" will be made to point to the re-usable memory. Note that this is not guaranteed.
	//! Returns a pair. result.first indicates if eviction was successful. result.second contains the
	//! reservation handle, which can be moved to the BlockHandle that will own the reservation.
	struct EvictionResult {
		bool success;
		TempBufferPoolReservation reservation;
	};
	virtual EvictionResult EvictBlocks(QueryContext context, MemoryTag tag, idx_t extra_memory, idx_t memory_limit,
	                                   unique_ptr<FileBuffer> *buffer = nullptr);
	virtual EvictionResult EvictBlocksInternal(QueryContext context, EvictionQueue &queue, MemoryTag tag,
	                                           idx_t extra_memory, idx_t memory_limit, idx_t keep,
	                                           unique_ptr<FileBuffer> *buffer);

	//! Evict object cache entries if needed.
	EvictionResult EvictObjectCacheEntries(MemoryTag tag, idx_t extra_memory, idx_t memory_limit);

	//! Return freed memory to the OS, optionally releasing up to extra_memory from the block allocator's pool
	void FlushAllocator(optional_idx extra_memory = optional_idx());
	//! Flush the allocator if the memory freed since the last flush could push resident memory past the limit
	void FlushOnBulkDeallocation();
	//! Freed memory that has to accumulate before the allocator is flushed
	idx_t GetBulkDeallocationFlushThreshold() const;

	//! Purge all blocks that haven't been pinned within the last N seconds
	idx_t PurgeAgedBlocks(uint32_t max_age_sec);

	//! A loaded block joins the eviction queue of its type and priority, and leaves it once it is unloaded or
	//! destroyed. Requires the block lock, or exclusive ownership of the block.
	void AddBlock(BlockMemory &memory);
	void RemoveBlock(BlockMemory &memory);
	//! Marks a block as used, which spares it one round of eviction. Takes no lock.
	bool TracksEvictionTimestamps() const {
		return track_eviction_timestamps;
	}
	void MarkUsed(BlockMemory &memory) {
		memory.SetReferenced();
		if (track_eviction_timestamps) {
			SetLRUTimestamp(memory);
		}
	}
	//! The blocks of [tag] evicted since start
	idx_t GetEvictions(MemoryTag tag) const;
	void CountEviction(const BlockMemory &memory);
	void SetLRUTimestamp(BlockMemory &memory);
	//! Gets the eviction queue for the specified type
	EvictionQueue &GetEvictionQueueForBlockMemory(const BlockMemory &memory);

	//! How many eviction queue types we have (BLOCK and EXTERNAL_FILE go into same queue, except EXTERNAL_FILE blocks
	//! that keep a copy, the blocks of remote files)
	static constexpr idx_t EVICTION_QUEUE_TYPES = 4;
	//! How many eviction queues we have for the different FileBufferTypes
	static constexpr idx_t BLOCK_AND_EXTERNAL_FILE_QUEUE_SIZE = 1;
	static constexpr idx_t REMOTE_FILE_QUEUE_SIZE = 1;
	static constexpr idx_t MANAGED_BUFFER_QUEUE_SIZE = 6;
	static constexpr idx_t TINY_BUFFER_QUEUE_SIZE = 1;
	//! Mapping and priority order for the eviction queues
	const array<idx_t, EVICTION_QUEUE_TYPES> eviction_queue_sizes;

protected:
	enum class MemoryUsageCaches {
		FLUSH,
		NO_FLUSH,
	};

	struct MemoryUsage {
		//! The maximum difference between memory statistics and actual usage is 2MB (64 * 32k)
		static constexpr idx_t MEMORY_USAGE_CACHE_COUNT = 64;
		static constexpr idx_t MEMORY_USAGE_CACHE_THRESHOLD = 32 << 10;
		static constexpr idx_t TOTAL_MEMORY_USAGE_INDEX = MEMORY_TAG_COUNT;
		using MemoryUsageCounters = array<atomic<int64_t>, MEMORY_TAG_COUNT + 1>;

		//! global memory usage counters
		MemoryUsageCounters memory_usage;
		//! cache memory usage to improve performance
		array<MemoryUsageCounters, MEMORY_USAGE_CACHE_COUNT> memory_usage_caches;

		MemoryUsage();

		idx_t GetUsedMemory(MemoryUsageCaches cache) {
			return GetUsedMemory(TOTAL_MEMORY_USAGE_INDEX, cache);
		}

		idx_t GetUsedMemory(MemoryTag tag, MemoryUsageCaches cache) {
			return GetUsedMemory((idx_t)tag, cache);
		}

		idx_t GetUsedMemory(idx_t index, MemoryUsageCaches cache) {
			if (cache == MemoryUsageCaches::NO_FLUSH) {
				auto used_memory = memory_usage[index].load(std::memory_order_relaxed);
				return used_memory > 0 ? static_cast<idx_t>(used_memory) : 0;
			}
			int64_t cached = 0;
			for (auto &cache : memory_usage_caches) {
				cached += cache[index].exchange(0, std::memory_order_relaxed);
			}
			auto used_memory = memory_usage[index].fetch_add(cached, std::memory_order_relaxed) + cached;
			return used_memory > 0 ? static_cast<idx_t>(used_memory) : 0;
		}

		void UpdateUsedMemory(MemoryTag tag, int64_t size);
	};

	//! The lock for changing the memory limit
	mutex limit_lock;
	//! The maximum amount of memory that the buffer manager can keep (in bytes)
	atomic<idx_t> maximum_memory;
	//! If bulk deallocation larger than this occurs, flush outstanding allocations
	atomic<idx_t> allocator_bulk_deallocation_flush_threshold;
	//! Freed memory is flushed once it exceeds this fraction of the memory limit
	static constexpr idx_t BULK_DEALLOCATION_FLUSH_DIVISOR = 16;
	atomic<idx_t> external_file_cache_min_memory;
	static constexpr idx_t EXTERNAL_FILE_CACHE_MIN_MEMORY_DIVISOR = 20;
	//! Record timestamps of buffer manager unpin() events. Usable by custom eviction policies.
	bool track_eviction_timestamps;
	//! Eviction queues
	vector<unique_ptr<EvictionQueue>> queues;
	//! Memory manager for concurrently used temporary memory, e.g., for physical operators
	unique_ptr<TemporaryMemoryManager> temporary_memory_manager;
	//! To improve performance, MemoryUsage maintains counter caches based on current cpu or thread id,
	//! and only updates the global counter when the cache value exceeds a threshold.
	//! Therefore, the statistics may have slight differences from the actual memory usage.
	mutable MemoryUsage memory_usage;
	//! The block allocator
	BlockAllocator &block_allocator;
	//! Per-database singleton object cache managed by buffer pool.
	optional_ptr<ObjectCache> object_cache = nullptr;
	array<atomic<idx_t>, MEMORY_TAG_COUNT> evictions_per_tag {};
};

} // namespace duckdb
