//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_state_machine.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state.hpp"
#include "duckdb/common/printer.hpp"

namespace duckdb {

class CSVStateMachine {
public:
	CSVStateMachine(const CSVReaderOptions &options_p, const CSVStateMachineOptions &state_machine_options);

	explicit CSVStateMachine(const CSVReaderOptions &options);

	void Print() const {
		Printer::Print(OutputStream::STREAM_STDOUT, string("State Machine Options"));
		Printer::Print(OutputStream::STREAM_STDOUT, string("Delim: ") + state_machine_options.delimiter.FormatValue());
		Printer::Print(OutputStream::STREAM_STDOUT, string("Quote: ") + state_machine_options.quote.FormatValue());
		Printer::Print(OutputStream::STREAM_STDOUT, string("Escape: ") + state_machine_options.escape.FormatValue());
		Printer::Print(OutputStream::STREAM_STDOUT, string("Comment: ") + state_machine_options.comment.FormatValue());
		Printer::Print(OutputStream::STREAM_STDOUT, string("---------------------"));
	}
	//! Options of this state machine
	const CSVStateMachineOptions state_machine_options;
	//! CSV Reader Options
	const CSVReaderOptions &options;
	//! Dialect options resulting from sniffing
	DialectOptions dialect_options;
};

} // namespace duckdb
