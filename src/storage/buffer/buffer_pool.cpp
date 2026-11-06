#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

static constexpr idx_t REMOTE_FILE_QUEUE_TYPE = 1;

static idx_t EvictionQueueTypeIdx(const BlockMemory &memory) {
	switch (memory.GetBufferType()) {
	case FileBufferType::BLOCK:
		return 0; // Evict these first (cheap, just free)
	case FileBufferType::EXTERNAL_FILE:
		return memory.KeepsCopy() ? REMOTE_FILE_QUEUE_TYPE : 0;
	case FileBufferType::MANAGED_BUFFER:
		return 2; // Then these (have to write to storage)
	case FileBufferType::TINY_BUFFER:
		return 3; // Evict tiny buffers last (last resort)
	default:
		throw InternalException("Unknown FileBufferType in EvictionQueueTypeIdx");
	}
}

static vector<FileBufferType> EvictionQueueTypeIdxToFileBufferTypes(const idx_t &queue_type_idx) {
	switch (queue_type_idx) {
	case 0:
		return {FileBufferType::BLOCK, FileBufferType::EXTERNAL_FILE};
	case REMOTE_FILE_QUEUE_TYPE:
		return {FileBufferType::EXTERNAL_FILE};
	case 2:
		return {FileBufferType::MANAGED_BUFFER};
	case 3:
		return {FileBufferType::TINY_BUFFER};
	default:
		throw InternalException("Unknown queue type index in EvictionQueueTypeIdxToFileBufferTypes");
	}
}

//! A clock over the loaded blocks of one type and priority. Pins and unpins never touch it: a block joins it once
//! it is loaded and leaves it once it is unloaded or destroyed. Eviction moves the hand past pinned blocks and gives
//! used blocks one more round. Blocks join the stripe of the CPU they are loaded on, so threads that allocate and
//! free blocks at once take different locks; eviction goes around the stripes in turn.
struct EvictionQueue {
public:
	EvictionQueue(const vector<FileBufferType> &file_buffer_types_p, bool counts_bytes_p)
	    : file_buffer_types(file_buffer_types_p), counts_bytes(counts_bytes_p) {
	}

	void Add(BlockMemory &memory) {
		const auto index = TaskScheduler::GetEstimatedCPUId() % STRIPES;
		auto &stripe = stripes[index];
		annotated_lock_guard<annotated_mutex> guard(stripe.lock);
		idx_t slot;
		if (stripe.free_slots.empty()) {
			slot = stripe.slots.size();
			stripe.slots.push_back(&memory);
		} else {
			slot = stripe.free_slots.back();
			stripe.free_slots.pop_back();
			stripe.slots[slot] = &memory;
		}
		memory.queue = this;
		memory.queue_stripe = index;
		memory.queue_slot = slot;
		stripe.blocks.store(stripe.blocks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		stripe.insertions.store(stripe.insertions.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		if (counts_bytes) {
			memory.queue_bytes = memory.GetMemoryUsage();
			bytes.fetch_add(memory.queue_bytes, std::memory_order_relaxed);
		}
	}

	void Remove(BlockMemory &memory) {
		auto &stripe = stripes[memory.queue_stripe];
		annotated_lock_guard<annotated_mutex> guard(stripe.lock);
		D_ASSERT(stripe.slots[memory.queue_slot] == &memory);
		stripe.slots[memory.queue_slot] = nullptr;
		stripe.free_slots.push_back(memory.queue_slot);
		memory.queue = nullptr;
		stripe.blocks.store(stripe.blocks.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
		if (counts_bytes) {
			bytes.fetch_sub(memory.queue_bytes, std::memory_order_relaxed);
		}
	}

	//! The next block to evict, or nothing once the budget is spent: each block and each stripe passed costs one. A
	//! budget of INVALID_INDEX starts at two rounds of the clock: one clears the marks of used blocks, the next evicts
	//! them.
	shared_ptr<BlockMemory> NextVictim(idx_t &budget) {
		if (budget == DConstants::INVALID_INDEX) {
			budget = 2 * (GetApproximateSize() + STRIPES);
		}
		while (budget > 0) {
			auto &stripe = stripes[next_stripe.load(std::memory_order_relaxed) % STRIPES];
			{
				annotated_lock_guard<annotated_mutex> guard(stripe.lock);
				// Go around this stripe once, then on to the next.
				for (idx_t passed = 0; passed < stripe.slots.size() && budget > 0; passed++) {
					stripe.hand = stripe.hand + 1 < stripe.slots.size() ? stripe.hand + 1 : 0;
					auto memory = stripe.slots[stripe.hand];
					if (!memory) {
						continue;
					}
					budget--;
					if (!memory->IsUnpinnedAndLoaded() || memory->TakeReferenced()) {
						continue;
					}
					// A block being destroyed waits for the lock to leave the queue, and cannot be locked anymore.
					auto victim = memory->weak_from_this().lock();
					if (victim) {
						return victim;
					}
				}
			}
			budget -= budget > 0;
			next_stripe.fetch_add(1, std::memory_order_relaxed);
		}
		return nullptr;
	}

	bool HasFileBufferType(const FileBufferType &type) const {
		return std::find(file_buffer_types.begin(), file_buffer_types.end(), type) != file_buffer_types.end();
	}
	idx_t GetApproximateSize() const {
		idx_t result = 0;
		for (auto &stripe : stripes) {
			result += stripe.blocks.load(std::memory_order_relaxed);
		}
		return result;
	}
	idx_t GetBytes() const {
		D_ASSERT(counts_bytes);
		return bytes.load(std::memory_order_relaxed);
	}
	idx_t GetTotalInsertions() const {
		idx_t result = 0;
		for (auto &stripe : stripes) {
			result += stripe.insertions.load(std::memory_order_relaxed);
		}
		return result;
	}

public:
	//! The type of the buffers in this queue (for verification only)
	const vector<FileBufferType> file_buffer_types;

private:
	static constexpr idx_t STRIPES = 64;
	struct alignas(64) Stripe {
		annotated_mutex lock;
		//! The blocks in the stripe, and the slots of blocks that left it
		vector<optional_ptr<BlockMemory>> slots DUCKDB_GUARDED_BY(lock);
		vector<idx_t> free_slots DUCKDB_GUARDED_BY(lock);
		//! The clock hand
		idx_t hand DUCKDB_GUARDED_BY(lock) = 0;
		//! Written under the lock, read without it
		atomic<idx_t> blocks {0};
		atomic<idx_t> insertions {0};
	};
	array<Stripe, STRIPES> stripes;
	//! The stripe eviction looks at
	atomic<idx_t> next_stripe {0};
	const bool counts_bytes;
	atomic<idx_t> bytes {0};
};

BufferPool::BufferPool(BlockAllocator &block_allocator, idx_t maximum_memory,
                       idx_t allocator_bulk_deallocation_flush_threshold, idx_t external_file_cache_min_memory)
    : eviction_queue_sizes({BLOCK_AND_EXTERNAL_FILE_QUEUE_SIZE, REMOTE_FILE_QUEUE_SIZE, MANAGED_BUFFER_QUEUE_SIZE,
                            TINY_BUFFER_QUEUE_SIZE}),
      maximum_memory(maximum_memory),
      allocator_bulk_deallocation_flush_threshold(allocator_bulk_deallocation_flush_threshold),
      external_file_cache_min_memory(external_file_cache_min_memory),
      temporary_memory_manager(make_uniq<TemporaryMemoryManager>()), block_allocator(block_allocator) {
	for (idx_t queue_type_idx = 0; queue_type_idx < EVICTION_QUEUE_TYPES; queue_type_idx++) {
		const auto types = EvictionQueueTypeIdxToFileBufferTypes(queue_type_idx);
		const auto &type_queue_size = eviction_queue_sizes[queue_type_idx];
		for (idx_t queue_idx = 0; queue_idx < type_queue_size; queue_idx++) {
			queues.push_back(make_uniq<EvictionQueue>(types, queue_type_idx == REMOTE_FILE_QUEUE_TYPE));
		}
	}
}
BufferPool::~BufferPool() {
}

void BufferPool::AddBlock(BlockMemory &memory) {
	if (memory.GetBufferType() == FileBufferType::TINY_BUFFER || memory.queue) {
		return;
	}
	GetEvictionQueueForBlockMemory(memory).Add(memory);
}

void BufferPool::RemoveBlock(BlockMemory &memory) {
	if (memory.queue) {
		memory.queue->Remove(memory);
	}
}

void BufferPool::CountEviction(const BlockMemory &memory) {
	evictions_per_tag[uint8_t(memory.GetMemoryTag())].fetch_add(1, std::memory_order_relaxed);
}

idx_t BufferPool::GetEvictions(MemoryTag tag) const {
	return evictions_per_tag[uint8_t(tag)].load(std::memory_order_relaxed);
}

EvictionQueue &BufferPool::GetEvictionQueueForBlockMemory(const BlockMemory &memory) {
	const auto &handle_buffer_type = memory.GetBufferType();

	// Get offset into eviction queues for this FileBufferType
	idx_t queue_index = 0;
	const auto handle_queue_type_idx = EvictionQueueTypeIdx(memory);
	for (idx_t type_idx = 0; type_idx < handle_queue_type_idx; type_idx++) {
		queue_index += eviction_queue_sizes[type_idx];
	}

	const auto &queue_size = eviction_queue_sizes[handle_queue_type_idx];
	// Adjust if eviction_queue_idx is set (idx == 0 -> add at back, idx >= queue_size -> add at front)
	auto eviction_queue_idx = memory.GetEvictionQueueIndex();
	if (eviction_queue_idx < queue_size) {
		queue_index += queue_size - eviction_queue_idx - 1;
	}

	D_ASSERT(queues[queue_index]->HasFileBufferType(handle_buffer_type));
	return *queues[queue_index];
}

void BufferPool::UpdateUsedMemory(MemoryTag tag, int64_t size) {
	memory_usage.UpdateUsedMemory(tag, size);
}

idx_t BufferPool::GetUsedMemory(bool flush) const {
	return memory_usage.GetUsedMemory(flush ? MemoryUsageCaches::FLUSH : MemoryUsageCaches::NO_FLUSH);
}

idx_t BufferPool::GetMaxMemory() const {
	return maximum_memory;
}

idx_t BufferPool::GetOperatorMemoryLimit() const {
	return GetMaxMemory();
}

TemporaryMemoryManager &BufferPool::GetTemporaryMemoryManager() {
	return *temporary_memory_manager;
}

BufferPool::EvictionResult BufferPool::EvictObjectCacheEntries(MemoryTag tag, idx_t extra_memory, idx_t memory_limit) {
	TempBufferPoolReservation r(tag, *this, extra_memory);
	bool success = false;
	while (true) {
		const idx_t used = memory_usage.GetUsedMemory(MemoryUsageCaches::NO_FLUSH);
		if (used <= memory_limit) {
			success = true;
			break;
		}
		// Break if all entries cannot be evicted.
		if (object_cache->IsEmpty() ||
		    object_cache->EvictToReduceMemory(MaxValue(extra_memory, used - memory_limit)) == 0) {
			break;
		}
	}
	if (!success) {
		r.Resize(0);
	} else if (extra_memory > allocator_bulk_deallocation_flush_threshold) {
		FlushAllocator(extra_memory);
	}
	return {success, std::move(r)};
}

BufferPool::EvictionResult BufferPool::EvictBlocks(QueryContext context, MemoryTag tag, idx_t extra_memory,
                                                   idx_t memory_limit, unique_ptr<FileBuffer> *buffer) {
	FlushOnBulkDeallocation();
	auto &remote_files = *queues[BLOCK_AND_EXTERNAL_FILE_QUEUE_SIZE];
	const auto remote_floor = GetExternalFileCacheMinMemory();
	for (auto &queue : queues) {
		const auto keep = queue.get() == &remote_files ? remote_floor : 0;
		auto block_result = EvictBlocksInternal(context, *queue, tag, extra_memory, memory_limit, keep, buffer);
		if (block_result.success) {
			return block_result;
		}
	}
	if (remote_floor > 0 && remote_files.GetBytes() > 0) {
		auto block_result = EvictBlocksInternal(context, remote_files, tag, extra_memory, memory_limit, 0, buffer);
		if (block_result.success) {
			return block_result;
		}
	}

	// Evict object cache, which is usually used to cache metadata and configs, when flushing buffer blocks alone is not
	// enough to limit overall memory consumption.
	return EvictObjectCacheEntries(tag, extra_memory, memory_limit);
}

BufferPool::EvictionResult BufferPool::EvictBlocksInternal(QueryContext context, EvictionQueue &queue, MemoryTag tag,
                                                           idx_t extra_memory, idx_t memory_limit, idx_t keep,
                                                           unique_ptr<FileBuffer> *buffer) {
	TempBufferPoolReservation r(tag, *this, extra_memory);
	bool found = false;

	if (memory_usage.GetUsedMemory(MemoryUsageCaches::NO_FLUSH) <= memory_limit) {
		if (extra_memory > allocator_bulk_deallocation_flush_threshold) {
			FlushAllocator(extra_memory);
		}
		return {true, std::move(r)};
	}

	idx_t budget = DConstants::INVALID_INDEX;
	while (keep == 0 || queue.GetBytes() > keep) {
		auto victim = queue.NextVictim(budget);
		if (!victim) {
			break;
		}
		auto lock = victim->GetLock();
		if (victim->CanUnload() != CanUnloadResult::CAN_UNLOAD) {
			continue;
		}
		const auto &victim_buffer = *victim->GetBuffer(lock);
		if (buffer && victim_buffer.OwnsInternalBuffer() && victim_buffer.AllocSize() == extra_memory) {
			// we can re-use the memory directly
			*buffer = victim->UnloadAndTakeBlock(lock, context);
			if (*buffer) {
				CountEviction(*victim);
				found = true;
				break;
			}
			continue;
		}
		victim->Unload(lock, context);
		if (victim->IsUnloaded()) {
			CountEviction(*victim);
		}
		if (memory_usage.GetUsedMemory(MemoryUsageCaches::NO_FLUSH) <= memory_limit) {
			found = true;
			break;
		}
	}

	if (!found) {
		r.Resize(0);
	} else if (extra_memory > allocator_bulk_deallocation_flush_threshold) {
		FlushAllocator(extra_memory);
	}

	return {found, std::move(r)};
}

void BufferPool::SetLimit(idx_t limit, const char *exception_postscript) {
	lock_guard<mutex> l_lock(limit_lock);
	// try to evict until the limit is reached
	if (!EvictBlocks(QueryContext(), MemoryTag::EXTENSION, 0, limit).success) {
		throw OutOfMemoryException(
		    "Failed to change memory limit to %lld: could not free up enough memory for the new limit%s", limit,
		    exception_postscript);
	}
	idx_t old_limit = maximum_memory;
	// set the global maximum memory to the new limit if successful
	maximum_memory = limit;
	// evict again
	if (!EvictBlocks(QueryContext(), MemoryTag::EXTENSION, 0, limit).success) {
		// failed: go back to old limit
		maximum_memory = old_limit;
		throw OutOfMemoryException(
		    "Failed to change memory limit to %lld: could not free up enough memory for the new limit%s", limit,
		    exception_postscript);
	}
	block_allocator.FlushAll();
}

void BufferPool::SetAllocatorBulkDeallocationFlushThreshold(idx_t threshold) {
	allocator_bulk_deallocation_flush_threshold = threshold;
}

void BufferPool::SetExternalFileCacheMinMemory(idx_t size) {
	external_file_cache_min_memory = size;
}

idx_t BufferPool::GetExternalFileCacheMinMemory() const {
	const auto size = external_file_cache_min_memory.load(std::memory_order_relaxed);
	if (size == DConstants::INVALID_INDEX) {
		return maximum_memory.load(std::memory_order_relaxed) / EXTERNAL_FILE_CACHE_MIN_MEMORY_DIVISOR;
	}
	return size;
}

idx_t BufferPool::GetBulkDeallocationFlushThreshold() const {
	// freed memory that the allocator still holds must stay a small fraction of the memory limit
	return maximum_memory.load(std::memory_order_relaxed) / BULK_DEALLOCATION_FLUSH_DIVISOR;
}

void BufferPool::FlushAllocator(const optional_idx extra_memory) {
	block_allocator.FlushAll(extra_memory);
}

void BufferPool::FlushOnBulkDeallocation() {
	const auto threshold = GetBulkDeallocationFlushThreshold();
	const auto deallocated = block_allocator.GetDeallocatedSinceFlush();
	if (deallocated < threshold) {
		return;
	}
	// freed memory that the allocator still holds only matters once it could push us past the memory limit
	const auto used_memory = memory_usage.GetUsedMemory(MemoryUsageCaches::NO_FLUSH);
	const auto maximum = maximum_memory.load(std::memory_order_relaxed);
	if (used_memory + deallocated <= maximum) {
		return;
	}
	const auto claimed = block_allocator.ClaimDeallocated(threshold);
	if (claimed == 0) {
		return;
	}
	const auto unpurged = Allocator::UnpurgedBytes();
	if (unpurged.IsValid() && used_memory + unpurged.GetIndex() <= maximum) {
		block_allocator.AddDeallocated(unpurged.GetIndex());
		return;
	}
	FlushAllocator(claimed);
}

vector<EvictionQueueInformation> BufferPool::GetEvictionQueueInfo() const {
	static const char *QUEUE_TYPE_NAMES[] = {"BLOCK_AND_EXTERNAL_FILE", "REMOTE_FILE", "MANAGED_BUFFER", "TINY_BUFFER"};
	vector<EvictionQueueInformation> result;
	idx_t global_queue_index = 0;
	for (idx_t type_idx = 0; type_idx < EVICTION_QUEUE_TYPES; type_idx++) {
		const auto &type_queue_size = eviction_queue_sizes[type_idx];
		for (idx_t local_queue_idx = 0; local_queue_idx < type_queue_size; local_queue_idx++) {
			auto &queue = *queues[global_queue_index];
			EvictionQueueInformation info;
			info.queue_index = global_queue_index;
			info.queue_type = QUEUE_TYPE_NAMES[type_idx];
			info.approximate_size = queue.GetApproximateSize();
			info.dead_nodes = 0;
			info.total_insertions = queue.GetTotalInsertions();
			result.push_back(std::move(info));
			global_queue_index++;
		}
	}
	return result;
}

BufferPool::MemoryUsage::MemoryUsage() {
	for (auto &v : memory_usage) {
		v = 0;
	}
	for (auto &cache : memory_usage_caches) {
		for (auto &v : cache) {
			v = 0;
		}
	}
}

void BufferPool::MemoryUsage::UpdateUsedMemory(MemoryTag tag, int64_t size) {
	auto tag_idx = (idx_t)tag;
	if ((idx_t)AbsValue(size) < MEMORY_USAGE_CACHE_THRESHOLD) {
		// update cache and update global counter when cache exceeds threshold
		// Get corresponding cache slot based on current CPU core index
		// Two threads may access the same cache simultaneously,
		// ensuring correctness through atomic operations
		auto cache_idx = (idx_t)TaskScheduler::GetEstimatedCPUId() % MEMORY_USAGE_CACHE_COUNT;
		auto &cache = memory_usage_caches[cache_idx];
		auto new_tag_size = cache[tag_idx].fetch_add(size, std::memory_order_relaxed) + size;
		if ((idx_t)AbsValue(new_tag_size) >= MEMORY_USAGE_CACHE_THRESHOLD) {
			// cached tag memory usage exceeds threshold
			auto tag_size = cache[tag_idx].exchange(0, std::memory_order_relaxed);
			memory_usage[tag_idx].fetch_add(tag_size, std::memory_order_relaxed);
		}
		auto new_total_size = cache[TOTAL_MEMORY_USAGE_INDEX].fetch_add(size, std::memory_order_relaxed) + size;
		if ((idx_t)AbsValue(new_total_size) >= MEMORY_USAGE_CACHE_THRESHOLD) {
			// cached total memory usage exceeds threshold
			auto total_size = cache[TOTAL_MEMORY_USAGE_INDEX].exchange(0, std::memory_order_relaxed);
			memory_usage[TOTAL_MEMORY_USAGE_INDEX].fetch_add(total_size, std::memory_order_relaxed);
		}
	} else {
		// update global counter
		memory_usage[tag_idx].fetch_add(size, std::memory_order_relaxed);
		memory_usage[TOTAL_MEMORY_USAGE_INDEX].fetch_add(size, std::memory_order_relaxed);
	}
}

} // namespace duckdb
