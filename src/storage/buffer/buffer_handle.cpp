#include "duckdb/storage/buffer/buffer_handle.hpp"

#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"

namespace duckdb {

BufferHandle::BufferHandle() : memory(nullptr), handle(nullptr), node(nullptr) {
}

BufferHandle::BufferHandle(shared_ptr<BlockHandle> handle_p, optional_ptr<FileBuffer> node_p)
    : memory(handle_p ? &handle_p->GetMemory() : nullptr), handle(std::move(handle_p)), node(node_p) {
}

BufferHandle::BufferHandle(BlockMemory &memory_p, optional_ptr<FileBuffer> node_p)
    : memory(&memory_p), handle(nullptr), node(node_p) {
}

BufferHandle::BufferHandle(BufferHandle &&other) noexcept : memory(nullptr), node(nullptr) {
	std::swap(memory, other.memory);
	std::swap(node, other.node);
	std::swap(handle, other.handle);
}

BufferHandle &BufferHandle::operator=(BufferHandle &&other) noexcept {
	std::swap(memory, other.memory);
	std::swap(node, other.node);
	std::swap(handle, other.handle);
	return *this;
}

BufferHandle::~BufferHandle() {
	Destroy();
}

bool BufferHandle::IsValid() const {
	return node != nullptr;
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

FileBuffer &BufferHandle::GetFileBuffer() {
	D_ASSERT(node);
	return *node;
}

} // namespace duckdb
