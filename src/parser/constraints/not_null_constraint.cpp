#include "duckdb/parser/constraints/not_null_constraint.hpp"

#include "duckdb/common/helper.hpp"

namespace duckdb {

NotNullConstraint::NotNullConstraint(LogicalIndex index) : Constraint(ConstraintType::NOT_NULL), index(index) {
}

NotNullConstraint::~NotNullConstraint() {
}

string NotNullConstraint::ToString() const {
	return "NOT NULL";
}

unique_ptr<Constraint> NotNullConstraint::Copy() const {
	auto result = make_uniq<NotNullConstraint>(index);
	result->constraint_name = constraint_name;
	return std::move(result);
}

} // namespace duckdb
