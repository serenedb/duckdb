//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/global_csv_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/scanner_boundary.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/string_value_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_validator.hpp"

namespace duckdb {
struct MultiFileBindData;

//! CSV Global State is used in the CSV Reader Table Function, it controls what each thread
struct CSVGlobalState : public GlobalTableFunctionState {
public:
	CSVGlobalState(ClientContext &context_p, const CSVReaderOptions &options, idx_t total_file_count,
	               const MultiFileBindData &bind_data, std::span<const int64_t> pk_lookups);

	~CSVGlobalState() override {
	}

	//! Generates a CSV Scanner, with information regarding the piece of buffer it should be read.
	//! In case it returns a nullptr it means we are done reading these files.
	void FinishScan(unique_ptr<StringValueScanner> scanner);
	unique_ptr<StringValueScanner> Next(shared_ptr<CSVFileScan> &file);
	void FinishLaunchingTasks(CSVFileScan &scan);

	void FillRejectsTable(CSVFileScan &scan);
	void FinishTask(CSVFileScan &scan);
	void FinishFile(CSVFileScan &scan);

	//! Whether or not to read individual CSV files single-threaded
	bool SingleThreadedRead() const {
		return single_threaded;
	}

	//! True when the caller handed us an exact-offset list (offset-seek mode).
	//! CSVFileScan::Scan uses this to select the batched-offset drain loop
	//! instead of the normal boundary scan.
	bool IsPkLookup() const {
		return !pk_lookups.empty();
	}

private:
	//! Build a scanner pinned to a single pre-known row-start offset (exact-offset mode).
	//! Returns nullptr once every offset in `pk_lookups` has been dispensed.
	unique_ptr<StringValueScanner> NextPkLookupScanner(shared_ptr<CSVFileScan> &current_file_ptr);

	//! Reference to the client context that created this scan
	ClientContext &context;
	const MultiFileBindData &bind_data;

	string sniffer_mismatch_error;

	bool initialized = false;

	bool single_threaded = false;

	atomic<idx_t> scanner_idx;

	shared_ptr<CSVBufferUsage> current_buffer_in_use;

	//! We hold information on the current scanner boundary
	CSVIterator current_boundary;

	vector<idx_t> rejects_file_indexes;

	//! Caller-provided sorted list of exact row-start byte offsets the scan
	//! should produce (from TableFunctionInitInput::pk_lookups). Empty
	//! means normal scan; non-empty makes Next() dispense one-per-offset
	//! scanners pinned to each offset -- O(|offsets|) IO + tokenization, no
	//! boundary iteration, no filter-expression roundtrip.
	std::span<const int64_t> pk_lookups;
	//! Lock-free dispenser for pk_lookups across concurrent Next() calls from
	//! parallel scanner threads. fetch_add returns a unique index per call.
	atomic<idx_t> lookup_cursor {0};
};

} // namespace duckdb
