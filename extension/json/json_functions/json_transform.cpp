#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "json_transform.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "json_geojson.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/formats/value_parsers.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"

namespace duckdb {

JSONTransformOptions::JSONTransformOptions() : parameters(false, &error_message) {
}

JSONTransformOptions::JSONTransformOptions(bool strict_cast_p, bool error_duplicate_key_p, bool error_missing_key_p,
                                           bool error_unknown_key_p)
    : strict_cast(strict_cast_p), error_duplicate_key(error_duplicate_key_p), error_missing_key(error_missing_key_p),
      error_unknown_key(error_unknown_key_p), parameters(false, &error_message) {
}

JSONTransformOptions::JSONTransformOptions(const JSONTransformOptions &other)
    : strict_cast(other.strict_cast), error_duplicate_key(other.error_duplicate_key),
      error_missing_key(other.error_missing_key), error_unknown_key(other.error_unknown_key),
      delay_error(other.delay_error), rendering(other.rendering), date_format_map(other.date_format_map),
      error_message(other.error_message), object_index(other.object_index), parameters(other.parameters) {
	parameters.error_message = &error_message;
}

JSONTransformOptions &JSONTransformOptions::operator=(const JSONTransformOptions &other) {
	strict_cast = other.strict_cast;
	error_duplicate_key = other.error_duplicate_key;
	error_missing_key = other.error_missing_key;
	error_unknown_key = other.error_unknown_key;
	delay_error = other.delay_error;
	rendering = other.rendering;
	date_format_map = other.date_format_map;
	error_message = other.error_message;
	object_index = other.object_index;
	parameters = other.parameters;
	parameters.error_message = &error_message;
	return *this;
}

//! Forward declaration for recursion
static LogicalType StructureStringToType(const JSONNode &val, ClientContext &context);

static LogicalType StructureStringToTypeArray(const JSONNode &arr, ClientContext &context) {
	if (arr.size != 1) {
		throw BinderException("Too many values in array of JSON structure");
	}
	return LogicalType::LIST(StructureStringToType(*arr.First(), context));
}

static LogicalType StructureToTypeObject(const JSONNode &obj, ClientContext &context) {
	unordered_set<string> names;
	child_list_t<LogicalType> child_types;
	for (auto &key : obj.Keys()) {
		auto &val = *(&key + 1);
		string key_str(key.GetString());
		if (key_str.empty()) {
			throw BinderException("Empty key in object in JSON structure");
		}
		if (names.find(key_str) != names.end()) {
			JSONCommon::ThrowValFormatError("Duplicate keys in object in JSON structure: %s", val);
		}
		names.insert(key_str);
		child_types.emplace_back(key_str, StructureStringToType(val, context));
	}
	D_ASSERT(obj.size == names.size());
	if (child_types.empty()) {
		throw BinderException("Empty object in JSON structure");
	}
	return LogicalType::STRUCT(child_types);
}

static LogicalType StructureStringToType(const JSONNode &val, ClientContext &context) {
	switch (val.type) {
	case JSONValueType::ARRAY:
		return StructureStringToTypeArray(val, context);
	case JSONValueType::OBJECT:
		return StructureToTypeObject(val, context);
	case JSONValueType::STRING:
		return TransformStringToLogicalType(string(val.GetString()), context);
	default:
		throw BinderException("invalid JSON structure");
	}
}

static unique_ptr<FunctionData> JSONTransformBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(bound_function.GetArguments().size() == 2);
	auto structure_val = input.GetConstant(1);
	if (structure_val.IsNull() || arguments[1]->GetReturnType() == LogicalTypeId::SQLNULL) {
		bound_function.SetReturnType(LogicalTypeId::SQLNULL);
	} else {
		auto json_structure_val = structure_val.DefaultTryCastAs(LogicalType::JSON());
		if (!json_structure_val) {
			throw BinderException("Cannot cast JSON structure to string");
		}
		auto structure_string = json_structure_val->GetValueUnsafe<string_t>();
		JSONTreeParser parser(JSONCommon::READ_FLAG);
		auto &root = JSONCommon::ReadDocument(parser, structure_string);
		bound_function.SetReturnType(StructureStringToType(root, context));
	}
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

static inline string_t GetString(const JSONNode &val) {
	return string_t(val.str, val.size);
}

static inline bool IsNull(const JSONNode *val) {
	return !val || val->type == JSONValueType::JSON_NULL;
}

template <class T, class OP = TryCast>
static inline bool GetValueNumerical(const JSONNode &val, T &result, JSONTransformOptions &options) {
	D_ASSERT(val.type != JSONValueType::JSON_NULL);
	bool success;
	switch (val.type) {
	case JSONValueType::STRING:
	case JSONValueType::RAW:
		success = OP::template Operation<string_t, T>(GetString(val), result, options.strict_cast);
		break;
	case JSONValueType::ARRAY:
	case JSONValueType::OBJECT:
		success = false;
		break;
	case JSONValueType::BOOLEAN:
		success = OP::template Operation<bool, T>(val.boolean, result, options.strict_cast);
		break;
	case JSONValueType::UNSIGNED_INTEGER:
		success = OP::template Operation<uint64_t, T>(val.unsigned_integer, result, options.strict_cast);
		break;
	case JSONValueType::SIGNED_INTEGER:
		success = OP::template Operation<int64_t, T>(val.signed_integer, result, options.strict_cast);
		break;
	case JSONValueType::DOUBLE:
		success = OP::template Operation<double, T>(val.real, result, options.strict_cast);
		break;
	default:
		throw InternalException("Unknown JSON value type in GetValueNumerical");
	}
	return success;
}

template <class T, class OP = TryCastToDecimal>
static inline bool GetValueDecimal(const JSONNode &val, T &result, uint8_t w, uint8_t s,
                                   JSONTransformOptions &options) {
	D_ASSERT(val.type != JSONValueType::JSON_NULL);
	bool success;
	switch (val.type) {
	case JSONValueType::STRING:
	case JSONValueType::RAW:
		success = OP::template Operation<string_t, T>(GetString(val), result, options.parameters, w, s);
		break;
	case JSONValueType::ARRAY:
	case JSONValueType::OBJECT:
		success = false;
		break;
	case JSONValueType::BOOLEAN:
		success = OP::template Operation<bool, T>(val.boolean, result, options.parameters, w, s);
		break;
	case JSONValueType::UNSIGNED_INTEGER:
		success = OP::template Operation<uint64_t, T>(val.unsigned_integer, result, options.parameters, w, s);
		break;
	case JSONValueType::SIGNED_INTEGER:
		success = OP::template Operation<int64_t, T>(val.signed_integer, result, options.parameters, w, s);
		break;
	case JSONValueType::DOUBLE:
		success = OP::template Operation<double, T>(val.real, result, options.parameters, w, s);
		break;
	default:
		throw InternalException("Unknown JSON value type in GetValueDecimal");
	}
	return success;
}

static inline bool GetValueString(const JSONNode &val, JSONTransformScratch &scratch, string_t &result,
                                  Vector &vector) {
	D_ASSERT(val.type != JSONValueType::JSON_NULL);
	switch (val.type) {
	case JSONValueType::STRING:
	case JSONValueType::RAW:
		result = GetString(val);
		return true;
	case JSONValueType::ARRAY:
	case JSONValueType::OBJECT:
		result = JSONCommon::WriteVal(val, scratch.writer, vector);
		return true;
	case JSONValueType::BOOLEAN:
		result = StringCast::Operation<bool>(val.boolean, StringVector::GetStringHeap(vector));
		return true;
	case JSONValueType::UNSIGNED_INTEGER:
		result = StringCast::Operation<uint64_t>(val.unsigned_integer, StringVector::GetStringHeap(vector));
		return true;
	case JSONValueType::SIGNED_INTEGER:
		result = StringCast::Operation<int64_t>(val.signed_integer, StringVector::GetStringHeap(vector));
		return true;
	case JSONValueType::DOUBLE:
		result = StringCast::Operation<double>(val.real, StringVector::GetStringHeap(vector));
		return true;
	default:
		throw InternalException("Unknown JSON value type in GetValueString");
	}
}

template <class T>
static bool TransformNumerical(const JSONNode *vals[], Vector &result, const idx_t count,
                               JSONTransformOptions &options) {
	auto data = FlatVector::GetDataMutable<T>(result);
	auto &validity = FlatVector::ValidityMutable(result);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNull(val)) {
			validity.SetInvalid(i);
		} else if (!GetValueNumerical<T>(*val, data[i], options)) {
			validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Failed to cast value to numerical: %s", JSONCommon::ValToString(*val, 50));
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

template <class T>
static bool TransformDecimal(const JSONNode *vals[], Vector &result, const idx_t count, uint8_t width, uint8_t scale,
                             JSONTransformOptions &options) {
	auto data = FlatVector::GetDataMutable<T>(result);
	auto &validity = FlatVector::ValidityMutable(result);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNull(val)) {
			validity.SetInvalid(i);
		} else if (!GetValueDecimal<T>(*val, data[i], width, scale, options)) {
			validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Failed to cast value to decimal: %s", JSONCommon::ValToString(*val, 50));
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

bool JSONTransform::GetStringVector(const JSONNode *vals[], const idx_t count, const LogicalType &target,
                                    Vector &string_vector, JSONTransformOptions &options) {
	if (count > STANDARD_VECTOR_SIZE) {
		string_vector.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	}
	auto data = FlatVector::GetDataMutable<string_t>(string_vector);
	auto &validity = FlatVector::ValidityMutable(string_vector);
	validity.SetAllValid(count);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNull(val)) {
			validity.SetInvalid(i);
			continue;
		}

		if (val->type != JSONValueType::STRING) {
			validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message = StringUtil::Format(
				    "Unable to cast '%s' to %s", JSONCommon::ValToString(*val, 50), EnumUtil::ToString(target.id()));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		data[i] = GetString(*val);
	}
	return success;
}

static bool CastValues(Vector &source, Vector &result, const idx_t count, JSONTransformOptions &options, bool success) {
	auto context = options.parameters.context;
	bool cast;
	if (context && options.rendering != ValueRendering::NATIVE && source.GetType().id() == LogicalTypeId::VARCHAR) {
		ErrorData error;
		cast = MakeValueDecoder(*context, result.GetType(), options.rendering)->Decode(source, result, count, error);
		if (!cast) {
			options.error_message = error.RawMessage();
		}
	} else if (context) {
		cast = VectorOperations::TryCast(*context, source, result, count, &options.error_message);
	} else {
		cast = VectorOperations::DefaultTryCast(source, result, count, &options.error_message);
	}
	if (!cast && options.strict_cast) {
		options.object_index = 0; // Can't get line number information here
		options.error_message +=
		    "\n If this error occurred during read_json, line/object number information is approximate";
		success = false;
	}
	return success;
}

static bool TransformFromString(const JSONNode *vals[], Vector &result, const idx_t count,
                                JSONTransformOptions &options) {
	Vector string_vector(LogicalTypeId::VARCHAR, count);
	const bool success = JSONTransform::GetStringVector(vals, count, result.GetType(), string_vector, options);
	return CastValues(string_vector, result, count, options, success);
}

//! GEOMETRY accepts both WKT strings and GeoJSON geometry fragments. A JSON object is never valid WKT, so the two
//! are unambiguous and no option is needed to tell them apart.
static bool TransformGeometry(const JSONNode *vals[], Vector &result, const idx_t count,
                              JSONTransformOptions &options) {
	bool any_object = false;
	for (idx_t i = 0; i < count; i++) {
		if (vals[i] && vals[i]->type == JSONValueType::OBJECT) {
			any_object = true;
			break;
		}
	}
	if (!any_object) {
		// Nothing but WKT here, so use the regular string cast
		return TransformFromString(vals, result, count, options);
	}

	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	validity.SetAllValid(count);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNull(val)) {
			validity.SetInvalid(i);
			continue;
		}

		string error;
		if (val->type == JSONValueType::OBJECT) {
			if (JSONGeometry::FromGeoJSON(val, data[i], result, error)) {
				continue;
			}
		} else if (val->type == JSONValueType::STRING) {
			try {
				if (Geometry::FromString(GetString(*val), data[i], result, true)) {
					continue;
				}
			} catch (std::exception &ex) {
				ErrorData error_data(ex);
				error = error_data.RawMessage();
			}
		}

		if (error.empty()) {
			error = StringUtil::Format("Unable to cast '%s' to GEOMETRY", JSONCommon::ValToString(*val, 50));
		}
		validity.SetInvalid(i);
		if (success && options.strict_cast) {
			options.error_message = error;
			options.object_index = i;
			success = false;
		}
	}
	return success;
}

static bool TryOneFormat(const StrpTimeFormat &fmt, bool iso_date, LogicalTypeId type_id, const string_t &input,
                         Vector &result, idx_t i, string &error_message) {
	if (type_id == LogicalTypeId::DATE) {
		date_t val;
		if (!(iso_date && FastDate(input.GetData(), input.GetSize(), val)) &&
		    !TryParseDate::Operation<date_t>(fmt, input, val, error_message)) {
			return false;
		}
		FlatVector::GetDataMutable<date_t>(result)[i] = val;
	} else {
		timestamp_t val;
		if (!TryParseTimeStamp::Operation<timestamp_t>(fmt, input, val, error_message)) {
			return false;
		}
		FlatVector::GetDataMutable<timestamp_t>(result)[i] = val;
	}
	return true;
}

template <class OP, class T>
static bool TransformStringWithFormat(const Vector &string_vector, const StrpTimeFormat &format, const idx_t count,
                                      Vector &result, JSONTransformOptions &options) {
	const auto source_strings = FlatVector::GetData<string_t>(string_vector);
	const auto &source_validity = FlatVector::Validity(string_vector);

	auto target_vals = FlatVector::GetDataMutable<T>(result);
	auto &target_validity = FlatVector::ValidityMutable(result);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		if (!source_validity.RowIsValid(i)) {
			target_validity.SetInvalid(i);
		} else if (!OP::template Operation<T>(format, source_strings[i], target_vals[i], options.error_message)) {
			target_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

static bool FormatStringVector(const Vector &string_vector, Vector &result, const idx_t count,
                               JSONTransformOptions &options, bool success) {
	const auto result_type_id = result.GetType().id();
	D_ASSERT(result_type_id == LogicalTypeId::DATE || result_type_id == LogicalTypeId::TIMESTAMP);
	const auto &formats = options.date_format_map->GetFormats(result_type_id);
	const auto source_strings = FlatVector::GetData<string_t>(string_vector);
	const auto &source_validity = FlatVector::Validity(string_vector);
	auto &target_validity = FlatVector::ValidityMutable(result);
	vector<bool> iso_dates;
	for (auto &format : formats) {
		iso_dates.push_back(result_type_id == LogicalTypeId::DATE && format.format_specifier == "%Y-%m-%d");
	}

	// Probe first non-null value to find the most-specific matching format for this vector
	idx_t matched_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < count && matched_idx == DConstants::INVALID_INDEX; i++) {
		if (!source_validity.RowIsValid(i)) {
			continue;
		}
		string error_message;
		for (idx_t j = formats.size(); j > 0; j--) {
			if (TryOneFormat(formats[j - 1], iso_dates[j - 1], result_type_id, source_strings[i], result, i,
			                 error_message)) {
				matched_idx = j - 1;
				break;
			}
		}
	}

	const idx_t parse_limit = (matched_idx != DConstants::INVALID_INDEX) ? matched_idx + 1 : formats.size();
	for (idx_t i = 0; i < count; i++) {
		if (!source_validity.RowIsValid(i)) {
			target_validity.SetInvalid(i);
			continue;
		}
		bool parsed = false;
		string error_message;
		// Reverse iter formats, beginning at matched_idx until one parses
		for (idx_t j = parse_limit; j > 0 && !parsed; j--) {
			parsed = TryOneFormat(formats[j - 1], iso_dates[j - 1], result_type_id, source_strings[i], result, i,
			                      error_message);
		}
		if (!parsed) {
			target_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				options.error_message = std::move(error_message);
				success = false;
			}
		}
	}
	return success;
}

static bool UsesDateFormats(const LogicalType &type, const JSONTransformOptions &options) {
	return (type == LogicalTypeId::TIMESTAMP || type == LogicalTypeId::DATE) && options.date_format_map &&
	       options.date_format_map->HasFormats(type.id());
}

static bool TransformFromStringWithFormat(const JSONNode *vals[], Vector &result, const idx_t count,
                                          JSONTransformOptions &options) {
	Vector string_vector(LogicalTypeId::VARCHAR, count);
	const bool success = JSONTransform::GetStringVector(vals, count, result.GetType(), string_vector, options);
	return FormatStringVector(string_vector, result, count, options, success);
}

static bool FastDates(const Vector &string_vector, Vector &result, const idx_t count) {
	const auto strings = FlatVector::GetData<string_t>(string_vector);
	const auto &string_validity = FlatVector::Validity(string_vector);
	auto dates = FlatVector::GetDataMutable<date_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < count; i++) {
		if (!string_validity.RowIsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}
		if (!FastDate(strings[i].GetData(), strings[i].GetSize(), dates[i])) {
			return false;
		}
	}
	return true;
}

bool JSONTransform::TransformStrings(Vector &string_vector, Vector &result, const idx_t count,
                                     JSONTransformOptions &options) {
	if (UsesDateFormats(result.GetType(), options)) {
		return FormatStringVector(string_vector, result, count, options, true);
	}
	if (result.GetType() == LogicalType::DATE && FastDates(string_vector, result, count)) {
		return true;
	}
	return CastValues(string_vector, result, count, options, true);
}

bool JSONTransform::ParsesFromString(const LogicalType &type) {
	if (type.IsJSONType()) {
		return false;
	}
	switch (type.id()) {
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BIT:
		return true;
	default:
		return false;
	}
}

static bool TransformToString(const JSONNode *vals[], JSONTransformScratch &scratch, Vector &result,
                              const idx_t count) {
	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNull(val)) {
			validity.SetInvalid(i);
		} else if (!GetValueString(*val, scratch, data[i], result)) {
			validity.SetInvalid(i);
		}
	}
	// Can always transform to string
	return true;
}

template <class T>
static T *AllocateScratch(JSONTransformScratch &scratch, idx_t count) {
	return reinterpret_cast<T *>(scratch.arena.AllocateAligned(sizeof(T) * MaxValue<idx_t>(count, 1)));
}

static bool TransformAliased(const JSONNode *vals[], JSONTransformScratch &scratch, Vector &result, const idx_t count,
                             JSONTransformOptions &options) {
	idx_t strings = 0;
	idx_t others = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!IsNull(vals[i])) {
			vals[i]->type == JSONValueType::STRING ? strings++ : others++;
		}
	}
	if (others == 0) {
		Vector string_vector(LogicalTypeId::VARCHAR, count);
		TransformToString(vals, scratch, string_vector, count);
		return CastValues(string_vector, result, count, options, true);
	}
	const auto storage_type = result.GetType().WithAlias("");
	if (strings == 0) {
		Vector storage(storage_type, count);
		const bool success = JSONTransform::Transform(vals, scratch, storage, count, options, nullptr);
		return CastValues(storage, result, count, options, success);
	}
	auto string_vals = AllocateScratch<const JSONNode *>(scratch, count);
	auto other_vals = AllocateScratch<const JSONNode *>(scratch, count);
	for (idx_t i = 0; i < count; i++) {
		const bool string = !IsNull(vals[i]) && vals[i]->type == JSONValueType::STRING;
		string_vals[i] = string ? vals[i] : nullptr;
		other_vals[i] = string ? nullptr : vals[i];
	}
	Vector from_strings(result.GetType(), count);
	bool success = TransformAliased(string_vals, scratch, from_strings, count, options);
	success = TransformAliased(other_vals, scratch, result, count, options) && success;
	for (idx_t i = 0; i < count; i++) {
		if (string_vals[i]) {
			result.SetValue(i, from_strings.GetValue(i));
		}
	}
	return success;
}

bool JSONTransform::TransformObject(const JSONNode *objects[], JSONTransformScratch &scratch, const idx_t count,
                                    const vector<string> &names, const vector<Vector *> &result_vectors,
                                    JSONTransformOptions &options,
                                    optional_ptr<const vector<ColumnIndex>> column_indices, bool error_unknown_key) {
	if (column_indices && column_indices->empty()) {
		column_indices = nullptr;
	}
	D_ASSERT(names.size() == result_vectors.size());
	D_ASSERT(!column_indices || column_indices->size() == names.size());
	const idx_t column_count = names.size();

	// Build hash map from key to column index so we don't have to linearly search using the key
	json_key_map_t<idx_t> key_map;
	json_key_ci_map_t<idx_t> ci_key_map;
	vector<const JSONNode **> nested_vals;
	nested_vals.reserve(column_count);
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		key_map.insert({{names[col_idx].c_str(), names[col_idx].length()}, col_idx});
		ci_key_map.emplace(JSONKey {names[col_idx].c_str(), names[col_idx].length()}, col_idx);
		nested_vals.push_back(AllocateScratch<const JSONNode *>(scratch, count));
	}

	idx_t found_key_count;
	// 0 = not found, 1 = case-insensitive match, 2 = exact match: an exact match overrides a
	// case-insensitive one, and only two exact (or two case-insensitive) matches are duplicates
	auto found_keys = AllocateScratch<uint8_t>(scratch, column_count);

	bool success = true;
	// The column of the exact match of the key at each position in the previous object: the objects of a column
	// mostly list their keys in the same order, so one comparison often replaces the hash lookups
	vector<optional_idx> predicted;

	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (IsNull(obj)) {
			// Set nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				nested_vals[col_idx][i] = nullptr;
			}
			continue;
		}

		if (obj->type != JSONValueType::OBJECT) {
			// Set nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				nested_vals[col_idx][i] = nullptr;
			}
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected OBJECT, but got %s: %s", JSONCommon::ValTypeToString(*obj),
				                       JSONCommon::ValToString(*obj, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		found_key_count = 0;
		memset(found_keys, 0, column_count);
		idx_t position = 0;
		for (auto &key : obj->Keys()) {
			auto val = &key + 1;
			auto key_ptr = key.str;
			auto key_len = key.size;
			optional_idx found_col;
			uint8_t match_strength = 2;
			if (position < predicted.size() && predicted[position].IsValid() &&
			    names[predicted[position].GetIndex()].size() == key_len &&
			    memcmp(names[predicted[position].GetIndex()].data(), key_ptr, key_len) == 0) {
				found_col = predicted[position];
			} else {
				auto it = key_map.find({key_ptr, key_len});
				if (it != key_map.end()) {
					found_col = it->second;
				} else {
					auto ci_it = ci_key_map.find({key_ptr, key_len});
					if (ci_it != ci_key_map.end()) {
						found_col = ci_it->second;
						match_strength = 1;
					}
				}
				if (position >= predicted.size()) {
					predicted.resize(position + 1);
				}
				predicted[position] = match_strength == 2 ? found_col : optional_idx();
			}
			position++;
			if (found_col.IsValid()) {
				const auto col_idx = found_col.GetIndex();
				if (found_keys[col_idx] == match_strength) {
					if (success && options.error_duplicate_key) {
						options.error_message =
						    StringUtil::Format("Object %s has duplicate key \"%s\"",
						                       JSONCommon::ValToString(*objects[i], 50), string(key_ptr, key_len));
						options.object_index = i;
						success = false;
					}
				} else if (found_keys[col_idx] < match_strength) {
					// first match, or an exact match overriding a case-insensitive one
					nested_vals[col_idx][i] = val;
					if (found_keys[col_idx] == 0) {
						found_key_count++;
					}
					found_keys[col_idx] = match_strength;
				}
			} else if (success && error_unknown_key && options.error_unknown_key) {
				options.error_message =
				    StringUtil::Format("Object %s has unknown key \"%s\"", JSONCommon::ValToString(*objects[i], 50),
				                       string(key_ptr, key_len));
				options.object_index = i;
				success = false;
			}
		}

		if (found_key_count != column_count) {
			// If 'error_missing_key, we throw an error if one of the keys was not found.
			// If not, we set the nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				if (found_keys[col_idx]) {
					continue;
				}
				nested_vals[col_idx][i] = nullptr;

				if (success && options.error_missing_key) {
					options.error_message = StringUtil::Format(
					    "Object %s does not have key \"%s\"", JSONCommon::ValToString(*objects[i], 50), names[col_idx]);
					options.object_index = i;
					success = false;
				}
			}
		}
	}

	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		if (!result_vectors[col_idx]) {
			continue;
		}
		auto child_column_index = column_indices ? &(*column_indices)[col_idx] : nullptr;
		if (!JSONTransform::Transform(nested_vals[col_idx], scratch, *result_vectors[col_idx], count, options,
		                              child_column_index)) {
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformObjectInternal(const JSONNode *objects[], JSONTransformScratch &scratch, Vector &result,
                                    const idx_t count, JSONTransformOptions &options,
                                    optional_ptr<const ColumnIndex> column_index) {
	if (column_index && column_index->ChildIndexCount() == 0) {
		column_index = nullptr;
	}

	// Set validity first
	auto &result_validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (IsNull(obj)) {
			result_validity.SetInvalid(i);
		}
	}

	// Get child vectors and names
	auto &child_vs = StructVector::GetEntries(result);
	vector<string> child_names;
	vector<Vector *> child_vectors;

	const auto child_count = column_index ? column_index->ChildIndexCount() : child_vs.size();
	child_names.reserve(child_count);
	child_vectors.reserve(child_count);

	unordered_set<idx_t> projected_indices;
	for (idx_t child_i = 0; child_i < child_count; child_i++) {
		const auto actual_i = column_index ? column_index->GetChildIndex(child_i).GetPrimaryIndex() : child_i;
		projected_indices.insert(actual_i);

		child_names.emplace_back(StructType::GetChildName(result.GetType(), actual_i));
		child_vectors.push_back(&child_vs[actual_i]);
	}

	for (idx_t child_i = 0; child_i < child_vs.size(); child_i++) {
		if (projected_indices.find(child_i) == projected_indices.end()) {
			ConstantVector::SetNull(child_vs[child_i], count_t(count));
		}
	}

	auto child_indices = column_index ? &column_index->GetChildIndexes() : nullptr;
	const auto error_unknown_key = child_count == child_vs.size(); // Nothing projected out, error if unknown
	return JSONTransform::TransformObject(objects, scratch, count, child_names, child_vectors, options, child_indices,
	                                      error_unknown_key);
}

static idx_t RowOfElement(const list_entry_t entries[], const ValidityMask &validity, idx_t element) {
	idx_t row = 0;
	while (!validity.RowIsValid(row) || element >= entries[row].offset + entries[row].length) {
		row++;
	}
	return row;
}

template <class ROW_OF>
static bool TransformElements(const JSONNode *elements[], idx_t element_count, JSONTransformScratch &scratch,
                              Vector &result, JSONTransformOptions &options, bool rows_failed, ROW_OF &&row_of) {
	const auto row_message = rows_failed ? options.error_message : string();
	const auto row_index = options.object_index;
	if (JSONTransform::Transform(elements, scratch, result, element_count, options, nullptr)) {
		return !rows_failed;
	}
	const auto element_row = row_of(options.object_index);
	if (rows_failed && row_index < element_row) {
		options.error_message = row_message;
		options.object_index = row_index;
	} else {
		options.object_index = element_row;
	}
	return false;
}

static bool TransformArrayToList(const JSONNode *arrays[], JSONTransformScratch &scratch, Vector &result,
                                 const idx_t count, JSONTransformOptions &options) {
	bool success = true;

	// Initialize list vector
	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &list_validity = FlatVector::ValidityMutable(result);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		if (IsNull(arr)) {
			list_validity.SetInvalid(i);
			continue;
		}

		if (arr->type != JSONValueType::ARRAY) {
			list_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(*arr),
				                       JSONCommon::ValToString(*arr, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto &entry = list_entries[i];
		entry.offset = offset;
		entry.length = arr->size;
		offset += entry.length;
	}
	ListVector::SetListSize(result, offset);
	ListVector::Reserve(result, offset);

	// Initialize array for the nested values
	auto nested_vals = AllocateScratch<const JSONNode *>(scratch, offset);

	// Get array values
	idx_t list_i = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!list_validity.RowIsValid(i)) {
			continue; // We already marked this as invalid
		}
		for (auto &val : arrays[i]->Elements()) {
			nested_vals[list_i] = &val;
			list_i++;
		}
	}
	D_ASSERT(list_i == offset);

	success = TransformElements(nested_vals, offset, scratch, ListVector::GetChildMutable(result), options, !success,
	                            [&](idx_t element) { return RowOfElement(list_entries, list_validity, element); });

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformArrayToTuple(const JSONNode *arrays[], JSONTransformScratch &scratch, Vector &result,
                                  const idx_t count, JSONTransformOptions &options) {
	// A TUPLE is serialized as a JSON array - read each array element positionally into the matching struct child
	bool success = true;
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto &child_vs = StructVector::GetEntries(result);
	const idx_t child_count = child_vs.size();

	// nested_vals[child_i * count + i] holds the JSON value for child child_i of row i
	auto nested_vals = AllocateScratch<const JSONNode *>(scratch, child_count * count);
	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		bool valid = !IsNull(arr) && arr->type == JSONValueType::ARRAY;
		if (!valid) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast && !IsNull(arr)) {
				options.error_message =
				    StringUtil::Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(*arr),
				                       JSONCommon::ValToString(*arr, 50));
				options.object_index = i;
				success = false;
			}
			for (idx_t child_i = 0; child_i < child_count; child_i++) {
				nested_vals[child_i * count + i] = nullptr;
			}
			continue;
		}
		// gather up to child_count elements - missing elements become NULL, extra elements are ignored
		idx_t child_i = 0;
		for (auto &val : arr->Elements()) {
			if (child_i < child_count) {
				nested_vals[child_i * count + i] = &val;
			}
			child_i++;
		}
		for (; child_i < child_count; child_i++) {
			nested_vals[child_i * count + i] = nullptr;
		}
	}

	for (idx_t child_i = 0; child_i < child_count; child_i++) {
		if (!JSONTransform::Transform(nested_vals + child_i * count, scratch, child_vs[child_i], count, options,
		                              nullptr)) {
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}
	return success;
}

static bool TransformArrayToArray(const JSONNode *arrays[], JSONTransformScratch &scratch, Vector &result,
                                  const idx_t count, JSONTransformOptions &options) {
	bool success = true;

	// Initialize array vector
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto array_size = ArrayType::GetSize(result.GetType());
	auto child_count = count * array_size;

	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		if (IsNull(arr)) {
			result_validity.SetInvalid(i);
			continue;
		}

		if (arr->type != JSONValueType::ARRAY) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(*arr),
				                       JSONCommon::ValToString(*arr, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		idx_t json_arr_size = arr->size;
		if (json_arr_size != array_size) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected array of size %u, but got '%s' with size %u", array_size,
				                       JSONCommon::ValToString(*arr, 50), json_arr_size);
				options.object_index = i;
				success = false;
			}
			continue;
		}
	}

	// Initialize array for the nested values
	auto nested_vals = AllocateScratch<const JSONNode *>(scratch, child_count);

	// Get array values
	idx_t nested_elem_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(i)) {
			// We already marked this as invalid, but we still need to increment nested_elem_idx
			// and set the nullptrs (otherwise indexing will break after compaction)
			for (idx_t j = 0; j < array_size; j++) {
				nested_vals[nested_elem_idx] = nullptr;
				nested_elem_idx++;
			};
		} else {
			for (auto &val : arrays[i]->Elements()) {
				nested_vals[nested_elem_idx] = &val;
				nested_elem_idx++;
			}
		}
	}

	success = TransformElements(nested_vals, child_count, scratch, ArrayVector::GetChildMutable(result), options,
	                            !success, [&](idx_t element) { return element / array_size; });

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformObjectToMap(const JSONNode *objects[], JSONTransformScratch &scratch, Vector &result,
                                 const idx_t count, JSONTransformOptions &options) {
	// Pre-allocate list vector
	idx_t list_size = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (!obj || obj->type != JSONValueType::OBJECT) {
			continue;
		}
		list_size += obj->size;
	}
	ListVector::Reserve(result, list_size);
	ListVector::SetListSize(result, list_size);

	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &list_validity = FlatVector::ValidityMutable(result);

	auto keys = AllocateScratch<const JSONNode *>(scratch, list_size);
	auto vals = AllocateScratch<const JSONNode *>(scratch, list_size);

	bool success = true;
	idx_t list_offset = 0;

	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (IsNull(obj)) {
			list_validity.SetInvalid(i);
			continue;
		}

		if (obj->type != JSONValueType::OBJECT) {
			list_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected OBJECT, but got %s: %s", JSONCommon::ValTypeToString(*obj),
				                       JSONCommon::ValToString(*obj, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto &list_entry = list_entries[i];
		list_entry.offset = list_offset;
		list_entry.length = obj->size;

		for (auto &key : obj->Keys()) {
			keys[list_offset] = &key;
			vals[list_offset] = &key + 1;
			list_offset++;
		}
	}
	D_ASSERT(list_offset == list_size);

	// Transform keys
	if (!JSONTransform::Transform(keys, scratch, MapVector::GetKeys(result), list_size, options, nullptr)) {
		throw ConversionException(
		    StringUtil::Format("%s. Cannot default to NULL, because map keys cannot be NULL", options.error_message));
	}

	success = TransformElements(vals, list_size, scratch, MapVector::GetValues(result), options, !success,
	                            [&](idx_t element) { return RowOfElement(list_entries, list_validity, element); });

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformToJSON(const JSONNode *vals[], JSONTransformScratch &scratch, Vector &result, const idx_t count) {
	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNull(val)) {
			validity.SetInvalid(i);
		} else {
			data[i] = JSONCommon::WriteVal(*val, scratch.writer, result);
		}
	}
	// Can always transform to JSON
	return true;
}

static bool TransformValueIntoUnion(const JSONNode **vals, JSONTransformScratch &scratch, Vector &result,
                                    const idx_t count, JSONTransformOptions &options) {
	auto type = result.GetType();

	auto fields = UnionType::CopyMemberTypes(type);
	vector<string> names;
	for (const auto &field : fields) {
		names.emplace_back(field.first);
	}

	bool success = true;

	auto &validity = FlatVector::ValidityMutable(result);

	auto set_error = [&](idx_t i, const string &message) {
		validity.SetInvalid(i);
		result.SetValue(i, Value(nullptr));
		if (success && options.strict_cast) {
			options.error_message = message;
			options.object_index = i;
			success = false;
		}
	};

	for (idx_t i = 0; i < count; i++) {
		const auto &obj = vals[i];

		if (IsNull(obj)) {
			validity.SetInvalid(i);
			result.SetValue(i, Value(nullptr));
			continue;
		}

		if (obj->type != JSONValueType::OBJECT) {
			set_error(i,
			          StringUtil::Format("Expected an object representing a union, got %s", JSONTypeDescription(*obj)));
			continue;
		}

		auto len = obj->size;
		if (len > 1) {
			set_error(i, "Found object containing more than one key, instead of union");
			continue;
		} else if (len == 0) {
			set_error(i, "Found empty object, instead of union");
			continue;
		}

		auto key = obj->First();
		const JSONNode *val = key + 1;
		const string key_str(key->GetString());

		auto tag = std::find(names.begin(), names.end(), key_str);
		if (tag == names.end()) {
			set_error(i, StringUtil::Format("Found object containing unknown key, instead of union: %s", key_str));
			continue;
		}

		idx_t actual_tag = tag - names.begin();

		Vector single(UnionType::GetMemberType(type, actual_tag), 1);
		if (!JSONTransform::Transform(&val, scratch, single, 1, options, nullptr)) {
			success = false;
		}

		result.SetValue(i, Value::UNION(fields, actual_tag, single.GetValue(0)));
	}

	return success;
}

bool JSONTransform::Transform(const JSONNode *vals[], JSONTransformScratch &scratch, Vector &result, const idx_t count,
                              JSONTransformOptions &options, optional_ptr<const ColumnIndex> column_index) {
	auto result_type = result.GetType();
	if (UsesDateFormats(result_type, options)) {
		return TransformFromStringWithFormat(vals, result, count, options);
	}

	if (result_type.IsJSONType()) {
		return TransformToJSON(vals, scratch, result, count);
	}
	if (options.rendering != ValueRendering::NATIVE && options.parameters.context &&
	    result_type.id() != LogicalTypeId::VARCHAR && !result_type.IsNested()) {
		Vector text(LogicalTypeId::VARCHAR, count);
		TransformToString(vals, scratch, text, count);
		return CastValues(text, result, count, options, true);
	}
	if (result_type.HasAlias()) {
		return TransformAliased(vals, scratch, result, count, options);
	}

	switch (result_type.id()) {
	case LogicalTypeId::SQLNULL:
		FlatVector::ValidityMutable(result).SetAllInvalid(count);
		return true;
	case LogicalTypeId::BOOLEAN:
		return TransformNumerical<bool>(vals, result, count, options);
	case LogicalTypeId::TINYINT:
		return TransformNumerical<int8_t>(vals, result, count, options);
	case LogicalTypeId::SMALLINT:
		return TransformNumerical<int16_t>(vals, result, count, options);
	case LogicalTypeId::INTEGER:
		return TransformNumerical<int32_t>(vals, result, count, options);
	case LogicalTypeId::BIGINT:
		return TransformNumerical<int64_t>(vals, result, count, options);
	case LogicalTypeId::UTINYINT:
		return TransformNumerical<uint8_t>(vals, result, count, options);
	case LogicalTypeId::USMALLINT:
		return TransformNumerical<uint16_t>(vals, result, count, options);
	case LogicalTypeId::UINTEGER:
		return TransformNumerical<uint32_t>(vals, result, count, options);
	case LogicalTypeId::UBIGINT:
		return TransformNumerical<uint64_t>(vals, result, count, options);
	case LogicalTypeId::HUGEINT:
		return TransformNumerical<hugeint_t>(vals, result, count, options);
	case LogicalTypeId::UHUGEINT:
		return TransformNumerical<uhugeint_t>(vals, result, count, options);
	case LogicalTypeId::FLOAT:
		return TransformNumerical<float>(vals, result, count, options);
	case LogicalTypeId::DOUBLE:
		return TransformNumerical<double>(vals, result, count, options);
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(result_type);
		auto scale = DecimalType::GetScale(result_type);
		switch (result_type.InternalType()) {
		case PhysicalType::INT16:
			return TransformDecimal<int16_t>(vals, result, count, width, scale, options);
		case PhysicalType::INT32:
			return TransformDecimal<int32_t>(vals, result, count, width, scale, options);
		case PhysicalType::INT64:
			return TransformDecimal<int64_t>(vals, result, count, width, scale, options);
		case PhysicalType::INT128:
			return TransformDecimal<hugeint_t>(vals, result, count, width, scale, options);
		default:
			throw InternalException("Unimplemented physical type for decimal");
		}
	}
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BIT:
		return TransformFromString(vals, result, count, options);
	case LogicalTypeId::GEOMETRY:
		return TransformGeometry(vals, result, count, options);
	case LogicalTypeId::BIGNUM: {
		Vector strings(LogicalTypeId::VARCHAR, count);
		TransformToString(vals, scratch, strings, count);
		return CastValues(strings, result, count, options, true);
	}
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		return TransformToString(vals, scratch, result, count);
	case LogicalTypeId::STRUCT:
		return TransformObjectInternal(vals, scratch, result, count, options, column_index);
	case LogicalTypeId::TUPLE:
		return TransformArrayToTuple(vals, scratch, result, count, options);
	case LogicalTypeId::LIST:
		return TransformArrayToList(vals, scratch, result, count, options);
	case LogicalTypeId::MAP:
		return TransformObjectToMap(vals, scratch, result, count, options);
	case LogicalTypeId::UNION:
		return TransformValueIntoUnion(vals, scratch, result, count, options);
	case LogicalTypeId::ARRAY:
		return TransformArrayToArray(vals, scratch, result, count, options);
	default:
		throw NotImplementedException("Cannot read a value of type %s from a json file", result_type.ToString());
	}
}

static bool TransformFunctionInternal(const Vector &input, const idx_t count, Vector &result,
                                      JSONFunctionLocalState &lstate, JSONTransformOptions &options) {
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	// Read documents: every document of the chunk stays alive until the transform is done
	JSONTransformScratch scratch {lstate.arena, lstate.writer};
	auto roots = AllocateScratch<idx_t>(scratch, count);
	auto vals = AllocateScratch<const JSONNode *>(scratch, count);
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto &parser = lstate.parser;
	if (TypeVisitor::Contains(result.GetType(), LogicalTypeId::DECIMAL) ||
	    TypeVisitor::Contains(result.GetType(), LogicalTypeId::BIGNUM)) {
		parser.SetFlags(JSONReadFlags::ALLOW_INF_AND_NAN | JSONReadFlags::ALLOW_TRAILING_COMMAS |
		                JSONReadFlags::NUMBER_AS_RAW);
	} else {
		parser.SetFlags(JSONCommon::READ_FLAG);
	}
	parser.Clear();
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			roots[i] = DConstants::INVALID_INDEX;
			result_validity.SetInvalid(i);
			continue;
		}
		JSONParseError error;
		auto root = parser.ParseAppend(inputs[idx].GetData(), inputs[idx].GetSize(), error);
		if (!root.IsValid()) {
			JSONCommon::ThrowParseError(inputs[idx].GetData(), inputs[idx].GetSize(), error);
		}
		roots[i] = root.GetIndex();
	}
	for (idx_t i = 0; i < count; i++) {
		vals[i] = roots[i] == DConstants::INVALID_INDEX ? nullptr : parser.Node(roots[i]);
	}
	return JSONTransform::Transform(vals, scratch, result, count, options, nullptr);
}

template <bool strict>
static void TransformFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);

	JSONTransformOptions options(strict, strict, strict, false);
	options.parameters.context = &state.GetContext();
	if (!TransformFunctionInternal(args.data[0], args.size(), result, lstate, options)) {
		throw InvalidInputException(options.error_message);
	}
}

static void GetTransformFunctionInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction fun({}, LogicalType::ANY, TransformFunction<false>, JSONTransformBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", input_type).AddParameter("structure", LogicalType::VARCHAR);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetTransformFunction() {
	ScalarFunctionSet set("json_transform");
	GetTransformFunctionInternal(set, LogicalType::VARCHAR);
	GetTransformFunctionInternal(set, LogicalType::JSON());
	set.SetFallible();
	return set;
}

static void GetTransformStrictFunctionInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction fun({}, LogicalType::ANY, TransformFunction<true>, JSONTransformBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", input_type).AddParameter("structure", LogicalType::VARCHAR);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetTransformStrictFunction() {
	ScalarFunctionSet set("json_transform_strict");
	GetTransformStrictFunctionInternal(set, LogicalType::VARCHAR);
	GetTransformStrictFunctionInternal(set, LogicalType::JSON());
	set.SetFallible();
	return set;
}

static bool JSONToAnyCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &lstate = parameters.local_state->Cast<JSONFunctionLocalState>();
	lstate.arena.Reset();

	JSONTransformOptions options(true, true, true, true);
	options.delay_error = true;
	options.parameters.context = parameters.context;

	auto success = TransformFunctionInternal(source, count, result, lstate, options);
	if (!success) {
		HandleCastError::AssignError(options.error_message, parameters);
	}
	return success;
}

static BoundCastInfo JSONToAnyCastBind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	return BoundCastInfo(JSONToAnyCast, nullptr, JSONFunctionLocalState::InitCastLocalState);
}

void JSONFunctions::RegisterJSONTransformCastFunctions(ExtensionLoader &loader) {
	// JSON can be cast to anything
	for (const auto &type : LogicalType::AllTypes()) {
		LogicalType target_type;
		switch (type.id()) {
		case LogicalTypeId::STRUCT:
			target_type = LogicalType::STRUCT({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::TUPLE:
			target_type = LogicalType::TUPLE({LogicalType::ANY});
			break;
		case LogicalTypeId::LIST:
			target_type = LogicalType::LIST(LogicalType::ANY);
			break;
		case LogicalTypeId::MAP:
			target_type = LogicalType::MAP(LogicalType::ANY, LogicalType::ANY);
			break;
		case LogicalTypeId::UNION:
			target_type = LogicalType::UNION({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::ARRAY:
			target_type = LogicalType::ARRAY(LogicalType::ANY, optional_idx());
			break;
		case LogicalTypeId::VARCHAR:
			// We skip this one here as it's handled in json_functions.cpp
			continue;
		default:
			target_type = type;
		}
		// Going from JSON to another type has the same cost as going from VARCHAR to that type
		const auto json_to_target_cost =
		    CastFunctionSet::ImplicitCastCost(loader.GetDatabaseInstance(), LogicalType::VARCHAR, target_type);
		loader.RegisterCastFunction(LogicalType::JSON(), target_type, JSONToAnyCastBind, json_to_target_cost);
	}
}

} // namespace duckdb
