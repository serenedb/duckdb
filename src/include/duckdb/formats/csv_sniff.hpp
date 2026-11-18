//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_sniff.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

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
	const CsvColumnCount &operator[](idx_t index) const {
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

	static idx_t SkipRows(CSVBufferManager &buffer_manager, const CSVStateMachine &state_machine, idx_t rows);

public:
	shared_ptr<CSVBufferManager> buffer_manager;
	shared_ptr<CSVStateMachine> state_machine;
	shared_ptr<CSVErrorHandler> error_handler;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool used_unstrictness = false;

private:
	struct Position {
		idx_t buffer_idx = 0;
		idx_t buffer_pos = 0;
		idx_t buffer_size = 0;

		idx_t operator-(const Position &other) const {
			if (other.buffer_idx == buffer_idx) {
				return buffer_pos - other.buffer_pos;
			}
			return other.buffer_size - other.buffer_pos + buffer_pos;
		}
	};

	void Process();
	void FinalizeChunkProcess();
	bool AddRow(idx_t buffer_pos);
	bool UnsetComment(idx_t buffer_pos);
	void SetComment();
	string LineText(const Position &begin, const Position &end);
	bool LineTooLong(const Position &end);

private:
	CsvColumnCounts result;
	CSVByteSkipper skipper;
	shared_ptr<CSVBufferHandle> cur_buffer_handle;
	const char *buffer_ptr = nullptr;
	idx_t buffer_idx = 0;
	idx_t buffer_pos = 0;
	idx_t cur_buffer_idx = 0;
	idx_t current_buffer_size = 0;
	bool done = false;
	bool initialized = false;
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	map<idx_t, shared_ptr<CSVBufferHandle>> buffer_handles;
	Position last_position;
	idx_t current_column_count = 0;
	idx_t empty_lines = 0;
	bool comment = false;
	bool cur_line_starts_as_comment = false;
};

} // namespace duckdb
