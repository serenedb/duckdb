#include "duckdb/common/serializer/buffered_file_reader.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

BufferedFileReader::BufferedFileReader(FileSystem &fs, const char *path, FileLockType lock_type,
                                       optional_ptr<FileOpener> opener)
    : fs(fs), data(make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE)), total_read(0) {
	SetReadWindow(data.get(), 0);
	handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | lock_type, opener.get());
	file_size = NumericCast<idx_t>(fs.GetFileSize(*handle));
}

BufferedFileReader::BufferedFileReader(FileSystem &fs, unique_ptr<FileHandle> handle_p)
    : fs(fs), data(make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE)), handle(std::move(handle_p)),
      total_read(0) {
	SetReadWindow(data.get(), 0);
	file_size = NumericCast<idx_t>(fs.GetFileSize(*handle));
}

void BufferedFileReader::Underflow(QueryContext context, data_ptr_t target_buffer, uint64_t read_size) {
	// first copy anything we can from the buffer
	data_ptr_t end_ptr = target_buffer + read_size;
	while (true) {
		idx_t to_read = MinValue<idx_t>(UnsafeNumericCast<idx_t>(end_ptr - target_buffer), ReadWindowRoom());
		if (to_read > 0) {
			memcpy(target_buffer, read_pos, to_read);
			read_pos += to_read;
			target_buffer += to_read;
		}
		if (target_buffer < end_ptr) {
			D_ASSERT(read_pos == read_end);
			// did not finish reading yet but exhausted buffer
			// read data into buffer
			total_read += UnsafeNumericCast<idx_t>(read_end - data.get());
			SetReadWindow(data.get(), 0);
			const auto read_data = UnsafeNumericCast<idx_t>(fs.Read(*handle, data.get(), BUFFER_SIZE));
			if (read_data == 0) {
				throw SerializationException("not enough data in file to deserialize result");
			}
			SetReadWindow(data.get(), read_data);
		} else {
			return;
		}
	}
}

bool BufferedFileReader::Finished() {
	return CurrentOffset() == file_size;
}

void BufferedFileReader::Seek(uint64_t location) {
	D_ASSERT(location <= file_size);
	handle->Seek(location);
	total_read = location;
	SetReadWindow(data.get(), 0);
}

void BufferedFileReader::Reset() {
	handle->Reset();
	total_read = 0;
	SetReadWindow(data.get(), 0);
}

uint64_t BufferedFileReader::CurrentOffset() {
	return total_read + UnsafeNumericCast<idx_t>(read_pos - data.get());
}

} // namespace duckdb
