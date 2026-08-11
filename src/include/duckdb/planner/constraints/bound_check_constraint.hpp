//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/constraints/bound_check_constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/bound_constraint.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/index_map.hpp"

namespace duckdb {

//! The CheckConstraint contains an expression that must evaluate to TRUE for
//! every row in a table
class BoundCheckConstraint : public BoundConstraint {
public:
	static constexpr const ConstraintType TYPE = ConstraintType::CHECK;

public:
	BoundCheckConstraint() : BoundConstraint(ConstraintType::CHECK) {
	}

	//! The expression
	unique_ptr<Expression> expression;
	//! The columns used by the CHECK constraint
	physical_index_set_t bound_columns;
	//! SereneDB: set when this is a Row-Level Security WITH CHECK constraint, so a
	//! violation reports the RLS-policy error instead of the generic CHECK error.
	bool is_rls = false;

public:
	unique_ptr<BoundConstraint> Copy() const override {
		auto result = make_uniq<BoundCheckConstraint>();
		result->expression = expression->Copy();
		result->bound_columns = bound_columns;
		result->is_rls = is_rls;
		return std::move(result);
	}
};

} // namespace duckdb
