//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/buffered_file_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/handle_file_writer.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/query_context.hpp"

namespace duckdb {

//! Writes a file in order. Its window is the memory its FileWriter gives for the next bytes of the file, so they
//! are written in place: into the buffer of a handle, a block of the external file cache or a part of an upload
class BufferedFileWriter : public WriteStream {
public:
	DUCKDB_API static const FileOpenFlags DEFAULT_OPEN_FLAGS;
	//! A write of at least this many bytes goes to the file as it is when the writer takes it without a copy
	static constexpr idx_t DIRECT_WRITE_SIZE = HandleFileWriter::BUFFER_SIZE;

	//! Writes [path] through a handle opened with [open_flags]: the file may exist, be appended to, synced and
	//! truncated. The optional QueryContext is used to attribute the written bytes to the query's I/O metrics.
	DUCKDB_API BufferedFileWriter(FileSystem &fs, const string &path,
	                              const FileOpenFlags &open_flags = DEFAULT_OPEN_FLAGS,
	                              QueryContext context = QueryContext());
	//! Writes [path] as a new file from start to end through [writer], which its file system gave for it
	//! (FileSystem::OpenFileWriter), and keeps the CRC32C of the bytes written. A stream destroyed before it
	//! closes aborts the write (FileWriter::Abort)
	DUCKDB_API BufferedFileWriter(FileSystem &fs, string path, unique_ptr<FileWriter> writer);
	//! The stream's window points into its writer's memory
	BufferedFileWriter(const BufferedFileWriter &) = delete;
	BufferedFileWriter &operator=(const BufferedFileWriter &) = delete;

	FileSystem &fs;
	string path;
	//! The handle of a file written through one
	optional_ptr<FileHandle> handle;

public:
	//! Flush all changes to the file and then close the file
	DUCKDB_API void Close();
	//! Flush all changes and fsync the file to disk
	DUCKDB_API void Sync();
	//! Fsync the file handle without flushing the buffer
	DUCKDB_API void SyncHandle();
	//! Flush the buffer to the file (without sync)
	DUCKDB_API void Flush();
	//! Returns the current size of the file
	DUCKDB_API idx_t GetFileSize();
	//! Truncate the size to a previous size (given that size <= GetFileSize())
	DUCKDB_API void Truncate(idx_t size);

	DUCKDB_API idx_t GetTotalWritten() const;
	//! The CRC32C of the bytes written since the last call, of a stream that keeps it
	DUCKDB_API uint32_t Checksum();

protected:
	DUCKDB_API void Overflow(const_data_ptr_t buffer, idx_t write_size) override;

private:
	//! Makes the memory the writer gives for the next bytes the window
	void OpenWindow();
	//! Extends the CRC32C of the window over the bytes written into it since it was last extended
	void ChecksumWindow();

private:
	unique_ptr<FileWriter> writer;
	//! Whether the stream computes the CRC32C of what it writes, which a file written in place takes with its bytes
	const bool checksummed;
	uint32_t checksum = 0;
	//! The CRC32C of the bytes of the window before [checksummed_until]: bytes copied into the window in bulk are
	//! checksummed by the pass that copies them
	uint32_t window_checksum = 0;
	data_ptr_t checksummed_until = nullptr;
};

} // namespace duckdb
