#include "json_scan.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "json_structure.hpp"
#include "json_transform.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"

namespace duckdb {

JSONScanData::JSONScanData() {
}

JSONScanData::~JSONScanData() {
}

void JSONScanData::InitializeFormats() {
	InitializeFormats(options.auto_detect);
}

void JSONScanData::InitializeFormats(bool auto_detect_p) {
	type_id_map_t<vector<StrpTimeFormat>> candidate_formats;
	// Initialize date_format_map if anything was specified
	if (!options.date_format.empty()) {
		DateFormatMap::AddFormat(candidate_formats, LogicalTypeId::DATE, options.date_format);
	}
	if (!options.timestamp_format.empty()) {
		DateFormatMap::AddFormat(candidate_formats, LogicalTypeId::TIMESTAMP, options.timestamp_format);
	}

	if (auto_detect_p) {
		static const type_id_map_t<vector<const char *>> FORMAT_TEMPLATES = {
		    {LogicalTypeId::DATE, {"%m-%d-%Y", "%m-%d-%y", "%d-%m-%Y", "%d-%m-%y", "%Y-%m-%d", "%y-%m-%d"}},
		    {LogicalTypeId::TIMESTAMP,
		     {"%Y-%m-%d %H:%M:%S.%f", "%m-%d-%Y %I:%M:%S %p", "%m-%d-%y %I:%M:%S %p", "%d-%m-%Y %H:%M:%S",
		      "%d-%m-%y %H:%M:%S", "%Y-%m-%d %H:%M:%S", "%y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M:%SZ",
		      "%Y-%m-%dT%H:%M:%S.%fZ", "%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%dT%H:%M:%S.%f%z"}},
		};

		// Populate possible date/timestamp formats, assume this is consistent across columns
		for (auto &kv : FORMAT_TEMPLATES) {
			const auto &logical_type = kv.first;
			if (DateFormatMap::HasFormats(candidate_formats, logical_type)) {
				continue; // Already populated
			}
			const auto &format_strings = kv.second;
			for (auto &format_string : format_strings) {
				DateFormatMap::AddFormat(candidate_formats, logical_type, format_string);
			}
		}
	}
	date_format_map = make_uniq<DateFormatMap>(std::move(candidate_formats));
}

void JSONScan::Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                         const BoundTableFunction &) {
	throw NotImplementedException("JSONScan Serialize not implemented");
}

unique_ptr<FunctionData> JSONScan::Deserialize(Deserializer &deserializer, BoundTableFunction &) {
	throw NotImplementedException("JSONScan Deserialize not implemented");
}

void JSONScan::TableFunctionDefaults(TableFunction &table_function) {
	table_function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("maximum_object_size", LogicalType::UINTEGER)
		    .Add("ignore_errors", LogicalType::BOOLEAN)
		    .Add("strict", LogicalType::BOOLEAN)
		    .Add("format", LogicalType::VARCHAR)
		    .Add("compression", LogicalType::VARCHAR);
	});

	table_function.serialize = Serialize;
	table_function.deserialize = Deserialize;

	table_function.projection_pushdown = true;
	table_function.filter_pushdown = true;
	table_function.filter_prune = true;
}

void JSONScan::AddReadJSONParameters(TableFunction &table_function) {
	// extends the options TableFunctionDefaults declares
	table_function.GetSignature().ExtendTypedKwargs([](TypedKwargs &options) {
		options.Add("columns", LogicalType::ANY)
		    .Add("auto_detect", LogicalType::BOOLEAN)
		    .Add("geojson", LogicalType::BOOLEAN)
		    .Add("sample_size", LogicalType::BIGINT)
		    .Add("dateformat", LogicalType::VARCHAR)
		    .Add("date_format", LogicalType::VARCHAR)
		    .Add("timestampformat", LogicalType::VARCHAR)
		    .Add("timestamp_format", LogicalType::VARCHAR)
		    .Add("records", LogicalType::ANY)
		    .Add("array", LogicalType::BOOLEAN)
		    .Add("maximum_sample_files", LogicalType::BIGINT);
	});
}

void JSONScan::AddAutoDetectParameters(TableFunction &table_function) {
	table_function.GetSignature().ExtendTypedKwargs([](TypedKwargs &options) {
		options.Add("maximum_depth", LogicalType::BIGINT)
		    .Add("field_appearance_threshold", LogicalType::DOUBLE)
		    .Add("convert_strings_to_integers", LogicalType::BOOLEAN)
		    .Add("map_inference_threshold", LogicalType::BIGINT);
	});
}

bool JSONScan::ParseOption(ClientContext &context, const Identifier &key, const Value &value,
                           JSONReaderOptions &options) {
	if (value.IsNull()) {
		throw BinderException("Cannot use NULL as argument to key %s", key);
	}
	if (key == "ignore_errors") {
		options.ignore_errors = BooleanValue::Get(value);
		return true;
	}
	if (key == "strict") {
		options.strict = BooleanValue::Get(value);
		return true;
	}
	if (key == "maximum_object_size") {
		options.maximum_object_size = MaxValue<idx_t>(UIntegerValue::Get(value), options.maximum_object_size);
		return true;
	}
	if (key == "format") {
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
		options.format_specified = true;
		return true;
	}
	if (key == "compression") {
		options.compression = FileCompressionType(StringValue::Get(value));
		return true;
	}
	if (key == "array") {
		// "array" is a shorthand for the format - a JSON array of values, or newline-delimited values
		options.format = BooleanValue::Get(value) ? JSONFormat::ARRAY : JSONFormat::NEWLINE_DELIMITED;
		options.format_specified = true;
		return true;
	}
	if (key == "columns") {
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
			options.name_list.emplace_back(name);
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
	if (key == "auto_detect") {
		options.auto_detect = BooleanValue::Get(value);
		options.auto_detect_specified = true;
		return true;
	}
	if (key == "geojson") {
		options.geojson = BooleanValue::Get(value);
		return true;
	}
	if (key == "sample_size") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.sample_size = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.sample_size = arg;
		} else {
			// LCOV_EXCL_START
			throw BinderException("read_json \"sample_size\" parameter must be positive, or -1 to sample all input "
			                      "files entirely, up to \"maximum_sample_files\" files.");
			// LCOV_EXCL_STOP
		}
		return true;
	}
	if (key == "maximum_depth") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.max_depth = NumericLimits<idx_t>::Maximum();
		} else {
			options.max_depth = arg;
		}
		return true;
	}
	if (key == "field_appearance_threshold") {
		auto arg = DoubleValue::Get(value);
		if (arg < 0 || arg > 1) {
			throw BinderException("read_json_auto \"field_appearance_threshold\" parameter must be between 0 and 1");
		}
		options.field_appearance_threshold = arg;
		return true;
	}
	if (key == "map_inference_threshold") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.map_inference_threshold = NumericLimits<idx_t>::Maximum();
		} else if (arg >= 0) {
			options.map_inference_threshold = arg;
		} else {
			// LCOV_EXCL_START
			throw BinderException("read_json_auto \"map_inference_threshold\" parameter must be 0 or positive, "
			                      "or -1 to disable map inference for consistent objects.");
			// LCOV_EXCL_STOP
		}
		return true;
	}
	if (key == "dateformat" || key == "date_format") {
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
	if (key == "timestampformat" || key == "timestamp_format") {
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
	if (key == "records") {
		// records is tri-state: 'auto', 'true' or 'false'. Accept a boolean for the latter two, since
		// records = false reads more naturally than records = 'false'.
		auto arg = value.type().id() == LogicalTypeId::BOOLEAN ? string(BooleanValue::Get(value) ? "true" : "false")
		                                                       : StringValue::Get(value);
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
	if (key == "maximum_sample_files") {
		auto arg = BigIntValue::Get(value);
		if (arg == -1) {
			options.maximum_sample_files = NumericLimits<idx_t>::Maximum();
		} else if (arg > 0) {
			options.maximum_sample_files = arg;
		} else {
			// LCOV_EXCL_START
			throw BinderException("read_json \"maximum_sample_files\" parameter must be positive, or -1 to remove "
			                      "the limit on the number of files used to sample \"sample_size\" rows.");
			// LCOV_EXCL_STOP
		}
		return true;
	}
	if (key == "convert_strings_to_integers") {
		options.convert_strings_to_integers = BooleanValue::Get(value);
		return true;
	}
	return false;
}

void JSONScan::BindSchema(ClientContext &context, JSONScanData &json_data, MultiFileList &files,
                          vector<shared_ptr<JsonSource>> &sampled_sources, vector<LogicalType> &return_types,
                          vector<Identifier> &names) {
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
			// LCOV_EXCL_START
			throw BinderException("When auto_detect=false, read_json requires columns to be specified through the "
			                      "\"columns\" parameter.");
			// LCOV_EXCL_STOP
		}
		// If we are reading VALUES, we can only have one column
		if (options.record_type == JSONRecordType::VALUES && return_types.size() != 1) {
			// LCOV_EXCL_START
			throw BinderException("read_json requires a single column to be specified through the \"columns\" "
			                      "parameter when \"records\" is set to 'false'.");
			// LCOV_EXCL_STOP
		}
	}

	// Reading GeoJSON is opt-in, but a .geojson / .geojsonl file name is opt-in enough
	if (!options.geojson.has_value()) {
		const auto first_file = files.GetFirstFile().path;
		options.geojson =
		    StringUtil::CIEndsWith(first_file, ".geojson") || StringUtil::CIEndsWith(first_file, ".geojsonl");
	}
	// COPY ... FROM hardcodes RECORDS because it takes its columns from the target table. For GeoJSON we still
	// need to know whether those columns live in a Feature's "properties", so let the structure decide.
	if (options.geojson.value() && !options.auto_detect && !options.name_list.empty() &&
	    options.record_type == JSONRecordType::RECORDS) {
		options.record_type = JSONRecordType::AUTO_DETECT;
	}

	json_data.InitializeFormats();

	if (options.auto_detect || options.record_type == JSONRecordType::AUTO_DETECT) {
		JSONScan::AutoDetect(context, json_data, files.GetAllFiles(), sampled_sources, return_types, names);
		D_ASSERT(return_types.size() == names.size());
	}
	json_data.key_names = IdentifiersToStrings(names);
}

void JSONScan::FinalizeBind(JSONScanData &json_data, vector<Identifier> &names) {
	auto &options = json_data.options;
	auto &transform_options = json_data.transform_options;
	transform_options.strict_cast = !options.ignore_errors;
	transform_options.error_duplicate_key = !options.ignore_errors;
	transform_options.error_missing_key = false;
	transform_options.error_unknown_key = options.auto_detect && !options.ignore_errors;
	transform_options.date_format_map = json_data.date_format_map.get();
	transform_options.delay_error = true;

	if (!options.auto_detect) {
		return;
	}
	// Note that we can't change json_data.key_names, because the JSON reader gets columns by exact name, not position
	DeduplicateColumnNames(names);
}

void JSONScan::DeduplicateColumnNames(vector<Identifier> &names) {
	identifier_map_t<idx_t> name_collision_count;
	for (auto &col_name : names) {
		// Taken from CSV header_detection.cpp
		while (name_collision_count.find(col_name) != name_collision_count.end()) {
			name_collision_count[col_name] += 1;
			col_name = Identifier(col_name + "_" + to_string(name_collision_count[col_name]));
		}
		name_collision_count[col_name] = 0;
	}
}

} // namespace duckdb
