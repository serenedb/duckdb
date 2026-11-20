#pragma once

#include "json_common.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {

struct JsonSerializer final : Serializer {
private:
	struct StackFrame {
		JSONBuildValue *val;
		const char *tag;
		bool can_be_omitted;
	};

	JSONBuilder &builder;
	const char *current_tag = nullptr;
	vector<StackFrame> stack;
	vector<bool> property_can_be_omitted;

	// Skip writing property if null
	bool skip_if_null = false;
	// Skip writing property if empty string, empty list or empty map.
	bool skip_if_empty = false;

	// Get the current json value
	inline JSONBuildValue *Current() {
		return stack.back().val;
	};
	inline bool CurrentPropertyCanBeOmitted() {
		return !property_can_be_omitted.empty() && property_can_be_omitted.back();
	}

	void AddValue(JSONBuildValue *val, const char *tag);
	// Either adds a value to the current object with the current tag, or appends it to the current array
	void PushValue(JSONBuildValue *val);

public:
	using List = TypedList<JsonSerializer>;
	using Serializer::WriteValue;

	explicit JsonSerializer(JSONBuilder &builder, bool skip_if_null, bool skip_if_empty, bool skip_if_default,
	                        SerializationOptions options_p = SerializationOptions())
	    : builder(builder), stack({StackFrame {builder.Object(), nullptr, false}}), skip_if_null(skip_if_null),
	      skip_if_empty(skip_if_empty) {
		options = std::move(options_p);
		options.serialize_enum_as_string = true;
		options.serialize_default_values = !skip_if_default;
	}

	template <class T>
	static JSONBuildValue *Serialize(T &value, JSONBuilder &builder, bool skip_if_null, bool skip_if_empty,
	                                 bool skip_if_default, SerializationOptions options_p = SerializationOptions()) {
		JsonSerializer serializer(builder, skip_if_null, skip_if_empty, skip_if_default, options_p);
		value.Serialize(serializer);
		return serializer.GetRootObject();
	}

	template <class T>
	static string SerializeToString(T &value) {
		ArenaAllocator arena(Allocator::DefaultAllocator());
		JSONBuilder builder(arena);
		JsonSerializer serializer(builder, false, false, false);
		value.Serialize(serializer);
		JSONStringWriter writer(JSONCommon::WRITE_PRETTY_FLAG);
		writer.Value(*serializer.GetRootObject());
		return writer.ToString();
	}

	JSONBuildValue *GetRootObject() {
		D_ASSERT(stack.size() == 1); // or we forgot to pop somewhere
		return stack.front().val;
	};

	//===--------------------------------------------------------------------===//
	// Nested Types Hooks
	//===--------------------------------------------------------------------===//
	void OnPropertyBegin(const field_id_t field_id, const char *tag) final;
	void OnPropertyEnd() final;
	void OnOptionalPropertyBegin(const field_id_t field_id, const char *tag, bool present) final;
	void OnOptionalPropertyEnd(bool present) final;

	void OnListBegin(idx_t count) final;
	void OnListEnd() final;
	void OnObjectBegin() final;
	void OnObjectEnd() final;
	void OnNullableBegin(bool present) final;
	void OnNullableEnd() final;

	//===--------------------------------------------------------------------===//
	// Primitive Types
	//===--------------------------------------------------------------------===//
	void WriteNull() final;
	void WriteValue(uint8_t value) final;
	void WriteValue(int8_t value) final;
	void WriteValue(uint16_t value) final;
	void WriteValue(int16_t value) final;
	void WriteValue(uint32_t value) final;
	void WriteValue(int32_t value) final;
	void WriteValue(uint64_t value) final;
	void WriteValue(int64_t value) final;
	void WriteValue(hugeint_t value) final;
	void WriteValue(uhugeint_t value) final;
	void WriteValue(float value) final;
	void WriteValue(double value) final;
	void WriteValue(const string_t value) final;
	void WriteValue(const string &value) final;
	void WriteValue(const char *value) final;
	void WriteValue(bool value) final;
	void WriteDataPtr(const_data_ptr_t ptr, idx_t count) final;
};

} // namespace duckdb
