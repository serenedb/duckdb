#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/execution/operator/csv_scanner/string_value_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"
#include "duckdb/execution/operator/csv_scanner/global_csv_state.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/function/table/read_csv.hpp"

namespace duckdb {

//! Bind data of read_single_csv_file - the regular CSV read data plus the single file that is read
struct ReadSingleCSVFileData : public ReadCSVData {
	OpenFileInfo file;
	//! The names/types this file is read with
	vector<Identifier> csv_names;
	vector<LogicalType> csv_types;
};

struct ReadSingleCSVFileGlobalState : public GlobalTableFunctionState {
public:
	ReadSingleCSVFileGlobalState(ClientContext &context, ReadSingleCSVFileData &csv_data,
	                             const TableFunctionInitInput &input)
	    : state(context, csv_data, csv_data.csv_names, TableFunctionFileInitInput::Get(input).file_count, input.op) {
	}

public:
	idx_t MaxThreads() const override {
		return max_threads;
	}

public:
	//! The file that is read - declared before the state below, which holds buffers of this file and must therefore
	//! be destroyed first
	shared_ptr<CSVFileScan> file_scan;
	CSVGlobalState state;
	//! Handing out the next part of the file is done single-threadedly
	mutable mutex lock;
	//! Whether we are done handing out parts of the file
	bool finished_launching = false;
	idx_t max_threads = 1;
};

struct ReadSingleCSVFileLocalState : public LocalTableFunctionState {
	CSVLocalState state;
	//! Whether our caller claims the batches we read - see table_function_claim_batch_t
	bool claimed_externally = false;
};

//! The columns "force_not_null" was given, looked up the way column names are compared
static identifier_set_t GetForceNotNullNames(const CSVReaderOptions &options) {
	identifier_set_t result;
	for (auto &force_name : options.force_not_null_names) {
		result.insert(Identifier(force_name));
	}
	return result;
}

//! "force_not_null" names columns of the scan, so it is verified against the schema of the scan rather than
//! against the columns of any single file of it
static void VerifyForceNotNull(const CSVReaderOptions &options, const vector<Identifier> &schema) {
	if (options.force_not_null_names.empty()) {
		return;
	}
	identifier_set_t column_names;
	for (auto &name : schema) {
		column_names.insert(name);
	}
	for (auto &force_name : GetForceNotNullNames(options)) {
		if (column_names.find(force_name) == column_names.end()) {
			throw BinderException("\"force_not_null\" expected to find %s, but it was not found in the table",
			                      force_name.GetIdentifierName());
		}
	}
}

//! The equivalent of MultiFileReaderInterface::FinalizeBindData - mark the columns of this file that
//! "force_not_null" names. A file of a multi-file scan only has some of the columns of the scan
static void ApplyForceNotNull(CSVReaderOptions &options, const vector<Identifier> &names) {
	if (options.force_not_null_names.empty()) {
		return;
	}
	const auto force_not_null = GetForceNotNullNames(options);
	options.force_not_null.clear();
	for (auto &name : names) {
		options.force_not_null.push_back(force_not_null.find(name) != force_not_null.end());
	}
}

//! Sniff this file - the dialect that is detected is stored in the options of the bind data. When a schema is given
//! the sniffed schema is reconciled with it, and files whose schema does not match it are reported
static void SniffCSVFile(ClientContext &context, ReadSingleCSVFileData &result, const CSVSchema &file_schema,
                         const MultiFileOptions &file_options, vector<LogicalType> &return_types,
                         vector<Identifier> &names) {
	auto &options = result.options;
	result.buffer_manager = CSVBufferManager::Open(context, options, options.file_path, false);
	auto &state_machine_cache = CSVStateMachineCache::Get(context);
	if (file_schema.Empty()) {
		// the columns of this file are fixed - only its dialect is sniffed
		CSVSniffer sniffer(options, file_options, result.buffer_manager, state_machine_cache);
		sniffer.SniffCSV();
		return;
	}
	if (result.buffer_manager->file_handle->FileSize() == 0) {
		// an empty file has no dialect for us to reconcile with the schema of the scan
		return;
	}
	CSVSniffer sniffer(options, file_options, result.buffer_manager, state_machine_cache, false);
	auto sniff_result = sniffer.AdaptiveSniff(file_schema);
	names = std::move(sniff_result.names);
	return_types = std::move(sniff_result.return_types);
}

static unique_ptr<FunctionData> ReadSingleCSVFileBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &file_input = TableFunctionFileBindInput::Get(input);
	auto result = make_uniq<ReadSingleCSVFileData>();
	auto &options = result->options;
	for (auto &kv : input.named_parameters) {
		options.ParseOption(context, kv.first, kv.second);
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("read_single_csv_file requires a non-NULL file name");
	}
	result->file = TableFunctionFileBindInput::GetFile(input);

	// the options of the scan this file is part of steer the sniffer - the file list is this single file
	MultiFileOptions file_options;
	if (file_input.multi_file_options) {
		file_options = *file_input.multi_file_options;
	}
	SimpleMultiFileList file_list(vector<OpenFileInfo> {result->file});

	//! Whether the options that name columns are verified against the columns of this file, which is the schema of
	//! the scan only when this file is the whole scan
	bool verify_against_file = false;
	optional_ptr<const ReadSingleCSVFileData> schema_source;
	if (file_input.HasExpectedSchema()) {
		if (file_input.expected_bind_data) {
			// the schema of the scan was determined on (other) files of this scan - start from the options it was
			// determined with, so that this file is read with the same dialect
			schema_source = file_input.expected_bind_data->Cast<ReadSingleCSVFileData>();
			options = schema_source->options;
			options.force_not_null.clear();
			names = *file_input.expected_names;
			return_types = *file_input.expected_types;
		} else {
			// the columns are known but the dialect of this file is not - this is the case for COPY, which takes
			// its columns from the target table
			options.name_list = *file_input.expected_names;
			options.sql_type_list = *file_input.expected_types;
			options.columns_set = true;
			options.sql_types_per_column.clear();
			for (idx_t i = 0; i < options.name_list.size(); i++) {
				options.sql_types_per_column[options.name_list[i]] = i;
			}
			names = options.name_list;
			return_types = options.sql_type_list;
		}
	}
	options.file_path = result->file.path;
	// when several files are read, options like "names" describe the scan and not this file - the sniffer is then
	// lenient about a file whose columns do not line up with them exactly
	options.multi_file_reader = file_input.multi_file_scan;
	options.Verify(file_options);

	// when the schema of the scan is known upfront the options are verified against it before this file is
	// sniffed, so that an option that does not match it is reported before any error in the file itself
	if (file_input.HasExpectedSchema()) {
		VerifyForceNotNull(options, *file_input.expected_names);
	} else if (!file_input.multi_file_scan) {
		// this file is the whole scan, so its own columns are the schema the options are verified against
		verify_against_file = true;
	}
	if (schema_source) {
		// the schema of this file must be reconcilable with the schema of the scan
		result->csv_schema = schema_source->csv_schema;
		if (options.auto_detect) {
			SniffCSVFile(context, *result, result->csv_schema, file_options, return_types, names);
		}
	} else if (file_input.HasExpectedSchema()) {
		if (options.auto_detect) {
			SniffCSVFile(context, *result, CSVSchema(), file_options, return_types, names);
		}
	} else if (options.auto_detect || file_options.union_by_name) {
		// a column without any value is kept as SQLNULL here, so the other files can still determine its type
		result->csv_schema = CSVSchemaDiscovery::SchemaDiscovery(context, result->buffer_manager, options, file_options,
		                                                         return_types, names, file_list, false);
	} else {
		if (!options.columns_set) {
			throw BinderException("read_csv requires columns to be specified through the 'columns' option. Use "
			                      "read_csv_auto or set read_csv(..., AUTO_DETECT=TRUE) to automatically guess "
			                      "columns.");
		}
		names = options.name_list;
		return_types = options.sql_type_list;
	}
	if (return_types.size() != names.size()) {
		throw BinderException("read_csv: mismatch between the number of column names (%d) and column types (%d)",
		                      names.size(), return_types.size());
	}
	options.dialect_options.num_cols = names.size();

	if (verify_against_file) {
		VerifyForceNotNull(options, names);
	}
	ApplyForceNotNull(options, names);
	if (!file_options.union_by_name) {
		for (auto &type : return_types) {
			if (type.id() == LogicalTypeId::SQLNULL) {
				// if we cannot tell the type of a column we default to the highest type, a VARCHAR
				type = LogicalType::VARCHAR;
			}
		}
	}
	result->Finalize();
	result->csv_names = names;
	result->csv_types = return_types;
	return std::move(result);
}

//! Combine the schemas of several CSV files the way the multi-file sniffer does - the resulting CSV schema is
//! handed to the bind of every file, whose dialect is then sniffed and reconciled with it
static unique_ptr<FunctionData> ReadSingleCSVFileCombineSchema(ClientContext &context,
                                                               TableFunctionCombineSchemaInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<Identifier> &names) {
	if (input.union_by_name) {
		// the columns of the files were unified by name - the options that name columns apply to the result of that
		auto &options = input.bind_data[0].get().Cast<ReadSingleCSVFileData>().options;
		VerifyForceNotNull(options, names);
		if (!options.sql_types_per_column.empty()) {
			const auto exception = CSVError::ColumnTypesError(options.sql_types_per_column, names);
			if (!exception.error_message.empty()) {
				throw BinderException(exception.error_message);
			}
			for (idx_t i = 0; i < names.size(); i++) {
				auto entry = options.sql_types_per_column.find(names[i]);
				if (entry != options.sql_types_per_column.end()) {
					return_types[i] = options.sql_type_list[entry->second];
				}
			}
		}
		for (auto &type : return_types) {
			if (type.id() == LogicalTypeId::SQLNULL) {
				// if we cannot tell the type of a column we default to the highest type, a VARCHAR
				type = LogicalType::VARCHAR;
			}
		}
		// the files have different columns - every file is read the way it was bound
		return nullptr;
	}
	optional_ptr<const ReadSingleCSVFileData> first_file;
	CSVSchema best_schema;
	for (auto &bind_data : input.bind_data) {
		auto &csv_data = bind_data.get().Cast<ReadSingleCSVFileData>();
		if (csv_data.csv_schema.Empty()) {
			// the schema of this file was not sniffed - fall back to combining the types
			return nullptr;
		}
		auto schema = csv_data.csv_schema;
		if (first_file && schema.IsEmptyFile()) {
			// a file without any data contributes no columns to the schema of the scan
			schema = CSVSchema(true);
		}
		if (!first_file) {
			first_file = csv_data;
		}
		if (best_schema.Empty() || best_schema.GetRowsRead() == 0) {
			// a schema is better than no schema, and any schema beats one without data rows
			best_schema = schema;
		} else if (schema.GetRowsRead() != 0) {
			best_schema.MergeSchemas(schema, first_file->options.null_padding);
		}
	}
	if (!first_file) {
		return nullptr;
	}
	if (best_schema.Empty()) {
		throw InvalidInputException("No columns found in CSV files. Provide the columns option or ensure at least one "
		                            "file contains a header or data row.");
	}
	best_schema.ReplaceNullWithVarchar();
	names = StringsToIdentifiers(best_schema.GetNames());
	return_types = best_schema.GetTypes();
	VerifyForceNotNull(first_file->options, names);

	auto result = make_uniq<ReadSingleCSVFileData>();
	// the options that were sniffed on the first file are the starting point for every file of the scan. The
	// columns are not set on them - they are carried by the CSV schema, which each file is reconciled with
	result->options = first_file->options;
	result->options.dialect_options.num_cols = names.size();
	// the buffer manager of the first file is kept, like the multi-file sniffer does - it tells the scan whether
	// the files can be read ahead, and the first file does not need to be opened again
	result->buffer_manager = first_file->buffer_manager;
	result->csv_schema = best_schema;
	result->csv_names = names;
	result->csv_types = return_types;
	result->Finalize();
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadSingleCSVFileInitGlobal(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	auto &file_input = TableFunctionFileInitInput::Get(input);
	auto &csv_data = input.bind_data->CastNoConst<ReadSingleCSVFileData>();

	// create the temporary rejects table
	if (csv_data.options.store_rejects.GetValue()) {
		CSVRejectsTable::GetOrCreate(context, csv_data.options.rejects_scan_name.GetValue(),
		                             csv_data.options.rejects_table_name.GetValue())
		    ->InitializeTable(context, csv_data);
	}

	auto result = make_uniq<ReadSingleCSVFileGlobalState>(context, csv_data, input);

	// this file was sniffed during binding, so its dialect and columns are known
	auto options = csv_data.options;
	options.auto_detect = false;
	MultiFileOptions file_options;
	CSVSchema no_schema;
	result->file_scan = make_shared_ptr<CSVFileScan>(
	    context, csv_data.file, std::move(options), file_options, csv_data.csv_names, csv_data.csv_types, no_schema,
	    result->state.SingleThreadedRead(), csv_data.buffer_manager, false);

	// perform projection pushdown - the scanner emits the columns in the order they are requested
	auto &file_scan = *result->file_scan;
	for (auto &column_index : input.column_indexes) {
		const auto col_id = column_index.GetPrimaryIndex();
		column_t virtual_column_id = DConstants::INVALID_INDEX;
		if (IsVirtualColumn(col_id)) {
			virtual_column_id = col_id;
		} else if (file_input.virtual_columns) {
			auto entry = file_input.virtual_columns->find(col_id);
			if (entry != file_input.virtual_columns->end()) {
				virtual_column_id = entry->second;
			}
		}
		if (virtual_column_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
			file_scan.column_ids.push_back(MultiFileLocalColumnId(col_id));
			file_scan.AddVirtualColumn(virtual_column_id);
			continue;
		}
		if (virtual_column_id != DConstants::INVALID_INDEX) {
			continue;
		}
		file_scan.column_ids.push_back(MultiFileLocalColumnId(col_id));
	}
	// the index of this file in the scan it is part of - it identifies the file in the rejects tables
	file_scan.file_list_idx = file_input.file_index.IsValid() ? file_input.file_index.GetIndex() : 0;
	if (file_input.cast_map) {
		// our caller needs some columns as a different type than this file has them - the scanner converts to those
		// types while parsing, so that "ignore_errors" applies to the conversions that fail
		for (auto &entry : *file_input.cast_map) {
			file_scan.cast_map[entry.first] = entry.second;
		}
	}
	file_scan.InitializeFileNamesTypes();
	file_scan.SetStart();

	if (!result->state.SingleThreadedRead()) {
		const idx_t bytes_per_thread = CSVIterator::BytesPerThread(csv_data.options);
		result->max_threads = file_scan.file_size / bytes_per_thread + 1;
	}
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ReadSingleCSVFileInitLocal(ExecutionContext &context,
                                                                      TableFunctionInitInput &input,
                                                                      GlobalTableFunctionState *global_state) {
	return make_uniq<ReadSingleCSVFileLocalState>();
}

//! Assign the next part of the file to this thread
static bool ClaimNextPart(ReadSingleCSVFileGlobalState &gstate, ReadSingleCSVFileLocalState &lstate) {
	lock_guard<mutex> guard(gstate.lock);
	gstate.state.FinishScan(std::move(lstate.state.csv_reader));
	lstate.state.claim_state = CSVLocalState::ClaimState::IDLE;
	if (gstate.finished_launching) {
		return false;
	}
	if (gstate.state.Next(gstate.file_scan, lstate.state)) {
		return true;
	}
	// we have handed out the entire file - this is also where the errors of the file are reported
	gstate.finished_launching = true;
	gstate.state.FinishLaunchingTasks(*gstate.file_scan);
	return false;
}

static bool ReadSingleCSVFileClaimBatch(ClientContext &context, TableFunctionInput &input) {
	auto &gstate = input.global_state->Cast<ReadSingleCSVFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();
	// our caller hands out the parts of the file, so we must not claim the next one ourselves
	lstate.claimed_externally = true;
	return ClaimNextPart(gstate, lstate);
}

//! The CSV scanner can be read ahead when the buffers of the file can be addressed individually
static bool ReadSingleCSVFileSupportsReadAhead(const FunctionData &bind_data) {
	auto &csv_data = bind_data.Cast<ReadSingleCSVFileData>();
	return csv_data.buffer_manager && csv_data.buffer_manager->file_handle &&
	       csv_data.buffer_manager->file_handle->HasKnownBufferRanges();
}

//! Load the buffers of the claimed part of the file that are not in memory yet
static AsyncResult ReadSingleCSVFileScheduleIO(ClientContext &context, TableFunctionInput &input) {
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();
	if (lstate.state.claim_state != CSVLocalState::ClaimState::PENDING) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	return AsyncResult::FromTasks(CSVCollectClaimIOTasks(lstate.state), TaskSchedulerType::ASYNC);
}

//! Release the part of the file this thread was reading
static void ReadSingleCSVFileFinishBatch(ClientContext &context, TableFunctionInput &input) {
	auto &gstate = input.global_state->Cast<ReadSingleCSVFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();
	lock_guard<mutex> guard(gstate.lock);
	gstate.state.FinishScan(std::move(lstate.state.csv_reader));
}

static void ReadSingleCSVFileFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ReadSingleCSVFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();

	while (true) {
		if (lstate.state.claim_state == CSVLocalState::ClaimState::IDLE) {
			if (lstate.claimed_externally) {
				// the next part of the file is claimed by our caller
				return;
			}
			if (!ClaimNextPart(gstate, lstate)) {
				// there is nothing left for us to read in this file
				return;
			}
		}
		if (lstate.state.claim_state == CSVLocalState::ClaimState::PENDING) {
			lstate.state.Materialize();
		}
		auto &csv_reader = *lstate.state.csv_reader;
		if (csv_reader.IsSuspended() || !csv_reader.FinishedIterator()) {
			csv_reader.Flush(output);
			if (csv_reader.IsSuspended()) {
				// the scanner needs a buffer that is not in memory - load it and resume
				csv_reader.buffer_manager->GetBuffer(csv_reader.PendingBufferIdx());
				continue;
			}
			if (output.size() != 0) {
				return;
			}
		}
		// this part of the file is done - grab the next one
		lock_guard<mutex> guard(gstate.lock);
		gstate.state.FinishScan(std::move(lstate.state.csv_reader));
		lstate.state.claim_state = CSVLocalState::ClaimState::IDLE;
	}
}

static double ReadSingleCSVFileProgress(ClientContext &context, const FunctionData *bind_data,
                                        const GlobalTableFunctionState *global_state) {
	if (!global_state) {
		return 0;
	}
	auto &gstate = global_state->Cast<ReadSingleCSVFileGlobalState>();
	// the buffers of the file are released when the last part of it is handed out - hold the lock so we do not read
	// them while that happens
	lock_guard<mutex> guard(gstate.lock);
	if (!gstate.file_scan) {
		return 0;
	}
	return gstate.file_scan->GetProgressInFile(context);
}

static unique_ptr<NodeStatistics> ReadSingleCSVFileCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto &csv_data = bind_data->Cast<ReadSingleCSVFileData>();
	// determined through the scientific method as the average amount of rows in a CSV file
	idx_t per_file_cardinality = 42;
	if (csv_data.buffer_manager && csv_data.buffer_manager->file_handle) {
		auto estimated_row_width = csv_data.csv_types.size() * 5;
		per_file_cardinality = csv_data.buffer_manager->file_handle->FileSize() / estimated_row_width;
	}
	return make_uniq<NodeStatistics>(per_file_cardinality);
}

static virtual_column_map_t ReadSingleCSVFileGetVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
	// file_row_number for CSV = byte offset of the row's start in the file. Unique per row, stable across re-reads
	// of the same file, so it can serve as a primary key for consumers that re-locate a row (the lookup function
	// below seeks to these offsets).
	virtual_column_map_t result;
	result.insert(make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
	                        TableColumn("file_row_number", LogicalType::BIGINT)));
	return result;
}

namespace {

// Cached lookup-mode state for read_csv. Built ONCE per query in init_global,
// reused across every per-batch CSVLookupScan call:
//   - file_scan: the CSV file scan -- buffer manager, state machine, error
//     handler, schema, projection. Holds the parsed CSV metadata.
//   - scanner: a single StringValueScanner repositioned via ResetForAppend
//     per pk. The scanner's internal result.parse_chunk accumulates all the
//     batch's rows; one Reinterpret per output column at end -- ZERO data copy.
//   - buffer_pin: pinned buffer-usage handle for the scanner's lifetime.
//     Single-buffer CSVs only -- multi-buffer would need to update this
//     when pks straddle buffer boundaries.
//   - output_to_file_col: per-output-column index into result.parse_chunk
//     (or DConstants::INVALID_INDEX for virtual slots), built at init from
//     input.column_indexes.
// pk_lookups itself is supplied per call via TableFunctionInput::pk_lookups.
struct CSVLookupGlobalState : public GlobalTableFunctionState {
	shared_ptr<CSVFileScan> file_scan;
	unique_ptr<StringValueScanner> scanner;
	shared_ptr<CSVBufferUsage> buffer_pin;
	std::vector<idx_t> output_to_file_col;
};

// Builds the lookup gstate from a caller-bound read_csv MultiFileBindData. We only
// touch the FIRST file in the list -- pk-lookup is a single-file operation.
// Constructs the CSVFileScan (file open + state machine + schema setup) and
// the reusable StringValueScanner pinned to the file's start_iterator;
// per-pk CSVLookupScan repositions it via Reset(MakeTightIterator(...)).
unique_ptr<GlobalTableFunctionState> CSVLookupInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->CastNoConst<MultiFileBindData>();
	auto &multi_file_data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	if (!multi_file_data.options.schema_bind_data) {
		throw InternalException("CSV lookup requires the bind data of the file the schema was determined on");
	}
	auto &csv_data = multi_file_data.options.schema_bind_data->Cast<ReadSingleCSVFileData>();
	auto state = make_uniq<CSVLookupGlobalState>();

	const auto &file = bind_data.file_list->GetFirstFile();
	auto options = csv_data.options;
	options.auto_detect = false; // schema already known
	state->file_scan = make_shared_ptr<CSVFileScan>(context, file, std::move(options), bind_data.file_options,
	                                                csv_data.csv_names, csv_data.csv_types, csv_data.csv_schema,
	                                                /*per_file_single_threaded=*/true, /*buffer_manager=*/nullptr,
	                                                /*fixed_schema=*/true);

	// Populate column_ids with the projected real columns BEFORE
	// InitializeFileNamesTypes -- it derives file_types / projection_ids from
	// column_ids. Order matters: column_ids must be sorted by file index for
	// the projection_ids sort to keep parse_chunk slots aligned.
	std::vector<idx_t> sorted_file_cols;
	for (auto &col : input.column_indexes) {
		if (!col.IsVirtualColumn()) {
			sorted_file_cols.push_back(col.GetPrimaryIndex());
		}
	}
	std::sort(sorted_file_cols.begin(), sorted_file_cols.end());
	for (auto file_col : sorted_file_cols) {
		state->file_scan->column_ids.push_back(MultiFileLocalColumnId(file_col));
	}
	state->file_scan->InitializeFileNamesTypes();

	// output-slot -> result.parse_chunk source-slot map. parse_chunk slots
	// are indexed by position in `sorted_file_cols`; build a reverse lookup
	// from input.column_indexes.
	state->output_to_file_col.reserve(input.column_indexes.size());
	for (auto &col : input.column_indexes) {
		if (col.IsVirtualColumn()) {
			state->output_to_file_col.push_back(DConstants::INVALID_INDEX);
			continue;
		}
		const auto file_col = col.GetPrimaryIndex();
		auto it = std::find(sorted_file_cols.begin(), sorted_file_cols.end(), file_col);
		D_ASSERT(it != sorted_file_cols.end());
		state->output_to_file_col.push_back(NumericCast<idx_t>(it - sorted_file_cols.begin()));
	}

	// Build the reusable scanner (pinned to start_iterator's buffer). Per-pk
	// Reset(iter) repositions it -- no per-pk allocation. Single-buffer CSVs
	// only; multi-buffer would need buffer_pin updates per cross-buffer pk.
	state->buffer_pin = make_shared_ptr<CSVBufferUsage>(*state->file_scan->buffer_manager,
	                                                    state->file_scan->start_iterator.GetBufferIdx());
	state->scanner = make_uniq<StringValueScanner>(
	    /*scanner_idx=*/0, state->file_scan->buffer_manager, state->file_scan->state_machine,
	    state->file_scan->error_handler, state->file_scan, /*sniffing=*/false, state->file_scan->start_iterator);
	state->scanner->buffer_tracker = state->buffer_pin;

	return std::move(state);
}

// Maps a global byte offset to (buffer_idx, buffer_pos) and returns a tight
// CSVIterator pinned to that offset. SetExactBoundary marks first_one=true
// so the scanner trusts the offset is a real row start (no SetStart).
//
// Quirk: the pk encoding for non-first rows points at the trailing newline of
// the previous row (pre-existing upstream behavior of
// line_positions_per_row.begin -- see StringValueResult::AddRowInternal).
// Peek at the byte; if it's \r and/or \n, advance past it so the parser
// starts at the actual row start. This lets each pk produce exactly one row
// (no leading empty record) -- enabling zero-copy Reinterpret accumulation.
CSVIterator MakeTightIterator(CSVFileScan &file_scan, idx_t global_offset, idx_t boundary_idx) {
	const auto buf_size = file_scan.buffer_manager->GetBufferSize();
	idx_t buf_idx = global_offset / buf_size;
	idx_t buf_pos = global_offset % buf_size;
	auto handle = file_scan.buffer_manager->GetBuffer(buf_idx);
	if (handle) {
		const auto *ptr = handle->Ptr();
		const idx_t actual = handle->actual_size;
		if (buf_pos < actual && ptr[buf_pos] == '\r') {
			++buf_pos;
		}
		if (buf_pos < actual && ptr[buf_pos] == '\n') {
			++buf_pos;
		}
	}
	CSVIterator iter = file_scan.start_iterator;
	iter.SetExactBoundary(buf_idx, buf_pos, buf_pos + 1, boundary_idx);
	return iter;
}

// Rebind parse_chunk's vectors to `output`, then append each requested row
// densely from output's current size (glob accumulates across per-file calls).
// CSV has no pushed filters and every offset is a valid row start, so every
// requested id survives, in order: pk_survivors[base + i] = i.
void CSVLookupScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<CSVLookupGlobalState>();
	if (data.pk_lookups.empty()) {
		return;
	}

	auto &result = gstate.scanner->GetStringValueResult();

	vector<Vector *> external_vectors(result.parse_chunk.data.size(), nullptr);
	for (idx_t c = 0; c < gstate.output_to_file_col.size(); ++c) {
		const auto pc = gstate.output_to_file_col[c];
		if (pc == DConstants::INVALID_INDEX) {
			continue;
		}
		D_ASSERT(pc < external_vectors.size());
		external_vectors[pc] = &output.data[c];
	}
	result.RebindParseChunkVectors(external_vectors);

	result.Reset();
	const idx_t base = output.size();
	result.number_of_rows = NumericCast<int64_t>(base);

	const idx_t num_rows = data.pk_lookups.size();
	for (idx_t i = 0; i < num_rows; ++i) {
		const auto offset = NumericCast<idx_t>(data.pk_lookups[i]);
		gstate.scanner->ResetForAppend(MakeTightIterator(*gstate.file_scan, offset, i));
		gstate.scanner->ParseChunkAppend();
		data.pk_survivors[base + i] = i;
	}
	output.SetCardinality(base + num_rows);
}

} // namespace

TableFunction MakeCSVLookupTableFunction() {
	TableFunction fn;
	fn.init_global = CSVLookupInitGlobal;
	fn.function = CSVLookupScan;
	return fn;
}

TableFunction ReadCSVTableFunction::GetSingleFileFunction() {
	TableFunction read_csv("read_single_csv_file", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
	                       ReadSingleCSVFileFunction, ReadSingleCSVFileBind, ReadSingleCSVFileInitGlobal,
	                       ReadSingleCSVFileInitLocal);
	read_csv.get_virtual_columns = ReadSingleCSVFileGetVirtualColumns;
	read_csv.table_scan_progress = ReadSingleCSVFileProgress;
	read_csv.cardinality = ReadSingleCSVFileCardinality;
	read_csv.projection_pushdown = true;
	ReadCSVAddNamedParameters(read_csv);
	return read_csv;
}

TableFunction ReadCSVTableFunction::GetMultiFileFunction(Identifier name) {
	// the multi-file CSV reader is the single-file CSV reader wrapped into a multi-file function
	TableFunctionMultiFileSettings settings;
	settings.glob_input = FileGlobInput(FileGlobOptions::FALLBACK_GLOB, "csv");
	settings.reader_type = "CSV";
	// like read_csv, the schema is determined by combining the schemas of up to "files_to_sniff" files
	settings.maximum_sample_files = 10;
	settings.sample_files_parameter = "files_to_sniff";
	// the schemas of the sampled files are reconciled with one another - every file must have every column
	settings.sampled_schema_is_union = false;
	settings.combine_schema = ReadSingleCSVFileCombineSchema;
	settings.claim_batch = ReadSingleCSVFileClaimBatch;
	settings.finish_batch = ReadSingleCSVFileFinishBatch;
	settings.supports_read_ahead = ReadSingleCSVFileSupportsReadAhead;
	settings.schedule_io = ReadSingleCSVFileScheduleIO;
	// the scanner converts to the types the scan asks for while parsing, rather than casting its output
	settings.supports_cast_map = true;
	return TableFunctionMultiFileWrapper::CreateFunction(GetSingleFileFunction(), std::move(name), std::move(settings));
}

} // namespace duckdb
