//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_source.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/formats/row_input.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

class JsonSource {
public:
	JsonSource(ClientContext &context, OpenFileInfo file, FileCompressionType compression);

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
	void Rewind() {
		consumed = 0;
	}
	void ReadInOrder() {
		if (input->file) {
			input->file->in_order = true;
		}
	}
	void Consume(idx_t bytes) {
		consumed += bytes;
	}
	double Progress() const;

private:
	OpenFileInfo file;
	shared_ptr<CSVBufferManager> manager;
	unique_ptr<RowInput> input;
	atomic<idx_t> consumed {0};
};

} // namespace duckdb
