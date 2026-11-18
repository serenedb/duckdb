//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_splitter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine_cache.hpp"

namespace duckdb {

enum CsvFieldFlags : uint8_t {
	CSV_FIELD_QUOTED = 1,
	CSV_FIELD_ESCAPED = 2,
};

struct CsvField {
	uint32_t start;
	uint32_t offset;
	uint32_t size;
	uint8_t flags;
};

struct CsvExtraField {
	idx_t row;
	idx_t column;
	CsvField field;
};

enum CsvRowFlags : uint8_t {
	CSV_ROW_EMPTY_LINE = 1,
	CSV_ROW_QUOTED_NEWLINE = 2,
	CSV_ROW_EVENTS = 4,
	CSV_ROW_UNFINISHED = 8,
};

struct CsvRow {
	idx_t start;
	idx_t begin;
	idx_t end;
	idx_t last_position;
	idx_t lines;
	uint32_t field_count;
	uint8_t flags;
};

enum class CsvRowEventKind : uint8_t {
	UNTERMINATED_QUOTES,
	INVALID_STATE,
};

struct CsvRowEvent {
	idx_t row;
	idx_t column;
	idx_t position;
	CsvRowEventKind kind;
};

struct CsvRangeStart {
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	idx_t line_end = DConstants::INVALID_INDEX;
	idx_t last_position = DConstants::INVALID_INDEX;

	void Inherit(const CsvRangeStart &previous) {
		if (line_end == DConstants::INVALID_INDEX) {
			line_end = previous.line_end;
		}
		if (last_position == DConstants::INVALID_INDEX) {
			last_position = previous.last_position;
		}
	}
};

struct CsvRangeEntry {
	idx_t first_boundary = DConstants::INVALID_INDEX;
	CsvRangeStart boundary;
	CsvRangeStart end;
};

using CsvRangeTransfer = array<CsvRangeEntry, StateMachine::NUM_STATES>;

struct CsvGrammar {
	const StateMachine *transitions = nullptr;
	idx_t extra_delimiter_bytes = 0;
	char quote = '\0';
	char escape = '\0';
	bool strict_mode = false;
	bool carry_on_newline = false;
	bool only_crlf = false;
	bool trim_quoted_spaces = false;
};

void CsvTransfer(const CsvGrammar &grammar, const char *bytes, idx_t base, idx_t begin, idx_t end,
                 CsvRangeTransfer &transfer);

class CsvSplitter {
public:
	CsvSplitter(const CsvGrammar &grammar, idx_t columns, idx_t capacity);

	void Reset(idx_t pos, idx_t lines, const CsvRangeStart &start = CsvRangeStart());
	idx_t Split(const char *bytes, idx_t base, idx_t end, idx_t max_rows, bool last,
	            idx_t stop_at = NumericLimits<idx_t>::Maximum(), bool append = false);
	void ClearRows();

	idx_t KeepFrom() const {
		return boundary.line_end < boundary.pos ? boundary.line_end : boundary.pos;
	}
	bool Finished() const {
		return finished;
	}
	const CsvField *Column(idx_t column) const {
		return fields.data() + column * capacity;
	}
	const vector<CsvExtraField> &Extras() const {
		return extras;
	}
	const CsvRow &Row(idx_t row) const {
		return rows[row];
	}
	const vector<CsvRowEvent> &Events() const {
		return events;
	}
	idx_t Columns() const {
		return columns;
	}
	idx_t Capacity() const {
		return capacity;
	}
	idx_t LinesRead() const {
		return lines_read;
	}

private:
	struct Boundary {
		idx_t pos = 0;
		CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
		idx_t last_position = 0;
		idx_t line_begin = 0;
		idx_t line_end = 0;
		idx_t lines_read = 0;
		bool ever_quoted = false;
	};

	char At(idx_t position) const {
		return bytes[position - base];
	}
	void Transition(char byte) {
		states[0] = states[1];
		states[1] = grammar.transitions->state_machine[static_cast<uint8_t>(byte)][static_cast<uint8_t>(states[1])];
	}
	void AddValue(idx_t position);
	void AddQuotedValue(idx_t position);
	void AddField(idx_t offset, idx_t size, bool empty);
	void AddRow(idx_t position);
	void EmptyLine(idx_t position);
	void UnsetComment(idx_t position);
	void Event(CsvRowEventKind kind, idx_t column, idx_t position);
	void Finish();
	void SaveBoundary();
	void RestoreBoundary();

private:
	const CsvGrammar grammar;
	const idx_t columns;
	const idx_t capacity;
	vector<CsvField> fields;
	vector<CsvExtraField> extras;
	vector<CsvRow> rows;
	vector<CsvRowEvent> events;
	Boundary boundary;

	const char *bytes = nullptr;
	idx_t base = 0;
	idx_t end = 0;
	idx_t pos = 0;
	idx_t scan_pos = 0;
	idx_t row_count = 0;
	idx_t lines_read = 0;
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	idx_t field_count = 0;
	idx_t last_position = 0;
	idx_t quoted_position = 0;
	bool quoted = false;
	bool ever_quoted = false;
	bool escaped = false;
	bool comment = false;
	idx_t position_before_comment = 0;
	uint8_t row_flags = 0;
	idx_t line_begin = 0;
	idx_t line_end = 0;
	bool finished = false;
};

} // namespace duckdb
