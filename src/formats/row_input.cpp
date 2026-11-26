#include "duckdb/formats/row_input.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/formats/row_stream.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

namespace {

class MappedRowBuffer : public RowBuffer {
public:
	explicit MappedRowBuffer(shared_ptr<MemoryMappedFile> mapping_p) : mapping(std::move(mapping_p)) {
	}

private:
	shared_ptr<MemoryMappedFile> mapping;
};

bool ExtendPrefix(atomic<idx_t> &prefix, idx_t &begin, idx_t end) {
	idx_t covered = prefix.load(std::memory_order_relaxed);
	while (begin <= covered) {
		if (covered >= end) {
			return false;
		}
		if (prefix.compare_exchange_weak(covered, end, std::memory_order_relaxed)) {
			begin = covered;
			return true;
		}
	}
	return true;
}

unique_ptr<MemoryMappedFile> MapForReading(ClientContext &context, const string &path, bool on_disk, idx_t size,
                                           idx_t tail) {
	if (!on_disk || size < MAPPED_FILE_MIN_BYTES || Settings::Get<CacheLocalFilesSetting>(context) ||
	    FindRowStream(context, path)) {
		return nullptr;
	}
	MMapOptions options;
	options.tail = tail;
	unique_ptr<MemoryMappedFile> mapping;
	try {
		mapping =
		    FileSystem::GetFileSystem(context).MemoryMapFile(OpenFileInfo(path), FileFlags::FILE_FLAGS_READ, options);
	} catch (IOException &) {
		return nullptr;
	} catch (NotImplementedException &) {
		return nullptr;
	}
	if (mapping->Size() != size || mapping->Tail() < tail) {
		return nullptr;
	}
	mapping->Advise(MappedFileAccess::RANDOM);
	return mapping;
}

} // namespace

shared_ptr<MappedRowSource> MappedRowSource::Open(ClientContext &context, CSVFileHandle &handle, const string &path) {
	auto mapping = MapForReading(context, path, handle.OnDiskFile(), handle.FileSize(), RowWindow::PADDING);
	if (!mapping) {
		return nullptr;
	}
	return make_shared_ptr<MappedRowSource>(shared_ptr<MemoryMappedFile>(std::move(mapping)), &handle);
}

MappedRowSource::MappedRowSource(shared_ptr<MemoryMappedFile> mapping_p, optional_ptr<CSVFileHandle> handle_p)
    : mapping(std::move(mapping_p)), handle(handle_p), buffer(make_shared_ptr<MappedRowBuffer>(mapping)),
      data(const_char_ptr_cast(mapping->GetData(0, mapping->Size()))), size(mapping->Size()) {
}

void MappedRowSource::Want(idx_t begin, idx_t end) {
	Fetch(begin, end);
}

void MappedRowSource::Done(idx_t begin, idx_t end) {
	mapping->Release(begin, end - begin);
}

void MappedRowSource::ReadInPlace(idx_t begin, idx_t end) {
	Report(begin, end);
}

idx_t MappedRowSource::Read(char *out, idx_t count, idx_t offset) const {
	const idx_t available = offset < size ? MinValue(count, size - offset) : 0;
	if (available > 0) {
		Fetch(offset, offset + available);
		memcpy(out, data + offset, available);
		Report(offset, offset + available);
	}
	return available;
}

void MappedRowSource::Fetch(idx_t begin, idx_t end) const {
	if (ExtendPrefix(wanted_to, begin, end) && !mapping->IsCached(begin, end - begin)) {
		mapping->Prefetch(begin, end - begin);
	}
}

void MappedRowSource::Report(idx_t begin, idx_t end) const {
	if (handle && ExtendPrefix(reported_to, begin, end)) {
		handle->ReadInPlace(end - begin, begin);
	}
}

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
	if (host_stream) {
		return;
	}
	mapped = manager.mapped;
	if (!mapped) {
		file = make_uniq<FileRowSource>(manager, size, bytes_read);
	}
}

} // namespace duckdb
