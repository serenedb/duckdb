//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/file_buffer_handle_group.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"

namespace duckdb {

class FileBufferHandleGroup {
public:
	//! Bytes of a file that stay valid while the handle lives: a pinned block cache block or an owned buffer
	struct MemoryHandle {
		MemoryHandle(BlockCachePin pin_p, const_data_ptr_t data_p, idx_t length_p)
		    : pin(std::move(pin_p)), data(data_p), length(length_p) {
		}

		BlockCachePin pin;
		//! The first byte
		const_data_ptr_t data;
		//! The number of bytes
		idx_t length;
	};

	FileBufferHandleGroup() = default;
	explicit FileBufferHandleGroup(vector<MemoryHandle> handles_p);

	const vector<MemoryHandle> &GetHandles() const;

	//! Copies the first [nr_bytes] bytes of the group to [dest]
	void CopyTo(data_ptr_t dest, idx_t nr_bytes) const;

	//! The bytes of a group of exactly one handle; throws otherwise
	const_data_ptr_t Ptr() const;

private:
	vector<MemoryHandle> handles;
};

} // namespace duckdb
