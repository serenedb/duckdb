//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_input.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
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

class RowInput {
public:
	static constexpr idx_t HOST_STREAM_BYTES = 1 << 21;

	RowInput(ClientContext &context, CSVBufferManager &manager, const string &path, bool utf8,
	         optional_ptr<atomic<idx_t>> bytes_read);

	static shared_ptr<RowSource> HostStream(ClientContext &context, const CSVFileHandle &handle, const string &path,
	                                        bool utf8);

	RowSource &Rows() const {
		if (host_stream) {
			return *host_stream;
		}
		if (mapped) {
			return *mapped;
		}
		return *file;
	}

public:
	shared_ptr<RowSource> host_stream;
	shared_ptr<MappedRowSource> mapped;
	unique_ptr<FileRowSource> file;
	bool stream;
	idx_t size;
};

} // namespace duckdb
