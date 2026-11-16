//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/handle_file_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/query_context.hpp"

namespace duckdb {

//! The writer of a file whose filesystem takes its bytes through a handle only, from start to end: its window is a
//! buffer it writes to the file a buffer at a time, and bytes of the caller's go to the file as they are. A file
//! written at the writer's own offsets is hinted to start writing back every WRITEBACK_HINT_SIZE bytes and at its close
class HandleFileWriter final : public FileWriter {
public:
	static constexpr idx_t BUFFER_SIZE = 64ULL * 1024;
	static constexpr idx_t WRITEBACK_HINT_SIZE = 8ULL * 1024 * 1024;

	//! With [sequential] the bytes go to the handle's own position: for a handle that appends, cannot seek or gets
	//! truncated, whose bytes land at offsets only the handle knows, so no writeback is hinted, and the file is not
	//! the writer's to drop: one destroyed before it closes leaves the bytes it wrote. [context] gets the written
	//! bytes in its I/O metrics
	DUCKDB_API explicit HandleFileWriter(unique_ptr<FileHandle> handle, QueryContext context = QueryContext(),
	                                     bool sequential = false);
	DUCKDB_API ~HandleFileWriter() override;

	FileHandle &Handle() {
		return *handle;
	}

	DUCKDB_API FileWriteWindow Window() override;
	DUCKDB_API void Commit(idx_t nr_bytes, uint32_t crc32c) override;
	DUCKDB_API bool Append(const_data_ptr_t data, idx_t nr_bytes) override;
	DUCKDB_API void Close() override;
	DUCKDB_API void Abort() noexcept override;

private:
	//! Writes [nr_bytes] bytes, the buffer's or the caller's, after the ones written before
	void Write(const_data_ptr_t data, idx_t nr_bytes);

private:
	unique_ptr<FileHandle> handle;
	QueryContext context;
	const bool sequential;
	const unsafe_unique_array<data_t> buffer;
	idx_t written = 0;
	idx_t unhinted = 0;
};

} // namespace duckdb
