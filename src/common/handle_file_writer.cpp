#include "duckdb/common/handle_file_writer.hpp"

namespace duckdb {

HandleFileWriter::HandleFileWriter(unique_ptr<FileHandle> handle_p, QueryContext context_p, bool sequential_p)
    : handle(std::move(handle_p)), context(context_p), sequential(sequential_p),
      buffer(make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE)) {
}

HandleFileWriter::~HandleFileWriter() {
	HandleFileWriter::Abort();
}

void HandleFileWriter::Write(const_data_ptr_t data, idx_t nr_bytes) {
	D_ASSERT(handle);
	const auto bytes = const_cast<data_ptr_t>(data); // NOLINT: wrong API in Write
	if (sequential) {
		handle->Write(context, bytes, nr_bytes);
		return;
	}
	handle->Write(context, bytes, nr_bytes, written);
	written += nr_bytes;
	unhinted += nr_bytes;
	if (unhinted < WRITEBACK_HINT_SIZE) {
		return;
	}
	const auto hinted = unhinted;
	unhinted = 0;
	handle->HintWriteback(written - hinted, hinted);
}

FileWriteWindow HandleFileWriter::Window() {
	return {buffer.get(), BUFFER_SIZE};
}

void HandleFileWriter::Commit(idx_t nr_bytes, uint32_t crc32c) {
	Write(buffer.get(), nr_bytes);
}

bool HandleFileWriter::Append(const_data_ptr_t data, idx_t nr_bytes) {
	Write(data, nr_bytes);
	return true;
}

void HandleFileWriter::Close() {
	if (!handle) {
		return;
	}
	if (unhinted != 0) {
		handle->HintWriteback(written - unhinted, unhinted);
	}
	handle->Close();
	handle.reset();
}

void HandleFileWriter::Abort() noexcept {
	if (!handle) {
		return;
	}
	if (!sequential) {
		try {
			handle->AbortWrite();
		} catch (std::exception &) { // NOLINT
		}
	}
	handle.reset();
}

} // namespace duckdb
