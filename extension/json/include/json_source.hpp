//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_source.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/formats/row_ranges.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

class JsonSource {
public:
	JsonSource(ClientContext &context, OpenFileInfo file, FileCompressionType compression);

	const string &Path() const {
		return file.path;
	}
	const OpenFileInfo &File() const {
		return file;
	}
	bool Positional() const {
		return positional;
	}
	bool Remote() const {
		return remote;
	}
	idx_t Size() const {
		return size;
	}
	idx_t ReadAt(char *data, idx_t count, idx_t offset, optional_ptr<FileHandle> thread_handle = nullptr);
	idx_t Read(char *data, idx_t count);
	void Release(idx_t offset);
	idx_t Position() const {
		return position;
	}
	void Rewind();
	void StopRecording() {
		recording = false;
	}
	double Progress() const;
	unique_ptr<FileHandle> OpenThreadHandle() const;

private:
	ClientContext &context;
	OpenFileInfo file;
	unique_ptr<FileHandle> handle;
	shared_ptr<RowSource> stream;
	bool positional = false;
	bool remote = false;
	bool pipe = false;
	bool compressed = false;
	idx_t size = 0;
	idx_t position = 0;
	bool recording = true;
	vector<AllocatedData> recorded;
	idx_t recorded_size = 0;
	idx_t replayed = 0;
	atomic<idx_t> consumed {0};
};

} // namespace duckdb
