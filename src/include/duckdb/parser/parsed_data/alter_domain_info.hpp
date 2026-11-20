//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_domain_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/domain_info.hpp"

namespace duckdb {

enum class AlterDomainType : uint8_t {
	SET_DEFAULT = 0,
	DROP_DEFAULT = 1,
	SET_NOT_NULL = 2,
	DROP_NOT_NULL = 3,
	ADD_CONSTRAINT = 4,
	DROP_CONSTRAINT = 5,
	RENAME_CONSTRAINT = 6,
	VALIDATE_CONSTRAINT = 7,
	RENAME = 8
};

struct AlterDomainInfo : public AlterInfo {
	AlterDomainInfo(AlterDomainType alter_domain_type_p, AlterEntryData data)
	    : AlterInfo(AlterType::ALTER_DOMAIN, std::move(data.qualified_name), data.if_not_found),
	      alter_domain_type(alter_domain_type_p) {
	}
	~AlterDomainInfo() override = default;

	AlterDomainType alter_domain_type;
	//! SET DEFAULT: the new default expression
	unique_ptr<ParsedExpression> default_value;
	//! ADD CONSTRAINT: the constraint to add; validated is false for NOT VALID
	DomainConstraint constraint;
	//! DROP / RENAME / VALIDATE CONSTRAINT: the constraint the action applies to
	string constraint_name;
	//! RENAME CONSTRAINT: the new constraint name
	string new_constraint_name;
	//! DROP CONSTRAINT: IF EXISTS
	bool constraint_if_exists = false;
	//! DROP CONSTRAINT: CASCADE
	bool cascade = false;
	//! RENAME: the new domain name
	Identifier new_name;

public:
	CatalogType GetCatalogType() const override {
		return CatalogType::TYPE_ENTRY;
	}
	optional_ptr<const Identifier> GetNewName() const override {
		return alter_domain_type == AlterDomainType::RENAME ? &new_name : nullptr;
	}
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	AlterDomainInfo() : AlterInfo(AlterType::ALTER_DOMAIN), alter_domain_type(AlterDomainType::SET_DEFAULT) {
	}
};

} // namespace duckdb
