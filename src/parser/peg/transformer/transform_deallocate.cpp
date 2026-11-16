#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

Identifier PEGTransformerFactory::TransformDeallocateAll(PEGTransformer &transformer) {
	return Identifier();
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDeallocateStatement(PEGTransformer &transformer,
                                                                             const optional<bool> &deallocate_prepare,
                                                                             const Identifier &deallocate_target) {
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::PREPARED_STATEMENT;
	info->SetName(deallocate_target);
	// Empty name == DEALLOCATE ALL: succeed even if there's nothing to clear.
	// Named DEALLOCATE: PG-shape error when the name doesn't exist.
	info->if_not_found = deallocate_target.empty() ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	return make_uniq<DropStatement>(std::move(info));
}

bool PEGTransformerFactory::TransformDeallocatePrepare(PEGTransformer &transformer) {
	return true;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDiscardStatement(PEGTransformer &transformer,
                                                                          const DiscardTarget &discard_target) {
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::PREPARED_STATEMENT;
	info->extra_drop_info = make_uniq<ExtraDropDiscardInfo>(discard_target);
	return make_uniq<DropStatement>(std::move(info));
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
