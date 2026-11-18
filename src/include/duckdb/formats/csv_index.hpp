//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/csv_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine_cache.hpp"

namespace duckdb {

struct CsvGrammar {
	static CsvGrammar For(const CSVStateMachine &state_machine);

	const StateMachine *transitions = nullptr;
	char delimiter[4] = {};
	idx_t delimiter_size = 1;
	idx_t extra_delimiter_bytes = 0;
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

struct CsvBlockBits {
	uint64_t sep = 0;
	uint64_t term = 0;
	uint64_t carriage = 0;
	uint64_t open = 0;
	uint64_t mark = 0;
	uint64_t unstrict = 0;
	uint64_t ever = 0;
	uint64_t quoted_nl = 0;
	uint64_t comment = 0;
	uint64_t invalid = 0;
	uint64_t in = 0;
	uint64_t commented = 0;
	uint64_t standard = 0;
};

class CsvIndexer {
public:
	explicit CsvIndexer(const CsvGrammar &grammar, bool detailed = false);

	void Reset(CSVState state);
	void Index(const char *data, idx_t size, CsvBlockBits &bits);
	CSVState State() const;
	CSVState PreviousState() const;

private:
	struct Snapshot {
		uint64_t region = 0;
		uint64_t in = 0;
		uint64_t escapes = 0;
		uint64_t quoted_nl = 0;
		uint64_t sep = 0;
		uint64_t term = 0;
		uint64_t carriage = 0;
		uint64_t unquoted_escapes = 0;
		uint64_t unquoted = 0;
		uint64_t partial = 0;
		uint64_t progress[2] = {0, 0};
		uint64_t empty_spaces = 0;
		idx_t last = 0;
		bool pending = false;
	};
	struct Masks {
		uint64_t quote;
		uint64_t escape;
		uint64_t comment;
		uint64_t line_feed;
		uint64_t carriage_return;
		uint64_t space;
		uint64_t delimiter_byte;
		uint64_t equal[4];
	};
	struct Delimiter {
		uint64_t ends;
		uint64_t bytes;
		uint64_t partial;
		uint64_t progress[3];
		uint64_t run;
		uint64_t phase;
	};
	struct Base {
		uint64_t escaped;
		uint64_t escapes;
		uint64_t quotes;
		uint64_t newline;
		Delimiter delimiter;
		uint64_t separator;
		uint64_t start;
		uint64_t opened;
		uint64_t parity;
		uint64_t held;
		uint64_t entry_after;
		uint64_t entry;
		uint64_t in;
	};
	struct Carry {
		uint64_t in = 0;
		uint64_t parity = 0;
		uint64_t hold = 0;
		uint64_t entry = 0;
		uint64_t opened = 0;
		uint64_t start = 1;
		uint64_t empty_space = 0;
		uint64_t escaped = 0;
		uint64_t unquoted = 0;
		uint64_t carriage_return = 0;
		uint64_t comment = 0;
		uint64_t partial = 0;
		uint64_t delimiter_run = 0;
		uint64_t delimiter_phase = 0;
		uint64_t prefix[3] = {0, 0, 0};
		bool exact = false;
	};

	void Match(const char *data, idx_t size, Masks &masks) const;
	void Delimiters(const Masks &masks, uint64_t allowed, uint64_t region, idx_t last, Delimiter &delimiter) const;
	void Compute(const Masks &masks, uint64_t region, idx_t last, Base &base) const;
	bool Fast(const Masks &masks, uint64_t valid, idx_t last, CsvBlockBits &bits);
	bool Bits(const Masks &masks, uint64_t valid, idx_t last, CsvBlockBits &bits);
	void Walk(const char *data, idx_t size, CsvBlockBits &bits);
	void CarryFromState(CSVState state);
	CSVState At(idx_t i) const;
	void Settle();

private:
	const CsvGrammar &grammar;
	const bool detailed;
	Carry carry;
	Snapshot snapshot;
	CSVState before_block = CSVState::NOT_SET;
	mutable CSVState state = CSVState::NOT_SET;
	mutable CSVState previous_state = CSVState::NOT_SET;
};

} // namespace duckdb
