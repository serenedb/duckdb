//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_tablespace_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"

namespace duckdb {

struct AlterTablespaceInfo : public AlterInfo {
	AlterTablespaceInfo(AlterEntryData data, Identifier new_name_p)
	    : AlterInfo(AlterType::ALTER_TABLESPACE, std::move(data.qualified_name), data.if_not_found),
	      new_name(std::move(new_name_p)) {
	}

	Identifier new_name;

public:
	CatalogType GetCatalogType() const override {
		return CatalogType::TABLESPACE_ENTRY;
	}
	optional_ptr<const Identifier> GetNewName() const override {
		return &new_name;
	}
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	AlterTablespaceInfo() : AlterInfo(AlterType::ALTER_TABLESPACE) {
	}
};

} // namespace duckdb
