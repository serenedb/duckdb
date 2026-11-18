#include "json_geojson.hpp"

#include "duckdb/common/bswap.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "json_functions.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// WKB cursors
//===--------------------------------------------------------------------===//
class WKBReader {
public:
	WKBReader(const char *data, const idx_t size) : pos(data), end(data + size) {
	}

	template <class T>
	T Read() {
		if (pos + sizeof(T) > end) {
			throw InvalidInputException("Unexpected end of WKB data while converting to GeoJSON");
		}
		T value;
		memcpy(&value, pos, sizeof(T));
		pos += sizeof(T);
		return BSwapIfBE(value);
	}

private:
	const char *pos;
	const char *end;
};

class WKBWriter {
public:
	template <class T>
	void Write(const T &value) {
		const auto le_value = BSwapIfBE(value);
		const auto ptr = reinterpret_cast<const char *>(&le_value);
		buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
	}

	const vector<char> &GetBuffer() const {
		return buffer;
	}

private:
	vector<char> buffer;
};

//! Header of a single WKB geometry: the byte order byte plus the type/dimension word
struct WKBHeader {
	GeometryType type;
	bool has_z;
	bool has_m;
};

WKBHeader ReadHeader(WKBReader &reader) {
	const auto byte_order = reader.Read<uint8_t>();
	if (byte_order != 1) {
		throw InvalidInputException("Unsupported byte order %d in WKB", byte_order);
	}
	const auto meta = reader.Read<uint32_t>();
	const auto type_id = (meta & 0x0000FFFF) % 1000;
	const auto flag_id = (meta & 0x0000FFFF) / 1000;
	if (type_id < 1 || type_id > 7) {
		throw InvalidInputException("Unsupported geometry type %d in WKB", type_id);
	}
	if (flag_id > 3) {
		throw InvalidInputException("Unsupported geometry flag %d in WKB", flag_id);
	}
	WKBHeader header;
	header.type = static_cast<GeometryType>(type_id);
	header.has_z = (flag_id & 0x01) != 0;
	header.has_m = (flag_id & 0x02) != 0;
	return header;
}

void WriteHeader(WKBWriter &writer, const GeometryType type, const bool has_z) {
	writer.Write<uint8_t>(1);
	writer.Write<uint32_t>(static_cast<uint32_t>(type) + (has_z ? 1000 : 0));
}

//===--------------------------------------------------------------------===//
// GeoJSON type names
//===--------------------------------------------------------------------===//
const char *GeometryTypeToGeoJSONName(const GeometryType type) {
	switch (type) {
	case GeometryType::POINT:
		return "Point";
	case GeometryType::LINESTRING:
		return "LineString";
	case GeometryType::POLYGON:
		return "Polygon";
	case GeometryType::MULTIPOINT:
		return "MultiPoint";
	case GeometryType::MULTILINESTRING:
		return "MultiLineString";
	case GeometryType::MULTIPOLYGON:
		return "MultiPolygon";
	case GeometryType::GEOMETRYCOLLECTION:
		return "GeometryCollection";
	default:
		throw InvalidInputException("Unsupported geometry type in WKB");
	}
}

bool GeoJSONNameToGeometryType(const char *name, const size_t len, GeometryType &result) {
	for (idx_t i = 1; i <= 7; i++) {
		const auto candidate = static_cast<GeometryType>(i);
		const auto candidate_name = GeometryTypeToGeoJSONName(candidate);
		if (strlen(candidate_name) == len && memcmp(candidate_name, name, len) == 0) {
			result = candidate;
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// WKB -> GeoJSON
//===--------------------------------------------------------------------===//
struct Position {
	double x;
	double y;
	double z;
	//! An empty point is stored as all-NaN ordinates, and has no GeoJSON representation
	bool is_empty;
};

Position ReadPosition(WKBReader &reader, const WKBHeader &header) {
	Position result;
	result.x = reader.Read<double>();
	result.y = reader.Read<double>();
	result.z = header.has_z ? reader.Read<double>() : 0;
	if (header.has_m) {
		reader.Read<double>(); // GeoJSON has no M dimension
	}
	result.is_empty = std::isnan(result.x) && std::isnan(result.y);
	return result;
}

void AppendOrdinates(JSONBuilder &builder, JSONBuildValue &arr, const Position &pos, const bool has_z) {
	JSONBuilder::Append(arr, builder.Double(pos.x));
	JSONBuilder::Append(arr, builder.Double(pos.y));
	if (has_z) {
		JSONBuilder::Append(arr, builder.Double(pos.z));
	}
}

//! Read a length-prefixed vertex run, appending one position array per vertex
void ReadVertexArray(WKBReader &reader, const WKBHeader &header, JSONBuilder &builder, JSONBuildValue &arr) {
	const auto vert_count = reader.Read<uint32_t>();
	for (uint32_t i = 0; i < vert_count; i++) {
		const auto pos = ReadPosition(reader, header);
		auto coord = builder.Array();
		AppendOrdinates(builder, *coord, pos, header.has_z);
		JSONBuilder::Append(arr, coord);
	}
}

void ToGeoJSONRecursive(WKBReader &reader, JSONBuilder &builder, JSONBuildValue &obj, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("Geometry exceeds maximum recursion depth of %d", Geometry::MAX_RECURSION_DEPTH);
	}

	const auto header = ReadHeader(reader);
	JSONBuilder::Add(obj, "type", builder.String(GeometryTypeToGeoJSONName(header.type)));

	if (header.type == GeometryType::GEOMETRYCOLLECTION) {
		auto geometries = builder.Array();
		JSONBuilder::Add(obj, "geometries", geometries);
		const auto geom_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < geom_count; i++) {
			auto child = builder.Object();
			ToGeoJSONRecursive(reader, builder, *child, depth + 1);
			JSONBuilder::Append(*geometries, child);
		}
		return;
	}

	auto coords = builder.Array();
	JSONBuilder::Add(obj, "coordinates", coords);

	switch (header.type) {
	case GeometryType::POINT: {
		const auto pos = ReadPosition(reader, header);
		if (!pos.is_empty) {
			AppendOrdinates(builder, *coords, pos, header.has_z);
		}
	} break;
	case GeometryType::LINESTRING:
		ReadVertexArray(reader, header, builder, *coords);
		break;
	case GeometryType::POLYGON: {
		const auto ring_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < ring_count; i++) {
			auto ring = builder.Array();
			ReadVertexArray(reader, header, builder, *ring);
			JSONBuilder::Append(*coords, ring);
		}
	} break;
	case GeometryType::MULTIPOINT: {
		// GeoJSON stores MultiPoint coordinates as a flat array of positions
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			const auto pos = ReadPosition(reader, part);
			if (pos.is_empty) {
				continue;
			}
			auto coord = builder.Array();
			AppendOrdinates(builder, *coord, pos, part.has_z);
			JSONBuilder::Append(*coords, coord);
		}
	} break;
	case GeometryType::MULTILINESTRING: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			auto line = builder.Array();
			ReadVertexArray(reader, part, builder, *line);
			JSONBuilder::Append(*coords, line);
		}
	} break;
	case GeometryType::MULTIPOLYGON: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			auto poly = builder.Array();
			const auto ring_count = reader.Read<uint32_t>();
			for (uint32_t j = 0; j < ring_count; j++) {
				auto ring = builder.Array();
				ReadVertexArray(reader, part, builder, *ring);
				JSONBuilder::Append(*poly, ring);
			}
			JSONBuilder::Append(*coords, poly);
		}
	} break;
	default:
		throw InvalidInputException("Unsupported geometry type in WKB");
	}
}

//===--------------------------------------------------------------------===//
// GeoJSON -> WKB
//===--------------------------------------------------------------------===//
//! Whether any position in this coordinate tree carries a third ordinate
bool CoordinatesHaveZ(const JSONNode *coords, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH || !coords || coords->type != JSONValueType::ARRAY) {
		return false;
	}
	for (auto &child : coords->Elements()) {
		if (child.IsNumber()) {
			// This array is a position rather than a nesting level
			return coords->size >= 3;
		}
		if (CoordinatesHaveZ(&child, depth + 1)) {
			return true;
		}
	}
	return false;
}

//! GeoJSON allows mixing 2D and 3D positions, but a geometry must have uniform dimensions, so a single
//! 3D position promotes the whole geometry to XYZ (missing ordinates are padded with 0)
bool GeometryHasZ(const JSONNode *val, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH || val->type != JSONValueType::OBJECT) {
		return false;
	}
	auto geometries = val->Get("geometries");
	if (geometries && geometries->type == JSONValueType::ARRAY) {
		for (auto &child : geometries->Elements()) {
			if (GeometryHasZ(&child, depth + 1)) {
				return true;
			}
		}
		return false;
	}
	return CoordinatesHaveZ(val->Get("coordinates"), depth);
}

void WritePosition(WKBWriter &writer, const JSONNode &pos, const bool has_z, const char *context) {
	if (pos.type != JSONValueType::ARRAY || pos.size < 2) {
		throw InvalidInputException("Invalid GeoJSON: %s position must be an array of at least 2 numbers", context);
	}
	const auto dims = has_z ? 3u : 2u;
	double ordinates[3] = {0, 0, 0};
	idx_t idx = 0;
	for (auto &child : pos.Elements()) {
		if (idx >= dims) {
			break;
		}
		if (!child.IsNumber()) {
			throw InvalidInputException("Invalid GeoJSON: %s position must contain only numbers", context);
		}
		ordinates[idx++] = child.GetNumber();
	}
	writer.Write(ordinates[0]);
	writer.Write(ordinates[1]);
	if (has_z) {
		writer.Write(ordinates[2]);
	}
}

void WriteVertexArray(WKBWriter &writer, const JSONNode &arr, const bool has_z, const char *context) {
	if (arr.type != JSONValueType::ARRAY) {
		throw InvalidInputException("Invalid GeoJSON: %s coordinates are nested too shallowly", context);
	}
	writer.Write<uint32_t>(arr.size);
	for (auto &child : arr.Elements()) {
		WritePosition(writer, child, has_z, context);
	}
}

void FromGeoJSONRecursive(const JSONNode &val, WKBWriter &writer, const bool has_z, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("GeoJSON geometry exceeds maximum recursion depth of %d",
		                            Geometry::MAX_RECURSION_DEPTH);
	}
	if (val.type != JSONValueType::OBJECT) {
		throw InvalidInputException("Invalid GeoJSON: geometry must be an object");
	}
	auto type_val = val.Get("type");
	if (!type_val || type_val->type != JSONValueType::STRING) {
		throw InvalidInputException("Invalid GeoJSON: geometry is missing a \"type\" string");
	}
	GeometryType type;
	if (!GeoJSONNameToGeometryType(type_val->str, type_val->size, type)) {
		throw InvalidInputException("Invalid GeoJSON: unknown geometry type \"%s\"", string(type_val->GetString()));
	}
	const auto context = GeometryTypeToGeoJSONName(type);
	WriteHeader(writer, type, has_z);

	if (type == GeometryType::GEOMETRYCOLLECTION) {
		auto geometries = val.Get("geometries");
		if (!geometries || geometries->type != JSONValueType::ARRAY) {
			throw InvalidInputException("Invalid GeoJSON: GeometryCollection requires a \"geometries\" array");
		}
		writer.Write<uint32_t>(geometries->size);
		for (auto &child : geometries->Elements()) {
			FromGeoJSONRecursive(child, writer, has_z, depth + 1);
		}
		return;
	}

	auto coords = val.Get("coordinates");
	if (!coords || coords->type != JSONValueType::ARRAY) {
		throw InvalidInputException("Invalid GeoJSON: %s requires a \"coordinates\" array", context);
	}

	switch (type) {
	case GeometryType::POINT:
		if (coords->size == 0) {
			// An empty point is stored as all-NaN ordinates
			const auto nan = std::numeric_limits<double>::quiet_NaN();
			writer.Write(nan);
			writer.Write(nan);
			if (has_z) {
				writer.Write(nan);
			}
		} else {
			WritePosition(writer, *coords, has_z, context);
		}
		break;
	case GeometryType::LINESTRING:
		WriteVertexArray(writer, *coords, has_z, context);
		break;
	case GeometryType::POLYGON:
		writer.Write<uint32_t>(coords->size);
		for (auto &child : coords->Elements()) {
			WriteVertexArray(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTIPOINT:
		writer.Write<uint32_t>(coords->size);
		for (auto &child : coords->Elements()) {
			WriteHeader(writer, GeometryType::POINT, has_z);
			WritePosition(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTILINESTRING:
		writer.Write<uint32_t>(coords->size);
		for (auto &child : coords->Elements()) {
			WriteHeader(writer, GeometryType::LINESTRING, has_z);
			WriteVertexArray(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTIPOLYGON:
		writer.Write<uint32_t>(coords->size);
		for (auto &child : coords->Elements()) {
			WriteHeader(writer, GeometryType::POLYGON, has_z);
			if (child.type != JSONValueType::ARRAY) {
				throw InvalidInputException("Invalid GeoJSON: %s coordinates are nested too shallowly", context);
			}
			writer.Write<uint32_t>(child.size);
			for (auto &ring : child.Elements()) {
				WriteVertexArray(writer, ring, has_z, context);
			}
		}
		break;
	default:
		throw InvalidInputException("Invalid GeoJSON: unsupported geometry type \"%s\"", context);
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Public interface
//===--------------------------------------------------------------------===//
JSONBuildValue *JSONGeometry::ToGeoJSON(const string_t &geometry, JSONBuilder &builder) {
	WKBReader reader(geometry.GetData(), geometry.GetSize());
	auto obj = builder.Object();
	ToGeoJSONRecursive(reader, builder, *obj, 0);
	return obj;
}

bool JSONGeometry::FromGeoJSON(const JSONNode *val, string_t &result, Vector &result_vector, string &error) {
	WKBWriter writer;
	try {
		FromGeoJSONRecursive(*val, writer, GeometryHasZ(val, 0), 0);
		const auto &buffer = writer.GetBuffer();
		const string_t wkb(buffer.data(), NumericCast<uint32_t>(buffer.size()));
		// Let the core geometry code validate and normalize what we produced
		return Geometry::FromBinary(wkb, result, StringVector::GetStringHeap(result_vector), true);
	} catch (std::exception &ex) {
		ErrorData error_data(ex);
		error = error_data.RawMessage();
		return false;
	}
}

bool JSONGeometry::IsGeoJSONGeometry(const JSONNode *val) {
	if (val->type != JSONValueType::OBJECT) {
		return false;
	}
	auto type_val = val->Get("type");
	if (!type_val || type_val->type != JSONValueType::STRING) {
		return false;
	}
	GeometryType type;
	if (!GeoJSONNameToGeometryType(type_val->str, type_val->size, type)) {
		return false;
	}
	const auto member = val->Get(type == GeometryType::GEOMETRYCOLLECTION ? "geometries" : "coordinates");
	return member && member->type == JSONValueType::ARRAY;
}

//===--------------------------------------------------------------------===//
// Scalar functions
//===--------------------------------------------------------------------===//
static void AsGeoJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	JSONBuilder builder(lstate.arena);

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &wkb) {
		auto geojson = JSONGeometry::ToGeoJSON(wkb, builder);
		lstate.writer.Clear();
		lstate.writer.Value(*geojson);
		return StringVector::AddString(result, lstate.writer.View());
	});
}

static void GeomFromGeoJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &input) {
		auto &root = JSONCommon::ReadDocument(lstate.parser, input);
		string_t geom;
		string error;
		if (!JSONGeometry::FromGeoJSON(&root, geom, result, error)) {
			throw InvalidInputException(error);
		}
		return geom;
	});
}

ScalarFunctionSet JSONFunctions::GetAsGeoJSONFunction() {
	ScalarFunctionSet set("st_asgeojson");
	ScalarFunction fun({}, LogicalType::JSON(), AsGeoJSONFunction, nullptr, nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("geom", LogicalType::GEOMETRY());
	fun.SetFallible();
	set.AddFunction(fun);
	return set;
}

ScalarFunctionSet JSONFunctions::GetGeomFromGeoJSONFunction() {
	ScalarFunctionSet set("st_geomfromgeojson");
	for (const auto &input_type : vector<LogicalType> {LogicalType::VARCHAR, LogicalType::JSON()}) {
		ScalarFunction fun({}, LogicalType::GEOMETRY(), GeomFromGeoJSONFunction, nullptr, nullptr,
		                   JSONFunctionLocalState::Init);
		fun.GetSignature().AddParameter("geojson", input_type);
		fun.SetFallible();
		set.AddFunction(fun);
	}
	return set;
}

} // namespace duckdb
