//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/deserializer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/serializer/serialization_data.hpp"
#include "duckdb/common/serializer/serialization_traits.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"
#include "duckdb/storage/table/per_column_metadata_blocks.hpp"
#include "duckdb/storage/table/per_column_metadata_blocks.hpp"

namespace duckdb {

class Deserializer {
protected:
	bool deserialize_enum_from_string = false;
	SerializationData data;

public:
	virtual ~Deserializer() {
	}

	class List {
		friend Deserializer;

	protected:
		Deserializer &deserializer;
		explicit List(Deserializer &deserializer) : deserializer(deserializer) {
		}

	public:
		// Deserialize an element
		template <class T>
		T ReadElement();

		//! Deserialize bytes
		template <class T>
		void ReadElement(data_ptr_t &ptr, idx_t size);

		// Deserialize an object
		template <class FUNC>
		void ReadObject(FUNC f);
	};

	template <class DESERIALIZER>
	class TypedList : public List {
		friend Deserializer;

		DESERIALIZER &typed;
		explicit TypedList(DESERIALIZER &deserializer) : List(deserializer), typed(deserializer) {
		}

	public:
		template <class T>
		T ReadElement() {
			return typed.template Read<T>();
		}

		template <class T>
		void ReadElement(data_ptr_t &ptr, idx_t size) {
			typed.ReadDataPtr(ptr, size);
		}

		template <class FUNC>
		void ReadObject(FUNC f) {
			typed.OnObjectBegin();
			f(typed);
			typed.OnObjectEnd();
		}
	};

public:
	virtual bool CanDeserializeProperty(const field_id_t field_id, const char *tag) = 0;

	// Read into an existing value
	template <typename T>
	inline void ReadProperty(this auto &self, const field_id_t field_id, const char *tag, T &ret) {
		self.OnPropertyBegin(field_id, tag);
		ret = self.template Read<T>();
		self.OnPropertyEnd();
	}

	// Read and return a value
	template <typename T>
	inline T ReadProperty(this auto &self, const field_id_t field_id, const char *tag) {
		self.OnPropertyBegin(field_id, tag);
		auto ret = self.template Read<T>();
		self.OnPropertyEnd();
		return ret;
	}

	// Default Value return
	template <typename T>
	inline T ReadPropertyWithDefault(this auto &self, const field_id_t field_id, const char *tag) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			self.OnOptionalPropertyEnd(false);
			return std::forward<T>(SerializationDefaultValue::GetDefault<T>());
		}
		auto ret = self.template Read<T>();
		self.OnOptionalPropertyEnd(true);
		return ret;
	}

	template <typename T>
	inline T ReadPropertyWithExplicitDefault(this auto &self, const field_id_t field_id, const char *tag,
	                                         T default_value) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			self.OnOptionalPropertyEnd(false);
			return std::forward<T>(default_value);
		}
		auto ret = self.template Read<T>();
		self.OnOptionalPropertyEnd(true);
		return ret;
	}

	// Default value in place
	template <typename T>
	inline void ReadPropertyWithDefault(this auto &self, const field_id_t field_id, const char *tag, T &ret) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			ret = std::forward<T>(SerializationDefaultValue::GetDefault<T>());
			self.OnOptionalPropertyEnd(false);
			return;
		}
		ret = self.template Read<T>();
		self.OnOptionalPropertyEnd(true);
	}

	template <typename T>
	inline void ReadPropertyWithExplicitDefault(this auto &self, const field_id_t field_id, const char *tag, T &ret,
	                                            T default_value) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			ret = std::forward<T>(default_value);
			self.OnOptionalPropertyEnd(false);
			return;
		}
		ret = self.template Read<T>();
		self.OnOptionalPropertyEnd(true);
	}

	template <typename T>
	inline void ReadPropertyWithExplicitDefault(this auto &self, const field_id_t field_id, const char *tag,
	                                            CSVOption<T> &ret, T default_value) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			ret = std::forward<T>(default_value);
			self.OnOptionalPropertyEnd(false);
			return;
		}
		ret = self.template Read<T>();
		self.OnOptionalPropertyEnd(true);
	}

	// Special case:
	// Read into an existing data_ptr_t
	inline void ReadProperty(this auto &self, const field_id_t field_id, const char *tag, data_ptr_t ret, idx_t count) {
		self.OnPropertyBegin(field_id, tag);
		self.ReadDataPtr(ret, count);
		self.OnPropertyEnd();
	}

	inline bool ReadOptionalProperty(this auto &self, const field_id_t field_id, const char *tag, data_ptr_t ret,
	                                 idx_t count) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			self.OnOptionalPropertyEnd(false);
			return false;
		}
		self.ReadDataPtr(ret, count);
		self.OnOptionalPropertyEnd(true);
		return true;
	}

	// Try to read a property, if it is not present, continue, otherwise read and discard the value
	template <typename T>
	inline void ReadDeletedProperty(this auto &self, const field_id_t field_id, const char *tag) {
		// Try to read the property. If not present, great!
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			self.OnOptionalPropertyEnd(false);
			return;
		}
		// Otherwise read and discard the value
		(void)self.template Read<T>();
		self.OnOptionalPropertyEnd(true);
	}

	//! Set a serialization property
	template <class T>
	void Set(T entry) {
		return data.Set<T>(entry);
	}

	//! Retrieve the last set serialization property of this type
	template <class T>
	T Get() {
		return data.Get<T>();
	}

	template <class T>
	optional_ptr<T> TryGet() {
		return data.TryGet<T>();
	}

	//! Unset a serialization property
	template <class T>
	void Unset() {
		return data.Unset<T>();
	}

	SerializationData &GetSerializationData() {
		return data;
	}

	void SetSerializationData(const SerializationData &other) {
		data = other;
	}

	template <class SELF, class FUNC>
	void ReadListInternal(this SELF &self, FUNC func) {
		auto size = self.OnListBegin();
		TypedList<SELF> list {self};
		for (idx_t i = 0; i < size; i++) {
			func(list, i);
		}
		self.OnListEnd();
	}

	template <class FUNC>
	void ReadList(this auto &self, const field_id_t field_id, const char *tag, FUNC func) {
		self.OnPropertyBegin(field_id, tag);
		self.ReadListInternal(func);
		self.OnPropertyEnd();
	}

	template <class FUNC>
	void ReadOptionalList(this auto &self, const field_id_t field_id, const char *tag, FUNC func) {
		if (!self.OnOptionalPropertyBegin(field_id, tag)) {
			self.OnOptionalPropertyEnd(false);
			return;
		}
		self.ReadListInternal(func);
		self.OnOptionalPropertyEnd(true);
	}

	template <class FUNC>
	void ReadObject(this auto &self, const field_id_t field_id, const char *tag, FUNC func) {
		self.OnPropertyBegin(field_id, tag);
		self.OnObjectBegin();
		func(self);
		self.OnObjectEnd();
		self.OnPropertyEnd();
	}

private:
	// Deserialize anything implementing a Deserialize method
	template <typename T = void>
	inline typename std::enable_if<has_deserialize<T>::value, T>::type Read(this auto &self) {
		self.OnObjectBegin();
		auto val = T::Deserialize(self);
		self.OnObjectEnd();
		return val;
	}

	// Deserialize a optionally_owned_ptr
	template <class T, typename ELEMENT_TYPE = typename is_optionally_owned_ptr<T>::ELEMENT_TYPE>
	inline typename std::enable_if<is_optionally_owned_ptr<T>::value, T>::type Read(this auto &self) {
		return optionally_owned_ptr<ELEMENT_TYPE>(self.template Read<unique_ptr<ELEMENT_TYPE>>());
	}

	// Deserialize unique_ptr if the element type has a Deserialize method
	template <class T, typename ELEMENT_TYPE = typename is_unique_ptr<T>::ELEMENT_TYPE>
	inline typename std::enable_if<is_unique_ptr<T>::value && has_deserialize<ELEMENT_TYPE>::value, T>::type
	Read(this auto &self) {
		unique_ptr<ELEMENT_TYPE> ptr = nullptr;
		auto is_present = self.OnNullableBegin();
		if (is_present) {
			self.OnObjectBegin();
			ptr = ELEMENT_TYPE::Deserialize(self);
			self.OnObjectEnd();
		}
		self.OnNullableEnd();
		return ptr;
	}

	// Deserialize a unique_ptr if the element type does not have a Deserialize method
	template <class T, typename ELEMENT_TYPE = typename is_unique_ptr<T>::ELEMENT_TYPE>
	inline typename std::enable_if<is_unique_ptr<T>::value && !has_deserialize<ELEMENT_TYPE>::value, T>::type
	Read(this auto &self) {
		unique_ptr<ELEMENT_TYPE> ptr = nullptr;
		auto is_present = self.OnNullableBegin();
		if (is_present) {
			self.OnObjectBegin();
			ptr = make_uniq<ELEMENT_TYPE>(self.template Read<ELEMENT_TYPE>());
			self.OnObjectEnd();
		}
		self.OnNullableEnd();
		return ptr;
	}

	// Deserialize shared_ptr
	template <typename T = void>
	inline typename std::enable_if<is_shared_ptr<T>::value, T>::type Read(this auto &self) {
		using ELEMENT_TYPE = typename is_shared_ptr<T>::ELEMENT_TYPE;
		shared_ptr<ELEMENT_TYPE> ptr = nullptr;
		auto is_present = self.OnNullableBegin();
		if (is_present) {
			self.OnObjectBegin();
			ptr = ELEMENT_TYPE::Deserialize(self);
			self.OnObjectEnd();
		}
		self.OnNullableEnd();
		return ptr;
	}

	// Deserialize a duckdb_optional
	template <class T, typename ELEMENT_TYPE = typename is_duckdb_optional<T>::ELEMENT_TYPE>
	inline typename std::enable_if<is_duckdb_optional<T>::value, T>::type Read(this auto &self) {
		auto is_present = self.OnNullableBegin();
		T result;
		if (is_present) {
			result = self.template Read<ELEMENT_TYPE>();
		}
		self.OnNullableEnd();
		return result;
	}

	// Deserialize a vector
	template <typename T = void>
	inline typename std::enable_if<is_vector<T>::value, T>::type Read(this auto &self) {
		using ELEMENT_TYPE = typename is_vector<T>::ELEMENT_TYPE;
		T vec;
		auto size = self.OnListBegin();
		for (idx_t i = 0; i < size; i++) {
			vec.push_back(self.template Read<ELEMENT_TYPE>());
		}
		self.OnListEnd();
		return vec;
	}

	template <typename T = void>
	inline typename std::enable_if<is_unsafe_vector<T>::value, T>::type Read(this auto &self) {
		using ELEMENT_TYPE = typename is_unsafe_vector<T>::ELEMENT_TYPE;
		T vec;
		auto size = self.OnListBegin();
		for (idx_t i = 0; i < size; i++) {
			vec.push_back(self.template Read<ELEMENT_TYPE>());
		}
		self.OnListEnd();

		return vec;
	}

	// Deserialize a map
	template <typename T = void>
	inline typename std::enable_if<is_unordered_map<T>::value, T>::type Read(this auto &self) {
		using KEY_TYPE = typename is_unordered_map<T>::KEY_TYPE;
		using VALUE_TYPE = typename is_unordered_map<T>::VALUE_TYPE;

		T map;
		auto size = self.OnListBegin();
		for (idx_t i = 0; i < size; i++) {
			self.OnObjectBegin();
			auto key = self.template ReadProperty<KEY_TYPE>(0, "key");
			auto value = self.template ReadProperty<VALUE_TYPE>(1, "value");
			self.OnObjectEnd();
			map[std::move(key)] = std::move(value);
		}
		self.OnListEnd();
		return map;
	}

	template <typename T = void>
	inline typename std::enable_if<is_map<T>::value, T>::type Read(this auto &self) {
		using KEY_TYPE = typename is_map<T>::KEY_TYPE;
		using VALUE_TYPE = typename is_map<T>::VALUE_TYPE;

		T map;
		auto size = self.OnListBegin();
		for (idx_t i = 0; i < size; i++) {
			self.OnObjectBegin();
			auto key = self.template ReadProperty<KEY_TYPE>(0, "key");
			auto value = self.template ReadProperty<VALUE_TYPE>(1, "value");
			self.OnObjectEnd();
			map[std::move(key)] = std::move(value);
		}
		self.OnListEnd();
		return map;
	}

	template <typename T = void>
	inline typename std::enable_if<is_insertion_preserving_map<T>::value, T>::type Read(this auto &self) {
		using VALUE_TYPE = typename is_insertion_preserving_map<T>::VALUE_TYPE;
		using KEY_TYPE = typename is_insertion_preserving_map<T>::KEY_TYPE;

		T map;
		auto size = self.OnListBegin();
		for (idx_t i = 0; i < size; i++) {
			self.OnObjectBegin();
			auto key = self.template ReadProperty<KEY_TYPE>(0, "key");
			auto value = self.template ReadProperty<VALUE_TYPE>(1, "value");
			self.OnObjectEnd();
			map[key] = std::move(value);
		}
		self.OnListEnd();
		return map;
	}

	// Deserialize an unordered set
	template <typename T = void>
	inline typename std::enable_if<is_unordered_set<T>::value, T>::type Read(this auto &self) {
		using ELEMENT_TYPE = typename is_unordered_set<T>::ELEMENT_TYPE;
		auto size = self.OnListBegin();
		T set;
		for (idx_t i = 0; i < size; i++) {
			set.insert(self.template Read<ELEMENT_TYPE>());
		}
		self.OnListEnd();
		return set;
	}

	// Deserialize a set
	template <typename T = void>
	inline typename std::enable_if<is_set<T>::value, T>::type Read(this auto &self) {
		using ELEMENT_TYPE = typename is_set<T>::ELEMENT_TYPE;
		auto size = self.OnListBegin();
		T set;
		for (idx_t i = 0; i < size; i++) {
			set.insert(self.template Read<ELEMENT_TYPE>());
		}
		self.OnListEnd();
		return set;
	}

	// Deserialize a pair
	template <typename T = void>
	inline typename std::enable_if<is_pair<T>::value, T>::type Read(this auto &self) {
		using FIRST_TYPE = typename is_pair<T>::FIRST_TYPE;
		using SECOND_TYPE = typename is_pair<T>::SECOND_TYPE;
		self.OnObjectBegin();
		auto first = self.template ReadProperty<FIRST_TYPE>(0, "first");
		auto second = self.template ReadProperty<SECOND_TYPE>(1, "second");
		self.OnObjectEnd();
		return std::make_pair(first, second);
	}

	// Deserialize a priority_queue
	template <typename T = void>
	inline typename std::enable_if<is_queue<T>::value, T>::type Read(this auto &self) {
		using ELEMENT_TYPE = typename is_queue<T>::ELEMENT_TYPE;
		T queue;
		auto size = self.OnListBegin();
		for (idx_t i = 0; i < size; i++) {
			queue.emplace(self.template Read<ELEMENT_TYPE>());
		}
		self.OnListEnd();
		return queue;
	}

	// Primitive types
	// Deserialize a bool
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, bool>::value, T>::type Read(this auto &self) {
		return self.ReadBool();
	}

	// Deserialize a char
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, char>::value, T>::type Read(this auto &self) {
		return self.ReadChar();
	}

	// Deserialize a int8_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, int8_t>::value, T>::type Read(this auto &self) {
		return self.ReadSignedInt8();
	}

	// Deserialize a uint8_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, uint8_t>::value, T>::type Read(this auto &self) {
		return self.ReadUnsignedInt8();
	}

	// Deserialize a int16_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, int16_t>::value, T>::type Read(this auto &self) {
		return self.ReadSignedInt16();
	}

	// Deserialize a uint16_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, uint16_t>::value, T>::type Read(this auto &self) {
		return self.ReadUnsignedInt16();
	}

	// Deserialize a int32_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, int32_t>::value, T>::type Read(this auto &self) {
		return self.ReadSignedInt32();
	}

	// Deserialize a uint32_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, uint32_t>::value, T>::type Read(this auto &self) {
		return self.ReadUnsignedInt32();
	}

	// Deserialize a int64_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, int64_t>::value, T>::type Read(this auto &self) {
		return self.ReadSignedInt64();
	}

	// Deserialize a uint64_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, uint64_t>::value, T>::type Read(this auto &self) {
		return self.ReadUnsignedInt64();
	}

	// Deserialize a float
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, float>::value, T>::type Read(this auto &self) {
		return self.ReadFloat();
	}

	// Deserialize a double
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, double>::value, T>::type Read(this auto &self) {
		return self.ReadDouble();
	}

	// Deserialize a string
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, string>::value, T>::type Read(this auto &self) {
		return self.ReadString();
	}

	// Deserialize an Identifier (stored identically to a plain string)
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, Identifier>::value, T>::type Read(this auto &self) {
		return Identifier(self.ReadString());
	}

	// Deserialize a Enum
	template <typename T = void>
	inline typename std::enable_if<std::is_enum<T>::value, T>::type Read(this auto &self) {
		if (self.deserialize_enum_from_string) {
			auto str = self.ReadString();
			return EnumUtil::FromString<T>(str.c_str());
		} else {
			return (T)self.template Read<typename std::underlying_type<T>::type>();
		}
	}

	// Deserialize a hugeint_t
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, hugeint_t>::value, T>::type Read(this auto &self) {
		return self.ReadHugeInt();
	}

	// Deserialize a uhugeint
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, uhugeint_t>::value, T>::type Read(this auto &self) {
		return self.ReadUhugeInt();
	}

	// Deserialize a LogicalIndex
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, LogicalIndex>::value, T>::type Read(this auto &self) {
		return LogicalIndex(self.ReadUnsignedInt64());
	}

	// Deserialize a PhysicalIndex
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, PhysicalIndex>::value, T>::type Read(this auto &self) {
		return PhysicalIndex(self.ReadUnsignedInt64());
	}

	// Deserialize a TableIndex
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, TableIndex>::value, T>::type Read(this auto &self) {
		return TableIndex(self.ReadUnsignedInt64());
	}

	// Deserialize a ProjectionIndex
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, ProjectionIndex>::value, T>::type Read(this auto &self) {
		return ProjectionIndex(self.ReadUnsignedInt64());
	}

	// Deserialize an optional_idx
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, optional_idx>::value, T>::type Read(this auto &self) {
		auto idx = self.ReadUnsignedInt64();
		return idx == DConstants::INVALID_INDEX ? optional_idx() : optional_idx(idx);
	}

	// Deserialize a ProjectionIndex
	template <typename T = void>
	inline typename std::enable_if<std::is_same<T, PerColumnMetadataBlock>::value, T>::type Read(this auto &self) {
		return PerColumnMetadataBlock::Unpack(self.ReadUnsignedInt64());
	}

public:
	// Hooks for subclasses to override to implement custom behavior
	virtual void OnPropertyBegin(const field_id_t field_id, const char *tag) = 0;
	virtual void OnPropertyEnd() = 0;
	virtual bool OnOptionalPropertyBegin(const field_id_t field_id, const char *tag) = 0;
	virtual void OnOptionalPropertyEnd(bool present) = 0;

	virtual void OnObjectBegin() = 0;
	virtual void OnObjectEnd() = 0;
	virtual idx_t OnListBegin() = 0;
	virtual void OnListEnd() = 0;
	virtual bool OnNullableBegin() = 0;
	virtual void OnNullableEnd() = 0;

	// Handle primitive types, a serializer needs to implement these.
	virtual bool ReadBool() = 0;
	virtual char ReadChar() {
		throw NotImplementedException("ReadChar not implemented");
	}
	virtual int8_t ReadSignedInt8() = 0;
	virtual uint8_t ReadUnsignedInt8() = 0;
	virtual int16_t ReadSignedInt16() = 0;
	virtual uint16_t ReadUnsignedInt16() = 0;
	virtual int32_t ReadSignedInt32() = 0;
	virtual uint32_t ReadUnsignedInt32() = 0;
	virtual int64_t ReadSignedInt64() = 0;
	virtual uint64_t ReadUnsignedInt64() = 0;
	virtual hugeint_t ReadHugeInt() = 0;
	virtual uhugeint_t ReadUhugeInt() = 0;
	virtual float ReadFloat() = 0;
	virtual double ReadDouble() = 0;
	virtual string ReadString() = 0;
	virtual void ReadDataPtr(data_ptr_t &ptr, idx_t count) = 0;
};

template <class FUNC>
void Deserializer::List::ReadObject(FUNC f) {
	deserializer.OnObjectBegin();
	f(deserializer);
	deserializer.OnObjectEnd();
}

template <class T>
T Deserializer::List::ReadElement() {
	return deserializer.Read<T>();
}

template <class T>
void Deserializer::List::ReadElement(data_ptr_t &ptr, idx_t size) {
	deserializer.ReadDataPtr(ptr, size);
}

} // namespace duckdb
