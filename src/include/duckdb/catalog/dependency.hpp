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
	uint8_t Value() {
		return value;
	}

private:
	uint8_t value;
};

//! Which piece of a dependent binds its subject: the sub-object a cascade can trim so the dependent
//! survives, where dropping it whole is the only alternative. NONE means the whole entry is the binding.
enum class DependencyPieceKind : uint8_t { NONE = 0, COLUMN_TYPE = 1, COLUMN_DEFAULT = 2, CHECK = 3, FOREIGN_KEY = 4 };

struct DependencyPiece {
	DependencyPieceKind kind = DependencyPieceKind::NONE;
	//! Host identifier of the sub-object (a column or a constraint); opaque to duckdb
	idx_t sub_object = DConstants::INVALID_INDEX;

	bool operator==(const DependencyPiece &other) const {
		return kind == other.kind && sub_object == other.sub_object;
	}
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

public:
	DependencyDependentFlags &SetBlocking() {
		Set<BLOCKING>();
		return *this;
	}
	DependencyDependentFlags &SetOwnedBy() {
		Set<OWNED_BY>();
		return *this;
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
		return result;
	}
};

struct CatalogEntryInfo {
public:
	CatalogType type;
	Identifier schema;
	Identifier name;
	//! Empty means the dependency manager's own catalog, which is what every
	//! dependency recorded before cross-catalog support looked like.
	Identifier catalog;
	//! When non-zero, the subject is addressed by this stable id alone -- the
	//! identity space CreateInfo::oid and CatalogEntry::oid share -- and the
	//! name-keyed fields do not participate in identity.
	idx_t oid = 0;

public:
	bool operator==(const CatalogEntryInfo &other) const {
		if (oid != 0 || other.oid != 0) {
			return oid == other.oid;
		}
		if (other.type != type) {
			return false;
		}
		if (other.schema != schema) {
			return false;
		}
		if (other.name != name) {
			return false;
		}
		if (other.catalog != catalog) {
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
