//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_sequential_buffer_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"

namespace duckdb {

//! Buffer manager for sequential sources (pipes, compressed or non-utf-8 files)
class CSVSequentialBufferManager : public CSVBufferManager {
public:
	CSVSequentialBufferManager(ClientContext &context, const CSVReaderOptions &options, const OpenFileInfo &file,
	                           bool per_file_single_threaded, unique_ptr<CSVFileHandle> file_handle);

	shared_ptr<CSVBufferHandle> GetBuffer(const idx_t buffer_idx) override;
	CSVBufferResidency GetBufferResidency(const idx_t buffer_idx, shared_ptr<CSVBufferHandle> &handle) override;
	void ResetBuffer(const idx_t buffer_idx) override;
	bool Done() const override;
	optional_idx KnownEnd() override;
	idx_t ReadInOrder(char *data, idx_t count, idx_t offset) override;
	void ResetBufferManager() override;

private:
	void Initialize();
	void MarkDone(const CSVBuffer &last);
	//! Releases the chain's own pin on the predecessor of an accessed buffer
	void UnpinPrevious(const idx_t pos);
	void Recycle(shared_ptr<CSVBuffer> &buffer);

	mutex read_lock;
	shared_ptr<CSVBuffer> spare;
	optional_idx direct_position;

	//! The last buffer it was accessed
	shared_ptr<CSVBuffer> last_buffer;
	idx_t global_csv_pos = 0;
	//! If this buffer manager is done (i.e., no more buffers to read beyond the ones that were cached
	bool done = false;
	optional_idx known_end;
	//! If the file_handle used seek
	bool has_seeked = false;
	unordered_set<idx_t> reset_when_possible;
	bool is_pipe;
};

} // namespace duckdb
