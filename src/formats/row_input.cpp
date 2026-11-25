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
	return in_order ? manager.ReadInOrder(data, count, offset) : manager.ReadBuffered(data, count, offset);
}

void FileRowSource::Release(idx_t position) {
	const idx_t buffer_bytes = manager.GetBufferSize();
	for (; released < position / buffer_bytes; released++) {
		*bytes_read += buffer_bytes;
		manager.ResetBuffer(released);
	}
}

shared_ptr<RowSource> RowInput::HostStream(ClientContext &context, const CSVFileHandle &handle, const string &path,
                                           bool utf8) {
	if (handle.compression_type != FileCompressionType::UNCOMPRESSED || !utf8) {
		return nullptr;
	}
	return FindRowStream(context, path);
}

RowInput::RowInput(ClientContext &context, CSVBufferManager &manager, const string &path, bool utf8,
                   optional_ptr<atomic<idx_t>> bytes_read)
    : host_stream(HostStream(context, *manager.file_handle, path, utf8)) {
	stream = host_stream || !manager.HasKnownBufferRanges();
	size = stream ? NumericLimits<idx_t>::Maximum() : manager.file_handle->FileSize();
	if (!host_stream) {
		file = make_uniq<FileRowSource>(manager, size, bytes_read);
	}
}

} // namespace duckdb
