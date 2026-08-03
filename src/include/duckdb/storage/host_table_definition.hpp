//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/host_table_definition.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

//! A storage table as the catalog that owns it describes it: what this file would otherwise have kept a
//! CreateInfo record for. The indexes come with it, because an index cannot attach to a table that does not
//! exist yet and the host is the only place their definitions live.
struct HostTableDefinition {
	unique_ptr<CreateTableInfo> table;
	vector<unique_ptr<CreateIndexInfo>> indexes;
};

} // namespace duckdb
