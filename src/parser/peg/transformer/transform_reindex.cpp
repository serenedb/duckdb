#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

// SereneDB extension: REINDEX INDEX [CONCURRENTLY] <name> rebuilds a
// view-backed inverted index from the current source state. It lowers to the
// serenedb_reindex pragma, whose parameters are the parts of the name as written.
// CONCURRENTLY is accepted and always-on: the rebuild stages invisibly and
// publishes atomically, so it never blocks readers.
unique_ptr<SQLStatement> PEGTransformerFactory::TransformReindexStatement(PEGTransformer &transformer,
                                                                          const string &reindex_kind,
                                                                          const optional<string> &concurrently,
                                                                          unique_ptr<BaseTableRef> table) {
	auto pragma = make_uniq<PragmaStatement>();
	pragma->info->name = "serenedb_reindex";
	for (auto &part : table->GetQualifiedName().Path()) {
		pragma->info->parameters.push_back(ConstantExpression::String(part.GetIdentifierName()));
	}
	return std::move(pragma);
}

} // namespace duckdb
