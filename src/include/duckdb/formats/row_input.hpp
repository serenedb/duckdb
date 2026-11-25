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

class RowInput {
public:
	static constexpr idx_t HOST_STREAM_BYTES = 1 << 21;

	RowInput(ClientContext &context, CSVBufferManager &manager, const string &path, bool utf8,
	         optional_ptr<atomic<idx_t>> bytes_read);

	static shared_ptr<RowSource> HostStream(ClientContext &context, const CSVFileHandle &handle, const string &path,
	                                        bool utf8);

	RowSource &Rows() const {
		return host_stream ? *host_stream : *file;
	}

public:
	shared_ptr<RowSource> host_stream;
	unique_ptr<FileRowSource> file;
	bool stream;
	idx_t size;
};

} // namespace duckdb
