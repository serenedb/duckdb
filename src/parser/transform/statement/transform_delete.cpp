#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<DeleteStatement> Transformer::TransformDelete(duckdb_libpgquery::PGDeleteStmt &stmt) {
	auto result = make_uniq<DeleteStatement>();
	auto &node = *result->node;
	if (stmt.withClause) {
		TransformCTE(*PGPointerCast<duckdb_libpgquery::PGWithClause>(stmt.withClause), node.cte_map);
	}

	node.condition = TransformExpression(stmt.whereClause);
	node.table = TransformRangeVar(*stmt.relation);
	if (node.table->type != TableReferenceType::BASE_TABLE) {
		throw InvalidInputException("Can only delete from base tables!");
	}
	if (stmt.usingClause) {
		for (auto n = stmt.usingClause->head; n != nullptr; n = n->next) {
			auto target = PGPointerCast<duckdb_libpgquery::PGNode>(n->data.ptr_value);
			auto using_entry = TransformTableRefNode(*target);
			node.using_clauses.push_back(std::move(using_entry));
		}
	}

	if (stmt.returningList) {
		TransformExpressionList(*stmt.returningList, node.returning_list);
	}

	return result;
}

unique_ptr<SQLStatement> Transformer::TransformTruncate(duckdb_libpgquery::PGTruncateStmt &stmt) {
	// Lower TRUNCATE to one DeleteStatement per listed relation, with the
	// `is_truncate` flag set on the DeleteQueryNode so the binder can carry
	// the user's intent (TRUNCATE != DELETE-without-WHERE; different
	// transactional semantics) all the way to the catalog's PlanDelete hook.
	// Multi-table TRUNCATE produces a MultiStatement chain.
	// CASCADE / RESTRICT (`stmt.behavior`) and RESTART / CONTINUE IDENTITY
	// (`stmt.restart_seqs`) are accepted for PG-syntax compat but ignored:
	// no FK enforcement, no OWNED-BY identity sequences.
	if (!stmt.relations || !stmt.relations->head) {
		throw ParserException("TRUNCATE requires at least one relation");
	}

	auto build_one = [&](duckdb_libpgquery::PGRangeVar &range_var) -> unique_ptr<DeleteStatement> {
		auto del = make_uniq<DeleteStatement>();
		del->node->table = TransformRangeVar(range_var);
		del->node->is_truncate = true;
		return del;
	};

	// Single relation: return the DeleteStatement directly so the protocol
	// layer sees a DELETE_STATEMENT rather than a MultiStatement.
	if (!stmt.relations->head->next) {
		auto &range_var = *PGPointerCast<duckdb_libpgquery::PGRangeVar>(stmt.relations->head->data.ptr_value);
		return build_one(range_var);
	}

	auto multi = make_uniq<MultiStatement>();
	for (auto n = stmt.relations->head; n != nullptr; n = n->next) {
		auto &range_var = *PGPointerCast<duckdb_libpgquery::PGRangeVar>(n->data.ptr_value);
		multi->statements.push_back(build_one(range_var));
	}
	return multi;
}

} // namespace duckdb
