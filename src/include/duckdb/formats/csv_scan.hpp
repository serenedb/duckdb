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
#include "duckdb/formats/row_ranges.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/table_filter_state.hpp"

#include <span>

namespace duckdb {

bool CsvDecodesDirectly(const LogicalType &type, bool icu_loaded);
string_t CsvRemoveEscape(const char *value, idx_t size, char escape, char quote, bool strict_mode, Vector &vector);
void CsvSanitize(string &text);

class CsvBufferView : public RowBuffer {
public:
	explicit CsvBufferView(shared_ptr<CSVBufferHandle> handle_p) : handle(std::move(handle_p)) {
	}

private:
	shared_ptr<CSVBufferHandle> handle;
};

class CsvRowSource : public RowSource {
public:
	CsvRowSource(CSVBufferManager &manager, idx_t size, optional_ptr<atomic<idx_t>> bytes_read);

	idx_t Read(char *data, idx_t count, idx_t offset) const override;
	void Release(idx_t position) override;

public:
	CSVBufferManager &manager;
	const idx_t size;
	optional_ptr<atomic<idx_t>> bytes_read;
	idx_t released = 0;
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
	bool placeholder = false;
};

enum class CsvRawKind : uint8_t { COMPARE, IN_LIST, IS_NULL, IS_NOT_NULL, LIKE, NOT_LIKE };

struct CsvRawPredicate {
	CsvRawKind kind;
	ExpressionType compare = ExpressionType::INVALID;
	vector<string> values;
	bool anchored_start = false;
	bool anchored_end = false;
};

struct LeanSource {
	bool IsNull(const char *value, idx_t size, bool quoted) const {
		if (quoted && !allow_quoted_nulls) {
			return false;
		}
		if (empty_only_null) {
			return size == 0;
		}
		for (auto &null_string : *null_strings) {
			if (size == null_string.size() && memcmp(null_string.c_str(), value, size) == 0) {
				return true;
			}
		}
		return false;
	}

	const uint32_t *begins;
	idx_t begin_stride;
	idx_t begin_bias;
	const uint32_t *ends;
	idx_t stride;
	const char *data;
	const char *limit;
	idx_t shift;
	char quote;
	char escape_probe;
	char unquoted_escape;
	bool quotes;
	bool doubled;
	bool padded;
	bool pad_empty;
	bool spaced;
	bool allow_quoted_nulls;
	bool empty_only_null;
	const vector<string> *null_strings;
	const vector<bool> *selected;
	idx_t first;
};

struct CsvScanFilter {
	idx_t output_column;
	reference<const TableFilter> filter;
	idx_t column = DConstants::INVALID_INDEX;
	vector<CsvRawPredicate> raw;
};

class CsvScanGlobal {
public:
	CsvScanGlobal(ClientContext &context, const CSVReaderOptions &options, CsvScanSource source);
	CsvScanGlobal(ClientContext &context, CSVFileScan &file, optional_ptr<TableFilterSet> filters);

	idx_t MaxThreads() const {
		return options.parallel ? MaxValue<idx_t>(ranges->range_count, 1) : 1;
	}
	bool Claim(idx_t &range) {
		return ranges->Claim(range);
	}
	bool FinishRange(idx_t range, idx_t lines);
	bool AllFinished() const {
		return ranges->AllFinished();
	}
	vector<unique_ptr<AsyncTask>> LoadTasks(idx_t range) const;
	shared_ptr<CSVBufferHandle> Buffer(idx_t buffer) const {
		return source.buffer_manager->GetBuffer(buffer);
	}

public:
	ClientContext &context;
	const CSVReaderOptions &options;
	CsvScanSource source;
	CsvGrammar grammar;
	unique_ptr<RowLexer> lean;
	unique_ptr<CsvRowSource> row_source;
	unique_ptr<RowRanges> ranges;
	idx_t num_columns = 0;
	bool count_only = false;
	bool lean_batches = false;
	bool empty_only_null = false;
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
	bool stream = false;
	idx_t skip_rows = 0;
	bool skip_bom = false;
	idx_t start = 0;
	idx_t start_lines = 0;

private:
	void FindStart(idx_t size, shared_ptr<CSVBufferHandle> &whole_file);
};

class CsvScan {
public:
	explicit CsvScan(CsvScanGlobal &global);

	void Start(idx_t range);
	bool Scan(DataChunk &output, idx_t max_rows = STANDARD_VECTOR_SIZE);
	idx_t Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output);
	bool Done() const {
		return done;
	}
	bool UsedUnstrictness() const {
		return used_unstrictness || cursor.unstrict || cursor.extras_dropped;
	}
	bool EverQuoted() const {
		return cursor.ever_quoted;
	}
	bool EverEscaped() const {
		return cursor.ever_escaped;
	}

	static bool NeedsFlushCast(const CsvScanColumn &column);
	static bool EvaluatesOnFieldBytes(const Expression &expr);

private:
	bool ScanLean(DataChunk &output, idx_t max_rows);
	bool DecodeLean(idx_t count, DataChunk &output);
	bool DecodeLeanBatch(idx_t first, idx_t count, DataChunk &output);
	void FinishLeanChunk(idx_t count, DataChunk &output);
	bool DecodeLeanColumn(idx_t column, idx_t first, idx_t count, DataChunk &output, bool selected_only,
	                      bool utf8_valid, bool hold);
	LeanSource LeanSourceFor(const CsvScanColumn &column, idx_t first, bool selected_only) const;
	bool LeanRawFilter(idx_t count);
	void LeanRowNumbers(idx_t count, DataChunk &output);
	void LeanRetry(const CsvScanColumn &column, Vector &vector);
	CsvField LeanField(const CsvScanColumn &column, idx_t row) const;
	CsvRow LeanRow(idx_t row) const;
	bool LeanNullExtra(idx_t begin, idx_t end, uint32_t end_bits) const;
	CSVErrorType EventType(const RowEvent &event) const;
	void LeanRowErrors(idx_t row, const RowAnomaly *anomaly, vector<RowEvent>::const_iterator &event, bool unfinished,
	                   bool long_row, vector<CsvRowError> &values, vector<CsvRowError> &errors);
	idx_t LeanLinesAt(idx_t position);
	void LeanReport(idx_t first, idx_t count);
	void ClearRowState(idx_t first, idx_t count);
	Vector &Target(idx_t column, DataChunk &output);
	void Filter(idx_t count, DataChunk &output);
	bool DecodeValue(const CsvScanColumn &column, const CsvField &field, Vector &vector, idx_t row, CsvRowError &error);
	bool IsNull(const CsvField &field, const char *value, idx_t size) const;
	void ReportRow(idx_t row_idx, const CsvRow &row, vector<CsvRowError> &errors);
	void Report(idx_t row, CSVError &error, bool force);
	void FlushCasts(idx_t count, DataChunk &output, bool late, bool filter_pass);
	string LineText(const CsvRow &row, bool &first_nl) const;

private:
	CsvScanGlobal &global;
	CsvScanSource &source;
	const CSVReaderOptions &options;
	const CsvGrammar &grammar;
	RowRangeReader reader;
	shared_ptr<RowBytes> &bytes;
	RowCursor &cursor;
	RowFields &fields;
	idx_t base = 0;
	bool done = true;
	bool started = false;
	bool reported = true;
	idx_t lean_rows = DConstants::INVALID_INDEX;
	bool counting = false;
	bool chunk_open = false;
	idx_t chunk_rows = 0;
	const RowBytes *held = nullptr;
	bool used_unstrictness = false;
	idx_t range = 0;
	std::span<const int64_t> lookup_offsets;
	vector<idx_t> lookup_rows;
	vector<vector<CsvRowError>> value_errors;
	vector<bool> failed;
	vector<bool> selected;
	vector<idx_t> retry;
	bool lean_errors = false;
	bool rows_failed = false;
	vector<unique_ptr<TableFilterState>> filter_states;
	SelectionVector filter_selection;
	SelectionVector filter_candidates;
	SelectionVector keep;
	vector<CsvRowError> row_errors;
	DataChunk parse_chunk;
	DataChunk scratch_chunk;
};

} // namespace duckdb
