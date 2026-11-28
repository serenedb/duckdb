#pragma once
#include "json_common.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

class JsonDeserializer final : public Deserializer {
public:
	using List = TypedList<JsonDeserializer>;

	explicit JsonDeserializer(const JSONNode *val) {
		deserialize_enum_from_string = true;
		stack.emplace_back(val);
	}

private:
	friend class Deserializer;

	struct StackFrame {
		const JSONNode *val;
		const JSONNode *next_element;
		idx_t remaining_elements;
		explicit StackFrame(const JSONNode *val)
		    : val(val), next_element(val->First()),
		      remaining_elements(val->type == JSONValueType::ARRAY ? val->size : 0) {
		}
	};

	const char *current_tag = nullptr;
	vector<StackFrame> stack;

	// Get the current json value
	inline StackFrame &Current() {
		return stack.back();
	};

	inline void Push(const JSONNode *val) {
		stack.emplace_back(val);
	}
	inline void Pop() {
		stack.pop_back();
	}
	const JSONNode *GetNextValue();

	void ThrowTypeError(const JSONNode *val, const char *expected);

	//===--------------------------------------------------------------------===//
	// Nested Types Hooks
	//===--------------------------------------------------------------------===//
	void OnPropertyBegin(const field_id_t field_id, const char *tag) final;
	void OnPropertyEnd() final;
	bool OnOptionalPropertyBegin(const field_id_t field_id, const char *tag) final;
	void OnOptionalPropertyEnd(bool present) final;

	void OnObjectBegin() final;
	void OnObjectEnd() final;
	idx_t OnListBegin() final;
	void OnListEnd() final;
	bool OnNullableBegin() final;
	void OnNullableEnd() final;

	bool CanDeserializeProperty(const field_id_t field_id, const char *tag) final;

	//===--------------------------------------------------------------------===//
	// Primitive Types
	//===--------------------------------------------------------------------===//
	bool ReadBool() final;
	int8_t ReadSignedInt8() final;
	uint8_t ReadUnsignedInt8() final;
	int16_t ReadSignedInt16() final;
	uint16_t ReadUnsignedInt16() final;
	int32_t ReadSignedInt32() final;
	uint32_t ReadUnsignedInt32() final;
	int64_t ReadSignedInt64() final;
	uint64_t ReadUnsignedInt64() final;
	float ReadFloat() final;
	double ReadDouble() final;
	string ReadString() final;
	hugeint_t ReadHugeInt() final;
	uhugeint_t ReadUhugeInt() final;
	void ReadDataPtr(data_ptr_t &ptr, idx_t count) final;
};

} // namespace duckdb
