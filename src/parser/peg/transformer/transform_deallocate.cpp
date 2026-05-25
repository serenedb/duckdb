#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

// DeallocateTarget <- DeallocateAll / Identifier
// Returns "" when DeallocateAll matched (-> drop every prepared statement),
// otherwise the identifier verbatim.
string PEGTransformerFactory::TransformDeallocateTarget(PEGTransformer &transformer, ParseResult &choice_result) {
	if (choice_result.name == "DeallocateAll") {
		return "";
	}
	return choice_result.Cast<IdentifierParseResult>().identifier.GetIdentifierName();
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDeallocateStatement(PEGTransformer &transformer,
                                                                             const optional<bool> &deallocate_prepare,
                                                                             const string &deallocate_target) {
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::PREPARED_STATEMENT;
	result->info->SetName(Identifier(deallocate_target));
	// Empty name == DEALLOCATE ALL: succeed even if there's nothing to clear.
	// Named DEALLOCATE: PG-shape error when the name doesn't exist.
	result->info->if_not_found =
	    deallocate_target.empty() ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	return std::move(result);
}

bool PEGTransformerFactory::TransformDeallocatePrepare(PEGTransformer &transformer) {
	return true;
}

// DiscardStatement <- 'DISCARD' DiscardTarget
// DiscardTarget    <- 'ALL' / 'PLANS' / 'SEQUENCES' / 'TEMPORARY' / 'TEMP'
// PG-compat: SereneDB has no temp tables / session sequences, so every DISCARD
// variant collapses to DEALLOCATE ALL (clear prepared statement cache).
// Mirrors the v2026.05.18 libpg_query path (deallocate.y).
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDiscardStatement(PEGTransformer &transformer) {
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::PREPARED_STATEMENT;
	result->info->SetName("");
	result->info->if_not_found = OnEntryNotFound::RETURN_NULL;
	return std::move(result);
}

} // namespace duckdb
