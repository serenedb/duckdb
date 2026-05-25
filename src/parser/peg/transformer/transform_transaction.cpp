#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformBeginTransaction(PEGTransformer &transformer, const bool &has_result,
                                                 const optional<TransactionIsolationLevel> &isolation_level_clause,
                                                 const optional<TransactionModifierType> &read_or_write,
                                                 const bool &has_result_1) {
	// has_result_1 is the optional [NOT] DEFERRABLE clause: only meaningful in PG
	// for SERIALIZABLE READ ONLY (rejected upfront here), so it is a parse-only no-op.
	auto info = make_uniq<TransactionInfo>(TransactionType::BEGIN_TRANSACTION);
	if (read_or_write) {
		info->modifier = *read_or_write;
	}
	if (isolation_level_clause) {
		info->isolation_level = *isolation_level_clause;
	}
	return make_uniq<TransactionStatement>(std::move(info));
}

// IsolationLevelClause <- 'ISOLATION' 'LEVEL' IsolationLevel
TransactionIsolationLevel
PEGTransformerFactory::TransformIsolationLevelClause(PEGTransformer &transformer,
                                                     const TransactionIsolationLevel &isolation_level) {
	return isolation_level;
}

TransactionIsolationLevel PEGTransformerFactory::TransformReadCommitted(PEGTransformer &transformer) {
	return TransactionIsolationLevel::READ_COMMITTED;
}

TransactionIsolationLevel PEGTransformerFactory::TransformReadUncommitted(PEGTransformer &transformer) {
	return TransactionIsolationLevel::READ_UNCOMMITTED;
}

TransactionIsolationLevel PEGTransformerFactory::TransformRepeatableRead(PEGTransformer &transformer) {
	return TransactionIsolationLevel::REPEATABLE_READ;
}

TransactionIsolationLevel PEGTransformerFactory::TransformSerializable(PEGTransformer &transformer) {
	return TransactionIsolationLevel::SERIALIZABLE;
}

TransactionModifierType
PEGTransformerFactory::TransformReadOrWrite(PEGTransformer &transformer,
                                            const TransactionModifierType &read_only_or_read_write) {
	return read_only_or_read_write;
}

TransactionModifierType PEGTransformerFactory::TransformReadOnly(PEGTransformer &transformer) {
	return TransactionModifierType::TRANSACTION_READ_ONLY;
}

TransactionModifierType PEGTransformerFactory::TransformReadWrite(PEGTransformer &transformer) {
	return TransactionModifierType::TRANSACTION_READ_WRITE;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCommitTransaction(PEGTransformer &transformer,
                                                                           const bool &has_result,
                                                                           const bool &has_result_1) {
	// has_result_1 is the optional AND [NO] CHAIN clause: serenedb commits without
	// re-opening, so it is a parse-only no-op (matches PG for non-chained callers).
	return make_uniq<TransactionStatement>(make_uniq<TransactionInfo>(TransactionType::COMMIT));
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformRollbackTransaction(PEGTransformer &transformer,
                                                                             const bool &has_result,
                                                                             const bool &has_result_1) {
	// has_result_1 is the optional AND [NO] CHAIN clause: parse-only no-op.
	return make_uniq<TransactionStatement>(make_uniq<TransactionInfo>(TransactionType::ROLLBACK));
}
} // namespace duckdb
