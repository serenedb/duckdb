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
#include "duckdb/formats/csv_splitter.hpp"

namespace duckdb {

enum class RowQuoting : uint8_t { NONE, FIELD_START, ANYWHERE };

struct RowDialect {
	static RowDialect DuckDB(const CsvGrammar &grammar);
	static RowDialect PostgresCsv(char delimiter, char quote, char escape);
	static RowDialect PostgresText(char delimiter);

	char delimiter = ',';
	char delimiter_tail[3] = {};
	idx_t delimiter_size = 1;
	bool distinct_delimiter = true;
	char quote = '\0';
	char escape = '\0';
	char comment = '\0';
	RowQuoting quoting = RowQuoting::NONE;
	bool backslash = false;
	bool escape_in_quotes = false;
	bool reject_escape = false;
	bool carriage_terms = false;
	bool strip_carriage = false;
	bool merge_terms = false;
	bool carry_on = false;
	bool strict = false;
	bool reopen = false;
	bool only_crlf = false;
	NewLineIdentifier new_line = NewLineIdentifier::NOT_SET;
	bool supported = true;
	const StateMachine *transitions = nullptr;
};

static constexpr uint32_t ROW_FIELD_FLAG = uint32_t(1) << 31;
static constexpr uint32_t ROW_FIELD_MASK = ROW_FIELD_FLAG - 1;

struct RowFields {
	void Clear() {
		row_count = 0;
		end_count = 0;
	}
	void Reserve(idx_t more);
	void Drop(idx_t rows, idx_t ends);

	unsafe_unique_array<uint32_t> rows;
	unsafe_unique_array<uint32_t> ends;
	idx_t capacity = 0;
	idx_t row_count = 0;
	idx_t end_count = 0;
};

struct RowCursor {
	idx_t origin = 0;
	idx_t pos = 0;
	uint64_t in_quotes = 0;
	uint64_t after_separator = 1;
	uint64_t after_term = 1;
	uint64_t after_cr = 0;
	uint64_t after_plain_cr = 0;
	uint64_t after_closer = 0;
	uint64_t after_opener = 0;
	uint64_t escape_carry = 0;
	uint64_t pending_flag = 0;
	uint64_t in_comment = 0;
	uint64_t comment_tail = 0;
	uint64_t delimiter_tail = 0;
	uint64_t non_ascii = 0;
	idx_t phase = 0;
	idx_t rows = 0;
	idx_t lines = 0;
	idx_t first_row_end = DConstants::INVALID_INDEX;
	idx_t last_row_end = DConstants::INVALID_INDEX;
	idx_t first_term = DConstants::INVALID_INDEX;
	idx_t last_term = DConstants::INVALID_INDEX;
	idx_t quoted_newline = DConstants::INVALID_INDEX;
	bool quotes = false;
	bool doubled = false;
	bool padded = false;
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

struct RowCountCursor {
	idx_t pos = 0;
	idx_t line_start = 0;
	CSVState states[2] = {CSVState::NOT_SET, CSVState::NOT_SET};
	idx_t delimiters = 0;
	idx_t empty_lines = 0;
	bool comment = false;
	bool starts_as_comment = false;
	bool ever_quoted = false;
	bool ever_escaped = false;
	bool used_unstrictness = false;
	bool invalid = false;
	bool unterminated = false;
};

class RowLexer {
public:
	RowLexer(const RowDialect &dialect, idx_t columns, idx_t max_line_size, bool error_mode, bool pad_rows = false);

	bool Eligible() const {
		return eligible;
	}
	const RowDialect &Dialect() const {
		return dialect;
	}
	idx_t Lookahead() const {
		return dialect.delimiter_size - 1;
	}
	void Start(RowCursor &cursor, idx_t begin, bool settled, bool check_arity = true) const;
	bool Feed(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields) const;
	bool Finish(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool eof, RowFields *fields,
	            CSVState origin_state, CsvRangeEntry &entry) const;
	idx_t CountLines(const char *data, idx_t base, idx_t begin, idx_t end) const;
	void CountRows(RowCountCursor &cursor, const char *data, idx_t base, idx_t end, idx_t max_rows,
	               vector<RowCount> &rows) const;
	void FinishCount(RowCountCursor &cursor, vector<RowCount> &rows) const;

private:
	template <bool EXTRACT>
	bool FeedBlocks(RowCursor &cursor, const char *data, idx_t base, idx_t end, bool last, RowFields *fields) const;
	bool CountByte(RowCountCursor &cursor, char byte, idx_t position, vector<RowCount> &rows, idx_t limit) const;
	bool CountPlain(RowCountCursor &cursor, const char *block, idx_t from, idx_t to, uint64_t delimiters,
	                uint64_t terminators, uint64_t pairs, idx_t offset, vector<RowCount> &rows, idx_t limit) const;
	CSVState PlainState(const char *block, idx_t position, idx_t first, CSVState incoming) const;
	void CountRow(RowCountCursor &cursor, idx_t position, uint8_t flags, vector<RowCount> &rows) const;

private:
	const RowDialect dialect;
	const idx_t columns;
	const idx_t max_line_size;
	const bool pad_rows;
	bool eligible;
	bool settle_strict;
	bool settle_length;
	vector<uint64_t> patterns;
	bool count_plain = false;
	bool count_line_feeds = false;
	bool count_carriage_pairs = false;
	char count_specials[4] = {};
	idx_t count_special_count = 0;
	char standard_stops[8] = {};
	idx_t standard_stop_count = DConstants::INVALID_INDEX;
	char quoted_stops[8] = {};
	idx_t quoted_stop_count = DConstants::INVALID_INDEX;
	char comment_stops[8] = {};
	idx_t comment_stop_count = DConstants::INVALID_INDEX;
};

} // namespace duckdb
