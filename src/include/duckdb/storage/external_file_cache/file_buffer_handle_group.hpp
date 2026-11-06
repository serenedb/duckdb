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
	FileBufferHandleGroup() = default;
	//! The bytes of a read, in order: pinned in block cache blocks or in buffers the pins own
	explicit FileBufferHandleGroup(vector<BlockCachePin> handles_p);

	const vector<BlockCachePin> &GetHandles() const;

	// Util function to copy from the start of the group to the destination address for the requested number of bytes
	void CopyTo(data_ptr_t dest, idx_t nr_bytes) const;

	// Return a pointer to the start of the first handle in the group.
	// Warning: this function requires exactly one handle for zero-copy access, otherwise it will throw an exception.
	const_data_ptr_t Ptr() const;

private:
	vector<BlockCachePin> handles;
};

} // namespace duckdb
