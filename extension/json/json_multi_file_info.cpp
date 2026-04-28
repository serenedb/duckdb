#include "json_multi_file_info.hpp"
#include "json_common.hpp"
#include "json_scan.hpp"
#include "json_transform.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/parallel/async_result.hpp"

namespace duckdb {

namespace {

// Drain helper for read_json's lookup TableFunction. gstate is a
// MultiFileGlobalState built by MultiFileInitGlobal -- pk_lookups was
// propagated from init_input into JSONScanGlobalState during init, where
// ReadJSONFunctionPkLookup uses it for offset-seek random reads.
void JSONLookupScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<MultiFileGlobalState>();
	if (gstate.pk_lookups.empty()) {
		output.SetCardinality(0);
		return;
	}

	DataChunk scratch;
	scratch.Initialize(context, output.GetTypes());
	idx_t total = 0;
	const idx_t num_rows = gstate.pk_lookups.size();
	auto saved_mode = data.results_execution_mode;
	data.results_execution_mode = AsyncResultsExecutionMode::SYNCHRONOUS;
	while (total < num_rows) {
		scratch.Reset();
		MultiFileFunction<JSONMultiFileInfo>::MultiFileScan(context, data, scratch);
		const auto scanned = scratch.size();
		if (scanned == 0) {
			break;
		}
		for (idx_t c = 0; c < scratch.ColumnCount(); ++c) {
			VectorOperations::Copy(scratch.data[c], output.data[c], scanned, 0, total);
		}
		total += scanned;
	}
	data.results_execution_mode = saved_mode;
	output.SetCardinality(total);
}

} // namespace

// Thin lookup specialization of read_json. init_global / init_local are the
// regular MultiFileFunction ones (init_input.pk_lookups propagates into
// JSONScanGlobalState::pk_lookups via InitializeGlobalState). function is
// the drain loop above.
TableFunction MakeJSONLookupTableFunction() {
	TableFunction fn;
	fn.init_global = MultiFileFunction<JSONMultiFileInfo>::MultiFileInitGlobal;
	fn.init_local = MultiFileFunction<JSONMultiFileInfo>::MultiFileInitLocal;
	fn.function = JSONLookupScan;
	return fn;
}

unique_ptr<MultiFileReaderInterface> JSONMultiFileInfo::CreateInterface(ClientContext &context) {
	return make_uniq<JSONMultiFileInfo>();
}

void JSONMultiFileInfo::GetVirtualColumns(ClientContext &, MultiFileBindData &, virtual_column_map_t &result) {
	// Mirrors CSV: file_row_number = byte offset of the JSON record's start in
	// the file. Unique per row, stable across re-reads, usable as an exact-seek
	// key by the standalone JSON lookup TableFunction.
	result.insert(make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
	                        TableColumn("file_row_number", LogicalType::BIGINT)));
}

unique_ptr<BaseFileReaderOptions> JSONMultiFileInfo::InitializeOptions(ClientContext &context,
                                                                       optional_ptr<TableFunctionInfo> info) {
	auto reader_options = make_uniq<JSONFileReaderOptions>();
	auto &options = reader_options->options;
	if (info) {
		auto &scan_info = info->Cast<JSONScanInfo>();
		options.type = scan_info.type;
		options.format = scan_info.format;
		options.record_type = scan_info.record_type;
		options.auto_detect = scan_info.auto_detect;
		if (scan_info.type == JSONScanType::READ_JSON_OBJECTS) {
			// read_json_objects always emits a single JSON column called "json"
			options.sql_type_list.push_back(LogicalType::JSON());
			options.name_list.emplace_back("json");
		}
	} else {
		// COPY
		options.type = JSONScanType::READ_JSON;
		options.record_type = JSONRecordType::RECORDS;
		options.format = JSONFormat::AUTO_DETECT;
		options.auto_detect = false;
	}
	return std::move(reader_options);
}

bool JSONMultiFileInfo::ParseOption(ClientContext &context, const string &key, const Value &value, MultiFileOptions &,
                                    BaseFileReaderOptions &options_p) {
	auto &reader_options = options_p.Cast<JSONFileReaderOptions>();
	auto &options = reader_options.options;
	if (value.IsNull()) {
		throw BinderException("Cannot use NULL as argument to key %s", key);
	}
	auto loption = StringUtil::Lower(key);
	if (loption == "ignore_errors") {
		options.ignore_errors = BooleanValue::Get(value);
		return true;
	}
	if (loption == "maximum_object_size") {
		options.maximum_object_size = MaxValue<idx_t>(UIntegerValue::Get(value), options.maximum_object_size);
		return true;
	}
	if (loption == "format") {
		auto arg = StringUtil::Lower(StringValue::Get(value));
		static const auto FORMAT_OPTIONS =
		    case_insensitive_map_t<JSONFormat> {{"auto", JSONFormat::AUTO_DETECT},
		                                        {"unstructured", JSONFormat::UNSTRUCTURED},
		                                        {"newline_delimited", JSONFormat::NEWLINE_DELIMITED},
		                                        {"nd", JSONFormat::NEWLINE_DELIMITED},
		                                        {"array", JSONFormat::ARRAY}};
		auto lookup = FORMAT_OPTIONS.find(arg);
		if (lookup == FORMAT_OPTIONS.end()) {
			vector<string> valid_options;
			for (auto &pair : FORMAT_OPTIONS) {
				valid_options.push_back(StringUtil::Format("'%s'", pair.first));
			}
			throw BinderException("format must be one of [%s], not '%s'", StringUtil::Join(valid_options, ", "), arg);
		}
		options.format = lookup->second;
		return true;
	}
	if (loption == "compression") {
		options.compression = EnumUtil::FromString<FileCompressionType>(StringUtil::Upper(StringValue::Get(value)));
		return true;
	}
	if (loption == "columns") {
		auto &child_type = value.type();
		if (child_type.id() != LogicalTypeId::STRUCT) {
			throw BinderException("read_json \"columns\" parameter requires a struct as input.");
		}
		auto &struct_children = StructValue::GetChildren(value);
		D_ASSERT(StructType::GetChildCount(child_type) == struct_children.size());
		for (idx_t i = 0; i < struct_children.size(); i++) {
			auto &name = StructType::GetChildName(child_type, i);
			auto &val = struct_children[i];
			if (val.IsNull()) {
				throw BinderException("read_json \"columns\" parameter type specification cannot be NULL.");
			}
			options.name_list.push_back(name);
			if (val.type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("read_json \"columns\" parameter type specification must be VARCHAR.");
			}
			options.sql_type_list.emplace_back(TransformStringToLogicalType(StringValue::Get(val), context));
		}
		D_ASSERT(options.name_list.size() == options.sql_type_list.size());
		if (options.name_list.empty()) {
			throw BinderException("read_json \"columns\" parameter needs at least one column.");
		}
		return true;
	}
	if (loption == "auto_detect") {
		options.auto_detect = BooleanValue::Get(value);
		return true;
	}
	if (loption == "sample_size") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.sample_size = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.sample_size = arg;
		} else {
			throw BinderException("read_json \"sample_size\" parameter must be positive, or -1 to sample all input "
			                      "files entirely, up to \"maximum_sample_files\" files.");
		}
		return true;
	}
	if (loption == "maximum_depth") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.max_depth = NumericLimits<idx_t>::Maximum();
		} else {
			options.max_depth = arg;
		}
		return true;
	}
	if (loption == "field_appearance_threshold") {
		auto arg = DoubleValue::Get(value);
		if (arg < 0 || arg > 1) {
			throw BinderException("read_json_auto \"field_appearance_threshold\" parameter must be between 0 and 1");
		}
		options.field_appearance_threshold = arg;
		return true;
	}
	if (loption == "map_inference_threshold") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.map_inference_threshold = NumericLimits<idx_t>::Maximum();
		} else if (arg >= 0) {
			options.map_inference_threshold = arg;
		} else {
			throw BinderException("read_json_auto \"map_inference_threshold\" parameter must be 0 or positive, "
			                      "or -1 to disable map inference for consistent objects.");
		}
		return true;
	}
	if (loption == "dateformat" || loption == "date_format") {
		auto format_string = StringValue::Get(value);
		if (StringUtil::Lower(format_string) == "iso") {
			format_string = "%Y-%m-%d";
		}
		options.date_format = format_string;

		StrpTimeFormat format;
		auto error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw BinderException("read_json could not parse \"dateformat\": '%s'.", error.c_str());
		}
		return true;
	}
	if (loption == "timestampformat" || loption == "timestamp_format") {
		auto format_string = StringValue::Get(value);
		if (StringUtil::Lower(format_string) == "iso") {
			format_string = "%Y-%m-%dT%H:%M:%S.%fZ";
		}
		options.timestamp_format = format_string;

		StrpTimeFormat format;
		auto error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw BinderException("read_json could not parse \"timestampformat\": '%s'.", error.c_str());
		}
		return true;
	}
	if (loption == "records") {
		auto arg = StringValue::Get(value);
		if (arg == "auto") {
			options.record_type = JSONRecordType::AUTO_DETECT;
		} else if (arg == "true") {
			options.record_type = JSONRecordType::RECORDS;
		} else if (arg == "false") {
			options.record_type = JSONRecordType::VALUES;
		} else {
			throw BinderException("read_json requires \"records\" to be one of ['auto', 'true', 'false'].");
		}
		return true;
	}
	if (loption == "maximum_sample_files") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.maximum_sample_files = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.maximum_sample_files = arg;
		} else {
			throw BinderException("read_json \"maximum_sample_files\" parameter must be positive, or -1 to remove "
			                      "the limit on the number of files used to sample \"sample_size\" rows.");
		}
		return true;
	}
	if (loption == "convert_strings_to_integers") {
		options.convert_strings_to_integers = BooleanValue::Get(value);
		return true;
	}
	return false;
}

static void JSONCheckSingleParameter(const string &key, const vector<Value> &values) {
	if (values.size() == 1) {
		return;
	}
	throw BinderException("COPY (FORMAT JSON) parameter %s expects a single argument.", key);
}

bool JSONMultiFileInfo::ParseCopyOption(ClientContext &context, const string &key, const vector<Value> &values,
                                        BaseFileReaderOptions &options_p, vector<string> &expected_names,
                                        vector<LogicalType> &expected_types) {
	auto &reader_options = options_p.Cast<JSONFileReaderOptions>();
	auto &options = reader_options.options;
	const auto &loption = StringUtil::Lower(key);
	if (loption == "dateformat" || loption == "date_format") {
		JSONCheckSingleParameter(key, values);
		options.date_format = StringValue::Get(values.back());
		return true;
	}
	if (loption == "timestampformat" || loption == "timestamp_format") {
		JSONCheckSingleParameter(key, values);
		options.timestamp_format = StringValue::Get(values.back());
		return true;
	}
	if (loption == "auto_detect") {
		if (values.empty()) {
			options.auto_detect = true;
		} else {
			JSONCheckSingleParameter(key, values);
			options.auto_detect = BooleanValue::Get(values.back().DefaultCastAs(LogicalTypeId::BOOLEAN));
			options.format = JSONFormat::NEWLINE_DELIMITED;
		}
		return true;
	}
	if (loption == "compression") {
		JSONCheckSingleParameter(key, values);
		options.compression =
		    EnumUtil::FromString<FileCompressionType>(StringUtil::Upper(StringValue::Get(values.back())));
		return true;
	}
	if (loption == "array") {
		if (values.empty()) {
			options.format = JSONFormat::ARRAY;
		} else {
			JSONCheckSingleParameter(key, values);
			if (BooleanValue::Get(values.back().DefaultCastAs(LogicalTypeId::BOOLEAN))) {
				options.format = JSONFormat::ARRAY;
			} else {
				// Default to newline-delimited otherwise
				options.format = JSONFormat::NEWLINE_DELIMITED;
			}
		}
		return true;
	}
	return false;
}

unique_ptr<TableFunctionData> JSONMultiFileInfo::InitializeBindData(MultiFileBindData &multi_file_data,
                                                                    unique_ptr<BaseFileReaderOptions> options) {
	auto &reader_options = options->Cast<JSONFileReaderOptions>();
	auto json_data = make_uniq<JSONScanData>();
	json_data->options = std::move(reader_options.options);
	return std::move(json_data);
}

void JSONMultiFileInfo::BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<string> &names,
                                   MultiFileBindData &bind_data) {
	auto &json_data = bind_data.bind_data->Cast<JSONScanData>();

	auto &options = json_data.options;
	names = options.name_list;
	return_types = options.sql_type_list;
	if (options.record_type == JSONRecordType::AUTO_DETECT && return_types.size() > 1) {
		// More than one specified column implies records
		options.record_type = JSONRecordType::RECORDS;
	}

	// Specifying column names overrides auto-detect
	if (!return_types.empty()) {
		options.auto_detect = false;
	}

	if (!options.auto_detect) {
		// Need to specify columns if RECORDS and not auto-detecting
		if (return_types.empty()) {
			throw BinderException("When auto_detect=false, read_json requires columns to be specified through the "
			                      "\"columns\" parameter.");
		}
		// If we are reading VALUES, we can only have one column
		if (json_data.options.record_type == JSONRecordType::VALUES && return_types.size() != 1) {
			throw BinderException("read_json requires a single column to be specified through the \"columns\" "
			                      "parameter when \"records\" is set to 'false'.");
		}
	}

	json_data.InitializeFormats();

	if (options.auto_detect || options.record_type == JSONRecordType::AUTO_DETECT) {
		JSONScan::AutoDetect(context, bind_data, return_types, names);
		D_ASSERT(return_types.size() == names.size());
	}
	json_data.key_names = names;

	bind_data.multi_file_reader->BindOptions(bind_data.file_options, *bind_data.file_list, return_types, names,
	                                         bind_data.reader_bind);

	auto &transform_options = json_data.transform_options;
	transform_options.strict_cast = !options.ignore_errors;
	transform_options.error_duplicate_key = !options.ignore_errors;
	transform_options.error_missing_key = false;
	transform_options.error_unknown_key = options.auto_detect && !options.ignore_errors;
	transform_options.date_format_map = json_data.date_format_map.get();
	transform_options.delay_error = true;

	if (options.auto_detect) {
		// JSON may contain columns such as "id" and "Id", which are duplicates for us due to case-insensitivity
		// We rename them so we can parse the file anyway. Note that we can't change json_data.key_names,
		// because the JSON reader gets columns by exact name, not position
		case_insensitive_map_t<idx_t> name_collision_count;
		for (auto &col_name : names) {
			// Taken from CSV header_detection.cpp
			while (name_collision_count.find(col_name) != name_collision_count.end()) {
				name_collision_count[col_name] += 1;
				col_name = col_name + "_" + to_string(name_collision_count[col_name]);
			}
			name_collision_count[col_name] = 0;
		}
	}
	bool reuse_readers = true;
	for (auto &union_reader : bind_data.union_readers) {
		if (!union_reader || !union_reader->reader) {
			// not all readers have been initialized - don't re-use any
			reuse_readers = false;
			break;
		}
		auto &json_reader = union_reader->reader->Cast<JSONReader>();
		if (!json_reader.IsOpen()) {
			// no open file-handle - close
			reuse_readers = false;
		}
	}
	if (!reuse_readers) {
		bind_data.union_readers.clear();
	} else {
		// re-use readers
		for (auto &union_reader : bind_data.union_readers) {
			auto &json_reader = union_reader->reader->Cast<JSONReader>();
			union_reader->names = names;
			union_reader->types = return_types;
			union_reader->reader->columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(names, return_types);
			json_reader.Reset();
		}
	}
}

void JSONMultiFileInfo::FinalizeCopyBind(ClientContext &context, BaseFileReaderOptions &options_p,
                                         const vector<string> &expected_names,
                                         const vector<LogicalType> &expected_types) {
	auto &reader_options = options_p.Cast<JSONFileReaderOptions>();
	auto &options = reader_options.options;
	options.name_list = expected_names;
	options.sql_type_list = expected_types;
	if (options.auto_detect && options.format != JSONFormat::ARRAY) {
		options.format = JSONFormat::AUTO_DETECT;
	}
}

unique_ptr<GlobalTableFunctionState> JSONMultiFileInfo::InitializeGlobalState(ClientContext &context,
                                                                              MultiFileBindData &bind_data,
                                                                              MultiFileGlobalState &global_state) {
	auto json_state = make_uniq<JSONGlobalTableFunctionState>(context, bind_data);
	auto &json_data = bind_data.bind_data->Cast<JSONScanData>();

	auto &gstate = json_state->state;
	// Perform projection pushdown
	for (idx_t col_idx = 0; col_idx < global_state.column_indexes.size(); col_idx++) {
		auto &column_index = global_state.column_indexes[col_idx];
		const auto &col_id = column_index.GetPrimaryIndex();

		// Skip any multi-file reader / row id stuff
		if (bind_data.reader_bind.filename_idx.IsValid() && col_id == bind_data.reader_bind.filename_idx.GetIndex()) {
			continue;
		}
		if (IsVirtualColumn(col_id)) {
			// Record the output-chunk slot for file_row_number so ReadJSONFunction
			// can fill it with byte offsets after the transform. Other virtual
			// columns (filename, file_index, ...) are still skipped.
			if (col_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
				gstate.file_row_number_idx = col_idx;
			}
			continue;
		}
		bool skip = false;
		for (const auto &hive_partitioning_index : bind_data.reader_bind.hive_partitioning_indexes) {
			if (col_id == hive_partitioning_index.index) {
				skip = true;
				break;
			}
		}
		if (skip) {
			continue;
		}

		gstate.names.push_back(json_data.key_names[col_id]);
		gstate.column_ids.push_back(col_idx);
		gstate.column_indices.push_back(column_index);
	}
	if (gstate.names.size() < json_data.key_names.size() || bind_data.file_options.union_by_name) {
		// If we are auto-detecting, but don't need all columns present in the file,
		// then we don't need to throw an error if we encounter an unseen column
		gstate.transform_options.error_unknown_key = false;
	}
	gstate.pk_lookups = global_state.pk_lookups;
	return std::move(json_state);
}

unique_ptr<LocalTableFunctionState> JSONMultiFileInfo::InitializeLocalState(ExecutionContext &context,
                                                                            GlobalTableFunctionState &global_state) {
	auto &gstate = global_state.Cast<JSONGlobalTableFunctionState>();
	auto result = make_uniq<JSONLocalTableFunctionState>(context.client, gstate.state);

	// Copy the transform options / date format map because we need to do thread-local stuff
	result->state.transform_options = gstate.state.transform_options;

	return std::move(result);
}

double JSONReader::GetProgressInFile(ClientContext &context) {
	return GetProgress();
}

shared_ptr<BaseFileReader> JSONMultiFileInfo::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate_p,
                                                           BaseUnionData &union_data,
                                                           const MultiFileBindData &bind_data_p) {
	auto &json_data = bind_data_p.bind_data->Cast<JSONScanData>();
	auto reader = make_shared_ptr<JSONReader>(context, json_data.options, union_data.GetFileName());
	reader->columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(union_data.names, union_data.types);
	return std::move(reader);
}

shared_ptr<BaseFileReader> JSONMultiFileInfo::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate_p,
                                                           const OpenFileInfo &file, idx_t file_idx,
                                                           const MultiFileBindData &bind_data) {
	auto &json_data = bind_data.bind_data->Cast<JSONScanData>();
	auto reader = make_shared_ptr<JSONReader>(context, json_data.options, file.path);
	reader->columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(bind_data.names, bind_data.types);
	return std::move(reader);
}

void JSONReader::PrepareReader(ClientContext &context, GlobalTableFunctionState &gstate_p) {
	auto &gstate = gstate_p.Cast<JSONGlobalTableFunctionState>().state;
	if (gstate.enable_parallel_scans) {
		// if we are doing parallel scans we need to open the file here
		Initialize(gstate.allocator, gstate.buffer_capacity);
	}
}

bool JSONReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate_p,
                                   LocalTableFunctionState &lstate_p) {
	auto &gstate = gstate_p.Cast<JSONGlobalTableFunctionState>().state;
	auto &lstate = lstate_p.Cast<JSONLocalTableFunctionState>().state;

	lstate.GetScanState().ResetForNextBuffer();
	return lstate.TryInitializeScan(gstate, *this);
}

// Exact-offset scan path. Each Materialize batch hands us a sorted list of
// byte offsets (extracted from a `file_row_number IN (...)` filter). For each
// offset we issue a point read, parse exactly one JSON value starting at that
// position, and drop it into `values[]` -- no buffer iteration, no state
// machine advancement through the whole file. Total work is O(|offsets|)
// reads + parses.
static void ReadJSONFunctionPkLookup(JSONReader &json_reader, JSONScanGlobalState &gstate,
                                       JSONScanLocalState &lstate, DataChunk &output) {
	auto &scan_state = lstate.GetScanState();
	// The reader's file handle isn't opened by the normal PrepareReader path
	// in exact-offset mode (we bypass the buffer iterator that triggers it).
	// Open it lazily on first use so ReadRawAtPosition has something to read.
	if (!json_reader.HasFileHandle()) {
		json_reader.OpenJSONFile();
	}
	auto &file_handle = json_reader.GetFileHandle();
	const auto max_row_size = json_reader.GetOptions().maximum_object_size;
	auto *alc = scan_state.allocator.GetYYAlc();

	// Reuse scan_state.read_buffer -- same pattern as the normal-scan path
	// (JSONReader::ReadNextBufferSeek): allocate once from global_allocator,
	// persist across ReadJSONFunctionPkLookup calls on this lstate. yyjson
	// (without YYJSON_READ_INSITU) copies string payloads out of the buffer
	// into `alc`, so overwriting the buffer between offsets is safe -- the
	// already-parsed yyjson_doc objects stay valid.
	// `read_buffer` is otherwise unused in pk-lookup mode (we don't walk the
	// file via the buffer iterator).
	if (!scan_state.read_buffer.IsSet() || scan_state.read_buffer.GetSize() < max_row_size + YYJSON_PADDING_SIZE) {
		scan_state.read_buffer = scan_state.global_allocator.Allocate(max_row_size + YYJSON_PADDING_SIZE);
	}

	const auto offsets = gstate.pk_lookups;
	yyjson_val **values = scan_state.values;
	int64_t emitted_offsets[STANDARD_VECTOR_SIZE];
	idx_t row = 0;
	while (row < STANDARD_VECTOR_SIZE) {
		const auto i = gstate.lookup_cursor.fetch_add(1, std::memory_order_relaxed);
		if (i >= offsets.size()) {
			break;
		}
		const idx_t offset = UnsafeNumericCast<idx_t>(offsets[i]);
		auto *ptr = char_ptr_cast(scan_state.read_buffer.get());
		const auto read_size = file_handle.ReadRawAtPosition(ptr, max_row_size, offset);
		if (read_size == 0) {
			continue;
		}
		// YYJSON_READ_STOP_WHEN_DONE stops at the first valid JSON value, so
		// trailing bytes (next records, unrelated whitespace) don't fail the parse.
		yyjson_read_err err;
		auto *doc = JSONCommon::ReadDocumentUnsafe(ptr, read_size, JSONCommon::READ_STOP_FLAG, alc, &err);
		if (!doc) {
			continue;
		}
		values[row] = doc->root;
		emitted_offsets[row] = offsets[i];
		++row;
	}

	if (row == 0) {
		output.SetCardinality(0);
		return;
	}

	if (!gstate.names.empty()) {
		vector<Vector *> result_vectors;
		result_vectors.reserve(gstate.column_ids.size());
		for (idx_t i = 0; i < gstate.column_ids.size(); i++) {
			result_vectors.emplace_back(&output.data[gstate.column_ids[i]]);
		}
		D_ASSERT(gstate.json_data.options.record_type == JSONRecordType::RECORDS);
		JSONTransform::TransformObject(values, alc, row, gstate.names, result_vectors,
		                               lstate.transform_options, gstate.column_indices,
		                               lstate.transform_options.error_unknown_key);
	}

	// file_row_number output = the original offsets we read from (known good).
	if (gstate.file_row_number_idx != DConstants::INVALID_INDEX) {
		auto &frn_vec = output.data[gstate.file_row_number_idx];
		auto *frn_data = FlatVector::GetDataMutable<int64_t>(frn_vec);
		for (idx_t i = 0; i < row; ++i) {
			frn_data[i] = emitted_offsets[i];
		}
	}

	output.SetCardinality(row);
}

void ReadJSONFunction(ClientContext &context, JSONReader &json_reader, JSONScanGlobalState &gstate,
                      JSONScanLocalState &lstate, DataChunk &output) {
	if (!gstate.pk_lookups.empty()) {
		ReadJSONFunctionPkLookup(json_reader, gstate, lstate, output);
		return;
	}
	auto &scan_state = lstate.GetScanState();
	D_ASSERT(RefersToSameObject(json_reader, *scan_state.current_reader));

	const auto count = lstate.Read();
	yyjson_val **values = scan_state.values;

	// Use gstate.column_ids (real-column output slots) rather than
	// json_reader.column_ids (which may include virtual columns like
	// file_row_number appended by the multi-file column mapper -- those are
	// filled post-transform below, not by JSONTransform).
	if (!gstate.names.empty()) {
		vector<Vector *> result_vectors;
		result_vectors.reserve(gstate.column_ids.size());
		for (idx_t i = 0; i < gstate.column_ids.size(); i++) {
			result_vectors.emplace_back(&output.data[gstate.column_ids[i]]);
		}

		D_ASSERT(gstate.json_data.options.record_type != JSONRecordType::AUTO_DETECT);
		bool success;
		if (gstate.json_data.options.record_type == JSONRecordType::RECORDS) {
			success = JSONTransform::TransformObject(values, scan_state.allocator.GetYYAlc(), count, gstate.names,
			                                         result_vectors, lstate.transform_options, gstate.column_indices,
			                                         lstate.transform_options.error_unknown_key);
		} else {
			D_ASSERT(gstate.json_data.options.record_type == JSONRecordType::VALUES);
			success = JSONTransform::Transform(values, scan_state.allocator.GetYYAlc(), *result_vectors[0], count,
			                                   lstate.transform_options, gstate.column_indices[0]);
		}

		if (!success) {
			string hint =
			    gstate.json_data.options.auto_detect
			        ? "\nTry increasing 'sample_size', reducing 'maximum_depth', specifying 'columns', 'format' or "
			          "'records' manually, setting 'ignore_errors' to true, or setting 'union_by_name' to true when "
			          "reading multiple files with a different structure."
			        : "\nTry setting 'auto_detect' to true, specifying 'format' or 'records' manually, or setting "
			          "'ignore_errors' to true.";
			lstate.AddTransformError(lstate.transform_options.object_index,
			                         lstate.transform_options.error_message + hint);
			return;
		}
	}

	// If file_row_number is projected, fill that output slot with each record's
	// absolute file byte offset. units[i].pointer points into the read buffer;
	// buffer_handle->buffer_start is the buffer-local position of fresh data
	// and buffer_handle->file_start is the file offset that position maps to.
	if (gstate.file_row_number_idx != DConstants::INVALID_INDEX) {
		auto &frn_vec = output.data[gstate.file_row_number_idx];
		auto *frn_data = FlatVector::GetDataMutable<int64_t>(frn_vec);
		const auto *buffer_handle = scan_state.current_buffer_handle.get();
		const auto *data_base =
		    buffer_handle ? scan_state.buffer_ptr + buffer_handle->buffer_start : scan_state.buffer_ptr;
		const auto file_base = buffer_handle ? buffer_handle->file_start : idx_t {0};
		for (idx_t i = 0; i < count; ++i) {
			const auto local = UnsafeNumericCast<idx_t>(scan_state.units[i].pointer - data_base);
			frn_data[i] = UnsafeNumericCast<int64_t>(file_base + local);
		}
	}
	output.SetCardinality(count);
}

void ReadJSONObjectsFunction(ClientContext &context, JSONReader &json_reader, JSONScanGlobalState &gstate,
                             JSONScanLocalState &lstate, DataChunk &output) {
	// Fetch next lines
	auto &scan_state = lstate.GetScanState();
	D_ASSERT(RefersToSameObject(json_reader, *scan_state.current_reader));

	const auto count = lstate.Read();
	const auto units = scan_state.units;
	const auto objects = scan_state.values;

	if (!gstate.names.empty()) {
		// Create the strings without copying them
		auto strings = FlatVector::GetDataMutable<string_t>(output.data[0]);
		auto &validity = FlatVector::ValidityMutable(output.data[0]);
		for (idx_t i = 0; i < count; i++) {
			if (objects[i]) {
				strings[i] = string_t(units[i].pointer, units[i].size);
			} else {
				validity.SetInvalid(i);
			}
		}
	}

	output.SetCardinality(count);
}

AsyncResult JSONReader::Scan(ClientContext &context, GlobalTableFunctionState &global_state,
                             LocalTableFunctionState &local_state, DataChunk &output) {
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	{
		vector<unique_ptr<AsyncTask>> tasks = AsyncResult::GenerateTestTasks();
		if (!tasks.empty()) {
			return AsyncResult(std::move(tasks));
		}
	}
#endif

	auto &gstate = global_state.Cast<JSONGlobalTableFunctionState>().state;
	auto &lstate = local_state.Cast<JSONLocalTableFunctionState>().state;
	auto &json_data = gstate.bind_data.bind_data->Cast<JSONScanData>();
	switch (json_data.options.type) {
	case JSONScanType::READ_JSON:
		ReadJSONFunction(context, *this, gstate, lstate, output);
		break;
	case JSONScanType::READ_JSON_OBJECTS:
		ReadJSONObjectsFunction(context, *this, gstate, lstate, output);
		break;
	default:
		throw InternalException("Unsupported scan type for JSONMultiFileInfo::Scan");
	}
	return AsyncResult(output.size() ? SourceResultType::HAVE_MORE_OUTPUT : SourceResultType::FINISHED);
}

void JSONReader::FinishFile(ClientContext &context, GlobalTableFunctionState &global_state) {
	auto &gstate = global_state.Cast<JSONGlobalTableFunctionState>().state;
	gstate.file_is_assigned = false;
}

void JSONMultiFileInfo::FinishReading(ClientContext &context, GlobalTableFunctionState &global_state,
                                      LocalTableFunctionState &local_state) {
	auto &lstate = local_state.Cast<JSONLocalTableFunctionState>().state;
	lstate.GetScanState().ResetForNextBuffer();
}

unique_ptr<NodeStatistics> JSONMultiFileInfo::GetCardinality(const MultiFileBindData &bind_data, idx_t file_count) {
	auto &json_data = bind_data.bind_data->Cast<JSONScanData>();
	idx_t per_file_cardinality = 42;
	// get the average per-file cardinality from the bind data (if it is set)
	if (json_data.estimated_cardinality_per_file.IsValid()) {
		per_file_cardinality = json_data.estimated_cardinality_per_file.GetIndex();
	}
	return make_uniq<NodeStatistics>(per_file_cardinality * file_count);
}

optional_idx JSONMultiFileInfo::MaxThreads(const MultiFileBindData &bind_data, const MultiFileGlobalState &global_state,
                                           FileExpandResult expand_result) {
	if (expand_result == FileExpandResult::MULTIPLE_FILES) {
		return optional_idx();
	}
	// get the max threads from the bind data (if it is set)
	auto &json_data = bind_data.bind_data->Cast<JSONScanData>();
	return json_data.max_threads;
}

FileGlobInput JSONMultiFileInfo::GetGlobInput() {
	return FileGlobInput(FileGlobOptions::FALLBACK_GLOB, "json");
}

} // namespace duckdb
