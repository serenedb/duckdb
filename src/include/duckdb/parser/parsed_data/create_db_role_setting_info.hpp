//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_db_role_setting_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

struct CreateDbRoleSettingInfo : public CreateInfo {
	CreateDbRoleSettingInfo();

	idx_t database = 0;
	idx_t role = 0;
	vector<string> config;

public:
	DUCKDB_API static Identifier NameOf(idx_t database, idx_t role);

	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
};

} // namespace duckdb
