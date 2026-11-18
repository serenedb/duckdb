
#include "parquet_geometry.hpp"

#include <unordered_set>
#include <utility>
#include <vector>

#include "column_reader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/scalar/geometry_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "reader/expression_column_reader.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "reader/string_column_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/planner/expression.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"

namespace duckdb {

//------------------------------------------------------------------------------
// GeoParquetFileMetadata
//------------------------------------------------------------------------------
unique_ptr<GeoParquetFileMetadata> GeoParquetFileMetadata::TryRead(const duckdb_parquet::FileMetaData &file_meta_data,
                                                                   ClientContext &context) {
	// Conversion not enabled, or spatial is not loaded!
	if (!IsGeoParquetConversionEnabled(context)) {
		return nullptr;
	}

	for (auto &kv : file_meta_data.key_value_metadata) {
		if (kv.key == "geo") {
			JSONTreeParser parser;
			JSONParseError error;
			const auto root = parser.Parse(kv.value.c_str(), kv.value.size(), error);
			if (!root) {
				// Could not parse the JSON
				return nullptr;
			}

			// Check the root object
			if (root->type != JSONValueType::OBJECT) {
				throw InvalidInputException("Geoparquet metadata is not an object");
			}

			// We dont actually care about the version for now, as we only support V1+native
			auto result = make_uniq<GeoParquetFileMetadata>(GeoParquetVersion::BOTH);

			// Check and parse the version
			const auto version_val = root->Get("version");
			if (!version_val || version_val->type != JSONValueType::STRING) {
				throw InvalidInputException("Geoparquet metadata does not have a version");
			}

			const auto version = version_val->GetString();
			if (StringUtil::StartsWith(version, "3")) {
				// Guard against a breaking future 3.0 version
				throw InvalidInputException("Geoparquet version %s is not supported", version);
			}

			// Check and parse the geometry columns
			const auto columns_val = root->Get("columns");
			if (!columns_val || columns_val->type != JSONValueType::OBJECT) {
				throw InvalidInputException("Geoparquet metadata does not have a columns object");
			}

			// Iterate over all geometry columns
			JSONStringWriter crs_writer;
			for (auto &column_key : columns_val->Keys()) {
				const auto &column_val = *(&column_key + 1);
				const auto column_name = string(column_key.GetString());

				auto &column = result->geometry_columns[column_name];

				if (column_val.type != JSONValueType::OBJECT) {
					throw InvalidInputException("Geoparquet column '%s' is not an object", column_name);
				}

				// Parse the encoding
				const auto encoding_val = column_val.Get("encoding");
				if (!encoding_val || encoding_val->type != JSONValueType::STRING) {
					throw InvalidInputException("Geoparquet column '%s' does not have an encoding", column_name);
				}
				const auto encoding_str = encoding_val->GetString();
				if (encoding_str == "WKB") {
					column.geometry_encoding = GeoParquetColumnEncoding::WKB;
				} else if (encoding_str == "point") {
					column.geometry_encoding = GeoParquetColumnEncoding::POINT;
				} else if (encoding_str == "linestring") {
					column.geometry_encoding = GeoParquetColumnEncoding::LINESTRING;
				} else if (encoding_str == "polygon") {
					column.geometry_encoding = GeoParquetColumnEncoding::POLYGON;
				} else if (encoding_str == "multipoint") {
					column.geometry_encoding = GeoParquetColumnEncoding::MULTIPOINT;
				} else if (encoding_str == "multilinestring") {
					column.geometry_encoding = GeoParquetColumnEncoding::MULTILINESTRING;
				} else if (encoding_str == "multipolygon") {
					column.geometry_encoding = GeoParquetColumnEncoding::MULTIPOLYGON;
				} else {
					throw InvalidInputException("Geoparquet column '%s' has an unsupported encoding", column_name);
				}

				// Parse the geometry types
				const auto geometry_types_val = column_val.Get("geometry_types");
				if (!geometry_types_val || geometry_types_val->type != JSONValueType::ARRAY) {
					throw InvalidInputException("Geoparquet column '%s' does not have geometry types", column_name);
				}

				// Parse the CRS
				const auto crs_val = column_val.Get("crs");
				if (crs_val && crs_val->type != JSONValueType::JSON_NULL) {
					// Parse the CRS
					if (crs_val->type != JSONValueType::OBJECT) {
						throw InvalidInputException("Geoparquet column '%s' has invalid CRS", column_name);
					}

					// Move the CRS as a JSON string
					crs_writer.Clear();
					crs_writer.Value(*crs_val);
					column.projjson = crs_writer.ToString();
				} else if (crs_val && crs_val->type == JSONValueType::JSON_NULL) {
					// If CRS is null, do nothing
				} else {
					// Otherwise, if no CRS, default to OGC:CRS84
					auto crs = CoordinateReferenceSystem::TryConvert(context, "OGC:CRS84",
					                                                 CoordinateReferenceSystemType::PROJJSON);
					if (crs) {
						column.projjson = crs->GetDefinition();
					}
				}

				// TODO: Parse the bounding box, other metadata that might be useful.
				// (Only encoding and geometry types are required to be present)
			}

			// Return the result
			return result;
		}
	}
	return nullptr;
}

static void ConvertCRS(ClientContext &context, GeoParquetColumnMetadata &column, const string &column_name,
                       const CoordinateReferenceSystem &crs, GeoParquetVersion version) {
	// This is the default in GeoParquet
	if (StringUtil::CIEquals("OGC:CRS84", crs.GetIdentifier())) {
		// Dont write the default
		return;
	}

	if (crs.GetType() == CoordinateReferenceSystemType::PROJJSON) {
		// If already PROJJSON, just write it directly
		column.projjson = crs.GetDefinition();
		return;
	}

	const auto lookup = CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::PROJJSON);

	if (lookup) {
		// Successfully converted to PROJJSON
		column.projjson = lookup->GetDefinition();
		return;
	}

	if (version != GeoParquetVersion::V2) {
		throw InvalidInputException("Cannot write GeoParquet V1 metadata for column '%s': GeoParquet V1 only "
		                            "supports PROJJSON CRS definitions",
		                            column_name);
	}

	// Fall back to writing the original CRS definition
	column.projjson = crs.GetDefinition();
}

void GeoParquetFileMetadata::AddGeoParquetStats(ClientContext &context, const string &column_name,
                                                const LogicalType &type, const GeometryStatsData &stats,
                                                GeoParquetVersion version) {
	// Lock the metadata
	lock_guard<mutex> glock(write_lock);

	const auto it = geometry_columns.find(column_name);
	if (it == geometry_columns.end()) {
		// Add a new column to the metadata
		auto &column = geometry_columns[column_name];

		// Attempt to convert the CRS to PROJJSON
		if (GeoType::HasCRS(type)) {
			ConvertCRS(context, column, column_name, GeoType::GetCRS(type), version);
		}

		// Merge the stats
		column.stats.Merge(stats);
		column.insertion_index = geometry_columns.size() - 1;

	} else {
		it->second.stats.Merge(stats);
	}
}

void GeoParquetFileMetadata::Write(duckdb_parquet::FileMetaData &file_meta_data) {
	// GeoParquet does not support M or ZM coordinates. So remove any columns that have them.
	unordered_set<string> invalid_columns;
	for (auto &column : geometry_columns) {
		if (column.second.stats.extent.HasM()) {
			invalid_columns.insert(column.first);
		}
	}
	for (auto &col_name : invalid_columns) {
		geometry_columns.erase(col_name);
	}
	// No columns remaining, nothing to write
	if (geometry_columns.empty()) {
		return;
	}

	// Find the primary geometry column
	const auto &random_first_column = *geometry_columns.begin();
	auto primary_geometry_column = random_first_column.first;
	auto primary_insertion_index = random_first_column.second.insertion_index;

	for (auto &column : geometry_columns) {
		if (column.second.insertion_index < primary_insertion_index) {
			primary_insertion_index = column.second.insertion_index;
			primary_geometry_column = column.first;
		}
	}

	JSONStringWriter writer;
	writer.StartObject();

	// Add the version
	switch (version) {
	case GeoParquetVersion::V1:
	case GeoParquetVersion::BOTH:
		writer.Key("version");
		writer.String("1.0.0");
		break;
	case GeoParquetVersion::V2:
		writer.Key("version");
		writer.String("2.0.0");
		break;
	case GeoParquetVersion::NONE:
	default:
		// Should never happen, we should not be writing anything
		throw InternalException("GeoParquetVersion::NONE should not write metadata");
	}

	// Add the primary column
	writer.Key("primary_column");
	writer.String(primary_geometry_column);

	// Add the columns
	writer.Key("columns");
	writer.StartObject();

	JSONTreeParser crs_parser;
	JSONParseError crs_error;
	for (auto &column : geometry_columns) {
		writer.Key(column.first);
		writer.StartObject();
		writer.Key("encoding");
		writer.String("WKB");
		writer.Key("geometry_types");
		writer.StartArray();

		for (auto &type_name : column.second.stats.types.ToString(false)) {
			writer.String(type_name);
		}
		writer.EndArray();

		const auto &bbox = column.second.stats.extent;

		if (bbox.HasXY()) {
			writer.Key("bbox");
			writer.StartArray();

			if (!column.second.stats.extent.HasZ()) {
				writer.Double(bbox.x_min);
				writer.Double(bbox.y_min);
				writer.Double(bbox.x_max);
				writer.Double(bbox.y_max);
			} else {
				writer.Double(bbox.x_min);
				writer.Double(bbox.y_min);
				writer.Double(bbox.z_min);
				writer.Double(bbox.x_max);
				writer.Double(bbox.y_max);
				writer.Double(bbox.z_max);
			}
			writer.EndArray();
		}

		// If the CRS is present, add it
		if (!column.second.projjson.empty()) {
			const auto crs_root =
			    crs_parser.Parse(column.second.projjson.c_str(), column.second.projjson.size(), crs_error);
			if (crs_root) {
				writer.Key("crs");
				writer.Value(*crs_root);
			} else {
				// Failed to parse the CRS JSON. If this is geoparquet v2, we support other CRS formats
				if (version != GeoParquetVersion::V2) {
					// Skip adding the CRS
					throw SerializationException("Failed to parse CRS PROJJSON for column '%s'", column.first.c_str());
				}
			}
		}
		writer.EndObject();
	}
	writer.EndObject();
	writer.EndObject();

	// Create a string from the JSON
	duckdb_parquet::KeyValue kv;
	kv.__set_key("geo");
	kv.__set_value(writer.ToString());

	file_meta_data.key_value_metadata.push_back(kv);
	file_meta_data.__isset.key_value_metadata = true;
}

bool GeoParquetFileMetadata::IsGeometryColumn(const string &column_name) const {
	return geometry_columns.find(column_name) != geometry_columns.end();
}

bool GeoParquetFileMetadata::IsGeoParquetConversionEnabled(const ClientContext &context) {
	bool geoparquet_enabled = false;
	context.TryGetCurrentSetting("enable_geoparquet_conversion", geoparquet_enabled);
	return geoparquet_enabled;
}

const unordered_map<string, GeoParquetColumnMetadata> &GeoParquetFileMetadata::GetColumnMeta() const {
	return geometry_columns;
}

optional_ptr<const GeoParquetColumnMetadata> GeoParquetFileMetadata::GetColumnMeta(const string &column_name) const {
	const auto it = geometry_columns.find(column_name);
	if (it == geometry_columns.end()) {
		return nullptr;
	}
	return &it->second;
}

unique_ptr<ColumnReader> GeometryColumnReader::Create(const ParquetReader &reader, const ParquetColumnSchema &schema,
                                                      ClientContext &context) {
	D_ASSERT(schema.type.id() == LogicalTypeId::GEOMETRY);
	D_ASSERT(schema.children.size() == 1 && schema.children[0].type.id() == LogicalTypeId::BLOB);

	// Make a string reader for the underlying WKB data
	auto string_reader = make_uniq<StringColumnReader>(reader, schema.children[0]);

	// Wrap the string reader in a geometry reader
	auto args = vector<unique_ptr<Expression>>();
	auto ref = make_uniq_base<Expression, BoundReferenceExpression>(LogicalTypeId::BLOB, 0);
	args.push_back(std::move(ref));

	// TODO: Pass the actual target type here so we get the CRS information too
	auto func = StGeomfromwkbFun::GetFunction();
	func.SetName("ST_GeomFromWKB");
	auto read_expr = func.Bind(context, std::move(args));
	auto type_expr = BoundCastExpression::AddDefaultCastToType(std::move(read_expr), schema.type);
	vector<unique_ptr<ColumnReader>> expression_children;
	expression_children.push_back(std::move(string_reader));
	return make_uniq<ExpressionColumnReader>(context, std::move(expression_children), std::move(type_expr), schema);
}

} // namespace duckdb
