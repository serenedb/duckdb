//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/serializer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/serializer/serialization_traits.hpp"
#include "duckdb/common/serializer/serialization_data.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optionally_owned_ptr.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/storage/table/per_column_metadata_blocks.hpp"

namespace duckdb {
inline constexpr field_id_t SERENEDB_FIELD_ID_BASE = 16384;
inline constexpr uint8_t SERENEDB_ENUM_VALUE_BASE = 200;

template <class T>
constexpr bool IsSereneDBEnumValue(T value) {
	constexpr bool extended = std::is_same_v<T, AlterTableType> || std::is_same_v<T, AlterType> ||
	                          std::is_same_v<T, CatalogType> || std::is_same_v<T, TableColumnType> ||
	                          std::is_same_v<T, WALType>;
	return extended && static_cast<uint8_t>(value) >= SERENEDB_ENUM_VALUE_BASE;
}

class SerializationOptions {
public:
	SerializationOptions() = default;
	explicit SerializationOptions(AttachedDatabase &db);

	bool serialize_enum_as_string = false;
	bool serialize_default_values = false;
	StorageCompatibility storage_compatibility = StorageCompatibility::SereneDBLatest();
};

class Serializer {
protected:
	SerializationOptions options;
	SerializationData data;

public:
	virtual ~Serializer() {
	}

	bool ShouldSerializeInternal(StorageVersion version_added) const {
		return options.storage_compatibility.Compare(version_added);
	}

	bool ShouldSerialize(StorageVersion version_added) const {
		return ShouldSerializeInternal(version_added);
	}

	void RequireSereneDBStorageVersion(const char *what) const {
		auto &compatibility = options.storage_compatibility;
		if (!IsSereneDBStorageVersion(compatibility.GetStorageVersionCompatibility())) {
			throw NotImplementedException("Cannot write %s to a database file with DuckDB storage version %s", what,
			                              compatibility.duckdb_version);
		}
	}

	class List {
		friend Serializer;

	protected:
		Serializer &serializer;
		explicit List(Serializer &serializer) : serializer(serializer) {
		}

	public:
		// Serialize an element
		template <class T>
		void WriteElement(const T &value);

		//! Serialize bytes
		void WriteElement(data_ptr_t ptr, idx_t size);

		// Serialize an object
		template <class FUNC>
		void WriteObject(FUNC f);
	};

	template <class SERIALIZER>
	class TypedList : public List {
		friend Serializer;

		SERIALIZER &typed;
		explicit TypedList(SERIALIZER &serializer) : List(serializer), typed(serializer) {
		}

	public:
		template <class T>
		void WriteElement(const T &value) {
			typed.WriteValue(value);
		}

		void WriteElement(data_ptr_t ptr, idx_t size) {
			typed.WriteDataPtr(ptr, size);
		}

		template <class FUNC>
		void WriteObject(FUNC f) {
			typed.OnObjectBegin();
			f(typed);
			typed.OnObjectEnd();
		}
	};

public:
	SerializationOptions GetOptions() {
		return options;
	}
	SerializationData &GetSerializationData() {
		return data;
	}

	void SetSerializationData(const SerializationData &other) {
		data = other;
	}

	// Serialize a value
	template <class T>
	void WriteProperty(this auto &self, const field_id_t field_id, const char *tag, const T &value) {
		self.OnPropertyBegin(field_id, tag);
		self.WriteValue(value);
		self.OnPropertyEnd();
	}

	// Default value
	template <class T>
	void WritePropertyWithDefault(this auto &self, const field_id_t field_id, const char *tag, const T &value) {
		// If current value is default, don't write it
		if (!self.options.serialize_default_values && SerializationDefaultValue::IsDefault<T>(value)) {
			self.OnOptionalPropertyBegin(field_id, tag, false);
			self.OnOptionalPropertyEnd(false);
			return;
		}
		self.OnOptionalPropertyBegin(field_id, tag, true);
		self.WriteValue(value);
		self.OnOptionalPropertyEnd(true);
	}

	template <class T>
	void WritePropertyWithDefault(this auto &self, const field_id_t field_id, const char *tag, const T &value,
	                              const T &default_value) {
		// If current value is default, don't write it
		bool is_default;
		if constexpr (std::is_same<T, Value>::value) {
			// Value comparison throws when comparing nulls
			is_default = ValueOperations::NotDistinctFrom(value, default_value);
		} else {
			is_default = value == default_value;
		}
		if (!self.options.serialize_default_values && is_default) {
			self.OnOptionalPropertyBegin(field_id, tag, false);
			self.OnOptionalPropertyEnd(false);
			return;
		}
		self.OnOptionalPropertyBegin(field_id, tag, true);
		self.WriteValue(value);
		self.OnOptionalPropertyEnd(true);
	}

	// Specialization for Value (default Value comparison throws when comparing nulls)
	template <class T>
	void WritePropertyWithDefault(this auto &self, const field_id_t field_id, const char *tag,
	                              const CSVOption<T> &value, const T &default_value) {
		// If current value is default, don't write it
		if (!self.options.serialize_default_values && (value == default_value)) {
			self.OnOptionalPropertyBegin(field_id, tag, false);
			self.OnOptionalPropertyEnd(false);
			return;
		}
		self.OnOptionalPropertyBegin(field_id, tag, true);
		self.WriteValue(value.GetValue());
		self.OnOptionalPropertyEnd(true);
	}

	// Special case: data_ptr_T
	void WriteProperty(this auto &self, const field_id_t field_id, const char *tag, const_data_ptr_t ptr, idx_t count) {
		self.OnPropertyBegin(field_id, tag);
		self.WriteDataPtr(ptr, count);
		self.OnPropertyEnd();
	}

	// Manually begin an object
	template <class FUNC>
	void WriteObject(this auto &self, const field_id_t field_id, const char *tag, FUNC f) {
		self.OnPropertyBegin(field_id, tag);
		self.OnObjectBegin();
		f(self);
		self.OnObjectEnd();
		self.OnPropertyEnd();
	}

	template <class SELF, class FUNC>
	void WriteList(this SELF &self, const field_id_t field_id, const char *tag, idx_t count, FUNC func) {
		self.OnPropertyBegin(field_id, tag);
		self.OnListBegin(count);
		TypedList<SELF> list {self};
		for (idx_t i = 0; i < count; i++) {
			func(list, i);
		}
		self.OnListEnd();
		self.OnPropertyEnd();
	}

protected:
	template <typename T>
	typename std::enable_if<std::is_enum<T>::value, void>::type WriteValue(this auto &self, const T value) {
		if (IsSereneDBEnumValue(value)) {
			self.RequireSereneDBStorageVersion(EnumUtil::ToChars(value));
		}
		if (self.options.serialize_enum_as_string) {
			// Use the enum serializer to lookup tostring function
			auto str = EnumUtil::ToChars(value);
			self.WriteValue(str);
		} else {
			// Use the underlying type
			self.WriteValue(static_cast<typename std::underlying_type<T>::type>(value));
		}
	}

	// Optionally Owned Pointer Ref
	template <typename T>
	void WriteValue(this auto &self, const optionally_owned_ptr<T> &ptr) {
		self.WriteValue(ptr.get());
	}

	// Unique Pointer Ref
	template <typename T>
	void WriteValue(this auto &self, const unique_ptr<T> &ptr) {
		self.WriteValue(ptr.get());
	}

	// Shared Pointer Ref
	template <typename T>
	void WriteValue(this auto &self, const shared_ptr<T> &ptr) {
		self.WriteValue(ptr.get());
	}

	// Optional Pointer Ref
	template <typename T>
	void WriteValue(const optional_ptr<T> &ptr) {
		WriteValue(ptr.get());
	}

	// Pointer
	template <typename T>
	void WriteValue(this auto &self, const T *ptr) {
		if (ptr == nullptr) {
			self.OnNullableBegin(false);
			self.OnNullableEnd();
		} else {
			self.OnNullableBegin(true);
			self.WriteValue(*ptr);
			self.OnNullableEnd();
		}
	}

	// DuckDB Optional
	template <typename T>
	void WriteValue(this auto &self, const optional<T> &opt) {
		if (!opt) {
			self.OnNullableBegin(false);
			self.OnNullableEnd();
		} else {
			self.OnNullableBegin(true);
			self.WriteValue(opt.value());
			self.OnNullableEnd();
		}
	}

	// Pair
	template <class K, class V>
	void WriteValue(this auto &self, const std::pair<K, V> &pair) {
		self.OnObjectBegin();
		self.WriteProperty(0, "first", pair.first);
		self.WriteProperty(1, "second", pair.second);
		self.OnObjectEnd();
	}

	// Reference Wrapper
	template <class T>
	void WriteValue(this auto &self, const reference<T> ref) {
		self.WriteValue(ref.get());
	}

	// Vector
	template <class T>
	void WriteValue(this auto &self, const vector<T> &vec) {
		auto count = vec.size();
		self.OnListBegin(count);
		for (const auto &item : vec) {
			self.WriteValue(item);
		}
		self.OnListEnd();
	}

	template <class T>
	void WriteValue(this auto &self, const unsafe_vector<T> &vec) {
		auto count = vec.size();
		self.OnListBegin(count);
		for (auto &item : vec) {
			self.WriteValue(item);
		}
		self.OnListEnd();
	}

	// UnorderedSet
	// Serialized the same way as a list/vector
	template <class T, class HASH, class CMP>
	void WriteValue(this auto &self, const duckdb::unordered_set<T, HASH, CMP> &set) {
		auto count = set.size();
		self.OnListBegin(count);
		for (auto &item : set) {
			self.WriteValue(item);
		}
		self.OnListEnd();
	}

	// Set
	// Serialized the same way as a list/vector
	template <class T, class HASH, class CMP>
	void WriteValue(this auto &self, const duckdb::set<T, HASH, CMP> &set) {
		auto count = set.size();
		self.OnListBegin(count);
		for (auto &item : set) {
			self.WriteValue(item);
		}
		self.OnListEnd();
	}

	// Map
	// serialized as a list of pairs
	template <class K, class V, class HASH, class CMP>
	void WriteValue(this auto &self, const duckdb::unordered_map<K, V, HASH, CMP> &map) {
		auto count = map.size();
		self.OnListBegin(count);
		for (auto &item : map) {
			self.OnObjectBegin();
			self.WriteProperty(0, "key", item.first);
			self.WriteProperty(1, "value", item.second);
			self.OnObjectEnd();
		}
		self.OnListEnd();
	}

	// Map
	// serialized as a list of pairs
	template <class K, class V, class HASH, class CMP>
	void WriteValue(this auto &self, const duckdb::map<K, V, HASH, CMP> &map) {
		auto count = map.size();
		self.OnListBegin(count);
		for (auto &item : map) {
			self.OnObjectBegin();
			self.WriteProperty(0, "key", item.first);
			self.WriteProperty(1, "value", item.second);
			self.OnObjectEnd();
		}
		self.OnListEnd();
	}

	// Insertion Order Preserving Map
	// serialized as a list of pairs
	template <class V, class KEY, class INDEX_MAP>
	void WriteValue(this auto &self, const duckdb::InsertionOrderPreservingMap<V, KEY, INDEX_MAP> &map) {
		auto count = map.size();
		self.OnListBegin(count);
		for (auto &entry : map) {
			self.OnObjectBegin();
			self.WriteProperty(0, "key", entry.first);
			self.WriteProperty(1, "value", entry.second);
			self.OnObjectEnd();
		}
		self.OnListEnd();
	}

	// priority queue
	template <typename T>
	void WriteValue(this auto &self, const std::priority_queue<T> &queue) {
		vector<T> placeholder;
		auto queue_copy = std::priority_queue<T>(queue);
		while (queue_copy.size() > 0) {
			placeholder.emplace_back(queue_copy.top());
			queue_copy.pop();
		}
		self.WriteValue(placeholder);
	}

	// class or struct implementing `Serialize(Serializer& Serializer)`;
	template <typename T>
	typename std::enable_if<has_serialize<T>::value>::type WriteValue(this auto &self, const T &value) {
		self.OnObjectBegin();
		value.Serialize(self);
		self.OnObjectEnd();
	}

public:
	// Hooks for subclasses to override to implement custom behavior
	virtual void OnPropertyBegin(const field_id_t field_id, const char *tag) = 0;
	virtual void OnPropertyEnd() = 0;
	virtual void OnOptionalPropertyBegin(const field_id_t field_id, const char *tag, bool present) = 0;
	virtual void OnOptionalPropertyEnd(bool present) = 0;
	virtual void OnObjectBegin() = 0;
	virtual void OnObjectEnd() = 0;
	virtual void OnListBegin(idx_t count) = 0;
	virtual void OnListEnd() = 0;
	virtual void OnNullableBegin(bool present) = 0;
	virtual void OnNullableEnd() = 0;

	// Handle primitive types, a serializer needs to implement these.
	virtual void WriteNull() = 0;
	virtual void WriteValue(char value) {
		throw NotImplementedException("Write char value not implemented");
	}
	virtual void WriteValue(bool value) = 0;
	virtual void WriteValue(uint8_t value) = 0;
	virtual void WriteValue(int8_t value) = 0;
	virtual void WriteValue(uint16_t value) = 0;
	virtual void WriteValue(int16_t value) = 0;
	virtual void WriteValue(uint32_t value) = 0;
	virtual void WriteValue(int32_t value) = 0;
	virtual void WriteValue(uint64_t value) = 0;
	virtual void WriteValue(int64_t value) = 0;
	virtual void WriteValue(hugeint_t value) = 0;
	virtual void WriteValue(uhugeint_t value) = 0;
	virtual void WriteValue(float value) = 0;
	virtual void WriteValue(double value) = 0;
	virtual void WriteValue(const string_t value) = 0;
	virtual void WriteValue(const string &value) = 0;
	virtual void WriteValue(const char *str) = 0;
	virtual void WriteDataPtr(const_data_ptr_t ptr, idx_t count) = 0;
	//! Identifiers are serialized identically to a plain string (preserving the original casing)
	void WriteValue(this auto &self, const Identifier &value) {
		self.WriteValue(value.GetIdentifierName());
	}
	void WriteValue(this auto &self, LogicalIndex value) {
		self.WriteValue(value.index);
	}
	void WriteValue(this auto &self, PhysicalIndex value) {
		self.WriteValue(value.index);
	}
	void WriteValue(this auto &self, TableIndex value) {
		self.WriteValue(value.index);
	}
	void WriteValue(this auto &self, ProjectionIndex value) {
		self.WriteValue(value.GetIndexUnsafe());
	}
	void WriteValue(this auto &self, optional_idx value) {
		self.WriteValue(value.IsValid() ? value.GetIndex() : DConstants::INVALID_INDEX);
	}
	void WriteValue(this auto &self, PerColumnMetadataBlock value) {
		self.WriteValue(value.GetPacked());
	}
};

// List Impl
template <class FUNC>
void Serializer::List::WriteObject(FUNC f) {
	serializer.OnObjectBegin();
	f(serializer);
	serializer.OnObjectEnd();
}

template <class T>
void Serializer::List::WriteElement(const T &value) {
	serializer.WriteValue(value);
}

} // namespace duckdb
