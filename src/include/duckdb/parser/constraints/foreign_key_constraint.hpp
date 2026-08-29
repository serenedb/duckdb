//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/constraints/foreign_key_constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/constraint.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class ForeignKeyConstraint : public Constraint {
public:
	static constexpr const ConstraintType TYPE = ConstraintType::FOREIGN_KEY;

public:
	DUCKDB_API ForeignKeyConstraint(vector<Identifier> pk_columns, vector<Identifier> fk_columns, ForeignKeyInfo info);

	//! The set of main key table's columns
	vector<Identifier> pk_columns;
	//! The set of foreign key table's columns
	vector<Identifier> fk_columns;
	ForeignKeyInfo info;
	//! Identity of the relation this key references, assigned by the hosting catalog. `info.schema` / `info.table`
	//! name the same relation, but a name moves and an identity does not, so a catalog that keeps this definition
	//! on disk resolves the target through this. Zero means "none"; duckdb's own catalog never sets it.
	idx_t host_referenced_id = 0;
	//! Identities of the referenced relation's key columns, parallel to `pk_columns`, assigned by the hosting
	//! catalog. `pk_columns` names the same columns, but a rename on the referenced side does not reach this
	//! definition, so a catalog that keeps it on disk resolves the key through these. Empty means "none".
	vector<idx_t> host_pk_column_ids;

public:
	DUCKDB_API string ToString() const override;

	DUCKDB_API unique_ptr<Constraint> Copy() const override;

	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<Constraint> Deserialize(Deserializer &deserializer);

private:
	ForeignKeyConstraint();
};

} // namespace duckdb
