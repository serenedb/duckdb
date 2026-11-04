#include "duckdb/storage/buffer/buffer_handle.hpp"

#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"

namespace duckdb {

BufferHandle::BufferHandle(shared_ptr<BlockHandle> handle_p, optional_ptr<FileBuffer> node_p)
    : memory(handle_p ? &handle_p->GetMemory() : nullptr), handle(std::move(handle_p)), node(node_p) {
}

shared_ptr<BlockHandle> BufferHandle::GetBlockHandle() const {
	if (handle || !memory) {
		return handle;
	}
	return memory->GetOwner();
}

void BufferHandle::Destroy() {
	if (!memory || !IsValid()) {
		return;
	}
	memory->GetBufferManager().Unpin(*memory);
	memory = nullptr;
	handle.reset();
	node = nullptr;
}

BufferHandle BufferHandle::Share() {
	D_ASSERT(IsValid() && memory->GetBufferType() != FileBufferType::TINY_BUFFER);
	memory->IncrementReaders();
	return BufferHandle(*memory, node);
}

FileBuffer &BufferHandle::GetFileBuffer() {
	D_ASSERT(node);
	return *node;
}

} // namespace duckdb
