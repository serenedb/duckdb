#include "duckdb/formats/row_input.hpp"

#include <algorithm>

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

} // namespace

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

shared_ptr<WholeFile> WholeFile::Read(ClientContext &context, FileHandle &handle) {
	auto result = make_shared_ptr<WholeFile>();
	result->size = handle.GetFileSize();
	result->mapping = MapForReading(context, handle.GetPath(), handle.OnDiskFile(), result->size, 0);
	if (result->mapping) {
		result->data = const_char_ptr_cast(result->mapping->ReadInPlace(handle, context, 0, result->size));
		return result;
	}
	if (result->size == 0) {
		return result;
	}
	result->owned = Allocator::DefaultAllocator().Allocate(result->size);
	result->data = const_char_ptr_cast(result->owned.get());
	result->size = NumericCast<idx_t>(handle.Read(context, result->owned.get(), result->size));
	return result;
}

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
	if (ExtendPrefix(wanted_to, begin, end)) {
		mapping->PrefetchMissing(begin, end - begin);
	}
}

void MappedRowSource::Report(idx_t begin, idx_t end) const {
	if (handle && ExtendPrefix(reported_to, begin, end)) {
		handle->ReadInPlace(end - begin, begin);
	}
}

shared_ptr<PiecedRowSource> PiecedRowSource::Open(ClientContext &context, const string &path,
                                                  const FileCompressionType &compression, bool on_disk, idx_t size) {
	if (!compression.IsCompressed()) {
		return nullptr;
	}
	auto compressed = FileSystem::GetFileSystem(context).GetCompressedFileSystem(compression, path);
	if (!compressed) {
		return nullptr;
	}
	auto mapping = MapForReading(context, path, on_disk, size, 0);
	if (!mapping) {
		return nullptr;
	}
	vector<PieceSpan> pieces;
	if (!compressed->FindPieces(mapping->GetData(0, size), size, pieces) || pieces.size() < 2) {
		return nullptr;
	}
	return make_shared_ptr<PiecedRowSource>(shared_ptr<MemoryMappedFile>(std::move(mapping)), *compressed,
	                                        std::move(pieces));
}

PiecedRowSource::PiecedRowSource(shared_ptr<MemoryMappedFile> mapping_p, CompressedFileSystem &compressed_p,
                                 vector<PieceSpan> pieces_p)
    : mapping(std::move(mapping_p)), compressed(compressed_p), pieces(std::move(pieces_p)),
      data(mapping->GetData(0, mapping->Size())), size(pieces.back().output_offset + pieces.back().output_size) {
}

class PiecedRowCursor : public RowSourceCursor {
public:
	explicit PiecedRowCursor(const PiecedRowSource &source_p) : source(source_p), decompressor(source.Take()) {
	}
	~PiecedRowCursor() override {
		source.Give(std::move(decompressor));
	}

	idx_t Read(char *out, idx_t count, idx_t offset) override {
		if (offset >= source.size) {
			return 0;
		}
		count = MinValue(count, source.size - offset);
		idx_t done = 0;
		while (done < count) {
			const idx_t position = offset + done;
			if (!begun || position != next) {
				Locate(position);
			}
			const auto &span = source.pieces[index];
			const idx_t end = span.output_offset + span.output_size;
			const idx_t take = MinValue(count - done, end - position);
			D_ASSERT(take > 0);
			const auto target = data_ptr_cast(out + done);
			if (!begun) {
				source.mapping->PrefetchMissing(
				    span.offset, MinValue(PiecedRowSource::PREFETCH_BYTES, source.mapping->Size() - span.offset));
				if (position == span.output_offset && take == span.output_size) {
					decompressor->DecompressAll(source.data + span.offset, span, target);
					done += take;
					continue;
				}
				decompressor->Begin(source.data + span.offset, span);
				decompressor->Continue(nullptr, position - span.output_offset);
				begun = true;
			}
			decompressor->Continue(target, take);
			done += take;
			next = position + take;
			begun = next < end;
		}
		return done;
	}

private:
	void Locate(idx_t position) {
		auto piece = std::upper_bound(source.pieces.begin(), source.pieces.end(), position,
		                              [](idx_t value, const PieceSpan &span) { return value < span.output_offset; });
		D_ASSERT(piece != source.pieces.begin());
		index = NumericCast<idx_t>(piece - source.pieces.begin()) - 1;
		begun = false;
	}

	const PiecedRowSource &source;
	unique_ptr<PieceDecompressor> decompressor;
	idx_t index = 0;
	idx_t next = 0;
	bool begun = false;
};

idx_t PiecedRowSource::Read(char *out, idx_t count, idx_t offset) const {
	PiecedRowCursor cursor(*this);
	return cursor.Read(out, count, offset);
}

const vector<PieceSpan> &PiecedRowSource::Pieces() const {
	return pieces;
}

unique_ptr<RowSourceCursor> PiecedRowSource::Cursor() const {
	return make_uniq<PiecedRowCursor>(*this);
}
unique_ptr<PieceDecompressor> PiecedRowSource::Take() const {
	{
		lock_guard<mutex> guard(lock);
		if (!idle.empty()) {
			auto decompressor = std::move(idle.back());
			idle.pop_back();
			return decompressor;
		}
	}
	return compressed.CreateDecompressor();
}

void PiecedRowSource::Give(unique_ptr<PieceDecompressor> decompressor) const {
	lock_guard<mutex> guard(lock);
	idle.push_back(std::move(decompressor));
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
	if (stream && utf8) {
		const auto &handle = *manager.file_handle;
		pieced = PiecedRowSource::Open(context, path, handle.compression_type, handle.OnDiskFile(), handle.FileSize());
		if (pieced) {
			stream = false;
			size = pieced->Size();
			return;
		}
	}
	mapped = manager.mapped;
	if (!mapped) {
		file = make_uniq<FileRowSource>(manager, size, bytes_read);
	}
}

} // namespace duckdb
