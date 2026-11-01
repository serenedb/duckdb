//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/block_handle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

// Forward declaration.
class BlockManager;
class BufferHandle;
class BufferManager;
class DatabaseInstance;
class BlockHandle;
class BufferPool;
struct EvictionQueue;

using BlockLock = unique_lock<mutex>;

enum class CanUnloadResult : uint8_t { CAN_UNLOAD, ALREADY_UNLOADED, PINNED, NO_TEMP_DIRECTORY };

class alignas(64) BlockMemory : public enable_shared_from_this<BlockMemory> {
	friend class BufferPool;
	friend struct EvictionQueue;

public:
	BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p, idx_t block_alloc_size);
	BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p, unique_ptr<FileBuffer> buffer_p,
	            DestroyBufferUpon destroy_buffer_upon_p, idx_t size_p, BufferPoolReservation &&reservation);
	~BlockMemory();

public:
	//! Returns a const reference to the buffer manager.
	const BufferManager &GetBufferManager() const {
		return buffer_manager;
	}
	//! Returns a reference to the buffer manager.
	BufferManager &GetBufferManager() {
		return buffer_manager;
	}
	//! Returns the block ID.
	block_id_t BlockId() const {
		return block_id;
	}
	//! Locks the memory block.
	BlockLock GetLock() {
		return BlockLock(lock);
	}
	//! Verification-only: ensure that the lock matches this memory's lock.
	void VerifyMutex(BlockLock &l) const {
		D_ASSERT(l.owns_lock());
		D_ASSERT(l.mutex() == &lock);
	}
	//! Returns the block state.
	BlockState GetState() const {
		return state;
	}
	//! Sets the block state.
	void SetState(BlockState state_p) {
		state = state_p;
		if (state_p == BlockState::BLOCK_LOADED) {
			readers.fetch_or(LOADED, std::memory_order_seq_cst);
		} else {
			readers.fetch_and(~LOADED, std::memory_order_seq_cst);
		}
	}
	//! Returns true, if the block state is BLOCK_UNLOADED.
	bool IsUnloaded() const {
		return state == BlockState::BLOCK_UNLOADED;
	}
	//! The reader word: the number of readers, whether the block is loaded, and whether it is orphaned. Readers pin
	//! a loaded block without the block lock, so it is unloaded only together with checking that it has no readers.
	static constexpr int32_t READERS = (int32_t(1) << 29) - 1;
	static constexpr int32_t LOADED = int32_t(1) << 29;
	//! Set once the block handle is gone while readers remain, which then own the memory.
	static constexpr int32_t ORPHANED = int32_t(1) << 30;
	//! Returns the number of readers.
	int32_t GetReaders() const {
		return readers.load(std::memory_order_seq_cst) & READERS;
	}
	//! Increments the number of readers prior to returning it.
	int32_t IncrementReaders() {
		return (readers.fetch_add(1, std::memory_order_seq_cst) + 1) & READERS;
	}
	//! Removes a reader, returning the reader word before.
	int32_t ReleaseReader() {
		return readers.fetch_sub(1, std::memory_order_seq_cst);
	}
	//! Adds a reader without the block lock, if the block is loaded.
	bool TryPinLoaded() {
		if (readers.fetch_add(1, std::memory_order_seq_cst) & LOADED) {
			return true;
		}
		readers.fetch_sub(1, std::memory_order_seq_cst);
		return false;
	}
	//! Marks the block unloaded if it has no readers besides [own_readers], returning whether it did.
	bool TryMarkUnloaded(int32_t own_readers = 0) {
		if ((readers.fetch_and(~LOADED, std::memory_order_seq_cst) & READERS) > own_readers) {
			readers.fetch_or(LOADED, std::memory_order_seq_cst);
			return false;
		}
		state = BlockState::BLOCK_UNLOADED;
		return true;
	}
	//! Called once the block handle is gone: readers that remain keep the memory alive, the last one frees it.
	void Orphan(BlockLock &l, const shared_ptr<BlockMemory> &self) {
		VerifyMutex(l);
		// Tiny buffers count no readers, and their pins own the block.
		if (buffer_type == FileBufferType::TINY_BUFFER) {
			return;
		}
		if (readers.fetch_or(ORPHANED, std::memory_order_seq_cst) & READERS) {
			orphan_owner = self;
		}
	}
	//! Hands over the memory's ownership of itself, taken by the last reader of an orphaned block.
	shared_ptr<BlockMemory> TakeOrphanOwner(BlockLock &l) {
		VerifyMutex(l);
		return std::move(orphan_owner);
	}
	//! The block handle, while it exists.
	shared_ptr<BlockHandle> GetOwner() const {
		return owner.lock();
	}
	void SetOwner(const shared_ptr<BlockHandle> &owner_p) {
		owner = owner_p;
	}
	//! Returns the memory tag.
	MemoryTag GetMemoryTag() const {
		return tag;
	}
	//! Returns the file buffer type.
	FileBufferType GetBufferType() const {
		return buffer_type;
	}
	//! Returns a reference to the unique file buffer pointer while holding the block lock.
	unique_ptr<FileBuffer> &GetBuffer(BlockLock &l) {
		VerifyMutex(l);
		return GetBuffer();
	}
	//! Returns a reference to the unique file buffer pointer.
	unique_ptr<FileBuffer> &GetBuffer() {
		return buffer;
	}
	//! Sets the file buffer.
	void SetBuffer(unique_ptr<FileBuffer> buffer_p) {
		buffer = std::move(buffer_p);
	}
	//! Marks the block as used since eviction last passed it.
	void SetReferenced() {
		if (!referenced.load(std::memory_order_relaxed)) {
			referenced.store(true, std::memory_order_relaxed);
		}
	}
	//! Clears the mark, returning whether it was set.
	bool TakeReferenced() {
		return referenced.load(std::memory_order_relaxed) && referenced.exchange(false, std::memory_order_relaxed);
	}
	//! Whether the block is loaded and nobody pins it, read without the block lock.
	bool IsUnpinnedAndLoaded() const {
		return (readers.load(std::memory_order_relaxed) & (READERS | LOADED)) == LOADED;
	}
	//! Get the LRU timestamp.
	int64_t GetLRUTimestamp() const {
		return lru_timestamp_msec;
	}
	//! Set the LRU timestamp.
	void SetLRUTimestamp(int64_t timestamp_msec) {
		lru_timestamp_msec = timestamp_msec;
	}
	//! Set the buffer destruction policy.
	void SetDestroyBufferUpon(DestroyBufferUpon destroy_buffer_upon_p) {
		destroy_buffer_upon = destroy_buffer_upon_p;
	}
	//! Returns true, if the buffer is destroyed once its last reader unpins it.
	bool DestroyedUponUnpin() const {
		return destroy_buffer_upon == DestroyBufferUpon::UNPIN;
	}
	//! Returns true, if the buffer cannot be destroyed, but must be kept alive in a temporary file.
	bool MustWriteToTemporaryFile() const {
		return destroy_buffer_upon == DestroyBufferUpon::BLOCK;
	}
	void SetHasTemporaryFile(bool has_temporary_file_p) {
		has_temporary_file = has_temporary_file_p;
	}
	//! Returns the memory usage.
	idx_t GetMemoryUsage() const {
		return memory_usage;
	}
	//! Sets the memory usage.
	void SetMemoryUsage(idx_t usage) {
		memory_usage = usage;
	}
	//! Get the memory charge while holding the block lock.
	BufferPoolReservation &GetMemoryCharge(BlockLock &l) {
		VerifyMutex(l);
		return GetMemoryCharge();
	}
	//! Get the memory charge.
	BufferPoolReservation &GetMemoryCharge() {
		return memory_charge;
	}
	//! Resize the memory charge.
	void ResizeMemory(BlockLock &l, idx_t alloc_size) {
		VerifyMutex(l);
		memory_charge.Resize(alloc_size);
	}
	//! Merge two memory charges.
	void MergeMemoryReservation(BlockLock &l, BufferPoolReservation reservation) {
		VerifyMutex(l);
		memory_charge.Merge(std::move(reservation));
	}
	//! Returns true, if there is a swizzled memory pointer, else false.
	bool IsSwizzled() const {
		return !unswizzled;
	}
	//! Sets the swizzled memory pointer.
	void SetSwizzling(const char *unswizzler) {
		unswizzled = unswizzler;
	}
	//! Sets the eviction queue index, moving a loaded block to that queue.
	void SetEvictionQueueIndex(const idx_t index);
	//! Returns the eviction queue index.
	idx_t GetEvictionQueueIndex() const {
		return eviction_queue_idx;
	}

public:
	void ChangeMemoryUsage(BlockLock &l, int64_t delta);
	void ConvertToPersistent(BlockLock &l, BlockHandle &new_block, unique_ptr<FileBuffer> new_buffer);
	void ResizeBuffer(BlockLock &l, idx_t block_size, idx_t block_header_size, int64_t memory_delta);
	//! Returns whether the block can be unloaded or the reason why it cannot be unloaded.
	//! The state here can change if the block lock is held.
	//! However, this method does not hold the block lock.
	CanUnloadResult CanUnload() const;
	unique_ptr<FileBuffer> UnloadAndTakeBlock(BlockLock &l, QueryContext context = QueryContext());
	void Unload(BlockLock &l, QueryContext context = QueryContext());

private:
	//! A reference to the buffer manager.
	BufferManager &buffer_manager;
	//! The block id of the block.
	const block_id_t block_id;
	//! The block-level lock.
	mutex lock;
	//! Whether the block is loaded or unloaded.
	atomic<BlockState> state;
	//! The reader word, on a cache line of its own: concurrent readers of a block write it, and the fields they
	//! read stay shared.
	alignas(64) atomic<int32_t> readers;
	//! The memory tag.
	alignas(64) const MemoryTag tag;
	//! The file buffer type.
	const FileBufferType buffer_type;
	//! A pointer to the loaded data, if any.
	unique_ptr<FileBuffer> buffer;
	//! The eviction queue the loaded block is in, and its stripe and slot there; guarded by that stripe's lock.
	optional_ptr<EvictionQueue> queue;
	idx_t queue_stripe = 0;
	idx_t queue_slot = 0;
	//! The block handle of this memory.
	weak_ptr<BlockHandle> owner;
	//! Set when the block handle went away while readers remained; the last reader releases it.
	shared_ptr<BlockMemory> orphan_owner;
	//! Whether the block was unpinned again since its queue entry was added.
	atomic<bool> referenced {false};
	bool has_temporary_file = false;
	//! The LRU timestamp for age-based eviction.
	atomic<int64_t> lru_timestamp_msec;
	//! When to destroy the data buffer.
	atomic<DestroyBufferUpon> destroy_buffer_upon;
	//! The memory usage of the block when loaded.
	//! Determines the memory to reserve when pinning/loading an unloaded block.
	atomic<idx_t> memory_usage;
	//! The current memory reservation/usage.
	BufferPoolReservation memory_charge;
	//! Swizzled memory pointers.
	const char *unswizzled;
	//! The eviction queue index, currently only FileBufferType::MANAGED_BUFFER.
	atomic<idx_t> eviction_queue_idx;
};

class alignas(64) BlockHandle : public enable_shared_from_this<BlockHandle> {
public:
	BlockHandle(BlockManager &block_manager, block_id_t block_id, MemoryTag tag);
	BlockHandle(BlockManager &block_manager, block_id_t block_id, MemoryTag tag, unique_ptr<FileBuffer> buffer,
	            DestroyBufferUpon destroy_buffer_upon, idx_t size, BufferPoolReservation &&reservation);
	~BlockHandle();

public:
	//! Returns a reference to the block manager.
	BlockManager &GetBlockManager() const {
		return block_manager;
	}
	//! Returns the block id.
	block_id_t BlockId() const {
		return block_id;
	}
	//! Returns the block allocation size of this block.
	idx_t GetBlockAllocSize() const {
		return block_alloc_size;
	}
	//! Returns the block header size including the 8-byte checksum.
	idx_t GetBlockHeaderSize() const {
		return block_header_size;
	}
	//! Returns the size of the block that is available for usage, as determined by the block manager that created the
	//! block. The block_alloc_size can differ from the memory_usage for blocks managed by the temporary block manager,
	//! thus, this should only be called for persistent blocks.
	idx_t GetBlockSize() const {
		return block_alloc_size - block_header_size;
	}
	//! Returns a const reference to the memory of a block.
	const BlockMemory &GetMemory() const {
		return memory;
	}
	//! Returns a reference to the memory of a block.
	BlockMemory &GetMemory() {
		return memory;
	}

public:
	BufferHandle LoadFromBuffer(BlockLock &l, data_ptr_t data, unique_ptr<FileBuffer> reusable_buffer,
	                            BufferPoolReservation reservation);
	BufferHandle Load(QueryContext context, unique_ptr<FileBuffer> buffer = nullptr);

private:
	//! The block manager, which loads the block.
	BlockManager &block_manager;

	//! The block allocation size, which is determined by the block manager creating the block.
	//! For non-temporary block managers the block_alloc_size corresponds to the memory_usage.
	//! If we are pinning/loading an unloaded block, then we know how much memory to reserve.
	//! This is NOT the actual memory available on a block.
	idx_t block_alloc_size;
	//! The size of the block header, including the checksum.
	idx_t block_header_size;
	//! The block id of the block.
	const block_id_t block_id;

	//! Pointer to the underlying memory of the block.
	const shared_ptr<BlockMemory> memory_p;
	//! Memory for fast access to the block memory.
	BlockMemory &memory;
};

} // namespace duckdb
