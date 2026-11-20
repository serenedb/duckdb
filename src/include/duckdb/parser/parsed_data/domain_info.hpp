#pragma once

#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
//! A NOT NULL or CHECK constraint of a domain
struct DomainConstraint {
	//! The constraint name, given or generated (<domain>_check, <domain>_not_null)
	string name;
	//! NOT_NULL or CHECK
	ConstraintType type = ConstraintType::INVALID;
	//! The CHECK expression over VALUE; empty for NOT NULL
	unique_ptr<ParsedExpression> expression;
	//! False after ADD ... NOT VALID until VALIDATE CONSTRAINT
	bool validated = true;

	DomainConstraint Copy() const;
	void Serialize(Serializer &serializer) const;
	static DomainConstraint Deserialize(Deserializer &deserializer);
};

//! What CREATE DOMAIN adds to a type over its base type
struct DomainInfo {
	//! DEFAULT expression, if any
	unique_ptr<ParsedExpression> default_value;
	//! COLLATE name, empty if none
	string collation;
	//! NOT NULL and CHECK constraints in declaration order
	vector<DomainConstraint> constraints;
	LogicalType base_type;

	//! Whether a NOT NULL constraint is present (pg_type.typnotnull)
	bool IsNotNull() const;

	unique_ptr<DomainInfo> Copy() const;
	void Serialize(Serializer &serializer) const;
	static unique_ptr<DomainInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
