#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDeleteStatement(
    PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
    unique_ptr<BaseTableRef> target_opt_alias, optional<vector<unique_ptr<TableRef>>> delete_using_clause,
    optional<unique_ptr<ParsedExpression>> where_clause,
    optional<vector<unique_ptr<ParsedExpression>>> returning_clause) {
	auto result = make_uniq<DeleteStatement>();
	auto &node = *result->node;
	if (with_clause && !with_clause->map.empty()) {
		node.cte_map = std::move(*with_clause);
	}
	node.table = std::move(target_opt_alias);
	if (delete_using_clause) {
		node.using_clauses = std::move(*delete_using_clause);
	}
	if (where_clause) {
		node.condition = std::move(*where_clause);
	}
	if (returning_clause) {
		node.returning_list = std::move(*returning_clause);
	}
	return std::move(result);
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformTargetOptAlias(PEGTransformer &transformer,
                                                                        unique_ptr<BaseTableRef> base_table_name,
                                                                        const optional<Identifier> &target_alias) {
	if (target_alias && !target_alias->empty()) {
		base_table_name->alias = Identifier(*target_alias);
	}
	return base_table_name;
}

Identifier PEGTransformerFactory::TransformTargetAlias(PEGTransformer &transformer, const bool &has_result,
                                                       const Identifier &col_id) {
	return col_id;
}

vector<unique_ptr<TableRef>> PEGTransformerFactory::TransformDeleteUsingClause(PEGTransformer &transformer,
                                                                               vector<unique_ptr<TableRef>> table_ref) {
	return table_ref;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformTruncateStatement(
    PEGTransformer &transformer, const bool &has_result, vector<unique_ptr<BaseTableRef>> truncate_target,
    const optional<bool> &truncate_identity_clause, const optional<bool> &drop_behavior) {
	const bool cascade = drop_behavior.value_or(false);
	const bool restart_identity = truncate_identity_clause.value_or(false);
	auto build_one = [&](unique_ptr<TableRef> table) -> unique_ptr<DeleteStatement> {
		auto del = make_uniq<DeleteStatement>();
		del->node->table = std::move(table);
		del->node->is_truncate = true;
		del->node->truncate_cascade = cascade;
		del->node->truncate_restart_identity = restart_identity;
		for (auto &target : truncate_target) {
			del->node->truncate_group.push_back(target->Copy());
		}
		return del;
	};

	if (truncate_target.size() == 1 && !cascade && !restart_identity) {
		return build_one(truncate_target[0]->Copy());
	}

	auto multi = make_uniq<MultiStatement>();
	for (idx_t i = 0; i < truncate_target.size(); i++) {
		multi->statements.push_back(build_one(truncate_target[i]->Copy()));
	}
	return std::move(multi);
}

// TruncateTarget <- TruncateOnly? BaseTableName TruncateStar?
// ONLY and * are accepted but discarded.
unique_ptr<BaseTableRef> PEGTransformerFactory::TransformTruncateTarget(PEGTransformer &transformer,
                                                                        const optional<bool> &truncate_only,
                                                                        unique_ptr<BaseTableRef> base_table_name,
                                                                        const optional<bool> &truncate_star) {
	return base_table_name;
}

bool PEGTransformerFactory::TransformTruncateOnly(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformTruncateStar(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformTruncateRestart(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformTruncateContinue(PEGTransformer &transformer) {
	return false;
}

} // namespace duckdb
