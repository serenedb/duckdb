//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/table_function_ref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/enums/ordinality_request_type.hpp"

namespace duckdb {
class TableFunction;

//! Represents a Table producing function
class TableFunctionRef : public TableRef {
public:
	static constexpr const TableReferenceType TYPE = TableReferenceType::TABLE_FUNCTION;

public:
	DUCKDB_API TableFunctionRef();

	unique_ptr<ParsedExpression> function;

	// if the function takes a subquery as argument its in here
	unique_ptr<SelectStatement> subquery;

	//! Whether or not WITH ORDINALITY has been invoked
	OrdinalityType with_ordinality = OrdinalityType::WITHOUT_ORDINALITY;
	//! A function bound as-is instead of being looked up in the catalog by name, for statements built in C++
	//! (never parsed or serialized). It takes no arguments; `function` still names it for errors and EXPLAIN.
	shared_ptr<TableFunction> inline_function;

public:
	string ToString() const override;

	bool Equals(const TableRef &other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Deserializes a blob back into a BaseTableRef
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableRef> Deserialize(Deserializer &source);
};
} // namespace duckdb
