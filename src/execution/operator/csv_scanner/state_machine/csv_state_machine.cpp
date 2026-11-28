#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"

namespace duckdb {

CSVStateMachine::CSVStateMachine(const CSVReaderOptions &options_p,
                                 const CSVStateMachineOptions &state_machine_options_p)
    : state_machine_options(state_machine_options_p), options(options_p) {
	dialect_options.state_machine_options = state_machine_options;
}

CSVStateMachine::CSVStateMachine(const CSVReaderOptions &options_p)
    : state_machine_options(options_p.dialect_options.state_machine_options), options(options_p),
      dialect_options(options.dialect_options) {
	dialect_options.state_machine_options = state_machine_options;
}

} // namespace duckdb
