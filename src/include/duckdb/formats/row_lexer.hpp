//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_lexer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/formats/csv_grammar.hpp"

namespace duckdb {

enum class RowQuoting : uint8_t { NONE, FIELD_START, ANYWHERE };

struct RowDialect {
	static RowDialect DuckDB(const CsvGrammar &grammar);
	static RowDialect PostgresCsv(char delimiter, char quote, char escape, char line_break = '\n');
	static RowDialect PostgresText(char delimiter, char line_break = '\n');

	char delimiter = ',';
	char delimiter_tail[3] = {};
	idx_t delimiter_size = 1;
	bool distinct_delimiter = true;
	char quote = '\0';
	char escape = '\0';
	char comment = '\0';
	RowQuoting quoting = RowQuoting::NONE;
	bool duckdb = false;
	bool backslash = false;
	bool escape_in_quotes = false;
	bool unquoted_escape = false;
	bool carriage_terms = false;
	bool strip_carriage = false;
	bool merge_terms = false;
	bool empty_lines = false;
	bool carry_on = false;
	bool strict = false;
	bool reopen = false;
	bool empty_space = false;
	bool quoted_spaces = false;
	bool physical_lines = false;
	char line_break = '\n';
	NewLineIdentifier new_line = NewLineIdentifier::NOT_SET;
};

static constexpr uint32_t ROW_FIELD_FLAG = uint32_t(1) << 31;
static constexpr uint32_t ROW_FIELD_MASK = ROW_FIELD_FLAG - 1;

struct RowAnomaly {
	uint32_t row;
	uint32_t fields;
	uint32_t end;
	uint32_t extras;
};

struct RowEvent {
	uint32_t row;
	uint32_t column;
	uint32_t position;
};

struct RowFields {
	void Clear() {
		rows = row_store.get();
		ends = end_store.get();
		row_count = 0;
		end_count = 0;
		base = 0;
		anomalies.clear();
		anomaly_head = 0;
		extras.clear();
		open_extras = 0;
		events.clear();
		event_head = 0;
	}
	void Reserve(idx_t more);
	bool Fits(idx_t rows_used, idx_t ends_used, idx_t more) const;
	void Drop(idx_t rows, idx_t ends);
	void Rebase(uint32_t delta);
	const RowAnomaly *Anomaly(idx_t row) const;
	bool HasAnomalies() const {
		return anomaly_head < anomalies.size();
	}

	uint32_t *rows = nullptr;
	uint32_t *ends = nullptr;
	unsafe_unique_array<uint32_t> row_store;
	unsafe_unique_array<uint32_t> end_store;
	idx_t capacity = 0;
	idx_t row_count = 0;
	idx_t end_count = 0;
	idx_t base = 0;
	vector<RowAnomaly> anomalies;
	idx_t anomaly_head = 0;
	vector<uint32_t> extras;
	idx_t open_extras = 0;
	vector<RowEvent> events;
	idx_t event_head = 0;
};

struct RowCursor {
	idx_t origin = 0;
	idx_t anchor = 0;
	idx_t pos = 0;
	uint64_t in_quotes = 0;
	uint64_t after_delimiter = 0;
	uint64_t after_term = 1;
	uint64_t after_cr = 0;
	uint64_t escaped_cr = 0;
	uint64_t maybe = 0;
	uint64_t after_plain_cr = 0;
	uint64_t after_closer = 0;
	uint64_t after_opener = 0;
	uint64_t start_space = 0;
	uint64_t escape_carry = 0;
	uint64_t pending_flag = 0;
	uint64_t in_comment = 0;
	uint64_t delimiter_tail = 0;
	uint64_t partial_tail = 0;
	uint64_t after_partial = 0;
	uint64_t after_event = 0;
	uint64_t partial_before_last = 0;
	uint64_t non_ascii = 0;
	idx_t phase = 0;
	idx_t open_rows = 0;
	idx_t rows = 0;
	idx_t lines = 0;
	idx_t first_row_end = DConstants::INVALID_INDEX;
	idx_t last_row_end = DConstants::INVALID_INDEX;
	idx_t first_term = DConstants::INVALID_INDEX;
	idx_t first_boundary = DConstants::INVALID_INDEX;
	idx_t last_term = DConstants::INVALID_INDEX;
	idx_t quoted_newline = DConstants::INVALID_INDEX;
	idx_t field_start = 0;
	idx_t delimiters = 0;
	idx_t empty_lines = 0;
	idx_t line_start = 0;
	bool field_blank = true;
	bool first_term_cr = false;
	bool last_term_cr = false;
	bool file_start = false;
	bool comment = false;
	bool comment_line = false;
	bool trailing_empty = false;
	bool reopen_last = false;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool invalid = false;
	bool quotes = false;
	bool doubled = false;
	bool padded = false;
	bool extras_dropped = false;
	bool long_rows = false;
	bool unstrict = false;
	bool unfinished = false;
	bool settled = false;
	bool unterminated = false;
	bool check_arity = true;
	bool ok = true;
};

static constexpr uint8_t ROW_COUNT_EMPTY_LAST = 1;
static constexpr uint8_t ROW_COUNT_COMMENT = 2;
static constexpr uint8_t ROW_COUNT_MID_COMMENT = 4;

struct RowCount {
	idx_t columns;
	idx_t empty_lines;
	idx_t begin;
	idx_t end;
	uint8_t flags;
};

struct RowShapes {
	vector<RowCount> &rows;
	idx_t limit;
	bool skip = false;
};

class RowLexer {
public:
	RowLexer(const RowDialect &dialect, idx_t columns, idx_t max_line_size, bool error_mode, bool pad_rows = false,
	         bool irregular_rows = false);

	const RowDialect &Dialect() const {
		return dialect;
	}
	idx_t Columns() const {
		return columns;
	}
	idx_t Lookahead() const {
		return dialect.delimiter_size - 1;
	}
	void Start(RowCursor &cursor, idx_t begin, bool settled, bool check_arity = true) const;
	bool Feed(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields) const;
	bool Finish(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool eof, RowFields *fields,
	            CSVState origin_state, CsvRangeEntry &entry) const;
	idx_t CountLines(const char *data, idx_t base, idx_t begin, idx_t end) const;
	void CountRows(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowShapes &shapes) const;
	void FinishCount(RowCursor &cursor, vector<RowCount> &rows) const;

private:
	template <bool EXTRACT, bool SHAPE = false>
	bool FeedBlocks(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields,
	                RowShapes *shapes = nullptr) const;
	template <bool EXTRACT, bool SHAPE>
	bool LeanBlocks(RowCursor &cursor, const char *data, idx_t base, idx_t end, uint32_t *&rows_out,
	                uint32_t *&ends_out, RowShapes *shapes) const;
	void EndRow(RowCursor &cursor, RowFields &fields, uint32_t entry, idx_t rows_written, uint32_t *&out) const;
	void ShapeRow(RowCursor &cursor, idx_t end, uint8_t flags, vector<RowCount> &rows) const;

private:
	const RowDialect dialect;
	const idx_t columns;
	const idx_t max_line_size;
	const bool pad_rows;
	const bool irregular_rows;
	bool settle_strict;
	bool settle_length;
	bool lean;
	bool lean_shapes;
	vector<uint64_t> patterns;
};

class RowSkip {
public:
	RowSkip(const RowDialect &dialect, idx_t rows, idx_t start = 0);

	bool Feed(const char *data, idx_t base, idx_t end, bool last);
	idx_t Start() const {
		return start;
	}
	idx_t Lines() const {
		return cursor.lines;
	}

private:
	RowLexer lexer;
	RowCursor cursor;
	vector<RowCount> skipped;
	vector<char> window;
	idx_t window_start;
	idx_t rows;
	idx_t start = DConstants::INVALID_INDEX;
};

} // namespace duckdb
