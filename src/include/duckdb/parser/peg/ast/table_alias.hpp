#pragma once
#include "duckdb/common/common.hpp"

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types.hpp"
namespace duckdb {
struct TableAlias {
	Identifier name;
	vector<Identifier> column_name_alias;
	vector<LogicalType> column_types;
};
} // namespace duckdb
