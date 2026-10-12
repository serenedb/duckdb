//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_insert.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/planner/bound_constraint.hpp"

namespace duckdb {
struct CreateInfo;
class TableCatalogEntry;

class Index;

//! LogicalInsert represents an insertion of data into a base table
class LogicalInsert : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_INSERT;

public:
	LogicalInsert(TableCatalogEntry &table, TableIndex table_index);

	vector<vector<unique_ptr<Expression>>> insert_values;
	//! Deprecated: The insertion map ([table_index -> index in result, or DConstants::INVALID_INDEX if not specified])
	physical_index_vector_t<idx_t> column_index_map;
	//! The expected types for the INSERT statement (obtained from the column types)
	vector<LogicalType> expected_types;
	//! The base table to insert into
	TableCatalogEntry &table;
	TableIndex table_index;
	//! if returning option is used, return actual chunk to projection
	bool return_chunk;
	//! The default statements used by the table
	vector<unique_ptr<Expression>> bound_defaults;
	//! The constraints used by the table
	vector<unique_ptr<BoundConstraint>> bound_constraints;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

protected:
	vector<ColumnBinding> GetColumnBindings() override;
	void ResolveTypes() override;

	idx_t EstimateCardinality(ClientContext &context) override;
	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

private:
	LogicalInsert(ClientContext &context, const unique_ptr<CreateInfo> table_info);
};
} // namespace duckdb
