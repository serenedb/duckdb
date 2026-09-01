//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/catalog/dependency_list.hpp"

#include "duckdb/catalog/catalog_permissions.hpp"

namespace duckdb {

enum class AlterType : uint8_t {
	INVALID = 0,
	ALTER_TABLE = 1,
	ALTER_VIEW = 2,
	ALTER_SEQUENCE = 3,
	CHANGE_OWNERSHIP = 4,
	ALTER_SCALAR_FUNCTION = 5,
	ALTER_TABLE_FUNCTION = 6,
	SET_COMMENT = 7,
	SET_COLUMN_COMMENT = 8,
	ALTER_DATABASE = 9,
	SET_PERMISSIONS = 10,
	ALTER_SCHEMA = 11
};

//! GRANT, REVOKE and OWNER TO, for a catalog that models postgres' owner + ACL on its entries.
enum class PermissionsAlterType : uint8_t {
	INVALID = 0,
	GRANT_PRIVILEGES = 1,
	REVOKE_PRIVILEGES = 2,
	CHANGE_ROLE_OWNER = 3,
	//! The whole definition was replaced, owner and ACL with it. Nothing a dependent bound against moves.
	REPLACE_DEFINITION = 4
};

enum class AlterBindMode { BIND_ON_ALTER, SKIP_BINDING };

struct AlterEntryData {
	AlterEntryData() {
	}
	AlterEntryData(QualifiedName qualified_name_p, OnEntryNotFound if_not_found)
	    : qualified_name(std::move(qualified_name_p)), if_not_found(if_not_found) {
	}

	const QualifiedName &GetQualifiedName() const {
		return qualified_name;
	}

	QualifiedName qualified_name;
	OnEntryNotFound if_not_found;
};

struct AlterInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::ALTER_INFO;

public:
	AlterInfo(AlterType type, QualifiedName name, OnEntryNotFound if_not_found);
	~AlterInfo() override;

	AlterType type;
	//! The identifier the catalog that owns this entry knows it by, zero when it owns none. A rename cannot reach
	//! it, so it is what a replay resolves the target by -- the name in this record is the one at the time of the
	//! alter, and the owning catalog has since moved on. Same role it serves on CreateInfo.
	idx_t oid = 0;
	//! if exists
	OnEntryNotFound if_not_found;
	//! Allow altering internal entries
	bool allow_internal;
	//! Determine whether to skip Bind
	AlterBindMode bind_mode = AlterBindMode::BIND_ON_ALTER;
	//! New dependencies for the altered entry (set during binding)
	unique_ptr<LogicalDependencyList> new_dependencies;

public:
	const QualifiedName &GetQualifiedName() const {
		return qualified_name;
	}
	QualifiedName &GetQualifiedNameMutable() {
		return qualified_name;
	}
	void SetQualifiedName(QualifiedName name) {
		qualified_name = std::move(name);
	}
	void SetQualifiedName(Identifier catalog, Identifier schema, Identifier name) {
		qualified_name = QualifiedName(std::move(catalog), std::move(schema), std::move(name));
	}
	void SetName(Identifier name) {
		qualified_name = qualified_name.WithName(std::move(name));
	}

public:
	virtual CatalogType GetCatalogType() const = 0;
	virtual unique_ptr<AlterInfo> Copy() const = 0;
	virtual string ToString() const = 0;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);

	virtual Identifier GetColumnName() const {
		return Identifier();
	};

	//! Whether this alter leaves a dependent of the given kind unusable, so the dependency manager must refuse it.
	//! Answered by the alters that have an exception; every other alter breaks whatever depended on the old shape.
	virtual bool BreaksDependent(CatalogType dependent_type) const {
		return true;
	}
	//! Whether a dependent that resolves its references at use, rather than binding them once, simply re-resolves
	//! after this alter -- a rename or a dropped piece degrades such a dependent instead of invalidating it.
	//! Only consulted for a catalog whose dependents work that way (Catalog::DependentsResolveByName).
	virtual bool DependentCanRebind() const {
		return false;
	}

	//! Whether the statement's grammar is the one every relation kind shares: a rename, and SET / RESET of storage
	//! options, arrive typed as a relation whether the statement said TABLE, VIEW or INDEX, so the kind stated here
	//! does not say which kind holds the name.
	bool TargetsSharedRelationGrammar() const;

	AlterEntryData GetAlterEntryData() const;
	//! ADD PRIMARY KEY or ADD UNIQUE: the constraint is backed by an index, so the ALTER has to build one over the
	//! existing rows. Without it the constraint is recorded and never rejects anything.
	bool IsAddIndexedConstraint() const;

protected:
	explicit AlterInfo(AlterType type);

	//! Qualified name of the entry to alter (catalog.schema.name)
	QualifiedName qualified_name;
};

//! A change to an entry's owner or its access control list. The privileges themselves stay with the catalog
//! implementation that models them: it hands CatalogSet::AlterEntry the replacement entry, and this record is
//! what the undo buffer and any log carry about the change.
struct SetPermissionsInfo : public AlterInfo {
	SetPermissionsInfo(PermissionsAlterType permissions_alter_type, CatalogType entry_catalog_type, QualifiedName name,
	                   CatalogPermissions permissions);

	PermissionsAlterType permissions_alter_type;
	CatalogType entry_catalog_type;
	//! The owner and grants the entry carries after this alter. Whole rather than a delta: a version states what it
	//! is, the way a rename states the name rather than the edit that produced it.
	CatalogPermissions permissions;

public:
	CatalogType GetCatalogType() const override;
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	//! An owner or ACL change leaves the shape a dependent bound against untouched.
	bool BreaksDependent(CatalogType dependent_type) const override {
		return false;
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	SetPermissionsInfo();
};

} // namespace duckdb
