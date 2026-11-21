#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/formats/csv_scan.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

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
	ReadSingleCSVFileGlobalState(ClientContext &context_p, const ReadSingleCSVFileData &csv_data_p,
	                             const TableFunctionInitInput &input)
	    : context(context_p), csv_data(csv_data_p), scan_op(input.op),
	      file_count(TableFunctionFileInitInput::Get(input).file_count) {
	}

public:
	idx_t MaxThreads() const override {
		return max_threads;
	}

public:
	ClientContext &context;
	const ReadSingleCSVFileData &csv_data;
	optional_ptr<const PhysicalOperator> scan_op;
	idx_t file_count;
	shared_ptr<CSVFileScan> file_scan;
	unique_ptr<CsvScanGlobal> scan;
	mutable mutex lock;
	bool finished = false;
	idx_t max_threads = 1;
};

struct ReadSingleCSVFileLocalState : public LocalTableFunctionState {
	bool claimed_externally = false;
	bool scanning = false;
	idx_t range = 0;
	unique_ptr<CsvScan> scan;
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
	if (file_schema.Empty()) {
		// the columns of this file are fixed - only its dialect is sniffed
		CSVSniffer sniffer(options, file_options, result.buffer_manager);
		sniffer.SniffCSV();
		return;
	}
	if (result.buffer_manager->file_handle->FileSize() == 0) {
		// an empty file has no dialect for us to reconcile with the schema of the scan
		return;
	}
	CSVSniffer sniffer(options, file_options, result.buffer_manager, false);
	auto sniff_result = sniffer.AdaptiveSniff(file_schema);
	names = std::move(sniff_result.names);
	return_types = std::move(sniff_result.return_types);
}

static void ApplyCopyRules(CSVReaderOptions &options, shared_ptr<const CopyFromRules> rules) {
	auto &copy = rules->options;
	const bool csv = copy.format == CopyFormat::CSV;
	options.auto_detect = false;
	options.SetDelimiter(string(1, copy.delimiter));
	options.SetQuote(csv ? string(1, copy.quote) : string());
	options.SetEscape(csv ? string(1, copy.escape) : string());
	options.SetComment(string());
	options.dialect_options.state_machine_options.new_line = NewLineIdentifier::SINGLE_N;
	options.SetHeader(copy.header);
	options.null_str = {copy.null_str};
	options.allow_quoted_nulls = false;
	options.ignore_errors.Set(copy.ignore_errors);
	options.rejects_limit = copy.reject_limit;
	options.encoding = copy.encoding;
	options.compression = copy.compression;
	options.rendering = copy.rendering;
	options.copy_rules = std::move(rules);
}

static unique_ptr<FunctionData> ReadSingleCSVFileBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &file_input = TableFunctionFileBindInput::Get(input);
	auto result = make_uniq<ReadSingleCSVFileData>();
	auto &options = result->options;
	optional_ptr<const ReadSingleCSVFileData> schema_source;
	if (file_input.HasExpectedSchema() && file_input.expected_bind_data) {
		// the schema of the scan was determined on (other) files of this scan - start from the options it was
		// determined with, so that this file is read with the same dialect
		schema_source = file_input.expected_bind_data->Cast<ReadSingleCSVFileData>();
		options = schema_source->options;
		options.force_not_null.clear();
	} else {
		for (auto &kv : input.named_parameters) {
			options.ParseOption(context, kv.first, kv.second);
		}
		if (file_input.copy_rules) {
			ApplyCopyRules(options, file_input.copy_rules);
		}
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
	if (file_input.HasExpectedSchema()) {
		if (schema_source) {
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
	auto &first = input.bind_data[0].get().Cast<ReadSingleCSVFileData>();
	if (!first.options.auto_detect) {
		VerifyForceNotNull(first.options, names);
		auto result = make_uniq<ReadSingleCSVFileData>();
		result->options = first.options;
		result->buffer_manager = first.buffer_manager;
		result->csv_names = names;
		result->csv_types = return_types;
		result->Finalize();
		return std::move(result);
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
	const bool many_files = result->file_count > 1 && result->file_count > TaskScheduler::QueryThreads(context) * 2;
	result->file_scan = make_shared_ptr<CSVFileScan>(
	    context, csv_data.file, std::move(options), file_options, csv_data.csv_names, csv_data.csv_types, no_schema,
	    many_files || !csv_data.options.parallel, csv_data.buffer_manager, false);

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
	result->scan = make_uniq<CsvScanGlobal>(context, file_scan, input.filters);
	result->max_threads = result->scan->MaxThreads();
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ReadSingleCSVFileInitLocal(ExecutionContext &context,
                                                                      TableFunctionInitInput &input,
                                                                      GlobalTableFunctionState *global_state) {
	return make_uniq<ReadSingleCSVFileLocalState>();
}

static void FillScanErrorTable(InternalAppender &appender, idx_t scan_idx, idx_t file_idx, CSVFileScan &file) {
	auto &options = file.options;
	auto &dialect = options.dialect_options;
	appender.BeginRow();
	appender.Append(scan_idx);
	appender.Append(file_idx);
	appender.Append(string_t(file.GetFileName()));
	appender.Append(string_t(dialect.state_machine_options.delimiter.FormatValue()));
	appender.Append(string_t(dialect.state_machine_options.quote.FormatValue()));
	appender.Append(string_t(dialect.state_machine_options.escape.FormatValue()));
	appender.Append(string_t(options.NewLineIdentifierToString()));
	appender.Append(Value::UINTEGER(NumericCast<uint32_t>(dialect.skip_rows.GetValue())));
	appender.Append(Value::BOOLEAN(dialect.header.GetValue()));
	auto &types = file.GetTypes();
	auto &names = file.GetNames();
	string columns = "{";
	for (idx_t i = 0; i < types.size(); i++) {
		columns += "'" + names[i] + "': '" + types[i].ToString() + "'";
		if (i + 1 != types.size()) {
			columns += ",";
		}
	}
	columns += "}";
	appender.Append(string_t(columns));
	for (auto type : {LogicalTypeId::DATE, LogicalTypeId::TIMESTAMP}) {
		auto format = dialect.date_format[type].GetValue();
		if (format.Empty()) {
			appender.Append(Value());
		} else {
			appender.Append(string_t(format.format_specifier));
		}
	}
	appender.Append(string_t(options.GetUserDefinedParameters()));
	appender.EndRow();
}

static void FillRejectsTable(ReadSingleCSVFileGlobalState &gstate, CSVFileScan &file) {
	auto &options = gstate.csv_data.options;
	if (!options.store_rejects.GetValue()) {
		return;
	}
	auto &context = gstate.context;
	auto rejects = CSVRejectsTable::GetOrCreate(context, options.rejects_scan_name.GetValue(),
	                                            options.rejects_table_name.GetValue());
	const lock_guard<mutex> lock(rejects->write_lock);
	auto &errors_table = rejects->GetErrorsTable(context);
	auto &scans_table = rejects->GetScansTable(context);
	InternalAppender errors_appender(context, errors_table);
	InternalAppender scans_appender(context, scans_table);
	const idx_t scan_idx = context.transaction.GetActiveQuery();
	const idx_t file_idx =
	    rejects->GetFileIndexBase(scan_idx, gstate.scan_op.get(), gstate.file_count) + file.GetFileIndex();
	file.error_handler->FillRejectsTable(errors_appender, file_idx, scan_idx, file, *rejects, gstate.csv_data.csv_names,
	                                     options.rejects_limit);
	if (rejects->count != 0) {
		rejects->count = 0;
		FillScanErrorTable(scans_appender, scan_idx, file_idx, file);
	}
	errors_appender.Close();
	scans_appender.Close();
}

static void NoticeSkippedRows(ClientContext &context, const CSVReaderOptions &options, CSVErrorHandler &handler) {
	auto &rules = options.copy_rules;
	if (!rules || !rules->options.ignore_errors) {
		return;
	}
	auto notices = context.registered_state->Get<CopyNotices>(CopyNotices::NAME);
	if (!notices) {
		return;
	}
	const auto rows = handler.CopySkippedRows();
	const auto verbosity = rules->options.log_verbosity;
	if (verbosity == CopyLogVerbosity::VERBOSE) {
		for (auto &row : rows) {
			notices->Notice(row);
		}
	}
	if (!rows.empty() && verbosity != CopyLogVerbosity::SILENT) {
		notices->Notice(std::to_string(rows.size()) + (rows.size() == 1 ? " row was" : " rows were") +
		                " skipped due to data type incompatibility");
	}
}

static void FinishFile(ReadSingleCSVFileGlobalState &gstate) {
	lock_guard<mutex> guard(gstate.lock);
	if (gstate.finished) {
		return;
	}
	gstate.finished = true;
	auto &file = *gstate.file_scan;
	file.Finish();
	file.error_handler->ErrorIfAny();
	NoticeSkippedRows(gstate.context, gstate.csv_data.options, *file.error_handler);
	FillRejectsTable(gstate, file);
}

static bool ClaimNextPart(ReadSingleCSVFileGlobalState &gstate, ReadSingleCSVFileLocalState &lstate) {
	if (!gstate.scan->Claim(lstate.range)) {
		if (gstate.scan->AllFinished()) {
			FinishFile(gstate);
		}
		return false;
	}
	if (!lstate.scan) {
		lstate.scan = make_uniq<CsvScan>(*gstate.scan);
	}
	lstate.scan->Start(lstate.range);
	lstate.scanning = true;
	return true;
}

static bool ReadSingleCSVFileClaimBatch(ClientContext &context, TableFunctionInput &input) {
	auto &gstate = input.global_state->Cast<ReadSingleCSVFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();
	lstate.claimed_externally = true;
	return ClaimNextPart(gstate, lstate);
}

static bool ReadSingleCSVFileSupportsRandomAccess(optional_ptr<const FunctionData> bind_data) {
	if (!bind_data) {
		return false;
	}
	auto &csv_data = bind_data->Cast<ReadSingleCSVFileData>();
	return csv_data.buffer_manager && csv_data.buffer_manager->file_handle &&
	       csv_data.buffer_manager->file_handle->HasKnownBufferRanges();
}

static bool ReadSingleCSVFileSupportsLookup(optional_ptr<const FunctionData> bind_data) {
	return ReadSingleCSVFileSupportsRandomAccess(bind_data) &&
	       !bind_data->Cast<ReadSingleCSVFileData>().options.ignore_errors.GetValue();
}

static void ReadSingleCSVFileFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ReadSingleCSVFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();
	while (true) {
		if (!lstate.scanning) {
			if (lstate.claimed_externally) {
				return;
			}
			if (!ClaimNextPart(gstate, lstate)) {
				return;
			}
		}
		const bool file_done = lstate.scan->Scan(output);
		if (output.size() != 0) {
			return;
		}
		lstate.scanning = false;
		if (file_done) {
			FinishFile(gstate);
		}
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
	result.emplace(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
	               TableColumn("file_row_number", LogicalType::BIGINT));
	return result;
}

static void ReadSingleCSVFileLookup(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ReadSingleCSVFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleCSVFileLocalState>();
	if (!lstate.scan) {
		lstate.scan = make_uniq<CsvScan>(*gstate.scan);
	}
	lstate.scan->Lookup(input.pk_lookups, input.pk_survivors, output);
}

static bool ReadSingleCSVFilePushdownExpression(ClientContext &, const LogicalGet &, Expression &expr) {
	return CsvScan::EvaluatesOnFieldBytes(expr);
}

TableFunction ReadCSVTableFunction::GetSingleFileFunction() {
	TableFunction read_csv("read_single_csv_file", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
	                       ReadSingleCSVFileFunction, ReadSingleCSVFileBind, ReadSingleCSVFileInitGlobal,
	                       ReadSingleCSVFileInitLocal);
	read_csv.get_virtual_columns = ReadSingleCSVFileGetVirtualColumns;
	read_csv.table_scan_progress = ReadSingleCSVFileProgress;
	read_csv.cardinality = ReadSingleCSVFileCardinality;
	read_csv.projection_pushdown = true;
	read_csv.filter_pushdown = true;
	read_csv.filter_prune = true;
	read_csv.pushdown_expression = ReadSingleCSVFilePushdownExpression;
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
	settings.lookup = ReadSingleCSVFileLookup;
	settings.supports_lookup = ReadSingleCSVFileSupportsLookup;
	settings.row_storage = true;
	settings.row_id_names = {"file_index", "file_offset"};
	// the scanner converts to the types the scan asks for while parsing, rather than casting its output
	settings.supports_cast_map = true;
	return TableFunctionMultiFileWrapper::CreateFunction(GetSingleFileFunction(), std::move(name), std::move(settings));
}

} // namespace duckdb
