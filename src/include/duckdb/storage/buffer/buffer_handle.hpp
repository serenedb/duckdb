//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_handle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class BlockHandle;
class BlockMemory;
class FileBuffer;

//! A pin of a block. A pin keeps the block's memory alive; it keeps the block handle alive only when it owns it.
class BufferHandle {
public:
	DUCKDB_API BufferHandle();
	//! A pin that owns a reference to [handle]
	DUCKDB_API explicit BufferHandle(shared_ptr<BlockHandle> handle, optional_ptr<FileBuffer> node);
	//! A pin of [memory], whose owner the caller holds
	DUCKDB_API BufferHandle(BlockMemory &memory, optional_ptr<FileBuffer> node);
	DUCKDB_API ~BufferHandle();
	// disable copy constructors
	BufferHandle(const BufferHandle &other) = delete;
	BufferHandle &operator=(const BufferHandle &) = delete;
	//! enable move constructors
	DUCKDB_API BufferHandle(BufferHandle &&other) noexcept;
	DUCKDB_API BufferHandle &operator=(BufferHandle &&) noexcept;

public:
	//! Returns whether or not the BufferHandle is valid.
	DUCKDB_API bool IsValid() const;
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
