#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/statement/call_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
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
	// Lower TRUNCATE to a single CALL of serenedb_truncate(LIST<schemas>,
	// LIST<tables>) -- one statement regardless of how many relations were
	// listed, so the PG protocol layer doesn't need a multi-statement chain.
	// CASCADE / RESTRICT (`stmt.behavior`) and RESTART / CONTINUE IDENTITY
	// (`stmt.restart_seqs`) are accepted for PG-syntax compat but ignored:
	// we have no FK enforcement and no OWNED-BY identity sequences.
	if (!stmt.relations || !stmt.relations->head) {
		throw ParserException("TRUNCATE requires at least one relation");
	}

	vector<unique_ptr<ParsedExpression>> schemas_children;
	vector<unique_ptr<ParsedExpression>> tables_children;
	for (auto n = stmt.relations->head; n != nullptr; n = n->next) {
		auto &range_var = *PGPointerCast<duckdb_libpgquery::PGRangeVar>(n->data.ptr_value);
		// Catalog-qualified TRUNCATE not supported (no cross-database TRUNCATE).
		if (range_var.catalogname) {
			throw ParserException("TRUNCATE: catalog-qualified relation names are not supported");
		}
		auto schema_val = range_var.schemaname ? Value(string(range_var.schemaname)) : Value(LogicalType::VARCHAR);
		auto table_val = Value(string(range_var.relname ? range_var.relname : ""));
		schemas_children.push_back(make_uniq<ConstantExpression>(std::move(schema_val)));
		tables_children.push_back(make_uniq<ConstantExpression>(std::move(table_val)));
	}

	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<FunctionExpression>("list_value", std::move(schemas_children)));
	args.push_back(make_uniq<FunctionExpression>("list_value", std::move(tables_children)));

	auto call = make_uniq<CallStatement>();
	call->function = make_uniq<FunctionExpression>("serenedb_truncate", std::move(args));
	return call;
}

} // namespace duckdb
