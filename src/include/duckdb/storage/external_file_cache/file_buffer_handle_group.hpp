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
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

namespace duckdb {

class FileBufferHandleGroup {
public:
	FileBufferHandleGroup() = default;
	//! The bytes of a read, in order: pinned in cache blocks or in buffers the pins own
	explicit FileBufferHandleGroup(vector<ExternalFileCachePin> handles_p);

	const vector<ExternalFileCachePin> &GetHandles() const;

	// Util function to copy from the start of the group to the destination address for the requested number of bytes
	void CopyTo(data_ptr_t dest, idx_t nr_bytes) const;

	// Return a pointer to the start of the first handle in the group.
	// Warning: this function requires exactly one handle for zero-copy access, otherwise it will throw an exception.
	const_data_ptr_t Ptr() const;

private:
	vector<ExternalFileCachePin> handles;
};

} // namespace duckdb
