#include "duckdb/parser/transformer.hpp"

#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/transformer.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

unique_ptr<SQLStatement> Transformer::TransformCreatePublication(duckdb_libpgquery::PGCreatePublicationStmt &stmt) {
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_publication";

	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(string(stmt.pubname))));

	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(stmt.for_all_tables)));

	if (stmt.tables) {
		for (auto cell = stmt.tables->head; cell != nullptr; cell = cell->next) {
			auto *rv = PGPointerCast<duckdb_libpgquery::PGRangeVar>(cell->data.ptr_value).get();
			string table_name = rv->schemaname ? string(rv->schemaname) + "." + rv->relname : string(rv->relname);
			result->info->named_parameters.emplace(table_name, make_uniq<ConstantExpression>(Value::BOOLEAN(true)));
		}
	}

	return std::move(result);
}

} // namespace duckdb