#pragma once
#include "duckdb/common/common.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/constraint.hpp"

namespace duckdb {
struct GeneratedColumnDefinition {
	unique_ptr<ParsedExpression> expr;
	bool virtual_column = false;
	bool default_column = false;
};

struct ConstraintColumnDefinition {
	ColumnDefinition column_definition;
	vector<pair<bool, ConstraintType>> constraint_types;
	vector<unique_ptr<Constraint>> constraints;
	// PG-compat: explicit `CONSTRAINT <name>` not consumed by an object constraint
	// (CHECK/FK); carried so the CreateTable transformer names the PK/UNIQUE/NOT NULL
	// it materializes from the column flags.
	string constraint_name;
};
} // namespace duckdb
