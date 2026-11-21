#include "duckdb/formats/row_input.hpp"

#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/formats/row_stream.hpp"

namespace duckdb {

FileRowSource::FileRowSource(CSVBufferManager &manager_p, idx_t size_p, optional_ptr<atomic<idx_t>> bytes_read_p)
    : manager(manager_p), size(size_p), bytes_read(bytes_read_p) {
}

idx_t FileRowSource::Read(char *data, idx_t count, idx_t offset) const {
	if (manager.HasKnownBufferRanges()) {
		const idx_t available = offset < size ? MinValue(count, size - offset) : 0;
		if (available > 0) {
			manager.file_handle->ReadAt(data, available, offset);
		}
		return available;
	}
	const idx_t buffer_bytes = manager.GetBufferSize();
	idx_t read = 0;
	while (read < count) {
		const idx_t position = offset + read;
		auto handle = manager.GetBuffer(position / buffer_bytes);
		const idx_t within = position % buffer_bytes;
		if (!handle || within >= handle->actual_size) {
			break;
		}
		const idx_t piece = MinValue(count - read, handle->actual_size - within);
		memcpy(data + read, handle->Ptr() + within, piece);
		read += piece;
	}
	return read;
}

void FileRowSource::Release(idx_t position) {
	const idx_t buffer_bytes = manager.GetBufferSize();
	for (; released < position / buffer_bytes; released++) {
		*bytes_read += buffer_bytes;
		manager.ResetBuffer(released);
	}
}

RowInput::RowInput(ClientContext &context, CSVBufferManager &manager, const string &path, bool utf8,
                   optional_ptr<atomic<idx_t>> bytes_read) {
	if (manager.file_handle->compression_type == FileCompressionType::UNCOMPRESSED && utf8) {
		host_stream = FindRowStream(context, path);
	}
	stream = host_stream || !manager.HasKnownBufferRanges();
	size = stream ? NumericLimits<idx_t>::Maximum() : manager.file_handle->FileSize();
	if (!host_stream) {
		file = make_uniq<FileRowSource>(manager, size, bytes_read);
	}
}

} // namespace duckdb
