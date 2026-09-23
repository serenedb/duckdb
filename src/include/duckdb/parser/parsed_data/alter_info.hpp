//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {
class LogicalDependencyList;

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
	ALTER_SCHEMA = 10,
	ALTER_INDEX = 202,
	REPLACE_DEFINITION = 203
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
	virtual optional_ptr<const Identifier> GetNewName() const {
		return nullptr;
	}

	AlterEntryData GetAlterEntryData() const;
	bool IsAddPrimaryKey() const;
	bool IsAddUniqueConstraint() const;

protected:
	explicit AlterInfo(AlterType type);

	//! Qualified name of the entry to alter (catalog.schema.name)
	QualifiedName qualified_name;
};

string RenameEntryToString(CatalogType entry_type, const string &target, OnEntryNotFound if_not_found,
                           const Identifier &new_name);

template <class DERIVED, class BASE, auto RENAME_TYPE>
struct RenameEntryInfo : public BASE {
	RenameEntryInfo(const AlterEntryData &data, Identifier new_name_p)
	    : BASE(RENAME_TYPE, data), new_name(std::move(new_name_p)) {
	}

	Identifier new_name;

public:
	optional_ptr<const Identifier> GetNewName() const override {
		return &new_name;
	}
	unique_ptr<AlterInfo> Copy() const override {
		return make_uniq_base<AlterInfo, DERIVED>(this->GetAlterEntryData(), new_name);
	}
	string ToString() const override {
		return RenameEntryToString(this->GetCatalogType(),
		                           this->GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA),
		                           this->if_not_found, new_name);
	}

protected:
	RenameEntryInfo() : BASE(RENAME_TYPE) {
	}
};

} // namespace duckdb
