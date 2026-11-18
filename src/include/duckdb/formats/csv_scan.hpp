//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/formats/csv_splitter.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/table_filter_state.hpp"

#include <span>

namespace duckdb {

bool CsvDecodesDirectly(const LogicalType &type, bool icu_loaded);
string_t CsvRemoveEscape(const char *value, idx_t size, char escape, char quote, bool strict_mode, Vector &vector);
void CsvSanitize(string &text);
idx_t CsvRangeBytes(const CSVReaderOptions &options);

struct CsvBytes {
	shared_ptr<CSVBufferHandle> buffer;
	unsafe_unique_array<char> owned;
	const char *data = nullptr;
	idx_t size = 0;
};

class CsvBytesHolder : public AuxiliaryDataHolder {
public:
	explicit CsvBytesHolder(shared_ptr<CsvBytes> bytes_p) : bytes(std::move(bytes_p)) {
	}

private:
	shared_ptr<CsvBytes> bytes;
};

struct CsvBufferUse {
	CsvBufferUse(CSVBufferManager &manager_p, idx_t buffer_p) : manager(manager_p), buffer(buffer_p) {
	}
	~CsvBufferUse() {
		manager.ResetBuffer(buffer);
	}

	CSVBufferManager &manager;
	const idx_t buffer;
};

struct CsvScanColumn {
	idx_t file_column;
	idx_t output_column;
	LogicalType type;
	LogicalType parse_type;
	bool validate_utf8;
	bool force_not_null;
};

struct CsvRowError {
	CSVErrorType type;
	idx_t column;
	idx_t position;
	string message;
};

struct CsvScanSource {
	shared_ptr<CSVStateMachine> state_machine;
	shared_ptr<CSVBufferManager> buffer_manager;
	shared_ptr<CSVErrorHandler> error_handler;
	vector<std::pair<idx_t, idx_t>> projection;
	vector<LogicalType> types;
	vector<string> names;
	string path;
	idx_t file_row_number = DConstants::INVALID_INDEX;
	optional_ptr<atomic<idx_t>> bytes_read;
	optional_ptr<TableFilterSet> filters;
	bool sniffing = false;
};

struct CsvScanFilter {
	idx_t output_column;
	reference<const TableFilter> filter;
};

class CsvScanGlobal {
public:
	CsvScanGlobal(ClientContext &context, const CSVReaderOptions &options, CsvScanSource source);
	CsvScanGlobal(ClientContext &context, CSVFileScan &file, optional_ptr<TableFilterSet> filters);

	idx_t MaxThreads() const {
		return parallel ? MaxValue<idx_t>(range_count, 1) : 1;
	}
	bool Claim(idx_t &range, vector<shared_ptr<CsvBufferUse>> &uses);
	bool FinishRange(idx_t range, idx_t lines);
	bool AllFinished() const;
	void Publish(idx_t range, const CsvRangeTransfer &transfer);
	void StartComputing(idx_t range);
	CsvRangeStart StartState(idx_t range);
	vector<unique_ptr<AsyncTask>> LoadTasks(idx_t range) const;
	idx_t RangeBuffer(idx_t range) const {
		return range / parts;
	}
	idx_t RangeBegin(idx_t range) const {
		return MaxValue(RangeBuffer(range) * buffer_size + (range % parts) * range_bytes, start);
	}
	idx_t RangeEnd(idx_t range) const {
		const idx_t buffer = RangeBuffer(range);
		const idx_t end = buffer * buffer_size + MinValue((range % parts + 1) * range_bytes, buffer_size);
		return MinValue(end, size);
	}
	bool LastPart(idx_t range) const {
		return range % parts == parts - 1;
	}
	shared_ptr<CSVBufferHandle> Buffer(idx_t buffer) const {
		return source.buffer_manager->GetBuffer(buffer);
	}
	idx_t Read(char *data, idx_t count, idx_t offset) const;

public:
	ClientContext &context;
	const CSVReaderOptions &options;
	CsvScanSource source;
	CsvGrammar grammar;
	vector<CsvScanColumn> columns;
	vector<CsvScanFilter> filters;
	vector<bool> filtered;
	bool late_materialization = false;
	StrpTimeFormat date_format;
	StrpTimeFormat timestamp_format;
	char decimal_separator = '.';
	vector<string> null_strings;
	bool empty_is_null = false;
	bool ignore_errors = false;
	bool print_line = true;
	bool parallel = false;
	idx_t skip_rows = 0;
	bool skip_bom = false;
	idx_t start = 0;
	idx_t start_lines = 0;
	idx_t size = 0;
	idx_t buffer_size = 0;
	idx_t buffer_count = 0;
	idx_t range_bytes = 0;
	idx_t parts = 1;
	idx_t range_count = 0;

private:
	void FindStart();
	void ComputeTransfer(idx_t range);
	shared_ptr<CsvBufferUse> Use(idx_t buffer);

private:
	mutable mutex lock;
	idx_t next_range = 0;
	idx_t finished_ranges = 0;
	idx_t first_range = 0;
	idx_t resolved = 0;
	bool sequential_claimed = false;
	vector<CsvRangeTransfer> transfers;
	vector<bool> published;
	vector<bool> computing;
	vector<CsvRangeStart> starts;
	vector<shared_ptr<CsvBufferUse>> buffer_uses;
};

class CsvScan {
public:
	explicit CsvScan(CsvScanGlobal &global);

	void Start(idx_t range, vector<shared_ptr<CsvBufferUse>> uses);
	bool Scan(DataChunk &output, idx_t max_rows = STANDARD_VECTOR_SIZE);
	idx_t Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output);
	bool Done() const {
		return done;
	}
	bool UsedUnstrictness() const {
		return used_unstrictness || splitter->UsedUnstrictness();
	}
	bool EverQuoted() const {
		return splitter->EverQuoted();
	}
	bool EverEscaped() const {
		return splitter->EverEscaped();
	}

private:
	static bool NeedsFlushCast(const CsvScanColumn &column);
	bool ReadMore(idx_t keep);
	bool ReadRange();
	void SkipRows();
	void View(shared_ptr<CSVBufferHandle> handle, idx_t position);
	void Decode(idx_t count, DataChunk &output);
	Vector &Target(idx_t column, DataChunk &output);
	void DecodeColumn(idx_t column, idx_t count, DataChunk &output, bool selected_only);
	void FillRowNumbers(idx_t count, DataChunk &output);
	void Filter(idx_t count, DataChunk &output);
	bool DecodeValue(const CsvScanColumn &column, const CsvField &field, Vector &vector, idx_t row, CsvRowError &error);
	bool IsNull(const CsvField &field, const char *value, idx_t size) const;
	void RowErrors(idx_t row, vector<CsvRowError> &value_errors, vector<CsvRowError> &errors);
	void ReportRow(idx_t row, vector<CsvRowError> &errors);
	void Report(idx_t row, CSVError &error, bool force);
	void FlushCasts(idx_t count, DataChunk &output, bool late, bool filter_pass);
	string LineText(const CsvRow &row, bool &first_nl) const;

private:
	CsvScanGlobal &global;
	CsvScanSource &source;
	const CSVReaderOptions &options;
	const CsvGrammar &grammar;
	unique_ptr<CsvSplitter> splitter;
	shared_ptr<CsvBytes> bytes;
	vector<shared_ptr<CsvBufferUse>> uses;
	idx_t base = 0;
	shared_ptr<CSVBufferHandle> pending;
	idx_t pending_start = 0;
	idx_t pending_used = 0;
	idx_t next_buffer = 0;
	idx_t next_buffer_start = 0;
	idx_t stitch_bytes = 0;
	bool last_buffer = false;
	bool done = true;
	bool started = false;
	bool reported = true;
	bool no_rows = false;
	bool used_unstrictness = false;
	idx_t range = 0;
	idx_t range_begin = 0;
	idx_t range_end = 0;
	std::span<const int64_t> lookup_offsets;
	vector<idx_t> lookup_rows;
	vector<vector<CsvRowError>> value_errors;
	vector<bool> failed;
	vector<bool> dropped;
	vector<bool> selected;
	vector<unique_ptr<TableFilterState>> filter_states;
	SelectionVector filter_selection;
	SelectionVector keep;
	vector<CsvRowError> row_errors;
	idx_t event_cursor = 0;
	idx_t extra_cursor = 0;
	DataChunk parse_chunk;
	DataChunk scratch_chunk;
};

} // namespace duckdb
