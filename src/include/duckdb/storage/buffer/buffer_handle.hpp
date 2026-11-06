//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_handle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_buffer.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class BlockHandle;
class FileBuffer;

//! A pin of a block. A pin keeps the block's memory alive; it keeps the block handle alive only when it owns it.
class BufferHandle {
public:
	BufferHandle() = default;
	//! A pin that owns a reference to [handle]
	DUCKDB_API explicit BufferHandle(shared_ptr<BlockHandle> handle, optional_ptr<FileBuffer> node);
	//! A pin of [memory], whose owner the caller holds
	[[gnu::always_inline]] BufferHandle(BlockMemory &memory_p, optional_ptr<FileBuffer> node_p)
	    : memory(&memory_p), node(node_p) {
	}
	[[gnu::always_inline]] ~BufferHandle() {
		if (IsValid() && (handle || !memory || !memory->TryUnpinFast())) {
			Destroy();
		}
	}
	// disable copy constructors
	BufferHandle(const BufferHandle &other) = delete;
	BufferHandle &operator=(const BufferHandle &) = delete;
	//! enable move constructors
	[[gnu::always_inline]] BufferHandle(BufferHandle &&other) noexcept {
		std::swap(memory, other.memory);
		std::swap(node, other.node);
		std::swap(handle, other.handle);
	}
	[[gnu::always_inline]] BufferHandle &operator=(BufferHandle &&other) noexcept {
		std::swap(memory, other.memory);
		std::swap(node, other.node);
		std::swap(handle, other.handle);
		return *this;
	}

public:
	//! Returns whether or not the BufferHandle is valid.
	bool IsValid() const {
		return node != nullptr;
	}
	//! Returns a pointer to the buffer data. Handle must be valid.
	inline const_data_ptr_t Ptr() const {
		D_ASSERT(IsValid());
		return node->GetData();
	}
	//! Returns a pointer to the buffer data. Handle must be valid.
	inline const_data_ptr_t Ptr() {
		D_ASSERT(IsValid());
		return node->GetData();
	}
	inline data_ptr_t GetDataMutable() {
		D_ASSERT(IsValid());
		return node->GetDataMutable();
	}
	//! Gets the underlying file buffer. Handle must be valid.
	DUCKDB_API FileBuffer &GetFileBuffer();
	//! Destroys the buffer handle
	DUCKDB_API void Destroy();
	//! Another pin of the pinned block, which does not own the block handle. Handle must be valid.
	DUCKDB_API BufferHandle Share();

	//! The pinned block, empty once its last other owner is gone
	DUCKDB_API shared_ptr<BlockHandle> GetBlockHandle() const;

private:
	//! The pinned memory
	optional_ptr<BlockMemory> memory;
	//! The block handle, when the pin owns it
	shared_ptr<BlockHandle> handle;
	//! The managed buffer node
	optional_ptr<FileBuffer> node;
};

} // namespace duckdb
