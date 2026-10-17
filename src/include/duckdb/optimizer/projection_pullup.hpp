#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

class Optimizer;
class LogicalOperator;

class ProjectionPullup : public LogicalOperatorVisitor {
public:
	explicit ProjectionPullup(Optimizer &optimizer_p, unique_ptr<LogicalOperator> &root)
	    : optimizer(optimizer_p), root(root), path(own_path) {
	}

	void Optimize(unique_ptr<LogicalOperator> &op);
	void VisitOperator(unique_ptr<LogicalOperator> &op) override;

private:
	ProjectionPullup(Optimizer &optimizer_p, unique_ptr<LogicalOperator> &root,
	                 vector<reference<unique_ptr<LogicalOperator>>> &path_p)
	    : optimizer(optimizer_p), root(root), path(path_p) {
	}

	Optimizer &optimizer;
	unique_ptr<LogicalOperator> &root;
	vector<reference<LogicalOperator>> parents;
	vector<reference<unique_ptr<LogicalOperator>>> own_path;
	vector<reference<unique_ptr<LogicalOperator>>> &path;
	optional_ptr<LogicalOperator> known_output_of;
	optional_ptr<const vector<ColumnBinding>> known_output;
	optional_ptr<LogicalOperator> FindParent(LogicalOperator &target, LogicalOperator &current);
	void PopParents(const LogicalOperator &op);
	void VisitPathOperator(unique_ptr<LogicalOperator> &op);
	void OptimizeBelow(unique_ptr<LogicalOperator> &projection,
	                   optional_ptr<const vector<ColumnBinding>> child_bindings);
	void ReplaceAbove(ColumnBindingReplacer &replacer, LogicalOperator &replaced);
	void InsertProjectionBelowOp(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &child,
	                             bool reads_by_position);
	void CanPullThrough(column_binding_map_t<unique_ptr<Expression>> &projection_map, bool &can_pull_through);
	void PullUpColrefProjection(unique_ptr<LogicalOperator> &op, LogicalProjection &proj,
	                            vector<ColumnBinding> &proj_bindings);
	void PullUpNonColrefProjection(unique_ptr<LogicalOperator> &op, LogicalProjection &proj,
	                               vector<ColumnBinding> &proj_bindings, idx_t pull_up_to_here);
};

} // namespace duckdb
