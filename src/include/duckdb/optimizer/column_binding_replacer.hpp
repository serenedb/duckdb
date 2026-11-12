//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/column_binding_replacer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

#include <functional>

namespace duckdb {

struct CorrelatedColumns;

class BoundColumnRefExpression;
class BoundSubqueryExpression;
class ColumnBindingReplacer;

struct ReplacementBinding {
public:
	ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding);
	ReplacementBinding(ColumnBinding old_binding, ColumnBinding new_binding, LogicalType new_type);

public:
	ColumnBinding old_binding;
	ColumnBinding new_binding;

	bool replace_type;
	LogicalType new_type;
};

//! A validated graph of direct binding replacement edges.
//! Resolution follows these edges transitively and is independent of insertion order.
class BindingReplacementGraph {
public:
	ColumnBinding Resolve(ColumnBinding binding) const;
	optional_ptr<const ReplacementBinding> Find(ColumnBinding old_binding) const;
	bool TryAdd(const ReplacementBinding &replacement);
	void Add(ColumnBinding old_binding, ColumnBinding new_binding);
	void Add(const ReplacementBinding &replacement);
	void Merge(const BindingReplacementGraph &replacements);
	void AddTo(ColumnBindingReplacer &replacer) const;

	bool Empty() const {
		return replacement_bindings.empty();
	}
	vector<ReplacementBinding>::const_iterator begin() const {
		return replacement_bindings.begin();
	}
	vector<ReplacementBinding>::const_iterator end() const {
		return replacement_bindings.end();
	}

private:
	ReplacementBinding ResolveReplacement(ColumnBinding binding) const;
	vector<ReplacementBinding> replacement_bindings;
	column_binding_map_t<idx_t> replacement_positions;
};

//! The ColumnBindingReplacer updates column bindings (e.g., after changing the operator plan), utility for optimizers
class ColumnBindingReplacer : public LogicalOperatorVisitor {
public:
	ColumnBindingReplacer();

public:
	//! Update each operator of the plan
	void VisitOperator(LogicalOperator &op) override;
	//! Update bindings owned by this operator without visiting its children
	virtual void VisitOperatorBindings(LogicalOperator &op);
	void VisitExpression(unique_ptr<Expression> *expression) override;
	//! Add binding replacements by position
	void AddReplacements(const vector<ColumnBinding> &old_bindings, const vector<ColumnBinding> &new_bindings);

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;

	class IndexScope {
	public:
		explicit IndexScope(ColumnBindingReplacer &replacer);
		~IndexScope();

	private:
		ColumnBindingReplacer &replacer;
	};

	template <class F>
	void ApplyReplacements(ColumnBinding binding, F &&apply);

public:
	//! Do not recurse further than this operator (optional)
	optional_ptr<LogicalOperator> stop_operator;

	//! Contains all bindings that need to be updated
	vector<ReplacementBinding> replacement_bindings;

private:
	void BuildReplacementIndex();

	idx_t index_depth = 0;
	idx_t scope_lookups = 0;
	bool index_built = false;
	column_binding_map_t<idx_t> first_replacement;
	vector<idx_t> next_replacement;
};

//! Like ColumnBindingReplacer, but also updates correlated-column metadata and nested subquery plans.
class CorrelatedColumnBindingReplacer : public ColumnBindingReplacer {
public:
	void VisitOperatorBindings(LogicalOperator &op) override;

protected:
	void ReplaceCorrelatedColumns(CorrelatedColumns &columns);
	using ColumnBindingReplacer::VisitReplace;
	unique_ptr<Expression> VisitReplace(BoundSubqueryExpression &expr, unique_ptr<Expression> *expr_ptr) override;
};

//! Applies binding replacements together with their projection-layout invariants.
class ColumnBindingRewrite {
public:
	//! Rewrite a child, preserving the parent's explicit and implicit projection selections and updating its bindings.
	//! The callback may replace or wrap the child and must record its binding replacements in the supplied graph.
	//! Callers must still rewrite affected ancestors and resolve operator types after the complete transformation.
	static void
	RewriteChild(unique_ptr<LogicalOperator> &op, idx_t child_index,
	             const std::function<void(unique_ptr<LogicalOperator> &, BindingReplacementGraph &)> &rewrite);
	//! Apply the output-boundary view of a complete replacement graph to one parent-child edge.
	static void ApplyToChild(unique_ptr<LogicalOperator> &op, idx_t child_index,
	                         vector<ColumnBinding> old_child_bindings, const BindingReplacementGraph &replacements);
	static vector<ColumnBinding> CaptureChildBindings(LogicalOperator &op, idx_t child_index);
	static void ApplyToOperatorBindings(LogicalOperator &op, const BindingReplacementGraph &replacements);
	//! Verify that every previous public output binding is still reachable at the rewritten output boundary.
	static void ValidateOutput(const vector<ColumnBinding> &old_output, const vector<ColumnBinding> &new_output,
	                           const BindingReplacementGraph &replacements);

private:
	static void RemapProjectionMapStrict(vector<ProjectionIndex> &projection_map,
	                                     const vector<ColumnBinding> &original_child_bindings,
	                                     const vector<ColumnBinding> &child_bindings_before,
	                                     const vector<ColumnBinding> &child_bindings_after);
};

//! Replaces bindings in the ancestors of a replaced subtree, which the caller visits bottom-up
class AncestorBindingReplacer {
public:
	explicit AncestorBindingReplacer(ColumnBindingReplacer &replacer);

	//! Update the bindings owned by the ancestor and return whether its output still carries a new binding
	bool Replace(LogicalOperator &ancestor);
	//! Verify that no operator outside the replaced subtree still references a replaced binding
	void Verify(LogicalOperator &root, LogicalOperator &replaced_subtree) const;

private:
	ColumnBindingReplacer &replacer;
	column_binding_set_t new_bindings;
};

} // namespace duckdb
