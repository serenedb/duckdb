//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_foreign_server_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

struct CreateForeignServerInfo : public CreateInfo {
	CreateForeignServerInfo();

	string server_type;
	string version;
	string fdw_name;
	case_insensitive_map_t<string> options;

public:
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
