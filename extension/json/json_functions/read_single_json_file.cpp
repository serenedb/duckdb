#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "json_common.hpp"
#include "json_engine.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"
#include "json_structure.hpp"
#include "json_transform.hpp"

namespace duckdb {

struct ReadSingleJSONFileData : public JSONScanData {
	OpenFileInfo file;
	vector<LogicalType> types;

	shared_ptr<JsonSource> TakeBindSource() const {
		lock_guard<mutex> guard(bind_source_lock);
		return std::move(bind_source);
	}

	void SetBindSource(shared_ptr<JsonSource> source) {
		lock_guard<mutex> guard(bind_source_lock);
		bind_source = std::move(source);
	}

private:
	mutable mutex bind_source_lock;
	mutable shared_ptr<JsonSource> bind_source;
};

struct ReadSingleJSONFileGlobalState : public GlobalTableFunctionState {
public:
	idx_t MaxThreads() const override {
		return global->MaxThreads();
	}

public:
	unique_ptr<JsonScanGlobal> global;
	vector<column_t> projection_ids;
	vector<LogicalType> scanned_types;
};

struct ReadSingleJSONFileLocalState : public LocalTableFunctionState {
public:
	ReadSingleJSONFileLocalState(ClientContext &context, JsonScanGlobal &global,
	                             const vector<LogicalType> &scanned_types)
	    : scan(global) {
		if (!scanned_types.empty()) {
			all_columns.Initialize(BufferAllocator::Get(context), scanned_types);
		}
	}

public:
	JsonScan scan;
	DataChunk all_columns;
	bool claimed_externally = false;
};

static unique_ptr<FunctionData> ReadSingleJSONFileBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &file_input = TableFunctionFileBindInput::Get(input);
	auto result = make_uniq<ReadSingleJSONFileData>();
	auto &options = result->options;
	if (input.info) {
		auto &scan_info = input.info->Cast<JSONScanInfo>();
		options.type = scan_info.type;
		options.format = scan_info.format;
		options.record_type = scan_info.record_type;
		options.auto_detect = scan_info.auto_detect;
		if (scan_info.type == JSONScanType::READ_JSON_OBJECTS) {
			// read_json_objects always emits a single JSON column called "json"
			options.sql_type_list.push_back(LogicalType::JSON());
			options.name_list.emplace_back("json");
		}
	}
	for (auto &kv : input.named_parameters) {
		if (JSONScan::ParseOption(context, kv.first, kv.second, options)) {
			continue;
		}
		throw NotImplementedException("Unimplemented option %s", kv.first);
	}
	if (options.auto_detect_specified && !options.auto_detect && !options.format_specified) {
		// auto-detection was explicitly turned off - we do not detect the format either
		options.format = JSONFormat::NEWLINE_DELIMITED;
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("read_single_json_file requires a non-NULL file name");
	}
	if (file_input.expected_bind_data && file_input.HasExpectedSchema()) {
		// the schema of the scan was already determined - read this file exactly the way it was determined, so that
		// every file of the scan produces the same columns from the same JSON keys
		auto &source = file_input.expected_bind_data->Cast<ReadSingleJSONFileData>();
		result->file = TableFunctionFileBindInput::GetFile(input);
		result->options.record_type = source.options.record_type;
		result->options.geojson = source.options.geojson;
		result->options.auto_detect = false;
		result->options.name_list = *file_input.expected_names;
		result->options.sql_type_list = *file_input.expected_types;
		result->key_names = source.key_names;
		result->feature_columns = source.feature_columns;
		// the date/timestamp formats that auto-detection settled on are part of how the scan is read
		result->date_format_map = make_uniq<DateFormatMap>(*source.date_format_map);
		JSONScan::FinalizeBind(*result, result->options.name_list);
		names = result->options.name_list;
		return_types = result->options.sql_type_list;
		result->types = return_types;
		return std::move(result);
	}
	if (file_input.HasExpectedSchema()) {
		// the schema is known but not how it was determined (COPY takes its columns from the target table) - read
		// this file using those columns
		options.name_list = *file_input.expected_names;
		options.sql_type_list = *file_input.expected_types;
	}
	result->file = TableFunctionFileBindInput::GetFile(input);

	// keep the detected structure around - it is used to combine the schema of this file with that of other files
	// when this function is wrapped into a multi-file function. This is only needed when the columns are not known
	result->keep_structure = options.name_list.empty();

	SimpleMultiFileList file_list(vector<OpenFileInfo> {result->file});
	vector<shared_ptr<JsonSource>> sampled_sources;
	JSONScan::BindSchema(context, *result, file_list, sampled_sources, return_types, names);
	JSONScan::FinalizeBind(*result, names);
	result->types = return_types;
	if (!sampled_sources.empty() && sampled_sources[0]) {
		result->SetBindSource(std::move(sampled_sources[0]));
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadSingleJSONFileInitGlobal(ClientContext &context,
                                                                         TableFunctionInitInput &input) {
	auto &json_data = input.bind_data->Cast<ReadSingleJSONFileData>();
	auto result = make_uniq<ReadSingleJSONFileGlobalState>();
	JsonScanColumns gstate;
	gstate.transform_options = json_data.transform_options;

	auto &file_input = TableFunctionFileInitInput::Get(input);
	for (idx_t col_idx = 0; col_idx < input.column_indexes.size(); col_idx++) {
		auto &column_index = input.column_indexes[col_idx];
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
		if (virtual_column_id != DConstants::INVALID_INDEX) {
			if (virtual_column_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
				gstate.file_row_number = col_idx;
			}
			continue;
		}
		gstate.names.push_back(json_data.key_names[col_id]);
		gstate.types.push_back(json_data.types[col_id]);
		gstate.column_ids.push_back(col_idx);
		gstate.column_indices.push_back(column_index);
		if (!json_data.feature_columns.empty()) {
			gstate.feature_columns.push_back(json_data.feature_columns[col_id]);
		}
	}
	if (gstate.names.size() < json_data.key_names.size()) {
		gstate.transform_options.error_unknown_key = false;
	}
	if (input.CanRemoveFilterColumns()) {
		result->projection_ids.assign(input.projection_ids.begin(), input.projection_ids.end());
		for (auto &column_index : input.column_indexes) {
			const auto col_id = column_index.GetPrimaryIndex();
			if (col_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
				result->scanned_types.push_back(LogicalType::BIGINT);
			} else if (IsVirtualColumn(col_id)) {
				result->scanned_types.push_back(LogicalType::BOOLEAN);
			} else {
				result->scanned_types.push_back(json_data.types[col_id]);
			}
		}
	}
	if (input.filters) {
		for (auto &entry : *input.filters) {
			JsonScanFilter filter {entry.GetIndex(), optional_idx(), entry.Filter()};
			for (idx_t i = 0; i < gstate.column_ids.size(); i++) {
				if (gstate.column_ids[i] == filter.output_column) {
					filter.name = i;
				}
			}
			gstate.filters.push_back(filter);
		}
	}
	auto source = json_data.TakeBindSource();
	if (!source) {
		source = make_shared_ptr<JsonSource>(context, json_data.file, json_data.options.compression);
	}
	const bool parallel = TaskScheduler::QueryThreads(context) > 1;
	result->global = make_uniq<JsonScanGlobal>(context, json_data, source, std::move(gstate), parallel);
	source->ReadInOrder();
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ReadSingleJSONFileInitLocal(ExecutionContext &context,
                                                                       TableFunctionInitInput &input,
                                                                       GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<ReadSingleJSONFileGlobalState>();
	return make_uniq<ReadSingleJSONFileLocalState>(context.client, *gstate.global, gstate.scanned_types);
}

static bool ReadSingleJSONFileClaimBatch(ClientContext &context, TableFunctionInput &input) {
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	lstate.claimed_externally = true;
	return lstate.scan.Claim();
}

static void ReadSingleJSONFileFinishBatch(ClientContext &context, TableFunctionInput &input) {
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	lstate.scan.Release();
}

static void ReadSingleJSONFileFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ReadSingleJSONFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	const bool projected = !gstate.projection_ids.empty();
	auto &target = projected ? lstate.all_columns : output;
	while (true) {
		if (!lstate.scan.Claimed()) {
			if (lstate.claimed_externally || !lstate.scan.Claim()) {
				return;
			}
		}
		if (projected) {
			target.Reset();
		}
		if (lstate.scan.Scan(target) != 0) {
			if (projected) {
				output.ReferenceColumns(target, gstate.projection_ids);
			}
			return;
		}
		lstate.scan.Release();
	}
}

//! Combine the schemas of several JSON files by merging the structures that were detected for them - this gives the
//! same schema as running the auto-detection over all of the files at once
static unique_ptr<FunctionData> ReadSingleJSONFileCombineSchema(ClientContext &context,
                                                                TableFunctionCombineSchemaInput &input,
                                                                vector<LogicalType> &return_types,
                                                                vector<Identifier> &names) {
	JSONStructureNode merged;
	optional_ptr<const ReadSingleJSONFileData> first_file;
	for (auto &bind_data : input.bind_data) {
		auto &json_data = bind_data.get().Cast<ReadSingleJSONFileData>();
		if (!json_data.structure) {
			// the schema of this file was not auto-detected - fall back to combining the types
			return nullptr;
		}
		JSONStructure::MergeNodes(merged, *json_data.structure);
		if (!first_file) {
			first_file = json_data;
		}
	}
	if (!first_file) {
		return nullptr;
	}
	// the result describes how every file of the scan is read - it is the bind the auto-detection would have
	// produced if it had run over all of the sampled files at once
	auto result = make_uniq<ReadSingleJSONFileData>();
	result->options = first_file->options;
	if (first_file->record_type_auto_detected) {
		// the record type is re-detected on the combined structure
		result->options.record_type = JSONRecordType::AUTO_DETECT;
	}
	// the date/timestamp formats that were settled on while detecting the structure are carried over
	result->date_format_map = make_uniq<DateFormatMap>(*first_file->date_format_map);
	// the merged structure describes the schema of the scan by itself
	return_types.clear();
	names.clear();
	JSONScan::StructureToColumns(context, result->options, merged, result->feature_columns, return_types, names);

	// the JSON reader looks columns up by their exact key, so the keys are kept before the column names that are
	// duplicates for us (e.g. "id" and "Id") are renamed
	result->key_names = IdentifiersToStrings(names);
	JSONScan::DeduplicateColumnNames(names);
	result->options.name_list = names;
	result->options.sql_type_list = return_types;
	result->options.auto_detect = false;
	JSONScan::FinalizeBind(*result, names);
	result->types = return_types;
	return std::move(result);
}

static double ReadSingleJSONFileProgress(ClientContext &context, const FunctionData *bind_data,
                                         const GlobalTableFunctionState *global_state) {
	if (!global_state) {
		return 0;
	}
	return 100.0 * global_state->Cast<ReadSingleJSONFileGlobalState>().global->Progress();
}

static unique_ptr<NodeStatistics> ReadSingleJSONFileCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto &json_data = bind_data->Cast<ReadSingleJSONFileData>();
	idx_t per_file_cardinality = 42;
	if (json_data.estimated_cardinality_per_file.IsValid()) {
		per_file_cardinality = json_data.estimated_cardinality_per_file.GetIndex();
	}
	return make_uniq<NodeStatistics>(per_file_cardinality);
}

static virtual_column_map_t ReadSingleJSONFileGetVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
	// file_row_number = byte offset of the record start; usable as an exact-seek key.
	virtual_column_map_t result;
	result.emplace(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
	               TableColumn("file_row_number", LogicalType::BIGINT));
	return result;
}

namespace {

bool ReadSingleJSONFilePushdownExpression(ClientContext &, const LogicalGet &, Expression &expr) {
	return JsonWalkPushdown(expr);
}

void ReadSingleJSONFileLookup(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	D_ASSERT(input.pk_lookups.size() <= STANDARD_VECTOR_SIZE);
	lstate.scan.Lookup(input.pk_lookups, input.pk_survivors, output);
}

bool ReadSingleJSONFileSupportsLookup(optional_ptr<const FunctionData> bind_data) {
	if (!bind_data) {
		return false;
	}
	auto &json_data = bind_data->Cast<ReadSingleJSONFileData>();
	auto &compression = json_data.options.compression;
	const auto path = StringUtil::Lower(json_data.file.path);
	const bool compressed = compression.IsCompressed() ||
	                        (compression.IsAutoDetect() && (IsFileCompressed(path, FileCompressionType::GZIP) ||
	                                                        IsFileCompressed(path, FileCompressionType::ZSTD)));
	return json_data.feature_columns.empty() && !compressed;
}

} // namespace

TableFunction JSONFunctions::GetReadSingleJSONFileTableFunction(shared_ptr<JSONScanInfo> function_info) {
	const auto scan_type = function_info->type;
	TableFunction table_function(
	    "read_single_json_file", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
	    ReadSingleJSONFileFunction, ReadSingleJSONFileBind, ReadSingleJSONFileInitGlobal, ReadSingleJSONFileInitLocal);
	JSONScan::TableFunctionDefaults(table_function);
	if (scan_type != JSONScanType::READ_JSON_OBJECTS) {
		// read_json_objects always emits a single JSON column - it has no schema options
		JSONScan::AddReadJSONParameters(table_function);
		JSONScan::AddAutoDetectParameters(table_function);
	}
	table_function.get_virtual_columns = ReadSingleJSONFileGetVirtualColumns;
	table_function.table_scan_progress = ReadSingleJSONFileProgress;
	table_function.pushdown_expression = ReadSingleJSONFilePushdownExpression;
	table_function.cardinality = ReadSingleJSONFileCardinality;
	table_function.function_info = std::move(function_info);
	return table_function;
}

TableFunction JSONFunctions::GetJSONTableFunction(Identifier name, shared_ptr<JSONScanInfo> function_info) {
	// every JSON read function is the single-file JSON reader wrapped into a multi-file function
	auto single_file_function = GetReadSingleJSONFileTableFunction(std::move(function_info));
	TableFunctionMultiFileSettings settings;
	settings.glob_input = FileGlobInput(FileGlobOptions::FALLBACK_GLOB, "json");
	settings.reader_type = "JSON";
	// the schema is determined by combining the schemas of up to 32 files - the keys of the files are unified, so a
	// file does not need to have every column of the combined schema
	settings.maximum_sample_files = 32;
	settings.combine_schema = ReadSingleJSONFileCombineSchema;
	settings.claim_batch = ReadSingleJSONFileClaimBatch;
	settings.finish_batch = ReadSingleJSONFileFinishBatch;
	settings.lookup = ReadSingleJSONFileLookup;
	settings.supports_lookup = ReadSingleJSONFileSupportsLookup;
	settings.row_storage = true;
	settings.row_id_names = {"file_index", "file_offset"};
	return TableFunctionMultiFileWrapper::CreateFunction(std::move(single_file_function), std::move(name),
	                                                     std::move(settings));
}

static TableFunctionSet CreateJSONFunctionSet(Identifier name, shared_ptr<JSONScanInfo> function_info) {
	return MultiFileReader::CreateFunctionSet(
	    JSONFunctions::GetJSONTableFunction(std::move(name), std::move(function_info)));
}

static shared_ptr<JSONScanInfo> ReadJSONInfo(JSONFormat format) {
	return make_shared_ptr<JSONScanInfo>(JSONScanType::READ_JSON, format, JSONRecordType::AUTO_DETECT, true);
}

static shared_ptr<JSONScanInfo> ReadJSONObjectsInfo(JSONFormat format) {
	return make_shared_ptr<JSONScanInfo>(JSONScanType::READ_JSON_OBJECTS, format, JSONRecordType::RECORDS, false);
}

TableFunctionSet JSONFunctions::GetReadSingleJSONFileFunction() {
	TableFunctionSet function_set("read_single_json_file");
	function_set.AddFunction(GetReadSingleJSONFileTableFunction(ReadJSONInfo(JSONFormat::AUTO_DETECT)));
	return function_set;
}

TableFunctionSet JSONFunctions::GetReadJSONFunction() {
	return CreateJSONFunctionSet("read_json", ReadJSONInfo(JSONFormat::AUTO_DETECT));
}

TableFunctionSet JSONFunctions::GetReadNDJSONFunction() {
	return CreateJSONFunctionSet("read_ndjson", ReadJSONInfo(JSONFormat::NEWLINE_DELIMITED));
}

TableFunctionSet JSONFunctions::GetReadJSONAutoFunction() {
	return CreateJSONFunctionSet("read_json_auto", ReadJSONInfo(JSONFormat::AUTO_DETECT));
}

TableFunctionSet JSONFunctions::GetReadNDJSONAutoFunction() {
	return CreateJSONFunctionSet("read_ndjson_auto", ReadJSONInfo(JSONFormat::NEWLINE_DELIMITED));
}

TableFunctionSet JSONFunctions::GetReadJSONObjectsFunction() {
	return CreateJSONFunctionSet("read_json_objects", ReadJSONObjectsInfo(JSONFormat::AUTO_DETECT));
}

TableFunctionSet JSONFunctions::GetReadNDJSONObjectsFunction() {
	return CreateJSONFunctionSet("read_ndjson_objects", ReadJSONObjectsInfo(JSONFormat::NEWLINE_DELIMITED));
}

TableFunctionSet JSONFunctions::GetReadJSONObjectsAutoFunction() {
	return CreateJSONFunctionSet("read_json_objects_auto", ReadJSONObjectsInfo(JSONFormat::AUTO_DETECT));
}

} // namespace duckdb
