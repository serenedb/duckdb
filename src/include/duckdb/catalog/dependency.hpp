//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/dependency.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"

namespace duckdb {
class CatalogEntry;

struct DependencyFlags {
public:
	DependencyFlags() : value(0) {
	}
	DependencyFlags(const DependencyFlags &other) : value(other.value) {
	}
	virtual ~DependencyFlags() = default;
	DependencyFlags &operator=(const DependencyFlags &other) {
		value = other.value;
		return *this;
	}
	bool operator==(const DependencyFlags &other) const {
		return other.value == value;
	}
	bool operator!=(const DependencyFlags &other) const {
		return !(*this == other);
	}

public:
	virtual string ToString() const = 0;

protected:
	template <uint8_t BIT>
	bool IsSet() const {
		static const uint8_t FLAG = (1 << BIT);
		return (value & FLAG) == FLAG;
	}
	template <uint8_t BIT>
	void Set() {
		static const uint8_t FLAG = (1 << BIT);
		value |= FLAG;
	}
	void Merge(uint8_t other) {
		value |= other;
	}
	uint8_t Value() const {
		return value;
	}

private:
	uint8_t value;
};

struct DependencySubjectFlags : public DependencyFlags {
private:
	static constexpr uint8_t OWNERSHIP = 0;

public:
	DependencySubjectFlags &Apply(DependencySubjectFlags other) {
		Merge(other.Value());
		return *this;
	}

public:
	bool IsOwnership() const {
		return IsSet<OWNERSHIP>();
	}

public:
	DependencySubjectFlags &SetOwnership() {
		Set<OWNERSHIP>();
		return *this;
	}

public:
	string ToString() const override {
		string result;
		if (IsOwnership()) {
			result += "OWNS";
		}
		return result;
	}
};

struct DependencyDependentFlags : public DependencyFlags {
private:
	static constexpr uint8_t BLOCKING = 0;
	static constexpr uint8_t OWNED_BY = 1;
	static constexpr uint8_t ALTER_BLOCKING = 2;

public:
	DependencyDependentFlags() = default;
	explicit DependencyDependentFlags(uint8_t raw_value) {
		Merge(raw_value);
	}

public:
	DependencyDependentFlags &Apply(DependencyDependentFlags other) {
		Merge(other.Value());
		return *this;
	}

public:
	bool IsBlocking() const {
		return IsSet<BLOCKING>();
	}
	bool IsOwnedBy() const {
		return IsSet<OWNED_BY>();
	}
	//! Whether this dependency should block ALTER of the entry it depends on (independent of whether it blocks DROP)
	bool IsAlterBlocking() const {
		return IsSet<ALTER_BLOCKING>();
	}

public:
	DependencyDependentFlags &SetBlocking() {
		Set<BLOCKING>();
		return *this;
	}
	DependencyDependentFlags &SetOwnedBy() {
		Set<OWNED_BY>();
		return *this;
	}
	DependencyDependentFlags &SetAlterBlocking() {
		Set<ALTER_BLOCKING>();
		return *this;
	}

public:
	uint8_t RawValue() const {
		return Value();
	}

public:
	string ToString() const override {
		string result;
		if (IsBlocking()) {
			result += "REGULAR";
		} else {
			result += "AUTOMATIC";
		}
		result += " | ";
		if (IsOwnedBy()) {
			result += "OWNED BY";
		}
		result += " | ";
		if (IsAlterBlocking()) {
			result += "ALTER BLOCKING";
		}
		return result;
	}

public:
	void Serialize(Serializer &serializer) const;
	static DependencyDependentFlags Deserialize(Deserializer &deserializer);
};

enum class AlterTableType : uint8_t;

struct SubDependency {
public:
	AlterTableType alter {};
	Identifier name;

public:
	bool operator==(const SubDependency &other) const {
		return other.alter == alter && other.name == name;
	}

public:
	void Serialize(Serializer &serializer) const;
	static SubDependency Deserialize(Deserializer &deserializer);
};

struct SubDependencyHashFunction {
	uint64_t operator()(const SubDependency &a) const {
		return CombineHash(Hash<uint8_t>(static_cast<uint8_t>(a.alter)), a.name.Hash());
	}
};

using subdependency_set_t = unordered_set<SubDependency, SubDependencyHashFunction>;

struct CatalogEntryInfo {
public:
	CatalogType type;
	Identifier schema;
	Identifier name;
	//! The table that owns this entry, for entries that are not unique within their schema (triggers)
	Identifier table;

public:
	bool operator==(const CatalogEntryInfo &other) const {
		if (other.type != type) {
			return false;
		}
		if (other.schema != schema) {
			return false;
		}
		if (other.name != name) {
			return false;
		}
		if (other.table != table) {
			return false;
		}
		return true;
	}

public:
	void Serialize(Serializer &serializer) const;
	static CatalogEntryInfo Deserialize(Deserializer &deserializer);
};

struct Dependency {
	Dependency(CatalogEntry &entry, // NOLINT: Allow implicit conversion from `CatalogEntry`
	           DependencyDependentFlags flags = DependencyDependentFlags().SetBlocking())
	    : entry(entry), flags(std::move(flags)) {
	}

	//! The catalog entry this depends on
	reference<CatalogEntry> entry;
	//! The type of dependency
	DependencyDependentFlags flags;
};

struct DependencyHashFunction {
	uint64_t operator()(const Dependency &a) const {
		std::hash<void *> hash_func;
		return hash_func((void *)&a.entry.get());
	}
};

struct DependencyEquality {
	bool operator()(const Dependency &a, const Dependency &b) const {
		return RefersToSameObject(a.entry, b.entry);
	}
};
using dependency_set_t = unordered_set<Dependency, DependencyHashFunction, DependencyEquality>;

} // namespace duckdb
