//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_role_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/permissions.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

struct CreateRoleInfo : public CreateInfo {
	CreateRoleInfo();

	RoleOption options = RoleOption::None;
	int32_t conn_limit = -1;
	int64_t valid_until = 0;
	string password;
	vector<Membership> member_of;
	vector<string> config;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
