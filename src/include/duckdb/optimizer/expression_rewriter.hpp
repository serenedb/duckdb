//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/expression_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/optimizer/rule.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class ClientContext;

//! The ExpressionRewriter performs a set of fixed rewrite rules on the expressions that occur in a SQL statement
class ExpressionRewriter : public LogicalOperatorVisitor {
public:
	explicit ExpressionRewriter(ClientContext &context) : context(context) {
	}

public:
	//! The set of rules as known by the Expression Rewriter
	vector<unique_ptr<Rule>> rules;

	ClientContext &context;

public:
	void VisitOperator(LogicalOperator &op) override;
	void VisitExpression(unique_ptr<Expression> *expression) override;

	// Generates either a constant_or_null(child) expression
	static unique_ptr<Expression> ConstantOrNull(ClientContext &context, unique_ptr<Expression> child, Value value);
	static unique_ptr<Expression> ConstantOrNull(ClientContext &context, vector<unique_ptr<Expression>> children,
	                                             Value value);

	optional_ptr<Expression> CurrentParent() const {
		return current_parent;
	}
	bool ChildThrows() const {
		return child_throws;
	}
	void MarkThrows() {
		throws = true;
	}
	bool ChildNotFoldable() const {
		return child_not_foldable;
	}
	void MarkNotFoldable() {
		not_foldable = true;
	}

private:
	struct RuleSet {
		vector<reference<Rule>> rules;
		identifier_map_t<vector<reference<Rule>>> function_rules;
	};

	//! Apply a set of rules to a specific expression
	unique_ptr<Expression> ApplyRules(LogicalOperator &op, const vector<rewrite_barrier_t> &barriers,
	                                  unique_ptr<Expression> expr, bool &changes_made, bool is_root = false);
	const vector<reference<Rule>> &RulesFor(const Expression &expr);
	vector<reference<Rule>> SelectRules(ExpressionClass expression_class, ExpressionType expression_type,
	                                    optional_ptr<const Identifier> function_name) const;

	optional_ptr<LogicalOperator> op;
	optional_ptr<Expression> current_parent;
	bool child_throws = false;
	bool throws = false;
	bool child_not_foldable = false;
	bool not_foldable = false;
	vector<rewrite_barrier_t> to_apply_barriers;
	unordered_map<uint16_t, RuleSet> rule_sets;
	idx_t rule_sets_rule_count = 0;
};

} // namespace duckdb
