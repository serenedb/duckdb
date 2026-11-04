#include "duckdb/storage/buffer/block_handle.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <absl/cleanup/cleanup.h>

namespace duckdb {

namespace {

mutex &LoadWait(const BlockMemory &memory) {
	static array<mutex, 64> waits;
	return waits[reinterpret_cast<uintptr_t>(&memory) / sizeof(BlockMemory) % waits.size()];
}

} // namespace

BlockMemory::BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p,
                         idx_t block_alloc_size_p)
    : BlockMemory(buffer_manager, block_id_p, tag_p, block_alloc_size_p, FileBufferType::BLOCK,
                  DestroyBufferUpon::BLOCK) {
}

BlockMemory::BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p,
                         idx_t block_alloc_size_p, FileBufferType buffer_type_p,
                         DestroyBufferUpon destroy_buffer_upon_p)
    : buffer_manager(buffer_manager), block_id(block_id_p), state(BlockState::BLOCK_UNLOADED), readers(0),
      buffer_type(buffer_type_p), destroy_buffer_upon(destroy_buffer_upon_p), tag(tag_p), buffer(nullptr),
      memory_usage(block_alloc_size_p), memory_charge(tag, buffer_manager.GetBufferPool()), unswizzled(nullptr),
      eviction_queue_idx(DConstants::INVALID_INDEX) {
}

void BlockMemory::WaitForLoad() {
	if (loading.load(std::memory_order_acquire) == 0) {
		return;
	}
	auto &wait = LoadWait(*this);
	TaskScheduler::BlockingScope blocking;
	wait.Lock();
	auto current = loading.load(std::memory_order_acquire);
	while (current == CLAIMED && !loading.compare_exchange_weak(current, AWAITED, std::memory_order_seq_cst)) {
	}
	if (current != 0) {
		wait.Await(absl::Condition(
		    +[](atomic<uint8_t> *flag) { return flag->load(std::memory_order_acquire) != AWAITED; }, &loading));
	}
	wait.Unlock();
}

void BlockMemory::FinishLoad() {
	uint8_t claimed = CLAIMED;
	if (loading.compare_exchange_strong(claimed, 0, std::memory_order_seq_cst)) {
		return;
	}
	D_ASSERT(claimed == AWAITED);
	auto &wait = LoadWait(*this);
	wait.Lock();
	loading.store(0, std::memory_order_seq_cst);
	wait.Unlock();
}

BlockMemory::BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p,
                         unique_ptr<FileBuffer> buffer_p, DestroyBufferUpon destroy_buffer_upon_p, idx_t size_p,
                         BufferPoolReservation &&reservation)
    : buffer_manager(buffer_manager), block_id(block_id_p), state(BlockState::BLOCK_LOADED), readers(LOADED),
      buffer_type(buffer_p->GetBufferType()), destroy_buffer_upon(destroy_buffer_upon_p), tag(tag_p),
      buffer(std::move(buffer_p)), memory_usage(size_p), memory_charge(tag, buffer_manager.GetBufferPool()),
      unswizzled(nullptr), eviction_queue_idx(DConstants::INVALID_INDEX) {
	memory_charge = std::move(reservation); // Moved to constructor body due to tidy check.
	loaded_buffer = buffer.get();
	loaded_data = buffer->GetDataMutable();
}

BlockMemory::~BlockMemory() { // NOLINT: allow internal exceptions
	// Leave the eviction queue first: eviction reads the queued block until then.
	GetBufferManager().GetBufferPool().RemoveBlock(*this);
	// The block memory is being destroyed, meaning that any unswizzled pointers are now binary junk.
	SetSwizzling(nullptr);
	D_ASSERT(!GetBuffer() || GetBuffer()->GetBufferType() == GetBufferType());

	// Erase the block memory, if it is still loaded.
	if (GetBuffer() && GetState() == BlockState::BLOCK_LOADED) {
		D_ASSERT(GetMemoryCharge().size > 0 || GetMemoryUsage() == 0);
		SetBuffer(nullptr);
		GetMemoryCharge().Resize(0);
	} else {
		D_ASSERT(GetMemoryCharge().size == 0);
	}

	try {
		if (BlockId() >= MAXIMUM_BLOCK && has_temporary_file) {
			// The memory buffer lives in memory.
			// Thus, it could've been offloaded to disk, and we should remove the file.
			GetBufferManager().DeleteTemporaryFile(*this);
		}
	} catch (...) { // NOLINT
		            // FIXME: log silent exceptions.
	}
}

void BlockMemory::ReleaseOrphan() {
	shared_ptr<BlockMemory> owner;
	auto lock = GetLock();
	owner = TakeOrphanOwner(lock);
}

void BlockMemory::ChangeMemoryUsage(BlockLock &l, int64_t delta) {
	VerifyMutex(l);
	// FIXME: Too clever ATM. The crux here is that the unsigned overflow is defined.
	// FIXME: It overflows twice to lead to the correct subtraction.
	D_ASSERT(delta < 0);
	memory_usage += static_cast<idx_t>(delta);
	GetMemoryCharge().Resize(GetMemoryUsage());
}

void BlockMemory::ConvertToPersistent(BlockLock &l, BlockHandle &new_block, unique_ptr<FileBuffer> new_buffer) {
	VerifyMutex(l);

	auto &new_block_memory = new_block.GetMemory();
	D_ASSERT(GetMemoryTag() == memory_charge.tag);
	if (GetMemoryTag() != new_block_memory.GetMemoryTag()) {
		const auto memory_charge_size = memory_charge.size;
		memory_charge.Resize(0);
		memory_charge.tag = new_block_memory.tag;
		memory_charge.Resize(memory_charge_size);
	}

	// Move the old block memory to the new block memory.
	new_block_memory.GetBuffer() = std::move(new_buffer);
	new_block_memory.SetState(BlockState::BLOCK_LOADED);
	new_block_memory.memory_usage = memory_usage.load();
	new_block_memory.memory_charge = std::move(memory_charge);

	// Clear the buffered data of this block.
	buffer_manager.GetBufferPool().RemoveBlock(*this);
	buffer.reset();
	SetState(BlockState::BLOCK_UNLOADED);
	memory_usage = 0;
}

void BlockMemory::SetEvictionQueueIndex(const idx_t index) {
	// The index can only be set once.
	D_ASSERT(eviction_queue_idx == DConstants::INVALID_INDEX);
	// It can only be set for managed buffers (for now).
	D_ASSERT(GetBufferType() == FileBufferType::MANAGED_BUFFER);
	auto l = GetLock();
	auto &pool = buffer_manager.GetBufferPool();
	const bool queued = queue != nullptr;
	pool.RemoveBlock(*this);
	eviction_queue_idx = index;
	if (queued) {
		pool.AddBlock(*this);
	}
}

void BlockMemory::ResizeBuffer(BlockLock &l, idx_t block_size, idx_t block_header_size, int64_t memory_delta) {
	// Resize and adjust the current memory.
	VerifyMutex(l);
	D_ASSERT(buffer);
	buffer->Resize(block_size, block_header_size);
	loaded_data = buffer->GetDataMutable();
	const auto new_memory_usage = NumericCast<idx_t>(NumericCast<int64_t>(memory_usage.load()) + memory_delta);
	memory_usage = new_memory_usage;
	D_ASSERT(memory_usage == buffer->AllocSize());
}

CanUnloadResult BlockMemory::CanUnload() const {
	if (GetState() == BlockState::BLOCK_UNLOADED) {
		// The block has already been unloaded.
		return CanUnloadResult::ALREADY_UNLOADED;
	}
	if (GetReaders() > 0) {
		// There are active readers.
		return CanUnloadResult::PINNED;
	}
	if (BlockId() >= MAXIMUM_BLOCK && MustWriteToTemporaryFile() && !GetBufferManager().HasTemporaryDirectory()) {
		// The block memory cannot be destroyed upon eviction/unpinning.
		// In order to unload this block we need to write it to a temporary buffer.
		// However, no temporary directory is specified, hence, we cannot unload.
		return CanUnloadResult::NO_TEMP_DIRECTORY;
	}
	return CanUnloadResult::CAN_UNLOAD;
}

unique_ptr<FileBuffer> BlockMemory::UnloadAndTakeBlock(BlockLock &l, QueryContext context) {
	VerifyMutex(l);

	if (GetState() == BlockState::BLOCK_UNLOADED) {
		// The block was already unloaded: nothing to do.
		return nullptr;
	}
	D_ASSERT(IsSwizzled());
	if (!TryMarkUnloaded()) {
		// A reader pinned the block without the block lock.
		return nullptr;
	}

	if (BlockId() >= MAXIMUM_BLOCK && (MustWriteToTemporaryFile() || (KeepsCopy() && !has_temporary_file))) {
		// A temporary block that cannot be destroyed upon evict/unpin is written to a temporary file, and one that
		// keeps a copy gets one while the copies have room.
		absl::Cleanup restore = [&] {
			SetState(BlockState::BLOCK_LOADED);
		};
		SDB_PARK_ONCE_ON_FAILURE("block_unload_before_temporary_write");
		if (MustWriteToTemporaryFile()) {
			buffer_manager.WriteTemporaryBuffer(context, GetMemoryTag(), BlockId(), *GetBuffer());
			has_temporary_file = true;
		} else {
			has_temporary_file = buffer_manager.TryWriteTemporaryCopy(context, GetMemoryTag(), BlockId(), *GetBuffer());
		}
		std::move(restore).Cancel();
	}
	memory_charge.Resize(0);
	buffer_manager.GetBufferPool().RemoveBlock(*this);
	return std::move(GetBuffer());
}

void BlockMemory::Unload(BlockLock &l, QueryContext context) {
	auto block = UnloadAndTakeBlock(l, context);
	block.reset();
}

BlockHandle::BlockHandle(BlockManager &block_manager, block_id_t block_id_p, MemoryTag tag_p)
    : block_manager(block_manager), block_alloc_size(block_manager.GetBlockAllocSize()),
      block_header_size(block_manager.GetBlockHeaderSize()), block_id(block_id_p),
      memory_p(make_shared_ptr<BlockMemory>(block_manager.GetBufferManager(), block_id_p, tag_p, block_alloc_size)),
      memory(*memory_p) {
}

BlockHandle::BlockHandle(BlockManager &block_manager, block_id_t block_id_p, MemoryTag tag_p,
                         FileBufferType buffer_type_p, DestroyBufferUpon destroy_buffer_upon_p, idx_t size_p)
    : block_manager(block_manager), block_alloc_size(block_manager.GetBlockAllocSize()),
      block_header_size(block_manager.GetBlockHeaderSize()), block_id(block_id_p),
      memory_p(make_shared_ptr<BlockMemory>(block_manager.GetBufferManager(), block_id_p, tag_p, size_p, buffer_type_p,
                                            destroy_buffer_upon_p)),
      memory(*memory_p) {
}

BlockHandle::BlockHandle(BlockManager &block_manager, block_id_t block_id_p, MemoryTag tag_p,
                         unique_ptr<FileBuffer> buffer_p, DestroyBufferUpon destroy_buffer_upon_p, idx_t size_p,
                         BufferPoolReservation &&reservation)
    : block_manager(block_manager), block_alloc_size(block_manager.GetBlockAllocSize()),
      block_header_size(block_manager.GetBlockHeaderSize()), block_id(block_id_p),
      memory_p(make_shared_ptr<BlockMemory>(block_manager.GetBufferManager(), block_id_p, tag_p, std::move(buffer_p),
                                            destroy_buffer_upon_p, size_p, std::move(reservation))),
      memory(*memory_p) {
}

BlockHandle::~BlockHandle() { // NOLINT: allow internal exceptions
	{
		auto lock = memory.GetLock();
		memory.Orphan(lock, memory_p);
	}
	// Early-out for temporary blocks.
	if (block_id >= MAXIMUM_BLOCK) {
		return;
	}
	try {
		block_manager.UnregisterPersistentBlock(*this);
	} catch (...) { // NOLINT
		            // FIXME: log silent exceptions.
	}
}

unique_ptr<Block> AllocateBlock(BlockManager &block_manager, unique_ptr<FileBuffer> reusable_buffer,
                                block_id_t block_id) {
	// A buffer that doesn't own its memory (e.g. it adopted a pointer into a memory-mapped
	// region) cannot be reused for a different block: rewriting its bytes would clobber the
	// original block on disk through the mapping.
	if (reusable_buffer && reusable_buffer->OwnsInternalBuffer() &&
	    reusable_buffer->GetHeaderSize() == block_manager.GetBlockHeaderSize()) {
		// Reusable buffer: reuse it.
		if (reusable_buffer->GetBufferType() == FileBufferType::BLOCK) {
			// Reuse the entire buffer.
			auto &block = reinterpret_cast<Block &>(*reusable_buffer);
			block.id = block_id;
			return unique_ptr_cast<FileBuffer, Block>(std::move(reusable_buffer));
		}
		auto block = block_manager.CreateBlock(block_id, reusable_buffer.get());
		reusable_buffer.reset();
		return block;
	}

	// Not a reusable buffer: allocate a new block.
	return block_manager.CreateBlock(block_id, nullptr);
}

BufferHandle BlockHandle::LoadFromBuffer(BlockLock &l, data_ptr_t data, unique_ptr<FileBuffer> reusable_buffer,
                                         BufferPoolReservation reservation) {
	memory.VerifyMutex(l);
	// Copy the data of the file buffer into the block.
	D_ASSERT(memory.GetState() != BlockState::BLOCK_LOADED);
	auto block = AllocateBlock(block_manager, std::move(reusable_buffer), block_id);
	memcpy(block->InternalBuffer(), data, block->AllocSize());
	memory.GetBuffer() = std::move(block);
	memory.SetState(BlockState::BLOCK_LOADED);
	memory.IncrementReaders();
	memory.GetMemoryCharge() = std::move(reservation);
	memory.GetBufferManager().GetBufferPool().AddBlock(memory);
	return BufferHandle(shared_from_this(), memory.GetBuffer());
}

BufferHandle BlockHandle::Load(QueryContext context, unique_ptr<FileBuffer> reusable_buffer) {
	if (memory.GetState() == BlockState::BLOCK_LOADED) {
		// The block has already been loaded.
		D_ASSERT(memory.GetBuffer());
		memory.IncrementReaders();
		return BufferHandle(shared_from_this(), memory.GetBuffer());
	}

	if (BlockId() < MAXIMUM_BLOCK) {
		auto block = AllocateBlock(block_manager, std::move(reusable_buffer), block_id);
		block_manager.Read(context, *block);
		memory.GetBuffer() = std::move(block);
	} else {
		if (!memory.HasTemporaryFile()) {
			// The buffer was destroyed upon unpin/evict, so there is no temporary buffer to read.
			return BufferHandle();
		}
		auto &buffer_manager = memory.GetBufferManager();
		auto &buffer = memory.GetBuffer();
		buffer = buffer_manager.ReadTemporaryBuffer(context, memory.GetMemoryTag(), *this, std::move(reusable_buffer));
		memory.SetHasTemporaryFile(memory.KeepsCopy());
		SDB_PARK_ONCE_ON_FAILURE("block_load_before_loaded");
	}
	memory.SetState(BlockState::BLOCK_LOADED);
	memory.IncrementReaders();
	memory.GetBufferManager().GetBufferPool().AddBlock(memory);
	return BufferHandle(shared_from_this(), memory.GetBuffer());
}

} // namespace duckdb
