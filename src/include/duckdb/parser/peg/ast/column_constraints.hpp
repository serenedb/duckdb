#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/peg/ast/column_constraint_entry.hpp"

namespace duckdb {

struct ColumnConstraint {
	vector<unique_ptr<Constraint>> constraints;
	vector<ColumnConstraintTypeInfo> constraint_types; // Used to create proper constraints when column index is known
	unique_ptr<ParsedExpression> default_value;
	// PG-compat: tracked so TransformCreateTableColumnList can flag a
	// `PRIMARY KEY NULL`-style conflict at table-construction time.
	bool has_explicit_null = false;
	bool has_not_null = false;
	bool has_primary_key = false;
	// PG-compat: column declared DEFAULT more than once.
	bool has_duplicate_default = false;
};

} // namespace duckdb
