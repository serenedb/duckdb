#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

Identifier PEGTransformerFactory::TransformDeallocateAll(PEGTransformer &transformer) {
	return Identifier();
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDeallocateStatement(PEGTransformer &transformer,
                                                                             const optional<bool> &deallocate_prepare,
                                                                             const Identifier &deallocate_target) {
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::PREPARED_STATEMENT;
	result->info->SetName(deallocate_target);
	// Empty name == DEALLOCATE ALL: succeed even if there's nothing to clear.
	// Named DEALLOCATE: PG-shape error when the name doesn't exist.
	result->info->if_not_found =
	    deallocate_target.empty() ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	return std::move(result);
}

bool PEGTransformerFactory::TransformDeallocatePrepare(PEGTransformer &transformer) {
	return true;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDiscardStatement(PEGTransformer &transformer,
                                                                          const DiscardTarget &discard_target) {
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::PREPARED_STATEMENT;
	result->info->extra_drop_info = make_uniq<ExtraDropDiscardInfo>(discard_target);
	return std::move(result);
}

DiscardTarget PEGTransformerFactory::TransformDiscardAll(PEGTransformer &transformer) {
	return DiscardTarget::ALL;
}

DiscardTarget PEGTransformerFactory::TransformDiscardPlans(PEGTransformer &transformer) {
	return DiscardTarget::PLANS;
}

DiscardTarget PEGTransformerFactory::TransformDiscardSequences(PEGTransformer &transformer) {
	return DiscardTarget::SEQUENCES;
}

DiscardTarget PEGTransformerFactory::TransformDiscardTemp(PEGTransformer &transformer) {
	return DiscardTarget::TEMP;
}

} // namespace duckdb
