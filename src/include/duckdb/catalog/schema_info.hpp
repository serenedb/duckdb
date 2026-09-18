//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/schema_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

class SchemaInfo {
public:
	SchemaInfo(idx_t oid, Identifier name) : oid(oid), name(std::move(name)) {
	}

	const idx_t oid;
	const Identifier name;
};

} // namespace duckdb
