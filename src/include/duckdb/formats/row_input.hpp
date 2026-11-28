//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_input.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/formats/row_ranges.hpp"

namespace duckdb {

class FileRowSource : public RowSource {
public:
	FileRowSource(CSVBufferManager &manager, idx_t size, optional_ptr<atomic<idx_t>> bytes_read);

	idx_t Read(char *data, idx_t count, idx_t offset) const override;
	void Release(idx_t position) override;
	optional_idx End() const override {
		return manager.KnownEnd();
	}

public:
	CSVBufferManager &manager;
	const idx_t size;
	optional_ptr<atomic<idx_t>> bytes_read;
	idx_t released = 0;
	bool in_order = false;
};

static constexpr idx_t MAPPED_FILE_MIN_BYTES = idx_t(1) << 21;

unique_ptr<MemoryMappedFile> MapForReading(ClientContext &context, const string &path, bool on_disk, idx_t size,
                                           idx_t tail);

class WholeFile : public RowBuffer {
public:
	static shared_ptr<WholeFile> Read(ClientContext &context, FileHandle &handle);

	const char *data = nullptr;
	idx_t size = 0;

private:
	unique_ptr<MemoryMappedFile> mapping;
	AllocatedData owned;
};

class MappedRowSource : public RowSource {
public:
	static shared_ptr<MappedRowSource> Open(ClientContext &context, CSVFileHandle &handle, const string &path);

	explicit MappedRowSource(shared_ptr<MemoryMappedFile> mapping, optional_ptr<CSVFileHandle> handle = nullptr);

	idx_t Read(char *data, idx_t count, idx_t offset) const override;
	optional_idx End() const override {
		return size;
	}
	RowMemory Memory() const override {
		return {buffer, data};
	}
	void Want(idx_t begin, idx_t end) override;
	void ReadInPlace(idx_t begin, idx_t end) override;
	void Done(idx_t begin, idx_t end) override;

private:
	void Fetch(idx_t begin, idx_t end) const;
	void Report(idx_t begin, idx_t end) const;

	shared_ptr<MemoryMappedFile> mapping;
	optional_ptr<CSVFileHandle> handle;
	shared_ptr<RowBuffer> buffer;
	const char *data;
	idx_t size;
	mutable atomic<idx_t> wanted_to {0};
	mutable atomic<idx_t> reported_to {0};
};

class PiecedRowSource : public RowSource {
public:
	static constexpr idx_t PREFETCH_BYTES = idx_t(1) << 22;

	static shared_ptr<PiecedRowSource> Open(ClientContext &context, const string &path,
	                                        const FileCompressionType &compression, bool on_disk, idx_t size);
	PiecedRowSource(shared_ptr<MemoryMappedFile> mapping, CompressedFileSystem &compressed, vector<PieceSpan> pieces);

	idx_t Read(char *data, idx_t count, idx_t offset) const override;
	unique_ptr<RowSourceCursor> Cursor() const override;
	optional_idx End() const override {
		return size;
	}
	idx_t Size() const {
		return size;
	}
	const vector<PieceSpan> &Pieces() const;

private:
	friend class PiecedRowCursor;

	unique_ptr<PieceDecompressor> Take() const;
	void Give(unique_ptr<PieceDecompressor> decompressor) const;

	shared_ptr<MemoryMappedFile> mapping;
	CompressedFileSystem &compressed;
	vector<PieceSpan> pieces;
	const_data_ptr_t data;
	idx_t size;
	mutable mutex lock;
	mutable vector<unique_ptr<PieceDecompressor>> idle;
};

class RowInput {
public:
	static constexpr idx_t HOST_STREAM_BYTES = 1 << 21;
	static constexpr idx_t STREAM_BYTES = 1 << 23;

	static idx_t StreamBytes(bool host_stream) {
		return host_stream ? HOST_STREAM_BYTES : STREAM_BYTES;
	}

	RowInput(ClientContext &context, CSVBufferManager &manager, const string &path, bool utf8,
	         optional_ptr<atomic<idx_t>> bytes_read);

	static shared_ptr<RowSource> HostStream(ClientContext &context, const CSVFileHandle &handle, const string &path,
	                                        bool utf8);

	RowSource &Rows() const {
		if (host_stream) {
			return *host_stream;
		}
		if (pieced) {
			return *pieced;
		}
		if (mapped) {
			return *mapped;
		}
		return *file;
	}
	void ReadInOrder() {
		if (file) {
			file->in_order = true;
		}
	}

public:
	shared_ptr<RowSource> host_stream;
	shared_ptr<PiecedRowSource> pieced;
	shared_ptr<MappedRowSource> mapped;
	unique_ptr<FileRowSource> file;
	bool stream;
	idx_t size;
};

class ScanFile {
public:
	ScanFile(ClientContext &context, OpenFileInfo file, shared_ptr<CSVBufferManager> manager);

	const string &Path() const {
		return file.path;
	}
	RowSource &Rows() const {
		return input->Rows();
	}
	bool Stream() const {
		return input->stream;
	}
	bool HostStream() const {
		return input->host_stream != nullptr;
	}
	idx_t Size() const {
		return input->size;
	}
	idx_t FileSize() const;
	void ReadInOrder();
	void Consume(idx_t bytes) {
		consumed += bytes;
	}
	void Rewind() {
		consumed = 0;
	}
	double Progress() const;

public:
	OpenFileInfo file;
	idx_t skipped = 0;

private:
	shared_ptr<CSVBufferManager> manager;
	unique_ptr<RowInput> input;
	atomic<idx_t> consumed {0};
};

} // namespace duckdb
