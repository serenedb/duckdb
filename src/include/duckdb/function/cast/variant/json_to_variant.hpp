#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

namespace duckdb {
namespace variant {

template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSON(const JSONNode &val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root);

template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSONArray(const JSONNode &arr, ToVariantGlobalResultData &result, idx_t result_index, bool is_root) {
	auto &variant = result.variant;

	auto children_offset_data = OffsetData::GetChildren(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index], nullptr,
	                                 0, VariantLogicalType::ARRAY);

	auto &children_list_entry = variant.children_data[result_index];
	uint32_t count = arr.size;
	auto start_child_index = children_list_entry.offset + children_offset_data[result_index];
	WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset_data[result_index], count,
	                               children_offset_data[result_index]);

	//! Reserve these indices for the array
	children_offset_data[result_index] += count;

	//! Iterate over all the children in the Array
	for (auto &val : arr.Elements()) {
		if (WRITE_DATA) {
			//! Set the child index
			variant.keys_index_validity.SetInvalid(start_child_index);
			variant.values_index_data[start_child_index++] = values_offset_data[result_index];
		}
		if (!ConvertJSON<WRITE_DATA, IGNORE_NULLS>(val, result, result_index, false)) {
			return false;
		}
	}
	return true;
}

template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSONObject(const JSONNode &obj, ToVariantGlobalResultData &result, idx_t result_index,
                              bool is_root) {
	struct JSONObjectEntry {
		const JSONNode *key;
		const JSONNode *value;
	};

	// Maps object keys to their entry index.
	string_map_t<idx_t> key_to_entry;
	vector<JSONObjectEntry> entries;
	key_to_entry.reserve(obj.size);
	entries.reserve(obj.size);

	for (auto &key : obj.Keys()) {
		auto key_string = string_t(key.GetString());
		auto inserted_entry = key_to_entry.emplace(key_string, entries.size());
		auto val = &key + 1;
		if (inserted_entry.second) {
			entries.push_back({&key, val});
		} else {
			entries[inserted_entry.first->second].value = val;
		}
	}

	auto keys_offset_data = OffsetData::GetKeys(result.offsets);
	auto children_offset_data = OffsetData::GetChildren(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index], nullptr,
	                                 0, VariantLogicalType::OBJECT);

	auto &variant = result.variant;
	auto &children_list_entry = variant.children_data[result_index];
	auto &keys_list_entry = variant.keys_data[result_index];
	uint32_t count = NumericCast<uint32_t>(entries.size());
	auto start_child_index = children_list_entry.offset + children_offset_data[result_index];
	WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset_data[result_index], count,
	                               children_offset_data[result_index]);

	auto start_key_index = keys_offset_data[result_index];
	//! Reserve these indices for the object
	children_offset_data[result_index] += count;
	keys_offset_data[result_index] += count;

	//! Iterate over all the children in the Object
	for (const auto &entry : entries) {
		if (WRITE_DATA) {
			auto key_string = string_t(entry.key->GetString());
			auto keys_index = start_key_index++;
			auto dictionary_index = result.GetOrCreateIndex(key_string);

			//! Set the keys_index
			variant.keys_index_data[start_child_index] = keys_index;
			//! Set the values_index
			variant.values_index_data[start_child_index++] = values_offset_data[result_index];
			result.keys_selvec.set_index(keys_list_entry.offset + keys_index, dictionary_index);
		}

		if (!ConvertJSON<WRITE_DATA, IGNORE_NULLS>(*entry.value, result, result_index, false)) {
			return false;
		}
	}
	return true;
}

template <bool WRITE_DATA>
static bool ConvertJSONPrimitive(const JSONNode &val, ToVariantGlobalResultData &result, idx_t result_index,
                                 bool is_root) {
	auto &variant = result.variant;
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto blob_data = data_ptr_cast(variant.blob_data[result_index].GetDataWriteable());

	switch (val.type) {
	case JSONValueType::STRING: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::VARCHAR);
		uint32_t length = val.size;
		auto length_varint_size = GetVarintSize(length);
		if (WRITE_DATA) {
			auto str_blob_data = blob_data + blob_offset_data[result_index];
			VarintEncode(length, str_blob_data);
			memcpy(str_blob_data + length_varint_size, const_data_ptr_cast(val.str), length);
		}
		blob_offset_data[result_index] += length_varint_size + length;
		break;
	}
	case JSONValueType::BOOLEAN: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0,
		                                 val.boolean ? VariantLogicalType::BOOL_TRUE : VariantLogicalType::BOOL_FALSE);
		break;
	}
	case JSONValueType::UNSIGNED_INTEGER: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::UINT64);
		if (WRITE_DATA) {
			auto value = val.unsigned_integer;
			memcpy(blob_data + blob_offset_data[result_index], const_data_ptr_cast(&value), sizeof(uint64_t));
		}
		blob_offset_data[result_index] += sizeof(uint64_t);
		break;
	}
	case JSONValueType::SIGNED_INTEGER: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::INT64);
		if (WRITE_DATA) {
			auto value = val.signed_integer;
			memcpy(blob_data + blob_offset_data[result_index], const_data_ptr_cast(&value), sizeof(int64_t));
		}
		blob_offset_data[result_index] += sizeof(int64_t);
		break;
	}
	case JSONValueType::RAW:
	case JSONValueType::DOUBLE: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::DOUBLE);
		double value;
		if (WRITE_DATA) {
			if (val.type == JSONValueType::RAW) {
				auto success = TryCast::Operation<string_t, double>(string_t(val.GetString()), value, true);
				if (!success) {
					return false;
				}
			} else {
				value = val.real;
			}
			memcpy(blob_data + blob_offset_data[result_index], const_data_ptr_cast(&value), sizeof(double));
		}
		blob_offset_data[result_index] += sizeof(double);
		break;
	}
	default:
		throw InternalException("Unknown JSON value type in ConvertJSON");
	}
	return true;
}

template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSON(const JSONNode &val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root) {
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	if (val.type == JSONValueType::JSON_NULL) {
		if (!IGNORE_NULLS) {
			HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
			                              nullptr, 0, is_root);
		}
		return true;
	}

	if (val.type == JSONValueType::ARRAY) {
		return ConvertJSONArray<WRITE_DATA, IGNORE_NULLS>(val, result, result_index, is_root);
	} else if (val.type == JSONValueType::OBJECT) {
		return ConvertJSONObject<WRITE_DATA, IGNORE_NULLS>(val, result, result_index, is_root);
	} else {
		return ConvertJSONPrimitive<WRITE_DATA>(val, result, result_index, is_root);
	}
}

template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertJSONToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                          optional_ptr<const SelectionVector> selvec,
                          optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	D_ASSERT(source.vec.GetType().IsJSONType());

	auto &source_format = source.source_format;
	auto source_data = source_format.GetData<string_t>(source_format);

	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	auto &variant = result.variant;
	auto &parser = result.json_parser;
	JSONParseError error;
	for (idx_t i = 0; i < count; i++) {
		auto source_index = source[i];
		auto result_index = selvec ? selvec->get_index(i) : i;

		if (!source_format.validity.RowIsValid(source_index)) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
				                              values_index_selvec, i, is_root);
			}
			continue;
		}

		if (WRITE_DATA && values_index_selvec) {
			//! Write the 'values_index' for the parent of this column
			variant.values_index_data[values_index_selvec->get_index(i)] = values_offset_data[result_index];
		}

		auto &val = source_data[source_index];
		auto root = parser.Parse(val.GetData(), val.GetSize(), error);
		if (!root) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
				                              values_index_selvec, i, is_root);
			}
			continue;
		}

		if (!ConvertJSON<WRITE_DATA, IGNORE_NULLS>(*root, result, result_index, is_root)) {
			return false;
		}
	}
	return true;
}

} // namespace variant
} // namespace duckdb
