//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/execution/progress_data.hpp"

namespace duckdb {
struct ReadCSVData;
class CSVFileScan;

//! Struct holding information over a CSV File we will scan
class CSVFileScan {
public:
	//! Constructor for new CSV Files, we must initialize the buffer manager and the state machine
	//! Path to this file
	CSVFileScan(ClientContext &context, const OpenFileInfo &file, CSVReaderOptions options,
	            const MultiFileOptions &file_options, const vector<Identifier> &names, const vector<LogicalType> &types,
	            CSVSchema &file_schema, bool per_file_single_threaded,
	            shared_ptr<CSVBufferManager> buffer_manager = nullptr, bool fixed_schema = false);

public:
	void SetNamesAndTypes(const vector<Identifier> &names, const vector<LogicalType> &types);

public:
	//! How far along we are in reading this file
	double GetProgressInFile(ClientContext &context);

	const string &GetFileName() const {
		return file.path;
	}
	void AddVirtualColumn(column_t virtual_column_id);

	idx_t GetFileIndex() const {
		return file_list_idx.GetIndex();
	}
	const vector<string> &GetNames();
	const vector<LogicalType> &GetTypes();
	void Finish();

	//! Initialize the actual names and types to be scanned from the file
	void InitializeFileNamesTypes();

public:
	//! The file that is read
	OpenFileInfo file;
	//! The index of this file within the scan it is part of - it identifies the file in the rejects tables
	optional_idx file_list_idx;
	//! The columns of this file
	vector<MultiFileColumnDefinition> columns;
	//! The columns that are read from this file, in the order they are emitted
	MultiFileLocalColumnIds<MultiFileLocalColumnId> column_ids;
	//! The types the columns must be read as, when they differ from the types this file has
	unordered_map<column_t, LogicalType> cast_map;
	//! Buffer Manager for the CSV File
	shared_ptr<CSVBufferManager> buffer_manager;
	//! State Machine for this file
	shared_ptr<CSVStateMachine> state_machine;
	//! How many bytes were read up to this point
	atomic<idx_t> bytes_read {0};
	//! The progress of a compressed file is estimated - keeps it monotonic
	MonotonicProgress compressed_progress;
	//! Size of this file
	idx_t file_size;
	//! Line Info used in error messages
	shared_ptr<CSVErrorHandler> error_handler;
	//! Whether or not this is an on-disk file
	bool on_disk_file = true;

	vector<LogicalType> file_types;

	//! Variables to handle projection pushdown
	set<idx_t> projected_columns;
	std::vector<std::pair<idx_t, idx_t>> projection_ids;

	//! Options for this CSV Reader
	CSVReaderOptions options;

	idx_t file_row_number_idx = DConstants::INVALID_INDEX;

private:
	vector<string> names;
	vector<LogicalType> types;
};
} // namespace duckdb
