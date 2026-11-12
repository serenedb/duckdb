#include "duckdb/transaction/transaction_manager.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

TransactionManager::TransactionManager(AttachedDatabase &db) : db(db) {
}

TransactionManager::~TransactionManager() {
}

void TransactionManager::PushAttach(Transaction &transaction, AttachedDatabase &attached_db) {
	throw InternalException("This transaction manager cannot attach databases");
}

} // namespace duckdb
