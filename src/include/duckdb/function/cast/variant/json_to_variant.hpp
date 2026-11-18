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

#include <absl/container/flat_hash_map.h>

namespace duckdb {
namespace variant {

namespace {

struct JSONObjectMembers {
public:
	struct Member {
		string_t key;
		const JSONNode *value;
	};

	static constexpr idx_t MAX_SCANNED_SIZE = 16;

public:
	void Push(const JSONNode &obj) {
		const auto begin = members.size();
		const auto scanned = obj.size <= MAX_SCANNED_SIZE;
		if (!scanned) {
			positions.clear();
			positions.reserve(obj.size);
		}
		for (auto &key : obj.Keys()) {
			const Member member {string_t(key.GetString()), &key + 1};
			auto position = begin;
			if (scanned) {
				while (position < members.size() && members[position].key != member.key) {
					position++;
				}
			} else {
				position = positions.try_emplace(member.key, members.size()).first->second;
			}
			if (position == members.size()) {
				members.push_back(member);
			} else {
				members[position].value = member.value;
			}
		}
	}

public:
	vector<Member> members;
	absl::flat_hash_map<string_t, idx_t, StringHash, StringEquality> positions;
};

//! Traversal state of an ARRAY or OBJECT whose children are still being converted
struct JSONConversionFrame {
	//! Whether this frame belongs to an OBJECT (ARRAY otherwise)
	bool is_object = false;
	//! Iteration state of an ARRAY
	JSONNodeRange::Iterator arr_iter {};
	idx_t members_begin = 0;
	idx_t members_end = 0;
	//! The next OBJECT entry to convert
	idx_t member_index = 0;
	//! The children index reserved for the container's next child
	idx_t child_index = 0;
	//! The keys index reserved for the OBJECT's next key
	uint32_t key_index = 0;
};

struct JSONConversionStack {
	vector<JSONConversionFrame> frames;
	JSONObjectMembers object_members;
};

} // namespace

template <bool WRITE_DATA>
static JSONConversionFrame ConvertJSONArray(const JSONNode &arr, ToVariantGlobalResultData &result,
                                            idx_t result_index) {
	JSONConversionFrame frame;
	frame.arr_iter = arr.Elements().begin();

	auto &variant = result.variant;

	auto children_offset_data = OffsetData::GetChildren(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index], nullptr,
	                                 0, VariantLogicalType::ARRAY);

	auto &children_list_entry = variant.children_data[result_index];
	uint32_t count = arr.size;
	frame.child_index = children_list_entry.offset + children_offset_data[result_index];
	WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset_data[result_index], count,
	                               children_offset_data[result_index]);

	//! Reserve these indices for the array
	children_offset_data[result_index] += count;
	return frame;
}

template <bool WRITE_DATA>
static JSONConversionFrame ConvertJSONObject(const JSONNode &obj, ToVariantGlobalResultData &result, idx_t result_index,
                                             JSONObjectMembers &object_members) {
	JSONConversionFrame frame;
	frame.is_object = true;
	frame.members_begin = object_members.members.size();
	object_members.Push(obj);
	frame.members_end = object_members.members.size();
	frame.member_index = frame.members_begin;

	auto keys_offset_data = OffsetData::GetKeys(result.offsets);
	auto children_offset_data = OffsetData::GetChildren(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index], nullptr,
	                                 0, VariantLogicalType::OBJECT);

	auto &variant = result.variant;
	auto &children_list_entry = variant.children_data[result_index];
	uint32_t count = NumericCast<uint32_t>(frame.members_end - frame.members_begin);
	frame.child_index = children_list_entry.offset + children_offset_data[result_index];
	WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset_data[result_index], count,
	                               children_offset_data[result_index]);

	frame.key_index = keys_offset_data[result_index];
	//! Reserve these indices for the object
	children_offset_data[result_index] += count;
	keys_offset_data[result_index] += count;
	return frame;
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

//! Convert a single value, pushing a frame for containers so their children are converted iteratively
template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSONValue(const JSONNode &val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root,
                             JSONConversionStack &stack) {
	if (val.type == JSONValueType::JSON_NULL) {
		if (!IGNORE_NULLS) {
			auto values_offset_data = OffsetData::GetValues(result.offsets);
			auto blob_offset_data = OffsetData::GetBlob(result.offsets);
			HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
			                              nullptr, 0, is_root);
		}
		return true;
	}
	if (val.type == JSONValueType::ARRAY) {
		stack.frames.push_back(ConvertJSONArray<WRITE_DATA>(val, result, result_index));
		return true;
	}
	if (val.type == JSONValueType::OBJECT) {
		stack.frames.push_back(ConvertJSONObject<WRITE_DATA>(val, result, result_index, stack.object_members));
		return true;
	}
	return ConvertJSONPrimitive<WRITE_DATA>(val, result, result_index, is_root);
}

//! Convert a JSON document with an explicit stack so deeply nested input cannot overflow the thread stack
template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSON(const JSONNode &val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root,
                        JSONConversionStack &stack) {
	auto &frames = stack.frames;
	auto &members = stack.object_members.members;
	D_ASSERT(frames.empty() && members.empty());
	if (!ConvertJSONValue<WRITE_DATA, IGNORE_NULLS>(val, result, result_index, is_root, stack)) {
		return false;
	}

	auto &variant = result.variant;
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	while (!frames.empty()) {
		auto &frame = frames.back();
		const JSONNode *child;
		if (frame.is_object) {
			if (frame.member_index == frame.members_end) {
				members.resize(frame.members_begin);
				frames.pop_back();
				continue;
			}
			const auto member = members[frame.member_index++];
			if (WRITE_DATA) {
				auto keys_index = frame.key_index++;
				auto dictionary_index = result.GetOrCreateIndex(member.key);
				auto &keys_list_entry = variant.keys_data[result_index];

				//! Set the keys_index
				variant.keys_index_data[frame.child_index] = keys_index;
				//! Set the values_index
				variant.values_index_data[frame.child_index++] = values_offset_data[result_index];
				result.keys_selvec.set_index(keys_list_entry.offset + keys_index, dictionary_index);
			}
			child = member.value;
		} else {
			if (frame.arr_iter.remaining == 0) {
				frames.pop_back();
				continue;
			}
			child = &*frame.arr_iter;
			++frame.arr_iter;
			if (WRITE_DATA) {
				//! Set the child index
				variant.keys_index_validity.SetInvalid(frame.child_index);
				variant.values_index_data[frame.child_index++] = values_offset_data[result_index];
			}
		}
		//! Converting the child may push a new frame, invalidating 'frame'
		if (!ConvertJSONValue<WRITE_DATA, IGNORE_NULLS>(*child, result, result_index, false, stack)) {
			return false;
		}
	}
	return true;
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
	JSONConversionStack stack;
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

		if (!ConvertJSON<WRITE_DATA, IGNORE_NULLS>(*root, result, result_index, is_root, stack)) {
			return false;
		}
	}
	return true;
}

} // namespace variant
} // namespace duckdb
