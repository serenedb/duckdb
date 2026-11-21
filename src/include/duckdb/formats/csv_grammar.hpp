//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_grammar.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"

namespace duckdb {

struct CsvGrammar {
	static CsvGrammar For(const CSVStateMachine &state_machine);

	char delimiter[4] = {};
	idx_t delimiter_size = 1;
	char quote = '\0';
	char escape = '\0';
	char comment = '\0';
	NewLineIdentifier new_line = NewLineIdentifier::NOT_SET;
	bool strict_mode = false;
	bool carry_on_newline = false;
	bool trim_quoted_spaces = false;
	bool empty_space = false;
	bool quoted_escape = false;
	bool unquoted_escape = false;
	bool distinct_delimiter = true;
};

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

struct CsvRow {
	idx_t start;
	idx_t begin;
	idx_t end;
	idx_t last_position;
	idx_t lines;
};

struct CsvRangeEntry;

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
	uint8_t Origin() const {
		return static_cast<uint8_t>(states[1]);
	}
	CsvRangeStart Next(const CsvRangeEntry &entry) const;
};

struct CsvRangeEntry {
	idx_t first_boundary = DConstants::INVALID_INDEX;
	CsvRangeStart boundary;
	CsvRangeStart end;
	bool computed = false;
};

inline CsvRangeStart CsvRangeStart::Next(const CsvRangeEntry &entry) const {
	auto next = entry.end;
	next.Inherit(*this);
	return next;
}

constexpr idx_t CSV_RANGE_ORIGINS = static_cast<idx_t>(CSVState::MAYBE_QUOTED) + 1;

using CsvRangeTransfer = array<CsvRangeEntry, CSV_RANGE_ORIGINS>;

string CsvStripSeparator(const char *value, idx_t size, char separator);

} // namespace duckdb
