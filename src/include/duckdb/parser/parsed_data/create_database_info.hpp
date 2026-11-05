//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_database_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

struct DatabaseSetting {
	idx_t role = 0;
	vector<string> config;

	bool operator==(const DatabaseSetting &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static DatabaseSetting Deserialize(Deserializer &deserializer);
};

struct CreateDatabaseInfo : public CreateInfo {
	CreateDatabaseInfo();

	case_insensitive_map_t<Value> options;
	vector<DatabaseSetting> settings;
	int32_t connection_limit = -1;
	bool allow_connections = true;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
