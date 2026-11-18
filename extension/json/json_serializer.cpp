#include "json_serializer.hpp"
#include "duckdb/common/stack_depth.hpp"
#include "duckdb/common/types/blob.hpp"

namespace duckdb {

void JsonSerializer::AddValue(JSONBuildValue *val, const char *tag) {
	auto &current = *Current();
	// Array case, just append the value
	if (current.type == JSONValueType::ARRAY) {
		JSONBuilder::Append(current, val);
	}
	// Object case, use the given tag
	else if (current.type == JSONValueType::OBJECT) {
		JSONBuilder::Add(current, tag, val);
	}
	// Else throw
	else {
		throw InternalException("Cannot add value to non-array/object json value");
	}
}

void JsonSerializer::PushValue(JSONBuildValue *val) {
	AddValue(val, current_tag);
}

void JsonSerializer::OnPropertyBegin(const field_id_t, const char *tag) {
	current_tag = tag;
	property_can_be_omitted.push_back(false);
}

void JsonSerializer::OnPropertyEnd() {
	property_can_be_omitted.pop_back();
}

void JsonSerializer::OnOptionalPropertyBegin(const field_id_t, const char *tag, bool present) {
	if (present) {
		current_tag = tag;
	}
	property_can_be_omitted.push_back(present);
}

void JsonSerializer::OnOptionalPropertyEnd(bool) {
	property_can_be_omitted.pop_back();
}

//-------------------------------------------------------------------------
// Nested Types
//-------------------------------------------------------------------------
void JsonSerializer::OnNullableBegin(bool present) {
	if (!present && !(skip_if_null && Current()->type == JSONValueType::OBJECT && CurrentPropertyCanBeOmitted())) {
		WriteNull();
	}
}

void JsonSerializer::OnNullableEnd() {
}

void JsonSerializer::OnListBegin(idx_t count) {
	auto new_value = builder.Array();
	auto can_be_omitted = Current()->type == JSONValueType::OBJECT && CurrentPropertyCanBeOmitted();
	// We always push a value to the stack, we just don't add it as a child to the current value
	// if skipping empty. Even though it is "unnecessary" to create an empty value just to discard it,
	// this allows the rest of the code to keep on like normal.
	if (!(count == 0 && skip_if_empty && can_be_omitted)) {
		PushValue(new_value);
	}
	stack.push_back({new_value, nullptr, can_be_omitted});
}

void JsonSerializer::OnListEnd() {
	stack.pop_back();
}

void JsonSerializer::OnObjectBegin() {
	StackDepth::Check();
	auto can_be_omitted = Current()->type == JSONValueType::OBJECT && CurrentPropertyCanBeOmitted();
	stack.push_back({builder.Object(), current_tag, can_be_omitted});
}

void JsonSerializer::OnObjectEnd() {
	auto frame = stack.back();
	stack.pop_back();
	if (frame.val->size == 0 && skip_if_empty && frame.can_be_omitted) {
		return;
	}
	AddValue(frame.val, frame.tag);
}

//-------------------------------------------------------------------------
// Primitive Types
//-------------------------------------------------------------------------
void JsonSerializer::WriteNull() {
	if (skip_if_null && Current()->type == JSONValueType::OBJECT && CurrentPropertyCanBeOmitted()) {
		return;
	}
	PushValue(builder.Null());
}

void JsonSerializer::WriteValue(uint8_t value) {
	PushValue(builder.UnsignedInteger(value));
}

void JsonSerializer::WriteValue(int8_t value) {
	PushValue(builder.SignedInteger(value));
}

void JsonSerializer::WriteValue(uint16_t value) {
	PushValue(builder.UnsignedInteger(value));
}

void JsonSerializer::WriteValue(int16_t value) {
	PushValue(builder.SignedInteger(value));
}

void JsonSerializer::WriteValue(uint32_t value) {
	PushValue(builder.UnsignedInteger(value));
}

void JsonSerializer::WriteValue(int32_t value) {
	PushValue(builder.SignedInteger(value));
}

void JsonSerializer::WriteValue(uint64_t value) {
	PushValue(builder.UnsignedInteger(value));
}

void JsonSerializer::WriteValue(int64_t value) {
	PushValue(builder.SignedInteger(value));
}

void JsonSerializer::WriteValue(hugeint_t value) {
	auto val = builder.Object();
	PushValue(val);
	stack.push_back({val, nullptr, false});
	WriteProperty(100, "upper", value.upper);
	WriteProperty(101, "lower", value.lower);
	stack.pop_back();
}

void JsonSerializer::WriteValue(uhugeint_t value) {
	auto val = builder.Object();
	PushValue(val);
	stack.push_back({val, nullptr, false});
	WriteProperty(100, "upper", value.upper);
	WriteProperty(101, "lower", value.lower);
	stack.pop_back();
}

static JSONBuildValue *DoubleToJSONValue(JSONBuilder &builder, double value) {
	if (Value::DoubleIsFinite(value)) {
		// simple - finite json
		return builder.Double(value);
	}
	// represent infinities as strings
	const char *str;
	if (!Value::IsNan(value)) {
		str = value < 0 ? "-Infinity" : "Infinity";
	} else {
		str = "NAN";
	}
	return builder.Raw(str);
}

void JsonSerializer::WriteValue(float value) {
	PushValue(DoubleToJSONValue(builder, value));
}

void JsonSerializer::WriteValue(double value) {
	PushValue(DoubleToJSONValue(builder, value));
}

void JsonSerializer::WriteValue(const string &value) {
	if (skip_if_empty && value.empty() && Current()->type == JSONValueType::OBJECT && CurrentPropertyCanBeOmitted()) {
		return;
	}
	PushValue(builder.String(value));
}

void JsonSerializer::WriteValue(const string_t value) {
	if (skip_if_empty && value.GetSize() == 0 && Current()->type == JSONValueType::OBJECT &&
	    CurrentPropertyCanBeOmitted()) {
		return;
	}
	PushValue(builder.String(std::string_view(value.GetData(), value.GetSize())));
}

void JsonSerializer::WriteValue(const char *value) {
	if (skip_if_empty && strlen(value) == 0 && Current()->type == JSONValueType::OBJECT &&
	    CurrentPropertyCanBeOmitted()) {
		return;
	}
	PushValue(builder.String(value));
}

void JsonSerializer::WriteValue(bool value) {
	PushValue(builder.Boolean(value));
}

void JsonSerializer::WriteDataPtr(const_data_ptr_t ptr, idx_t count) {
	auto blob = Blob::ToString(string_t(const_char_ptr_cast(ptr), count));
	PushValue(builder.String(blob));
}

} // namespace duckdb
