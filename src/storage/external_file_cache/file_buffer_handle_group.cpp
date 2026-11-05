#include "duckdb/storage/external_file_cache/file_buffer_handle_group.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

FileBufferHandleGroup::FileBufferHandleGroup(vector<BlockCachePin> handles_p) : handles(std::move(handles_p)) {
}

const vector<BlockCachePin> &FileBufferHandleGroup::GetHandles() const {
	return handles;
}

void FileBufferHandleGroup::CopyTo(data_ptr_t dest, idx_t nr_bytes) const {
	idx_t copied = 0;
	for (auto &pin : handles) {
		if (copied >= nr_bytes) {
			break;
		}
		const idx_t to_copy = MinValue(pin.Size(), nr_bytes - copied);
		if (to_copy == 0) {
			continue;
		}
		memcpy(dest + copied, pin.Ptr(), to_copy);
		copied += to_copy;
	}
	if (copied != nr_bytes) {
		throw InternalException("FileBufferHandleGroup::CopyTo: handles contain %llu bytes but %llu were requested",
		                        copied, nr_bytes);
	}
}

const_data_ptr_t FileBufferHandleGroup::Ptr() const {
	if (handles.size() != 1) {
		throw InternalException("FileBufferHandleGroup::Ptr() requires exactly one handle, got %llu", handles.size());
	}
	return handles[0].Ptr();
}

} // namespace duckdb
