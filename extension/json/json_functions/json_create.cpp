#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "json_common.hpp"
#include "duckdb/main/client_context.hpp"
#include "json_functions.hpp"
#include "json_geojson.hpp"

namespace duckdb {

static constexpr const char *JSON_COPY_TO_JSON_INTERNAL_NAME = "__internal_json_copy_to_json";
static constexpr const char *JSON_COPY_TO_GEOJSON_INTERNAL_NAME = "__internal_json_copy_to_geojson";

struct JSONCreateFunctionData : public FunctionData {
public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JSONCreateFunctionData>();
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

//! How a GEOMETRY value is represented in JSON. WKT is the default so that plain JSON output is unchanged;
//! COPY ... TO ... (FORMAT GEOJSON) selects GEOJSON. A global setting could flip the default later.
enum class JSONGeometryFormat : uint8_t { WKT, GEOJSON };

struct JSONCopyFormatOptions {
	JSONGeometryFormat geometry_format = JSONGeometryFormat::WKT;
	optional_ptr<StrfTimeFormat> date_format;
	optional_ptr<StrfTimeFormat> timestamp_format;
	optional_ptr<ClientContext> context;
	optional_ptr<const Expression> timestamptz_format_expression;
	optional_ptr<const Expression> timestamptz_ns_format_expression;
};

static JSONGeometryFormat ParseGeometryFormat(const Value &value) {
	if (value.IsNull()) {
		return JSONGeometryFormat::WKT;
	}
	const auto format = StringUtil::Lower(StringValue::Get(value));
	if (format == "wkt") {
		return JSONGeometryFormat::WKT;
	}
	if (format == "geojson") {
		return JSONGeometryFormat::GEOJSON;
	}
	throw InvalidInputException("Invalid value \"%s\" for json_geometry_format, expected 'wkt' or 'geojson'",
	                            StringValue::Get(value));
}

void JSONFunctions::ValidateGeometryFormat(ClientContext &, SetScope, Value &parameter) {
	ParseGeometryFormat(parameter);
}

//! GEOMETRY is written as WKT unless the json_geometry_format setting says otherwise. COPY ... TO ...
//! (FORMAT GEOJSON) writes GeoJSON regardless, by overriding this afterwards.
static JSONGeometryFormat ClientGeometryFormat(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting("json_geometry_format", value)) {
		return JSONGeometryFormat::WKT;
	}
	return ParseGeometryFormat(value);
}

static JSONCopyFormatOptions ClientFormatOptions(ClientContext &context) {
	JSONCopyFormatOptions result;
	result.context = context;
	result.geometry_format = ClientGeometryFormat(context);
	return result;
}

struct JSONCopyToJSONFunctionData : public FunctionData {
public:
	JSONCopyToJSONFunctionData(bool has_date_format_p, string date_format_string_p, StrfTimeFormat date_format_p,
	                           bool has_timestamp_format_p, string timestamp_format_string_p,
	                           StrfTimeFormat timestamp_format_p,
	                           unique_ptr<Expression> timestamptz_format_expression_p,
	                           unique_ptr<Expression> timestamptz_ns_format_expression_p)
	    : has_date_format(has_date_format_p), date_format_string(std::move(date_format_string_p)),
	      date_format(std::move(date_format_p)), has_timestamp_format(has_timestamp_format_p),
	      timestamp_format_string(std::move(timestamp_format_string_p)),
	      timestamp_format(std::move(timestamp_format_p)),
	      timestamptz_format_expression(std::move(timestamptz_format_expression_p)),
	      timestamptz_ns_format_expression(std::move(timestamptz_ns_format_expression_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JSONCopyToJSONFunctionData>(
		    has_date_format, date_format_string, date_format, has_timestamp_format, timestamp_format_string,
		    timestamp_format, timestamptz_format_expression ? timestamptz_format_expression->Copy() : nullptr,
		    timestamptz_ns_format_expression ? timestamptz_ns_format_expression->Copy() : nullptr);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JSONCopyToJSONFunctionData>();
		return has_date_format == other.has_date_format && has_timestamp_format == other.has_timestamp_format &&
		       date_format_string == other.date_format_string &&
		       timestamp_format_string == other.timestamp_format_string;
	}

	JSONCopyFormatOptions GetFormatOptions(ClientContext &context) {
		JSONCopyFormatOptions result;
		result.geometry_format = ClientGeometryFormat(context);
		result.date_format = has_date_format ? optional_ptr<StrfTimeFormat>(&date_format) : nullptr;
		result.timestamp_format = has_timestamp_format ? optional_ptr<StrfTimeFormat>(&timestamp_format) : nullptr;
		result.context = context;
		result.timestamptz_format_expression = timestamptz_format_expression
		                                           ? optional_ptr<const Expression>(timestamptz_format_expression.get())
		                                           : nullptr;
		result.timestamptz_ns_format_expression =
		    timestamptz_ns_format_expression ? optional_ptr<const Expression>(timestamptz_ns_format_expression.get())
		                                     : nullptr;
		return result;
	}

public:
	bool has_date_format;
	string date_format_string;
	StrfTimeFormat date_format;
	bool has_timestamp_format;
	string timestamp_format_string;
	StrfTimeFormat timestamp_format;
	unique_ptr<Expression> timestamptz_format_expression;
	unique_ptr<Expression> timestamptz_ns_format_expression;
};

//! Bind data for the GeoJSON Feature writer: which payload child is the geometry, which are properties, plus the
//! regular date/timestamp formatting options used for the property values
struct JSONCopyToGeoJSONFunctionData : public FunctionData {
public:
	JSONCopyToGeoJSONFunctionData(unique_ptr<JSONCopyToJSONFunctionData> formats_p, optional_idx geometry_index_p,
	                              optional_idx id_index_p, vector<idx_t> property_indices_p, bool write_bbox_p)
	    : formats(std::move(formats_p)), geometry_index(geometry_index_p), id_index(id_index_p),
	      property_indices(std::move(property_indices_p)), write_bbox(write_bbox_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto formats_copy = unique_ptr_cast<FunctionData, JSONCopyToJSONFunctionData>(formats->Copy());
		return make_uniq<JSONCopyToGeoJSONFunctionData>(std::move(formats_copy), geometry_index, id_index,
		                                                property_indices, write_bbox);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JSONCopyToGeoJSONFunctionData>();
		return geometry_index == other.geometry_index && id_index == other.id_index &&
		       property_indices == other.property_indices && write_bbox == other.write_bbox &&
		       formats->Equals(*other.formats);
	}

public:
	unique_ptr<JSONCopyToJSONFunctionData> formats;
	//! Invalid when there is no geometry column, i.e. every Feature gets a null geometry
	optional_idx geometry_index;
	//! Invalid when no column is written as the Feature-level "id"
	optional_idx id_index;
	vector<idx_t> property_indices;
	//! Whether to generate a "bbox" member from the geometry
	bool write_bbox;
};

static bool HasOwnJSONCast(const CastFunctionSet &casts, const LogicalType &type) {
	return type.HasAlias() && casts.HasExactRegisteredCast(type, LogicalType::JSON());
}

static LogicalType GetJSONType(const CastFunctionSet &casts, const LogicalType &type) {
	if (type.IsJSONType()) {
		return type;
	}
	if (HasOwnJSONCast(casts, type)) {
		return LogicalType::JSON();
	}

	switch (type.id()) {
	// These types can go directly into JSON
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DECIMAL:
	// GEOMETRY is emitted as either WKT or GeoJSON, so it must reach EncodeValues untouched
	case LogicalTypeId::GEOMETRY:
		return type;
	case LogicalTypeId::VARIANT:
		return LogicalType::JSON();
	case LogicalTypeId::LIST:
		return LogicalType::LIST(GetJSONType(casts, ListType::GetChildType(type)));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(GetJSONType(casts, ArrayType::GetChildType(type)), ArrayType::GetSize(type));
	// Struct and MAP are treated as JSON values
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> child_types;
		for (const auto &child_type : StructType::GetChildTypes(type)) {
			child_types.emplace_back(child_type.first, GetJSONType(casts, child_type.second));
		}
		return LogicalType::STRUCT(child_types);
	}
	// A TUPLE is an unnamed struct: it is serialized as a JSON array (matching how Python serializes tuples)
	case LogicalTypeId::TUPLE: {
		vector<LogicalType> child_types;
		for (const auto &child_type : StructType::GetChildTypes(type)) {
			child_types.push_back(GetJSONType(casts, child_type.second));
		}
		return LogicalType::TUPLE(std::move(child_types));
	}
	case LogicalTypeId::MAP: {
		return LogicalType::MAP(MapType::KeyType(type), GetJSONType(casts, MapType::ValueType(type)));
	}
	case LogicalTypeId::UNION: {
		child_list_t<LogicalType> member_types;
		for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(type); member_idx++) {
			auto &member_name = UnionType::GetMemberName(type, member_idx);
			auto &member_type = UnionType::GetMemberType(type, member_idx);
			member_types.emplace_back(member_name, GetJSONType(casts, member_type));
		}
		return LogicalType::UNION(member_types);
	}
	// All other types (e.g. date) are cast to VARCHAR
	default:
		return LogicalTypeId::VARCHAR;
	}
}

static unique_ptr<FunctionData> JSONCreateBindParams(BindScalarFunctionInput &input, bool object) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto &casts = CastFunctionSet::Get(input.GetClientContext());
	auto &bound_arguments = bound_function.GetArguments();
	bound_arguments.clear();
	bound_arguments.reserve(arguments.size());
	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &type = arguments[i]->GetReturnType();
		if (arguments[i]->HasParameter()) {
			throw ParameterNotResolvedException();
		} else if (object && i % 2 == 0) {
			if (type != LogicalType::VARCHAR) {
				throw BinderException("json_object() keys must be VARCHAR, add an explicit cast to argument %s",
				                      arguments[i]->GetName());
			}
			bound_arguments.push_back(LogicalType::VARCHAR);
		} else {
			// Value, cast to types that we can put in JSON
			bound_arguments.push_back(GetJSONType(casts, type));
		}
	}
	return make_uniq<JSONCreateFunctionData>();
}

static unique_ptr<FunctionData> JSONObjectBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.size() % 2 != 0) {
		throw BinderException("json_object() requires an even number of arguments");
	}
	return JSONCreateBindParams(input, true);
}

static unique_ptr<FunctionData> JSONArrayBind(BindScalarFunctionInput &input) {
	return JSONCreateBindParams(input, false);
}

static unique_ptr<FunctionData> ToJSONBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("to_json() takes exactly one argument");
	}
	return JSONCreateBindParams(input, false);
}

static unique_ptr<FunctionData> ArrayToJSONBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("array_to_json() takes exactly one argument");
	}
	auto arg_id = arguments[0]->GetReturnType().id();
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (arg_id != LogicalTypeId::LIST && arg_id != LogicalTypeId::ARRAY && arg_id != LogicalTypeId::SQLNULL) {
		throw BinderException("array_to_json() argument type must be LIST or ARRAY");
	}
	return JSONCreateBindParams(input, false);
}

static unique_ptr<FunctionData> RowToJSONBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("row_to_json() takes exactly one argument");
	}
	auto arg_id = arguments[0]->GetReturnType().id();
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (arg_id == LogicalTypeId::TUPLE) {
		child_list_t<LogicalType> fields;
		for (auto &child : StructType::GetChildTypes(arguments[0]->GetReturnType())) {
			fields.emplace_back(Identifier("f" + to_string(fields.size() + 1)), child.second);
		}
		arguments[0] = BoundCastExpression::AddCastToType(input.GetClientContext(), std::move(arguments[0]),
		                                                  LogicalType::STRUCT(std::move(fields)));
	} else if (arg_id != LogicalTypeId::STRUCT && arg_id != LogicalTypeId::SQLNULL) {
		throw BinderException("row_to_json() argument type must be STRUCT");
	}
	return JSONCreateBindParams(input, false);
}

static bool BindJSONCopyFormat(ClientContext &context, Expression &argument, const string &name, string &format_string,
                               StrfTimeFormat &format) {
	if (argument.HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (!argument.IsFoldable()) {
		throw InvalidInputException(argument, "json_copy_to_json %s format must be a constant", name);
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, argument);
	if (value.IsNull()) {
		return false;
	}
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("json_copy_to_json %s format must be VARCHAR or NULL", name);
	}
	format_string = StringValue::Get(value);
	auto error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
	if (!error.empty()) {
		throw InvalidInputException(argument, "Failed to parse format specifier %s: %s", format_string, error);
	}
	return true;
}

static void ParseJSONCopyFormatString(const string &format_string, const string &name, StrfTimeFormat &format) {
	auto error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
	if (!error.empty()) {
		throw InvalidInputException("Failed to parse %s format specifier %s: %s", name, format_string, error);
	}
}

static unique_ptr<Expression> BindJSONCopyTimestampTZFormatter(ClientContext &context, const string &format_string,
                                                               const LogicalType &type) {
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(type, 0));
	children.push_back(make_uniq<BoundConstantExpression>(Value(format_string)));

	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(Identifier::DefaultSchema(), "strftime", std::move(children));
}

static unique_ptr<JSONCopyToJSONFunctionData>
CreateJSONCopyToJSONFunctionData(ClientContext &context, bool has_date_format, string date_format_string,
                                 bool has_timestamp_format, string timestamp_format_string) {
	StrfTimeFormat date_format;
	StrfTimeFormat timestamp_format;
	if (has_date_format) {
		ParseJSONCopyFormatString(date_format_string, "date", date_format);
	}
	if (has_timestamp_format) {
		ParseJSONCopyFormatString(timestamp_format_string, "timestamp", timestamp_format);
	}

	unique_ptr<Expression> timestamptz_format_expression;
	unique_ptr<Expression> timestamptz_ns_format_expression;
	if (has_timestamp_format) {
		timestamptz_format_expression =
		    BindJSONCopyTimestampTZFormatter(context, timestamp_format_string, LogicalType::TIMESTAMP_TZ);
		timestamptz_ns_format_expression =
		    BindJSONCopyTimestampTZFormatter(context, timestamp_format_string, LogicalType::TIMESTAMP_TZ_NS);
	}

	return make_uniq<JSONCopyToJSONFunctionData>(has_date_format, std::move(date_format_string), std::move(date_format),
	                                             has_timestamp_format, std::move(timestamp_format_string),
	                                             std::move(timestamp_format), std::move(timestamptz_format_expression),
	                                             std::move(timestamptz_ns_format_expression));
}

static unique_ptr<JSONCopyToJSONFunctionData>
CreateJSONCopyToJSONFunctionData(ClientContext &context, Expression &date_argument, Expression &timestamp_argument) {
	StrfTimeFormat date_format;
	StrfTimeFormat timestamp_format;
	string date_format_string;
	string timestamp_format_string;
	auto has_date_format = BindJSONCopyFormat(context, date_argument, "date", date_format_string, date_format);
	auto has_timestamp_format =
	    BindJSONCopyFormat(context, timestamp_argument, "timestamp", timestamp_format_string, timestamp_format);
	return CreateJSONCopyToJSONFunctionData(context, has_date_format, std::move(date_format_string),
	                                        has_timestamp_format, std::move(timestamp_format_string));
}

static unique_ptr<FunctionData> JSONCopyToJSONBind(BindScalarFunctionInput &) {
	throw BinderException("%s is for internal use only", JSON_COPY_TO_JSON_INTERNAL_NAME);
}

static constexpr auto JSON_CREATE_FLAGS = JSONCommon::WRITE_FLAG;
static constexpr idx_t JSON_TEXT_SLACK = 32;
static constexpr char JSON_COMMA[JSON_TEXT_SLACK + 1] = ",";

static inline char *CopyText(char *out, const char *text, idx_t size) {
	if (size <= JSON_TEXT_SLACK) {
		memcpy(out, text, 16);
		memcpy(out + 16, text + 16, 16);
	} else {
		memcpy(out, text, size);
	}
	return out + size;
}

template <idx_t N>
static inline char *AppendText(char *out, const char (&text)[N]) {
	memcpy(out, text, N - 1);
	return out + N - 1;
}

static inline char *WriteNull(char *out) {
	return AppendText(out, "null");
}

static inline char *WriteBoolean(char *out, bool value) {
	return value ? AppendText(out, "true") : AppendText(out, "false");
}

struct JSONTexts {
	const char *data = nullptr;
	const idx_t *offsets = nullptr;

	const char *Begin(idx_t row) const {
		return data + offsets[row];
	}
	idx_t Length(idx_t row) const {
		return offsets[row + 1] - offsets[row];
	}
	idx_t Bytes(idx_t begin, idx_t end) const {
		return offsets[end] - offsets[begin];
	}
};

class JSONTextsWriter {
public:
	JSONTextsWriter(ArenaAllocator &offset_arena, ArenaAllocator &arena_p, idx_t count, idx_t size)
	    : arena(arena_p), capacity(size + JSON_TEXT_SLACK),
	      offsets(reinterpret_cast<idx_t *>(offset_arena.AllocateAligned(sizeof(idx_t) * (count + 1)))),
	      data(char_ptr_cast(arena.Allocate(capacity))) {
		offsets[0] = 0;
	}

	char *Next() {
		return data + used;
	}
	char *Reserve(idx_t size) {
		if (used + size + JSON_TEXT_SLACK > capacity) {
			Grow(size);
		}
		return data + used;
	}
	void EndRow(idx_t row, const char *end) {
		used = static_cast<idx_t>(end - data);
		offsets[row + 1] = used;
	}
	JSONTexts Texts() const {
		return {data, offsets};
	}

private:
	void Grow(idx_t size) {
		const auto new_capacity = MaxValue<idx_t>(capacity * 2, used + size + JSON_TEXT_SLACK);
		data = char_ptr_cast(arena.Reallocate(data_ptr_cast(data), capacity, new_capacity));
		capacity = new_capacity;
	}

	ArenaAllocator &arena;
	idx_t capacity;
	idx_t used = 0;
	idx_t *offsets;
	char *data;
};

struct JSONEncodeContext {
	JSONEncodeContext(JSONFunctionLocalState &lstate, const JSONCopyFormatOptions &options_p)
	    : arena(lstate.arena), parser(lstate.parser), writer(lstate.writer), options(options_p) {
	}

	ArenaAllocator &arena;
	JSONTreeParser &parser;
	JSONStringWriter &writer;
	const JSONCopyFormatOptions &options;
};

struct JSONMember {
	std::string_view key;
	JSONTexts texts;
};

static JSONTexts EncodeValues(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena);

static JSONTexts EncodeNulls(JSONEncodeContext &ctx, idx_t count, ArenaAllocator &text_arena) {
	JSONTextsWriter texts(ctx.arena, text_arena, count, count * 4);
	for (idx_t i = 0; i < count; i++) {
		texts.EndRow(i, WriteNull(texts.Next()));
	}
	return texts.Texts();
}

template <class T, class WRITE>
static JSONTexts EncodeScalars(JSONEncodeContext &ctx, const Vector &value_v, idx_t count, ArenaAllocator &text_arena,
                               idx_t max_size, WRITE &&write) {
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	const auto values = UnifiedVectorFormat::GetData<T>(value_data);
	const auto room = MaxValue<idx_t>(max_size, 4);
	JSONTextsWriter texts(ctx.arena, text_arena, count, count * room);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = value_data.sel->get_index(i);
		const auto out = texts.Reserve(room);
		texts.EndRow(i, value_data.validity.RowIsValid(idx) ? write(out, values[idx]) : WriteNull(out));
	}
	return texts.Texts();
}

template <class T>
static JSONTexts EncodeSignedIntegers(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                      ArenaAllocator &text_arena) {
	return EncodeScalars<T>(ctx, value_v, count, text_arena, 20,
	                        [](char *out, T value) { return JSONStringWriter::WriteSignedInteger(out, value); });
}

template <class T>
static JSONTexts EncodeUnsignedIntegers(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                        ArenaAllocator &text_arena) {
	return EncodeScalars<T>(ctx, value_v, count, text_arena, 20,
	                        [](char *out, T value) { return JSONStringWriter::WriteUnsignedInteger(out, value); });
}

template <class T>
static JSONTexts EncodeHugeIntegers(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                    ArenaAllocator &text_arena) {
	return EncodeScalars<T>(ctx, value_v, count, text_arena, 41, [](char *out, const T &value) {
		const auto text = value.ToString();
		memcpy(out, text.data(), text.size());
		return out + text.size();
	});
}

template <class T>
static JSONTexts EncodeFloatingPoints(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                      ArenaAllocator &text_arena) {
	return EncodeScalars<T>(
	    ctx, value_v, count, text_arena, JSONStringWriter::MAX_DOUBLE_LENGTH, [](char *out, T value) {
		    return JSONStringWriter::WriteCheckedDouble(out, static_cast<double>(value), JSON_CREATE_FLAGS);
	    });
}

template <class T>
static JSONTexts EncodeDecimals(JSONEncodeContext &ctx, const Vector &value_v, idx_t count, ArenaAllocator &text_arena,
                                uint8_t width, uint8_t scale) {
	const int64_t fixed_min = scale > 4 ? NumericHelper::POWERS_OF_TEN[scale - 4] : 0;
	return EncodeScalars<T>(
	    ctx, value_v, count, text_arena, JSONStringWriter::MAX_DOUBLE_LENGTH, [&](char *out, T value) {
		    if (value != 0 && value > -fixed_min && value < fixed_min) {
			    double real;
			    CastParameters parameters;
			    TryCastFromDecimal::Operation<T, double>(value, real, parameters, width, scale);
			    return JSONStringWriter::WriteCheckedDouble(out, real, JSON_CREATE_FLAGS);
		    }
		    const auto size = UnsafeNumericCast<idx_t>(DecimalToString::DecimalLength<T>(value, width, scale));
		    DecimalToString::FormatDecimal<T>(value, width, scale, out, size);
		    auto end = out + size;
		    if (scale == 0) {
			    return AppendText(end, ".0");
		    }
		    while (end[-1] == '0' && end[-2] != '.') {
			    end--;
		    }
		    return end;
	    });
}

template <class WRITE>
static JSONTexts EncodeStringValues(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                    ArenaAllocator &text_arena, WRITE &&write) {
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	const auto values = UnifiedVectorFormat::GetData<string_t>(value_data);
	JSONTextsWriter texts(ctx.arena, text_arena, count, count * 16);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = value_data.sel->get_index(i);
		if (!value_data.validity.RowIsValid(idx)) {
			texts.EndRow(i, WriteNull(texts.Reserve(4)));
			continue;
		}
		texts.EndRow(i, write(texts, values[idx]));
	}
	return texts.Texts();
}

static char *WriteVerbatim(JSONTextsWriter &texts, std::string_view text) {
	const auto out = texts.Reserve(text.size());
	memcpy(out, text.data(), text.size());
	return out + text.size();
}

static JSONTexts EncodeStrings(JSONEncodeContext &ctx, const Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	return EncodeStringValues(ctx, value_v, count, text_arena, [](JSONTextsWriter &texts, const string_t &value) {
		const auto size = value.GetSize();
		return JSONStringWriter::WriteCheckedString(texts.Reserve(JSONStringWriter::MaxStringLength(size)),
		                                            std::string_view(value.GetData(), size), JSON_CREATE_FLAGS);
	});
}

static JSONTexts EncodeRawStrings(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                  ArenaAllocator &text_arena) {
	return EncodeStringValues(ctx, value_v, count, text_arena, [](JSONTextsWriter &texts, const string_t &value) {
		return WriteVerbatim(texts, std::string_view(value.GetData(), value.GetSize()));
	});
}

static JSONTexts EncodeJSONStrings(JSONEncodeContext &ctx, const Vector &value_v, idx_t count,
                                   ArenaAllocator &text_arena) {
	return EncodeStringValues(ctx, value_v, count, text_arena, [&](JSONTextsWriter &texts, const string_t &value) {
		JSONParseError error;
		if (!ctx.parser.Write(value.GetData(), value.GetSize(), ctx.writer, error)) {
			JSONCommon::ThrowParseError(value.GetData(), value.GetSize(), error);
		}
		return WriteVerbatim(texts, ctx.writer.View());
	});
}

static JSONTexts EncodeGeoJSON(JSONEncodeContext &ctx, const Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	JSONBuilder builder(ctx.arena);
	return EncodeStringValues(ctx, value_v, count, text_arena, [&](JSONTextsWriter &texts, const string_t &value) {
		ctx.writer.Clear();
		ctx.writer.Value(*JSONGeometry::ToGeoJSON(value, builder));
		return WriteVerbatim(texts, ctx.writer.View());
	});
}

static JSONTexts EncodeCastStrings(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	Vector string_vector(LogicalTypeId::VARCHAR, count);
	VectorOperations::DefaultCast(value_v, string_vector, count);
	return EncodeStrings(ctx, string_vector, count, text_arena);
}

template <class FILL>
static JSONTexts EncodeFormatted(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena,
                                 bool has_format, FILL &&fill_strings) {
	if (!has_format) {
		return EncodeCastStrings(ctx, value_v, count, text_arena);
	}
	Vector string_vector(LogicalType::VARCHAR, count);
	fill_strings(string_vector);
	return EncodeStrings(ctx, string_vector, count, text_arena);
}

template <class T, class WRITE>
static JSONTexts EncodeQuoted(JSONEncodeContext &ctx, const Vector &value_v, idx_t count, ArenaAllocator &text_arena,
                              idx_t max_size, WRITE &&write) {
	return EncodeScalars<T>(ctx, value_v, count, text_arena, max_size + 2, [&](char *out, T value) {
		*out = '"';
		out += 1 + write(value, out + 1);
		*out = '"';
		return out + 1;
	});
}

static JSONTexts EncodeDates(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &options = ctx.options;
	if (!options.date_format) {
		return EncodeQuoted<date_t>(ctx, value_v, count, text_arena, DateToStringCast::MAX_LENGTH,
		                            [](date_t value, char *out) { return DateToStringCast::Write(value, out); });
	}
	return EncodeFormatted(ctx, value_v, count, text_arena, true, [&](Vector &string_vector) {
		options.date_format.get_mutable()->ConvertDateVector(value_v, string_vector);
	});
}

static JSONTexts EncodeTimestamps(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &options = ctx.options;
	if (options.timestamp_format && value_v.GetType().id() != LogicalTypeId::TIMESTAMP) {
		Vector timestamp_vector(LogicalType::TIMESTAMP, count);
		VectorOperations::DefaultCast(value_v, timestamp_vector, count);
		return EncodeTimestamps(ctx, timestamp_vector, count, text_arena);
	}
	if (!options.timestamp_format && value_v.GetType().id() == LogicalTypeId::TIMESTAMP) {
		return EncodeQuoted<timestamp_t>(
		    ctx, value_v, count, text_arena, TimestampToStringCast::MAX_LENGTH,
		    [](timestamp_t value, char *out) { return TimestampToStringCast::Write(value, out); });
	}
	return EncodeFormatted(ctx, value_v, count, text_arena, bool(options.timestamp_format), [&](Vector &string_vector) {
		options.timestamp_format.get_mutable()->ConvertTimestampVector(value_v, string_vector);
	});
}

static JSONTexts EncodeTimestampsNS(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &options = ctx.options;
	if (!options.timestamp_format) {
		return EncodeQuoted<timestamp_ns_t>(
		    ctx, value_v, count, text_arena, TimestampToStringCast::MAX_LENGTH,
		    [](timestamp_ns_t value, char *out) { return TimestampToStringCast::Write(value, out); });
	}
	return EncodeFormatted(ctx, value_v, count, text_arena, true, [&](Vector &string_vector) {
		options.timestamp_format.get_mutable()->ConvertTimestampNSVector(value_v, string_vector);
	});
}

static JSONTexts EncodeTimestampsTZ(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &options = ctx.options;
	if (!options.timestamp_format) {
		// no COPY format was given - use the full cast set so that the session time zone is applied
		Vector string_vector(LogicalType::VARCHAR, count);
		if (options.context) {
			VectorOperations::Cast(*options.context.get_mutable(), value_v, string_vector, count, true);
		} else {
			VectorOperations::DefaultCast(value_v, string_vector, count);
		}
		return EncodeStrings(ctx, string_vector, count, text_arena);
	}
	return EncodeFormatted(ctx, value_v, count, text_arena, true, [&](Vector &string_vector) {
		auto format_expression = value_v.GetType().id() == LogicalTypeId::TIMESTAMP_TZ_NS
		                             ? options.timestamptz_ns_format_expression
		                             : options.timestamptz_format_expression;
		if (!options.context || !format_expression) {
			throw InternalException("Missing bound TIMESTAMPTZ formatter for JSON COPY");
		}
		DataChunk input;
		input.InitializeEmpty({value_v.GetType()});
		input.data[0].Reference(value_v);
		input.CheckCardinality(count);
		ExpressionExecutor executor(*options.context.get_mutable(), *format_expression);
		executor.ExecuteExpression(input, string_vector);
	});
}

static JSONTexts EncodeDecimalValues(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &type = value_v.GetType();
	const auto width = DecimalType::GetWidth(type);
	const auto scale = DecimalType::GetScale(type);
	if (width > 15) {
		Vector string_vector(LogicalTypeId::VARCHAR, count);
		VectorOperations::DefaultCast(value_v, string_vector, count);
		return EncodeRawStrings(ctx, string_vector, count, text_arena);
	}
	switch (type.InternalType()) {
	case PhysicalType::INT16:
		return EncodeDecimals<int16_t>(ctx, value_v, count, text_arena, width, scale);
	case PhysicalType::INT32:
		return EncodeDecimals<int32_t>(ctx, value_v, count, text_arena, width, scale);
	case PhysicalType::INT64:
		return EncodeDecimals<int64_t>(ctx, value_v, count, text_arena, width, scale);
	default:
		throw InternalException("Unexpected physical type of a DECIMAL in a JSON create function");
	}
}

static std::string_view EncodeKey(JSONEncodeContext &ctx, const string &name, bool first) {
	const auto key = char_ptr_cast(ctx.arena.Allocate(JSONStringWriter::MaxStringLength(name.size()) + 2));
	auto out = key;
	if (!first) {
		*out++ = ',';
	}
	out = JSONStringWriter::WriteCheckedKey(out, name, JSON_CREATE_FLAGS);
	return std::string_view(key, static_cast<idx_t>(out - key));
}

static JSONTexts AssembleMembers(JSONEncodeContext &ctx, const vector<JSONMember> &members, idx_t count, char open,
                                 char close, optional_ptr<const UnifiedVectorFormat> parent,
                                 ArenaAllocator &text_arena) {
	idx_t size = count * 4;
	for (const auto &member : members) {
		size += count * member.key.size() + member.texts.Bytes(0, count);
	}
	JSONTextsWriter texts(ctx.arena, text_arena, count, size);
	for (idx_t i = 0; i < count; i++) {
		auto out = texts.Next();
		if (parent && !parent->validity.RowIsValid(parent->sel->get_index(i))) {
			texts.EndRow(i, WriteNull(out));
			continue;
		}
		*out++ = open;
		for (const auto &member : members) {
			out = CopyText(out, member.key.data(), member.key.size());
			out = CopyText(out, member.texts.Begin(i), member.texts.Length(i));
		}
		*out++ = close;
		texts.EndRow(i, out);
	}
	return texts.Texts();
}

static JSONTexts EncodeStructs(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	auto &entries = StructVector::GetEntries(value_v);
	vector<JSONMember> members(entries.size());
	for (idx_t m = 0; m < entries.size(); m++) {
		members[m].texts = EncodeValues(ctx, entries[m], count, ctx.arena);
		members[m].key = EncodeKey(ctx, StructType::GetChildName(value_v.GetType(), m).GetIdentifierName(), m == 0);
	}
	UnifiedVectorFormat struct_data;
	value_v.ToUnifiedFormat(struct_data);
	return AssembleMembers(ctx, members, count, '{', '}', struct_data, text_arena);
}

static JSONTexts EncodeTuples(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	auto &entries = StructVector::GetEntries(value_v);
	vector<JSONMember> members(entries.size());
	for (idx_t m = 0; m < entries.size(); m++) {
		members[m].texts = EncodeValues(ctx, entries[m], count, ctx.arena);
		members[m].key = std::string_view(JSON_COMMA, m == 0 ? 0 : 1);
	}
	UnifiedVectorFormat tuple_data;
	value_v.ToUnifiedFormat(tuple_data);
	return AssembleMembers(ctx, members, count, '[', ']', tuple_data, text_arena);
}

template <class ENTRY>
static JSONTexts AssembleLists(JSONEncodeContext &ctx, const JSONTexts &children, const UnifiedVectorFormat &list_data,
                               idx_t count, ArenaAllocator &text_arena, ENTRY &&get_entry) {
	JSONTextsWriter texts(ctx.arena, text_arena, count, count * 4);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(idx)) {
			texts.EndRow(i, WriteNull(texts.Reserve(4)));
			continue;
		}
		const list_entry_t entry = get_entry(idx);
		const auto end = entry.offset + entry.length;
		auto out = texts.Reserve(2 + entry.length + children.Bytes(entry.offset, end));
		*out++ = '[';
		for (idx_t k = entry.offset; k < end; k++) {
			out = CopyText(out, children.Begin(k), children.Length(k));
			*out++ = ',';
		}
		out -= entry.length > 0;
		*out++ = ']';
		texts.EndRow(i, out);
	}
	return texts.Texts();
}

static JSONTexts EncodeLists(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	auto &child_v = ListVector::GetChildMutable(value_v);
	const auto children = EncodeValues(ctx, child_v, ListVector::GetListSize(value_v), ctx.arena);
	UnifiedVectorFormat list_data;
	value_v.ToUnifiedFormat(list_data);
	const auto entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	return AssembleLists(ctx, children, list_data, count, text_arena, [&](idx_t idx) { return entries[idx]; });
}

static JSONTexts EncodeArrays(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	value_v.Flatten();
	auto &child_v = ArrayVector::GetChildMutable(value_v);
	const auto array_size = ArrayType::GetSize(value_v.GetType());
	const auto children = EncodeValues(ctx, child_v, count * array_size, ctx.arena);
	UnifiedVectorFormat array_data;
	value_v.ToUnifiedFormat(array_data);
	return AssembleLists(ctx, children, array_data, count, text_arena,
	                     [&](idx_t idx) { return list_entry_t(idx * array_size, array_size); });
}

static JSONTexts EncodeMapKeys(JSONEncodeContext &ctx, Vector &value_v, idx_t count) {
	switch (value_v.GetType().id()) {
	case LogicalTypeId::DATE:
		return EncodeDates(ctx, value_v, count, ctx.arena);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return EncodeTimestamps(ctx, value_v, count, ctx.arena);
	case LogicalTypeId::TIMESTAMP_NS:
		return EncodeTimestampsNS(ctx, value_v, count, ctx.arena);
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return EncodeTimestampsTZ(ctx, value_v, count, ctx.arena);
	default:
		return EncodeCastStrings(ctx, value_v, count, ctx.arena);
	}
}

static JSONTexts EncodeMaps(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto entry_count = ListVector::GetListSize(value_v);
	const auto keys = EncodeMapKeys(ctx, MapVector::GetKeys(value_v), entry_count);
	const auto values = EncodeValues(ctx, MapVector::GetValues(value_v), entry_count, ctx.arena);
	UnifiedVectorFormat map_data;
	value_v.ToUnifiedFormat(map_data);
	const auto entries = UnifiedVectorFormat::GetData<list_entry_t>(map_data);
	JSONTextsWriter texts(ctx.arena, text_arena, count, count * 4);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = map_data.sel->get_index(i);
		if (!map_data.validity.RowIsValid(idx)) {
			texts.EndRow(i, WriteNull(texts.Reserve(4)));
			continue;
		}
		const auto &entry = entries[idx];
		const auto end = entry.offset + entry.length;
		auto out =
		    texts.Reserve(2 + 2 * entry.length + keys.Bytes(entry.offset, end) + values.Bytes(entry.offset, end));
		*out++ = '{';
		bool first = true;
		for (idx_t k = entry.offset; k < end; k++) {
			const auto key = keys.Begin(k);
			if (*key != '"') {
				continue;
			}
			if (!first) {
				*out++ = ',';
			}
			first = false;
			out = CopyText(out, key, keys.Length(k));
			*out++ = ':';
			out = CopyText(out, values.Begin(k), values.Length(k));
		}
		*out++ = '}';
		texts.EndRow(i, out);
	}
	return texts.Texts();
}

static JSONTexts EncodeUnions(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &type = value_v.GetType();
	vector<JSONMember> members(UnionType::GetMemberCount(type));
	for (idx_t m = 0; m < members.size(); m++) {
		members[m].texts = EncodeValues(ctx, UnionVector::GetMember(value_v, m), count, ctx.arena);
		members[m].key = EncodeKey(ctx, UnionType::GetMemberName(type, m).GetIdentifierName(), true);
	}
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	UnifiedVectorFormat tag_data;
	UnionVector::GetTags(value_v).ToUnifiedFormat(tag_data);
	const auto tags = UnifiedVectorFormat::GetData<union_tag_t>(tag_data);
	JSONTextsWriter texts(ctx.arena, text_arena, count, count * 4);
	for (idx_t i = 0; i < count; i++) {
		if (!value_data.validity.RowIsValid(value_data.sel->get_index(i))) {
			texts.EndRow(i, WriteNull(texts.Reserve(4)));
			continue;
		}
		const auto tag_idx = tag_data.sel->get_index(i);
		if (!tag_data.validity.RowIsValid(tag_idx)) {
			texts.EndRow(i, AppendText(texts.Reserve(2), "{}"));
			continue;
		}
		const auto &member = members[tags[tag_idx]];
		auto out = texts.Reserve(2 + member.key.size() + member.texts.Length(i));
		*out++ = '{';
		out = CopyText(out, member.key.data(), member.key.size());
		out = CopyText(out, member.texts.Begin(i), member.texts.Length(i));
		*out++ = '}';
		texts.EndRow(i, out);
	}
	return texts.Texts();
}

static JSONTexts EncodeValues(JSONEncodeContext &ctx, Vector &value_v, idx_t count, ArenaAllocator &text_arena) {
	const auto &type = value_v.GetType();
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		return EncodeNulls(ctx, count, text_arena);
	case LogicalTypeId::BOOLEAN:
		return EncodeScalars<bool>(ctx, value_v, count, text_arena, 5, WriteBoolean);
	case LogicalTypeId::TINYINT:
		return EncodeSignedIntegers<int8_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::SMALLINT:
		return EncodeSignedIntegers<int16_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::INTEGER:
		return EncodeSignedIntegers<int32_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::BIGINT:
		return EncodeSignedIntegers<int64_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::HUGEINT:
		return EncodeHugeIntegers<hugeint_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::UHUGEINT:
		return EncodeHugeIntegers<uhugeint_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::UTINYINT:
		return EncodeUnsignedIntegers<uint8_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::USMALLINT:
		return EncodeUnsignedIntegers<uint16_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::UINTEGER:
		return EncodeUnsignedIntegers<uint32_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::UBIGINT:
		return EncodeUnsignedIntegers<uint64_t>(ctx, value_v, count, text_arena);
	case LogicalTypeId::FLOAT:
		return EncodeFloatingPoints<float>(ctx, value_v, count, text_arena);
	case LogicalTypeId::DOUBLE:
		return EncodeFloatingPoints<double>(ctx, value_v, count, text_arena);
	case LogicalTypeId::VARCHAR:
		if (type.IsJSONType()) {
			return EncodeJSONStrings(ctx, value_v, count, text_arena);
		}
		return EncodeStrings(ctx, value_v, count, text_arena);
	case LogicalTypeId::STRUCT:
		return EncodeStructs(ctx, value_v, count, text_arena);
	case LogicalTypeId::TUPLE:
		return EncodeTuples(ctx, value_v, count, text_arena);
	case LogicalTypeId::MAP:
		return EncodeMaps(ctx, value_v, count, text_arena);
	case LogicalTypeId::LIST:
		return EncodeLists(ctx, value_v, count, text_arena);
	case LogicalTypeId::UNION:
		return EncodeUnions(ctx, value_v, count, text_arena);
	case LogicalTypeId::ARRAY:
		return EncodeArrays(ctx, value_v, count, text_arena);
	case LogicalTypeId::DATE:
		return EncodeDates(ctx, value_v, count, text_arena);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return EncodeTimestamps(ctx, value_v, count, text_arena);
	case LogicalTypeId::TIMESTAMP_NS:
		return EncodeTimestampsNS(ctx, value_v, count, text_arena);
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return EncodeTimestampsTZ(ctx, value_v, count, text_arena);
	case LogicalTypeId::BIT:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::LEGACY_AGGREGATE_STATE:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::UUID:
		return EncodeCastStrings(ctx, value_v, count, text_arena);
	case LogicalTypeId::GEOMETRY:
		if (ctx.options.geometry_format == JSONGeometryFormat::GEOJSON) {
			return EncodeGeoJSON(ctx, value_v, count, text_arena);
		}
		return EncodeCastStrings(ctx, value_v, count, text_arena);
	case LogicalTypeId::BIGNUM: {
		Vector string_vector(LogicalTypeId::VARCHAR, count);
		VectorOperations::DefaultCast(value_v, string_vector, count);
		return EncodeRawStrings(ctx, string_vector, count, text_arena);
	}
	case LogicalTypeId::VARIANT: {
		Vector json_vector(LogicalType::JSON(), count);
		VectorOperations::DefaultCast(value_v, json_vector, count);
		return EncodeJSONStrings(ctx, json_vector, count, text_arena);
	}
	case LogicalTypeId::DECIMAL:
		return EncodeDecimalValues(ctx, value_v, count, text_arena);
	case LogicalTypeId::INVALID:
	case LogicalTypeId::UNKNOWN:
	case LogicalTypeId::ANY:
	case LogicalTypeId::TEMPLATE:
	case LogicalTypeId::UNBOUND:
	case LogicalTypeId::TYPE:
	case LogicalTypeId::CHAR:
	case LogicalTypeId::STRING_LITERAL:
	case LogicalTypeId::INTEGER_LITERAL:
	case LogicalTypeId::POINTER:
	case LogicalTypeId::VALIDITY:
	case LogicalTypeId::TABLE:
	case LogicalTypeId::LAMBDA:
		break;
	}
	throw InternalException("Unsupported type arrived at JSON create function");
}

static string_t TextString(const JSONTexts &texts, idx_t row) {
	const auto size = texts.Length(row);
	if (size > string_t::MAX_STRING_SIZE) {
		string_t::ThrowTooLarge(size);
	}
	return string_t(texts.Begin(row), UnsafeNumericCast<uint32_t>(size));
}

static void WriteTexts(const JSONTexts &texts, idx_t count, Vector &result) {
	auto objects = FlatVector::GetDataMutable<string_t>(result);
	for (idx_t i = 0; i < count; i++) {
		objects[i] = TextString(texts, i);
	}
}

static void ObjectFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	const auto options = ClientFormatOptions(state.GetContext());
	JSONEncodeContext ctx(lstate, options);

	const idx_t count = args.size();
	const idx_t pair_count = args.data.size() / 2;
	vector<UnifiedVectorFormat> keys(pair_count);
	vector<JSONTexts> values(pair_count);
	for (idx_t p = 0; p < pair_count; p++) {
		args.data[p * 2].ToUnifiedFormat(keys[p]);
		values[p] = EncodeValues(ctx, args.data[p * 2 + 1], count, ctx.arena);
	}
	JSONTextsWriter texts(ctx.arena, StringVector::GetStringAllocator(result), count, count * 2);
	for (idx_t i = 0; i < count; i++) {
		idx_t size = 2;
		for (idx_t p = 0; p < pair_count; p++) {
			const auto key_idx = keys[p].sel->get_index(i);
			if (!keys[p].validity.RowIsValid(key_idx)) {
				throw InvalidInputException("JSON key cannot be NULL");
			}
			const auto key_size = UnifiedVectorFormat::GetData<string_t>(keys[p])[key_idx].GetSize();
			size += JSONStringWriter::MaxStringLength(key_size) + 2 + values[p].Length(i);
		}
		auto out = texts.Reserve(size);
		*out++ = '{';
		for (idx_t p = 0; p < pair_count; p++) {
			if (p > 0) {
				*out++ = ',';
			}
			const auto &key = UnifiedVectorFormat::GetData<string_t>(keys[p])[keys[p].sel->get_index(i)];
			out = JSONStringWriter::WriteCheckedKey(out, std::string_view(key.GetData(), key.GetSize()),
			                                        JSON_CREATE_FLAGS);
			out = CopyText(out, values[p].Begin(i), values[p].Length(i));
		}
		*out++ = '}';
		texts.EndRow(i, out);
	}
	WriteTexts(texts.Texts(), count, result);
}

static void ArrayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	const auto options = ClientFormatOptions(state.GetContext());
	JSONEncodeContext ctx(lstate, options);

	const idx_t count = args.size();
	vector<JSONMember> members(args.data.size());
	for (idx_t m = 0; m < members.size(); m++) {
		members[m].texts = EncodeValues(ctx, args.data[m], count, ctx.arena);
		members[m].key = std::string_view(JSON_COMMA, m == 0 ? 0 : 1);
	}
	const auto texts =
	    AssembleMembers(ctx, members, count, '[', ']', nullptr, StringVector::GetStringAllocator(result));
	WriteTexts(texts, count, result);
}

static void ToJSONFunctionInternal(Vector &input, const idx_t count, Vector &result, JSONFunctionLocalState &lstate,
                                   const JSONCopyFormatOptions &options) {
	const bool constant = input.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const auto rows = constant ? MinValue<idx_t>(count, 1) : count;
	JSONEncodeContext ctx(lstate, options);
	const auto texts = EncodeValues(ctx, input, rows, StringVector::GetStringAllocator(result));

	auto objects = FlatVector::GetDataMutable<string_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	for (idx_t i = 0; i < rows; i++) {
		if (input_data.validity.RowIsValid(input_data.sel->get_index(i))) {
			objects[i] = TextString(texts, i);
		} else {
			result_validity.SetInvalid(i);
		}
	}

	if (constant || count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void ToJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto options = ClientFormatOptions(state.GetContext());

	ToJSONFunctionInternal(args.data[0], args.size(), result, lstate, options);
}

static void JSONCopyToJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<JSONCopyToJSONFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto options = info.GetFormatOptions(state.GetContext());

	ToJSONFunctionInternal(args.data[0], args.size(), result, lstate, options);
}

//===--------------------------------------------------------------------===//
// COPY ... TO ... (FORMAT GEOJSON)
//===--------------------------------------------------------------------===//
//! Generate a "bbox" from each geometry. GeoJSON orders it as all the minimums followed by all the maximums, and
//! includes Z when the geometry has it. Geometries without a finite extent (NULL or empty) get no bbox at all.
static char *WriteGeoJSONBoundingBox(char *out, const UnifiedVectorFormat &geometry_data, idx_t row) {
	const auto idx = geometry_data.sel->get_index(row);
	if (!geometry_data.validity.RowIsValid(idx)) {
		return out;
	}
	auto extent = GeometryExtent::Empty();
	Geometry::GetExtent(UnifiedVectorFormat::GetData<string_t>(geometry_data)[idx], extent);
	if (!extent.HasXY()) {
		return out;
	}
	const auto has_z = extent.HasZ();
	const auto write = [&](double value) {
		out = JSONStringWriter::WriteCheckedDouble(out, value, JSON_CREATE_FLAGS);
	};
	out = AppendText(out, ",\"bbox\":[");
	write(extent.x_min);
	*out++ = ',';
	write(extent.y_min);
	if (has_z) {
		*out++ = ',';
		write(extent.z_min);
	}
	*out++ = ',';
	write(extent.x_max);
	*out++ = ',';
	write(extent.y_max);
	if (has_z) {
		*out++ = ',';
		write(extent.z_max);
	}
	*out++ = ']';
	return out;
}

//! Turns one row into a GeoJSON Feature: the geometry column becomes "geometry", the ID_COLUMN column becomes
//! "id", and every other column becomes a member of "properties".
static void JSONCopyToGeoJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<JSONCopyToGeoJSONFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto options = info.formats->GetFormatOptions(state.GetContext());
	options.geometry_format = JSONGeometryFormat::GEOJSON;
	JSONEncodeContext ctx(lstate, options);

	const auto count = args.size();
	auto &payload = args.data[0];
	auto &payload_type = payload.GetType();
	auto &entries = StructVector::GetEntries(payload);

	JSONTexts ids;
	if (info.id_index.IsValid()) {
		ids = EncodeValues(ctx, entries[info.id_index.GetIndex()], count, ctx.arena);
	}
	JSONTexts geometries;
	UnifiedVectorFormat geometry_data;
	if (info.geometry_index.IsValid()) {
		auto &geometry_v = entries[info.geometry_index.GetIndex()];
		geometries = EncodeValues(ctx, geometry_v, count, ctx.arena);
		geometry_v.ToUnifiedFormat(geometry_data);
	}
	vector<JSONMember> members(info.property_indices.size());
	for (idx_t m = 0; m < members.size(); m++) {
		const auto property_index = info.property_indices[m];
		members[m].texts = EncodeValues(ctx, entries[property_index], count, ctx.arena);
		members[m].key =
		    EncodeKey(ctx, StructType::GetChildName(payload_type, property_index).GetIdentifierName(), m == 0);
	}
	const auto properties = AssembleMembers(ctx, members, count, '{', '}', nullptr, ctx.arena);

	const idx_t bbox_size = info.write_bbox ? 6 * JSONStringWriter::MAX_DOUBLE_LENGTH + 16 : 0;
	JSONTextsWriter texts(ctx.arena, StringVector::GetStringAllocator(result), count, count * 64);
	for (idx_t i = 0; i < count; i++) {
		idx_t size = 64 + bbox_size + properties.Length(i);
		if (info.id_index.IsValid()) {
			size += ids.Length(i);
		}
		if (info.geometry_index.IsValid()) {
			size += geometries.Length(i);
		}
		auto out = AppendText(texts.Reserve(size), "{\"type\":\"Feature\"");
		if (info.id_index.IsValid()) {
			out = AppendText(out, ",\"id\":");
			out = CopyText(out, ids.Begin(i), ids.Length(i));
		}
		out = AppendText(out, ",\"geometry\":");
		if (info.geometry_index.IsValid()) {
			out = CopyText(out, geometries.Begin(i), geometries.Length(i));
			if (info.write_bbox) {
				out = WriteGeoJSONBoundingBox(out, geometry_data, i);
			}
		} else {
			out = WriteNull(out);
		}
		out = AppendText(out, ",\"properties\":");
		out = CopyText(out, properties.Begin(i), properties.Length(i));
		*out++ = '}';
		texts.EndRow(i, out);
	}

	const auto features = texts.Texts();
	auto objects = FlatVector::GetDataMutable<string_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	UnifiedVectorFormat payload_data;
	payload.ToUnifiedFormat(payload_data);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = payload_data.sel->get_index(i);
		if (payload_data.validity.RowIsValid(idx)) {
			objects[i] = TextString(features, i);
		} else {
			result_validity.SetInvalid(i);
		}
	}
	if (payload.GetVectorType() == VectorType::CONSTANT_VECTOR || count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//! The column-selecting options are three-state: a NULL argument means the option was not given (use the default
//! behavior), an empty string means it was explicitly unset, and anything else names a column
static bool TryGetGeoJSONColumnOption(ClientContext &context, const Expression &argument, string &result) {
	const auto value = ExpressionExecutor::EvaluateScalar(context, argument);
	if (value.IsNull()) {
		return false;
	}
	result = StringValue::Get(value);
	return true;
}

//! Look up a column that the user named in a COPY option
static optional_idx FindGeoJSONColumn(const child_list_t<LogicalType> &child_types, const string &name,
                                      const char *option_name) {
	for (idx_t i = 0; i < child_types.size(); i++) {
		if (child_types[i].first == name) {
			return i;
		}
	}
	vector<string> available;
	for (const auto &child_type : child_types) {
		available.push_back(child_type.first.GetIdentifierName());
	}
	throw BinderException("COPY ... TO ... (FORMAT GEOJSON) option %s refers to column \"%s\", which is not being "
	                      "written. Available columns: %s",
	                      option_name, name, StringUtil::Join(available, ", "));
}

static unique_ptr<FunctionData> JSONCopyToGeoJSONBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 6) {
		throw BinderException("%s is for internal use only", JSON_COPY_TO_GEOJSON_INTERNAL_NAME);
	}
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	auto &payload_type = arguments[0]->GetReturnType();
	if (payload_type.id() != LogicalTypeId::STRUCT) {
		throw BinderException("%s is for internal use only", JSON_COPY_TO_GEOJSON_INTERNAL_NAME);
	}
	auto &child_types = StructType::GetChildTypes(payload_type);

	// GEOMETRY_COLUMN selects the feature geometry, defaulting to the first GEOMETRY column
	optional_idx geometry_index;
	string geometry_column;
	if (TryGetGeoJSONColumnOption(input.GetClientContext(), *arguments[3], geometry_column)) {
		if (!geometry_column.empty()) {
			geometry_index = FindGeoJSONColumn(child_types, geometry_column, "GEOMETRY_COLUMN");
			if (child_types[geometry_index.GetIndex()].second.id() != LogicalTypeId::GEOMETRY) {
				throw BinderException("COPY ... TO ... (FORMAT GEOJSON) option GEOMETRY_COLUMN refers to column "
				                      "\"%s\", which has type %s instead of GEOMETRY",
				                      geometry_column, child_types[geometry_index.GetIndex()].second.ToString());
			}
		}
	} else {
		for (idx_t i = 0; i < child_types.size(); i++) {
			if (child_types[i].second.id() == LogicalTypeId::GEOMETRY) {
				geometry_index = i;
				break;
			}
		}
		if (!geometry_index.IsValid()) {
			throw BinderException(
			    "COPY ... TO ... (FORMAT GEOJSON) requires a GEOMETRY column, but none of the columns "
			    "being written has type GEOMETRY."
			    "\n Use GEOMETRY_COLUMN NULL to write features without a geometry instead.");
		}
	}

	// ID_COLUMN selects the Feature-level "id", defaulting to a column named "id" if there is one, so that a
	// GeoJSON file read by DuckDB round-trips without needing the option
	optional_idx id_index;
	string id_column;
	if (TryGetGeoJSONColumnOption(input.GetClientContext(), *arguments[4], id_column)) {
		if (!id_column.empty()) {
			id_index = FindGeoJSONColumn(child_types, id_column, "ID_COLUMN");
		}
	} else {
		for (idx_t i = 0; i < child_types.size(); i++) {
			if (child_types[i].first == "id") {
				id_index = i;
				break;
			}
		}
	}
	if (geometry_index.IsValid() && id_index.IsValid() && geometry_index.GetIndex() == id_index.GetIndex()) {
		throw BinderException("COPY ... TO ... (FORMAT GEOJSON) cannot write the same column as both the geometry "
		                      "and the Feature id");
	}

	// Everything that is not the geometry or the id becomes a property
	vector<idx_t> property_indices;
	for (idx_t i = 0; i < child_types.size(); i++) {
		if ((geometry_index.IsValid() && geometry_index.GetIndex() == i) ||
		    (id_index.IsValid() && id_index.GetIndex() == i)) {
			continue;
		}
		property_indices.push_back(i);
	}

	const auto write_bbox =
	    BooleanValue::Get(ExpressionExecutor::EvaluateScalar(input.GetClientContext(), *arguments[5]));

	auto &bound_arguments = bound_function.GetArguments();
	bound_arguments[0] = GetJSONType(CastFunctionSet::Get(input.GetClientContext()), payload_type);
	bound_function.SetReturnType(LogicalType::JSON());

	auto formats = CreateJSONCopyToJSONFunctionData(input.GetClientContext(), *arguments[1], *arguments[2]);
	return make_uniq<JSONCopyToGeoJSONFunctionData>(std::move(formats), geometry_index, id_index,
	                                                std::move(property_indices), write_bbox);
}

static void JSONCopyToJSONSerialize(Serializer &serializer, optional_ptr<FunctionData> bind_data,
                                    const BoundScalarFunction &) {
	if (!bind_data) {
		throw InternalException("%s is missing bind data during serialization", JSON_COPY_TO_JSON_INTERNAL_NAME);
	}
	auto &data = bind_data->Cast<JSONCopyToJSONFunctionData>();
	serializer.WriteProperty(100, "has_date_format", data.has_date_format);
	serializer.WriteProperty(101, "date_format", data.date_format_string);
	serializer.WriteProperty(102, "has_timestamp_format", data.has_timestamp_format);
	serializer.WriteProperty(103, "timestamp_format", data.timestamp_format_string);
}

static unique_ptr<FunctionData> JSONCopyToJSONDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	auto has_date_format = deserializer.ReadProperty<bool>(100, "has_date_format");
	auto date_format_string = deserializer.ReadProperty<string>(101, "date_format");
	auto has_timestamp_format = deserializer.ReadProperty<bool>(102, "has_timestamp_format");
	auto timestamp_format_string = deserializer.ReadProperty<string>(103, "timestamp_format");

	auto &arguments = function.GetArguments();
	if (arguments.size() != 3) {
		throw InternalException("%s must have exactly three arguments during deserialization",
		                        JSON_COPY_TO_JSON_INTERNAL_NAME);
	}

	auto &context = deserializer.Get<ClientContext &>();
	return CreateJSONCopyToJSONFunctionData(context, has_date_format, std::move(date_format_string),
	                                        has_timestamp_format, std::move(timestamp_format_string));
}

unique_ptr<Expression> JSONFunctions::CreateJSONCopyToJSONExpression(ClientContext &context,
                                                                     unique_ptr<Expression> payload,
                                                                     unique_ptr<Expression> date_format,
                                                                     unique_ptr<Expression> timestamp_format) {
	auto json_type = GetJSONType(CastFunctionSet::Get(context), payload->GetReturnType());
	auto bind_data = CreateJSONCopyToJSONFunctionData(context, *date_format, *timestamp_format);

	payload = BoundCastExpression::AddCastToType(context, std::move(payload), json_type);

	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(payload));
	children.push_back(std::move(date_format));
	children.push_back(std::move(timestamp_format));

	auto function = GetJSONCopyToJSONFunction();
	BoundScalarFunction bound_function(function);
	auto &arguments = bound_function.GetArguments();
	arguments.clear();
	arguments.push_back(std::move(json_type));
	arguments.push_back(LogicalType::VARCHAR);
	arguments.push_back(LogicalType::VARCHAR);
	bound_function.SetReturnType(LogicalType::JSON());

	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(children), std::move(bind_data));
}

ScalarFunctionSet JSONFunctions::GetObjectFunction() {
	ScalarFunction fun("json_object", {}, LogicalType::JSON(), ObjectFunction, JSONObjectBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// throws if a key is NULL
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetArrayFunction() {
	ScalarFunction fun("json_array", {}, LogicalType::JSON(), ArrayFunction, JSONArrayBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetToJSONFunction() {
	ScalarFunction fun("to_json", {}, LogicalType::JSON(), ToJSONFunction, ToJSONBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunction JSONFunctions::GetJSONCopyToJSONFunction() {
	ScalarFunction fun(JSON_COPY_TO_JSON_INTERNAL_NAME, {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   LogicalType::JSON(), JSONCopyToJSONFunction, JSONCopyToJSONBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().GetParameter(0).SetName("value");
	fun.GetSignature().GetParameter(1).SetName("date_format");
	fun.GetSignature().GetParameter(2).SetName("timestamp_format");
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(JSONCopyToJSONSerialize);
	fun.SetDeserializeCallback(JSONCopyToJSONDeserialize);
	return fun;
}

ScalarFunction JSONFunctions::GetJSONCopyToGeoJSONFunction() {
	ScalarFunction fun(JSON_COPY_TO_GEOJSON_INTERNAL_NAME,
	                   {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                    LogicalType::VARCHAR, LogicalType::BOOLEAN},
	                   LogicalType::JSON(), JSONCopyToGeoJSONFunction, JSONCopyToGeoJSONBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().GetParameter(0).SetName("value");
	fun.GetSignature().GetParameter(1).SetName("date_format");
	fun.GetSignature().GetParameter(2).SetName("timestamp_format");
	fun.GetSignature().GetParameter(3).SetName("geometry_column");
	fun.GetSignature().GetParameter(4).SetName("id_column");
	fun.GetSignature().GetParameter(5).SetName("write_bbox");
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunctionSet JSONFunctions::GetArrayToJSONFunction() {
	ScalarFunction fun("array_to_json", {}, LogicalType::JSON(), ToJSONFunction, ArrayToJSONBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.SetFallible();
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetRowToJSONFunction() {
	ScalarFunction fun("row_to_json", {}, LogicalType::JSON(), ToJSONFunction, RowToJSONBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.SetFallible();
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	return ScalarFunctionSet(fun);
}

struct NestedToJSONCastData : public BoundCastData {
public:
	NestedToJSONCastData(optional_ptr<ClientContext> client, LogicalType json_type, BoundCastInfo json_type_cast)
	    : client(client), json_type(std::move(json_type)), json_type_cast(std::move(json_type_cast)) {
	}

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<NestedToJSONCastData>(client, json_type, json_type_cast.Copy());
	}

public:
	optional_ptr<ClientContext> client;
	LogicalType json_type;
	BoundCastInfo json_type_cast;
};

struct NestedToJSONLocalState : public JSONFunctionLocalState {
	using JSONFunctionLocalState::JSONFunctionLocalState;

	unique_ptr<FunctionLocalState> json_type_cast_state;
};

static unique_ptr<FunctionLocalState> InitNestedToJSONLocalState(CastLocalStateParameters &parameters) {
	auto result = parameters.context ? make_uniq<NestedToJSONLocalState>(*parameters.context)
	                                 : make_uniq<NestedToJSONLocalState>(Allocator::DefaultAllocator());
	auto &json_type_cast = parameters.cast_data->Cast<NestedToJSONCastData>().json_type_cast;
	if (json_type_cast.HasFunction() && json_type_cast.HasInitLocalState()) {
		CastLocalStateParameters json_type_parameters(parameters, json_type_cast.GetCastData());
		result->json_type_cast_state = json_type_cast.InitLocalState(json_type_parameters);
	}
	return std::move(result);
}

static bool AnyToJSONCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &lstate = parameters.local_state->Cast<NestedToJSONLocalState>();
	lstate.arena.Reset();
	auto &cast_data = parameters.cast_data->Cast<NestedToJSONCastData>();

	JSONCopyFormatOptions options;
	options.context = cast_data.client;
	if (cast_data.client) {
		options.geometry_format = ClientGeometryFormat(*cast_data.client);
	}
	if (!cast_data.json_type_cast.HasFunction()) {
		ToJSONFunctionInternal(source, count, result, lstate, options);
		return true;
	}
	Vector json_typed(cast_data.json_type, count);
	CastParameters json_type_parameters(parameters, cast_data.json_type_cast.GetCastData(),
	                                    lstate.json_type_cast_state);
	const auto success = cast_data.json_type_cast.Cast(source, json_typed, count, json_type_parameters);
	ToJSONFunctionInternal(json_typed, count, result, lstate, options);
	return success;
}

static BoundCastInfo AnyToJSONCastBind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	auto json_type = GetJSONType(input.function_set, source);
	BoundCastInfo json_type_cast(nullptr);
	if (TypeVisitor::Contains(source,
	                          [&](const LogicalType &type) { return HasOwnJSONCast(input.function_set, type); })) {
		json_type_cast = input.GetCastFunction(source, json_type);
	}
	return BoundCastInfo(
	    AnyToJSONCast, make_uniq<NestedToJSONCastData>(input.context, std::move(json_type), std::move(json_type_cast)),
	    InitNestedToJSONLocalState);
}

void JSONFunctions::RegisterJSONCreateCastFunctions(ExtensionLoader &loader) {
	// Anything can be cast to JSON
	for (const auto &type : LogicalType::AllTypes()) {
		LogicalType source_type;
		switch (type.id()) {
		case LogicalTypeId::STRUCT:
			source_type = LogicalType::STRUCT({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::TUPLE:
			source_type = LogicalType::TUPLE({LogicalType::ANY});
			break;
		case LogicalTypeId::LIST:
			source_type = LogicalType::LIST(LogicalType::ANY);
			break;
		case LogicalTypeId::MAP:
			source_type = LogicalType::MAP(LogicalType::ANY, LogicalType::ANY);
			break;
		case LogicalTypeId::UNION:
			source_type = LogicalType::UNION({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::ARRAY:
			source_type = LogicalType::ARRAY(LogicalType::ANY, optional_idx());
			break;
		case LogicalTypeId::VARCHAR:
			// We skip this one here as it's handled in json_functions.cpp
			continue;
		default:
			source_type = type;
		}
		// We prefer going to JSON over going to VARCHAR if a function can do either
		const auto source_to_json_cost = MaxValue<int64_t>(
		    CastFunctionSet::ImplicitCastCost(loader.GetDatabaseInstance(), source_type, LogicalType::VARCHAR) - 1, 0);
		loader.RegisterCastFunction(source_type, LogicalType::JSON(), AnyToJSONCastBind, source_to_json_cost);
	}
}

} // namespace duckdb
