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

	//! Alias was synthesized (e.g. by a replacement scan from a file name) rather
	//! than written by the user; PG-style single-column alias renaming must skip it
	bool implicit_alias = false;

public:
	string ToString() const override;

	bool Equals(const TableRef &other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Deserializes a blob back into a BaseTableRef
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableRef> Deserialize(Deserializer &source);
};
} // namespace duckdb
