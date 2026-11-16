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

// A group of FilePins that together represent contiguous file content.
// For example, ZSTD supports stream-based decompression, which doesn't require to prepare a contiguous buffer;
// instead users are able to iterate all handles and decompress them on the fly.
class FileBufferHandleGroup {
public:
	FileBufferHandleGroup() = default;
	explicit FileBufferHandleGroup(vector<FilePin> handles_p);

	// Read-only access to the underlying handles.
	const vector<FilePin> &GetHandles() const;

	// Util function to copy from the start of the group to the destination address for the requested number of bytes
	void CopyTo(data_ptr_t dest, idx_t nr_bytes) const;

private:
	vector<FilePin> handles;
};

} // namespace duckdb
