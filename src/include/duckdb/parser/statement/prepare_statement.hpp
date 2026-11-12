//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/statement/prepare_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class PrepareStatement : public SQLStatement {
public:
	static constexpr const StatementType TYPE = StatementType::PREPARE_STATEMENT;

public:
	PrepareStatement();

	unique_ptr<SQLStatement> statement;
	Identifier name;
	//! Type-only per-parameter hints (e.g. PG protocol Parse OIDs): they pin a parameter's bind type without
	//! constant-folding it, so the slot is re-bound at execute
	case_insensitive_map_t<LogicalType> parameter_type_hints;

protected:
	PrepareStatement(const PrepareStatement &other);

public:
	unique_ptr<SQLStatement> Copy() const override;
	string ToString() const override;
};
} // namespace duckdb
