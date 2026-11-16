//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/statement/drop_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class DropStatement : public SQLStatement {
public:
	static constexpr const StatementType TYPE = StatementType::DROP_STATEMENT;

public:
	DropStatement();
	explicit DropStatement(unique_ptr<DropInfo> info);

	vector<unique_ptr<DropInfo>> infos;

protected:
	DropStatement(const DropStatement &other);

public:
	unique_ptr<SQLStatement> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
