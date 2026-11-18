#include "json_deserializer.hpp"
#include "duckdb/common/stack_depth.hpp"
#include "duckdb/common/types/blob.hpp"

namespace duckdb {

void JsonDeserializer::OnPropertyBegin(const field_id_t, const char *tag) {
	current_tag = tag;
}

void JsonDeserializer::OnPropertyEnd() {
}

bool JsonDeserializer::OnOptionalPropertyBegin(const field_id_t, const char *tag) {
	auto &parent = Current();
	auto present = parent.val->Get(tag) != nullptr;
	if (present) {
		current_tag = tag;
	}
	return present;
}

void JsonDeserializer::OnOptionalPropertyEnd(bool) {
}

// If inside an object, return the value associated by the current tag (property name)
// If inside an array, return the next element in the sequence
const JSONNode *JsonDeserializer::GetNextValue() {
	auto &parent_val = Current();
	const JSONNode *val;
	if (parent_val.val->type == JSONValueType::OBJECT) {
		val = parent_val.val->Get(current_tag);
		if (!val) {
			auto msg = StringUtil::Format("Expected but did not find property '%s' in json object: '%s'", current_tag,
			                              JSONCommon::ValToString(*parent_val.val));
			throw ParserException(msg);
		}
	} else if (parent_val.val->type == JSONValueType::ARRAY) {
		if (parent_val.remaining_elements == 0) {
			auto msg = StringUtil::Format("Expected but did not find another value after exhausting json array: '%s'",
			                              JSONCommon::ValToString(*parent_val.val));
			throw ParserException(msg);
		}
		val = parent_val.next_element;
		parent_val.next_element = val->Next();
		parent_val.remaining_elements--;
	} else {
		// unreachable?
		throw InternalException("Cannot get value from non-array/object");
	}
	return val;
}

void JsonDeserializer::ThrowTypeError(const JSONNode *val, const char *expected) {
	auto actual = JSONTypeDescription(*val);
	auto &parent = Current();
	if (parent.val->type == JSONValueType::OBJECT) {
		auto msg =
		    StringUtil::Format("property '%s' expected type '%s', but got type: '%s'", current_tag, expected, actual);
		throw ParserException(msg);
	} else if (parent.val->type == JSONValueType::ARRAY) {
		auto msg = StringUtil::Format("Sequence expect child of type '%s', but got type: %s", expected, actual);
		throw ParserException(msg);
	} else {
		// unreachable?
		throw InternalException("cannot get nested value from non object or array-type");
	}
}

//===--------------------------------------------------------------------===//
// Nested Types Hooks
//===--------------------------------------------------------------------===//
void JsonDeserializer::OnObjectBegin() {
	StackDepth::Check();
	auto val = GetNextValue();
	if (val->type != JSONValueType::OBJECT) {
		ThrowTypeError(val, "object");
	}
	Push(val);
}

void JsonDeserializer::OnObjectEnd() {
	stack.pop_back();
}

idx_t JsonDeserializer::OnListBegin() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::ARRAY) {
		ThrowTypeError(val, "array");
	}
	Push(val);
	return val->size;
}

void JsonDeserializer::OnListEnd() {
	Pop();
}

bool JsonDeserializer::OnNullableBegin() {
	auto &parent_val = Current();
	const auto next_element = parent_val.next_element;
	const auto remaining_elements = parent_val.remaining_elements;
	auto val = GetNextValue();

	// Recover the iterator if we are inside an array
	parent_val.next_element = next_element;
	parent_val.remaining_elements = remaining_elements;

	if (val->type == JSONValueType::JSON_NULL) {
		return false;
	}

	return true;
}

void JsonDeserializer::OnNullableEnd() {
}

bool JsonDeserializer::CanDeserializeProperty(const field_id_t, const char *tag) {
	auto &parent = Current();
	return parent.val->Get(tag) != nullptr;
}

//===--------------------------------------------------------------------===//
// Primitive Types
//===--------------------------------------------------------------------===//
bool JsonDeserializer::ReadBool() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::BOOLEAN) {
		ThrowTypeError(val, "bool");
	}
	return val->boolean;
}

int8_t JsonDeserializer::ReadSignedInt8() {
	auto val = GetNextValue();
	if (!val->IsInteger()) {
		ThrowTypeError(val, "int8_t");
	}
	return NumericCast<int8_t>(val->signed_integer);
}

uint8_t JsonDeserializer::ReadUnsignedInt8() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::UNSIGNED_INTEGER) {
		ThrowTypeError(val, "uint8_t");
	}
	return val->unsigned_integer;
}

int16_t JsonDeserializer::ReadSignedInt16() {
	auto val = GetNextValue();
	if (!val->IsInteger()) {
		ThrowTypeError(val, "int16_t");
	}
	return NumericCast<int16_t>(val->signed_integer);
}

uint16_t JsonDeserializer::ReadUnsignedInt16() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::UNSIGNED_INTEGER) {
		ThrowTypeError(val, "uint16_t");
	}
	return val->unsigned_integer;
}

int32_t JsonDeserializer::ReadSignedInt32() {
	auto val = GetNextValue();
	if (!val->IsInteger()) {
		ThrowTypeError(val, "int32_t");
	}
	return NumericCast<int32_t>(val->signed_integer);
}

uint32_t JsonDeserializer::ReadUnsignedInt32() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::UNSIGNED_INTEGER) {
		ThrowTypeError(val, "uint32_t");
	}
	return val->unsigned_integer;
}

int64_t JsonDeserializer::ReadSignedInt64() {
	auto val = GetNextValue();
	if (!val->IsInteger()) {
		ThrowTypeError(val, "int64_t");
	}
	return val->signed_integer;
}

uint64_t JsonDeserializer::ReadUnsignedInt64() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::UNSIGNED_INTEGER) {
		ThrowTypeError(val, "uint64_t");
	}
	return val->unsigned_integer;
}

float JsonDeserializer::ReadFloat() {
	return LossyNumericCast<float>(ReadDouble());
}

double JsonDeserializer::ReadDouble() {
	auto val = GetNextValue();
	if (val->type == JSONValueType::RAW) {
		double res;
		string_t input(val->str, val->size);
		if (!TryCast::Operation<string_t, double>(input, res)) {
			throw InvalidInputException("Expected a floating point number, but got the string %s", input.GetString());
		}
		return res;
	}
	if (val->type != JSONValueType::DOUBLE) {
		ThrowTypeError(val, "double");
	}
	return val->real;
}

string JsonDeserializer::ReadString() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::STRING) {
		ThrowTypeError(val, "string");
	}
	return string(val->GetString());
}

hugeint_t JsonDeserializer::ReadHugeInt() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::OBJECT) {
		ThrowTypeError(val, "object");
	}
	Push(val);
	hugeint_t result;
	ReadProperty(100, "upper", result.upper);
	ReadProperty(101, "lower", result.lower);
	Pop();
	return result;
}

uhugeint_t JsonDeserializer::ReadUhugeInt() {
	auto val = GetNextValue();
	if (val->type != JSONValueType::OBJECT) {
		ThrowTypeError(val, "object");
	}
	Push(val);
	uhugeint_t result;
	ReadProperty(100, "upper", result.upper);
	ReadProperty(101, "lower", result.lower);
	Pop();
	return result;
}

void JsonDeserializer::ReadDataPtr(data_ptr_t &ptr, idx_t count) {
	auto val = GetNextValue();
	if (val->type != JSONValueType::STRING) {
		ThrowTypeError(val, "string");
	}
	D_ASSERT(val->size == count);
	auto blob = string_t(val->str, val->size);
	Blob::ToString(blob, char_ptr_cast(ptr));
}

} // namespace duckdb
