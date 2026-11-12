//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/constraints/bound_check_constraint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/constraints/check_constraint.hpp"
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
	explicit BoundCheckConstraint(const CheckConstraint &source)
	    : BoundConstraint(ConstraintType::CHECK), source(source) {
	}

	//! The expression
	unique_ptr<Expression> expression;
	//! The columns used by the CHECK constraint
	physical_index_set_t bound_columns;
	const CheckConstraint &source;

public:
	unique_ptr<BoundConstraint> Copy() const override {
		auto result = make_uniq<BoundCheckConstraint>(source);
		result->expression = expression->Copy();
		result->bound_columns = bound_columns;
		return std::move(result);
	}
};

} // namespace duckdb
