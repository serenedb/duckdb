//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_sniff.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bitset.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/formats/csv_scan.hpp"

namespace duckdb {

struct CsvColumnCount {
	idx_t number_of_columns = 0;
	bool last_value_always_empty = true;
	bool is_comment = false;
	bool is_mid_comment = false;
	idx_t empty_lines = 0;
};

class CsvColumnCounts {
public:
	CsvColumnCounts(CSVStateMachine &state_machine, idx_t result_size);

	CsvColumnCount &operator[](idx_t index) {
		return column_counts[index];
	}
	idx_t GetMostFrequentColumnCount() const;

public:
	CSVStateMachine &state_machine;
	vector<CsvColumnCount> column_counts;
	map<idx_t, idx_t> rows_per_column_count;
	const idx_t result_size;
	idx_t result_position = 0;
	bool error = false;
};

class CsvSniffScanner {
public:
	CsvSniffScanner(shared_ptr<CSVBufferManager> buffer_manager, shared_ptr<CSVStateMachine> state_machine,
	                shared_ptr<CSVErrorHandler> error_handler, idx_t result_size);

	CsvSniffScanner &ParseChunk();
	DataChunk &ToChunk() {
		return chunk;
	}
	bool FinishedFile() const {
		return scan->Done();
	}
	CSVStateMachine &GetStateMachine() const {
		return *state_machine;
	}

public:
	shared_ptr<CSVStateMachine> state_machine;
	shared_ptr<CSVErrorHandler> error_handler;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool used_unstrictness = false;

private:
	const idx_t result_size;
	unique_ptr<CsvScanGlobal> global;
	unique_ptr<CsvScan> scan;
	DataChunk chunk;
	DataChunk part;
};

class CsvColumnCounter {
public:
	CsvColumnCounter(shared_ptr<CSVBufferManager> buffer_manager, shared_ptr<CSVStateMachine> state_machine,
	                 shared_ptr<CSVErrorHandler> error_handler, idx_t result_size, idx_t start = 0);

	CsvColumnCounts &ParseChunk();
	CsvColumnCounts &GetResult() {
		return result;
	}
	CSVStateMachine &GetStateMachine() const {
		return *state_machine;
	}
	unique_ptr<CsvSniffScanner> UpgradeToStringValueScanner() const;
	bool FinishedFile() const;
	const RowDialect &Dialect() const {
		return lexer.Dialect();
	}
	idx_t CountedTo() const;
	void CountLike(const CsvColumnCounter &other);

	static idx_t SkipRows(CSVBufferManager &buffer_manager, const CSVStateMachine &state_machine, idx_t rows);

public:
	shared_ptr<CSVBufferManager> buffer_manager;
	shared_ptr<CSVStateMachine> state_machine;
	shared_ptr<CSVErrorHandler> error_handler;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool used_unstrictness = false;
	idx_t group = 0;

private:
	bool NextBuffer();
	bool AddRows(idx_t first);
	void AddRow(const RowCount &row);
	string LineText(idx_t begin, idx_t end) const;
	bool LineTooLong(idx_t begin, idx_t end);

private:
	CsvColumnCounts result;
	RowLexer lexer;
	RowCursor cursor;
	vector<RowCount> rows;
	shared_ptr<CSVBufferHandle> cur_buffer_handle;
	vector<char> window;
	vector<idx_t> boundaries;
	idx_t next_boundary = 0;
	idx_t window_start = 0;
	idx_t buffer_idx = 0;
	idx_t buffer_start = 0;
	idx_t buffer_used = 0;
	bool done = false;
	bool stitching = false;
	bool eof = false;
	bool finished = false;
};

RowDialect CountingDialect(const RowDialect &dialect, const bitset<256> &present);

class CsvCandidateBytes {
public:
	static constexpr idx_t START_BYTES = 1 << 20;

	CsvCandidateBytes(CSVBufferManager &buffer_manager, const vector<unique_ptr<CsvColumnCounter>> &counters);

	bool Covers(idx_t end);
	RowDialect Counting(const RowDialect &dialect) const {
		return CountingDialect(dialect, present);
	}
	idx_t NewGroup() {
		return next_group++;
	}

private:
	shared_ptr<CSVBufferHandle> first;
	bitset<256> tracked;
	bitset<256> present;
	idx_t covered = 0;
	idx_t next_group = 0;
};

} // namespace duckdb
