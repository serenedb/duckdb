//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_schema_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"

#include "duckdb/common/identifier.hpp"

namespace duckdb {

//! ALTER SCHEMA ... RENAME TO. The renamed entry carries the same catalog sets, so nothing the schema holds is
//! rebuilt or bound again.
struct RenameSchemaInfo : public AlterInfo {
public:
	RenameSchemaInfo();
	RenameSchemaInfo(Identifier schema_p, Identifier new_name_p, OnEntryNotFound if_not_found);

	Identifier new_name;

public:
	CatalogType GetCatalogType() const override;
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	void Serialize(Serializer &serializer) const override;
};

} // namespace duckdb
