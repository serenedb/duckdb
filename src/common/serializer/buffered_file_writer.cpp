#include "duckdb/common/serializer/buffered_file_writer.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/typedefs.hpp"

#include <absl/crc/crc32c.h>
#include <cstring>
#include <utility>

namespace duckdb {

const FileOpenFlags BufferedFileWriter::DEFAULT_OPEN_FLAGS =
    FileOpenFlags(FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE);

BufferedFileWriter::BufferedFileWriter(FileSystem &fs, const string &path_p, const FileOpenFlags &open_flags,
                                       QueryContext context_p)
    : fs(fs), path(path_p), checksummed(false) {
	// the file may append or be a pipe, so its bytes go to the handle's own position
	auto handle_writer =
	    make_uniq<HandleFileWriter>(fs.OpenFile(path, open_flags | FileLockType::WRITE_LOCK), context_p, true);
	handle = handle_writer->Handle();
	writer = std::move(handle_writer);
	OpenWindow();
}

BufferedFileWriter::BufferedFileWriter(FileSystem &fs, string path_p, unique_ptr<FileWriter> writer_p)
    : fs(fs), path(std::move(path_p)), writer(std::move(writer_p)), checksummed(true) {
	OpenWindow();
}

void BufferedFileWriter::OpenWindow() {
	const auto window = writer->Window();
	D_ASSERT(window.size != 0);
	SetWindow(window.data, window.size);
	window_checksum = 0;
	checksummed_until = write_begin;
}

void BufferedFileWriter::ChecksumWindow() {
	D_ASSERT(checksummed && checksummed_until <= write_pos);
	window_checksum = static_cast<uint32_t>(
	    absl::ExtendCrc32c(absl::crc32c_t {window_checksum}, {const_char_ptr_cast(checksummed_until),
	                                                          static_cast<size_t>(write_pos - checksummed_until)}));
	checksummed_until = write_pos;
}

idx_t BufferedFileWriter::GetFileSize() {
	return NumericCast<idx_t>(fs.GetFileSize(*handle)) + WindowSize();
}

idx_t BufferedFileWriter::GetTotalWritten() const {
	return Position();
}

void BufferedFileWriter::Overflow(const_data_ptr_t source, idx_t write_size) {
	D_ASSERT(write_size > WindowRoom());
	if (!writer) {
		throw InternalException("Cannot write to the closed file \"%s\"", path);
	}
	const auto fill = [&] {
		const auto chunk = MinValue(write_size, WindowRoom());
		if (checksummed) {
			ChecksumWindow();
			window_checksum =
			    static_cast<uint32_t>(absl::MemcpyCrc32c(write_pos, source, chunk, absl::crc32c_t {window_checksum}));
			checksummed_until = write_pos + chunk;
		} else {
			memcpy(write_pos, source, chunk);
		}
		write_pos += chunk;
		source += chunk;
		write_size -= chunk;
	};
	if (write_pos != write_begin) {
		// a window that holds bytes is filled first, so that a write to the file is never a small one
		fill();
	}
	Flush();
	// the rest goes to the file as it is, instead of in buffers of the window's size, when the writer takes it so
	if (write_size >= DIRECT_WRITE_SIZE && writer->Append(source, write_size)) {
		if (checksummed) {
			checksum = static_cast<uint32_t>(
			    absl::ExtendCrc32c(absl::crc32c_t {checksum}, {const_char_ptr_cast(source), write_size}));
		}
		write_offset += write_size;
		return;
	}
	while (true) {
		OpenWindow();
		fill();
		if (write_size == 0) {
			return;
		}
		Flush();
	}
}

void BufferedFileWriter::Flush() {
	const auto buffered = WindowSize();
	if (buffered != 0) {
		if (checksummed) {
			ChecksumWindow();
			checksum = static_cast<uint32_t>(
			    absl::ConcatCrc32c(absl::crc32c_t {checksum}, absl::crc32c_t {window_checksum}, buffered));
		}
		writer->Commit(buffered, window_checksum);
		write_offset += buffered;
	}
	// the writer gives the memory of the next bytes when there are some
	CloseWindow();
}

void BufferedFileWriter::Close() {
	if (!writer) {
		return;
	}
	Flush();
	writer->Close();
	writer.reset();
	handle = nullptr;
}

void BufferedFileWriter::SyncHandle() {
	handle->Sync();
}

void BufferedFileWriter::Sync() {
	Flush();
	handle->Sync();
}

void BufferedFileWriter::Truncate(idx_t size) {
	auto persistent = NumericCast<idx_t>(fs.GetFileSize(*handle));
	D_ASSERT(size <= persistent + WindowSize());
	if (persistent <= size) {
		// truncating into the pending write buffer.
		write_pos = write_begin + (size - persistent);
	} else {
		// truncate the physical file on disk
		handle->Truncate(NumericCast<int64_t>(size));
		// reset anything written in the buffer
		write_pos = write_begin;
		// Reset the seek position if applicable
		if (handle->CanSeek() && handle->SeekPosition() > size) {
			handle->Seek(size);
		}
	}
}

uint32_t BufferedFileWriter::Checksum() {
	D_ASSERT(checksummed);
	Flush();
	return std::exchange(checksum, 0);
}

} // namespace duckdb
