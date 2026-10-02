#include "duckdb/transaction/meta_transaction.hpp"

#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/secret/secret_storage.hpp"

namespace duckdb {

MetaTransaction::MetaTransaction(ClientContext &context_p, timestamp_t start_timestamp_p,
                                 transaction_t transaction_id_p)
    : context(context_p), start_timestamp(start_timestamp_p), global_transaction_id(transaction_id_p),
      transaction_validity(*context_p.db, ValidChecker::Scope::TRANSACTION), active_query(MAXIMUM_QUERY_ID),
      modified_database(nullptr), is_read_only(false) {
}

MetaTransaction::~MetaTransaction() = default;

MetaTransaction &MetaTransaction::Get(ClientContext &context) {
	return context.transaction.ActiveTransaction();
}

ValidChecker &ValidChecker::Get(MetaTransaction &transaction) {
	return transaction.transaction_validity;
}

void MetaTransaction::RefreshStartTime() {
	vector<std::pair<reference<AttachedDatabase>, reference<Transaction>>> to_refresh;
	{
		lock_guard<mutex> guard(lock);
		for (auto &db : all_transactions) {
			auto entry = transactions.find(db.get());
			if (entry == transactions.end()) {
				continue;
			}
			to_refresh.emplace_back(db, entry->second.transaction);
		}
	}
	for (auto &entry : to_refresh) {
		entry.first.get().GetTransactionManager().RefreshStartTime(entry.second.get());
	}
}

Transaction &Transaction::Get(ClientContext &context, AttachedDatabase &db) {
	auto &meta_transaction = MetaTransaction::Get(context);
	return meta_transaction.GetTransaction(db);
}

optional_ptr<Transaction> Transaction::TryGet(ClientContext &context, AttachedDatabase &db) {
	auto &meta_transaction = MetaTransaction::Get(context);
	return meta_transaction.TryGetTransaction(db);
}

#ifdef DEBUG
static void VerifyAllTransactionsUnique(AttachedDatabase &db, vector<reference<AttachedDatabase>> &all_transactions) {
	for (auto &tx : all_transactions) {
		if (RefersToSameObject(db, tx.get())) {
			throw InternalException("Database is already present in all_transactions");
		}
	}
}
#endif

optional_ptr<Transaction> MetaTransaction::TryGetTransaction(AttachedDatabase &db) {
	lock_guard<mutex> guard(lock);
	if (scoped_override_txn && scoped_override_db && RefersToSameObject(*scoped_override_db, db)) {
		return scoped_override_txn;
	}
	auto entry = transactions.find(db);
	if (entry == transactions.end()) {
		return nullptr;
	} else {
		return &entry->second.transaction;
	}
}

Transaction &MetaTransaction::GetTransaction(AttachedDatabase &db) {
	if (ValidChecker::IsInvalidated(db)) {
		throw IOException("%s", ValidChecker::InvalidatedMessage(db));
	}
	{
		lock_guard<mutex> guard(lock);
		if (scoped_override_txn && scoped_override_db && RefersToSameObject(*scoped_override_db, db)) {
			return *scoped_override_txn;
		}
		auto entry = transactions.find(db);
		if (entry != transactions.end()) {
			D_ASSERT(entry->second.transaction.active_query == active_query);
			return entry->second.transaction;
		}
	}
	auto &new_transaction = db.GetTransactionManager().StartTransaction(context);
	new_transaction.active_query = active_query.load();
	unique_lock<mutex> guard(lock);
	auto existing = transactions.find(db);
	if (existing != transactions.end()) {
		auto &transaction = existing->second.transaction;
		guard.unlock();
		db.GetTransactionManager().RollbackTransaction(new_transaction);
		return transaction;
	}
#ifdef DEBUG
	VerifyAllTransactionsUnique(db, all_transactions);
#endif
	// Rollback looks every entry of all_transactions up in transactions, so the two must not get out of sync:
	// reserve first, then insert, so that a failing allocation happens before either is modified and the
	// push_back that follows cannot allocate.
	all_transactions.reserve(all_transactions.size() + 1);
	transactions.emplace(db, TransactionReference(new_transaction));
	all_transactions.push_back(db);
	auto shared_db = db.shared_from_this();
	UseDatabase(shared_db);

	return new_transaction;
}

void MetaTransaction::RemoveTransaction(AttachedDatabase &db) {
	auto entry = transactions.find(db);
	if (entry == transactions.end()) {
		throw InternalException("MetaTransaction::RemoveTransaction called but meta transaction did not have a "
		                        "transaction for this database");
	}
	transactions.erase(entry);
	for (idx_t i = 0; i < all_transactions.size(); i++) {
		auto &db_entry = all_transactions[i];
		if (RefersToSameObject(db_entry.get(), db)) {
			all_transactions.erase_at(i);
			break;
		}
	}
}

void MetaTransaction::PushTransactionOverride(AttachedDatabase &db, Transaction &transaction) {
	lock_guard<mutex> guard(lock);
	if (scoped_override_txn) {
		throw InternalException("MetaTransaction::PushTransactionOverride called while an override is already active");
	}
	scoped_override_db = &db;
	scoped_override_txn = &transaction;
}

void MetaTransaction::PopTransactionOverride(AttachedDatabase &db) {
	lock_guard<mutex> guard(lock);
	if (!scoped_override_db || !RefersToSameObject(*scoped_override_db, db)) {
		throw InternalException("MetaTransaction::PopTransactionOverride called without a matching active override");
	}
	scoped_override_db = nullptr;
	scoped_override_txn = nullptr;
}

void MetaTransaction::SetReadOnly() {
	if (modified_database) {
		throw InternalException("Cannot set MetaTransaction to read only - modifications have already been made");
	}
	this->is_read_only = true;
}

bool MetaTransaction::IsReadOnly() const {
	return is_read_only;
}

Transaction &Transaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog.GetAttached());
}

optional_ptr<Catalog> MetaTransaction::CatalogLogForCommit() {
	optional_ptr<AttachedDatabase> writer;
	idx_t writers = 0;
	bool catalog_changes = false;
	for (auto &db_ref : all_transactions) {
		auto &db = db_ref.get();
		if (db.IsSystem() || db.IsTemporary()) {
			continue;
		}
		auto entry = transactions.find(db);
		if (entry == transactions.end() || entry->second.state != TransactionState::UNCOMMITTED ||
		    !entry->second.transaction.IsDuckTransaction()) {
			continue;
		}
		auto &transaction = entry->second.transaction.Cast<DuckTransaction>();
		if (!transaction.ChangesMade()) {
			continue;
		}
		if (!db.GetCatalog().UsesCatalogLog()) {
			return nullptr;
		}
		writer = db;
		writers++;
		catalog_changes = catalog_changes || transaction.catalog_version >= TRANSACTION_ID_START;
	}
	if ((!catalog_changes && writers < 2) || !writer->GetCatalog().CatalogLog()) {
		return nullptr;
	}
	return writer->GetCatalog();
}

ErrorData MetaTransaction::CommitThroughCatalogLog(Catalog &catalog) {
	auto catalog_log_ref = catalog.CatalogLog();
	auto &catalog_storage = catalog_log_ref->GetStorageManager();
	auto catalog_lock = catalog_storage.GetCommitLock();
	catalog_log_ref = catalog.CatalogLog();
	auto &catalog_log = *catalog_log_ref;
	auto &log_owner = catalog_storage.GetAttached().GetCatalog();
	auto commit_state = catalog_storage.GenStorageCommitState(catalog_log);
	const auto txid = UUID::GenerateRandomUUID();
	vector<pair<idx_t, idx_t>> prepared;
	ErrorData error;
	vector<reference<TransactionReference>> participants;
	for (idx_t i = all_transactions.size(); i > 0; i--) {
		auto &db = all_transactions[i - 1].get();
		auto &transaction_ref = transactions.find(db)->second;
		if (transaction_ref.state != TransactionState::UNCOMMITTED) {
			continue;
		}
		if (ValidChecker::IsInvalidated(db)) {
			error.Merge(ErrorData(IOException("%s", ValidChecker::InvalidatedMessage(db))));
			break;
		}
		participants.push_back(transaction_ref);
		auto &transaction_manager = db.GetTransactionManager();
		if (!transaction_manager.IsDuckTransactionManager()) {
			continue;
		}
		error = transaction_manager.Cast<DuckTransactionManager>().PrepareTransaction(
		    context, transaction_ref.transaction, catalog_log, txid, prepared);
		if (error.HasError()) {
			break;
		}
	}
	idx_t decision_offset = 0;
	if (!error.HasError()) {
		try {
			log_owner.OnCatalogLogPrepared();
			if (!prepared.empty()) {
				catalog_log.WriteCommitPrepared(txid, prepared);
			}
			decision_offset = commit_state->FlushCommit(false);
			if (!prepared.empty()) {
				DatabaseManager::Get(context).CommitPrepared(txid, std::move(prepared));
			}
		} catch (std::exception &ex) {
			error = ErrorData(ex);
		}
	}
	if (error.HasError()) {
		commit_state->RevertCommit();
		for (auto &participant : participants) {
			auto &transaction_ref = participant.get();
			try {
				transaction_ref.transaction.manager.RollbackTransaction(transaction_ref.transaction);
			} catch (std::exception &ex) {
				error.Merge(ErrorData(ex));
			}
			transaction_ref.state = TransactionState::ROLLED_BACK;
		}
		return error;
	}
	log_owner.BeginCatalogLogCommit();
	catalog_lock.unlock();
	if (decision_offset > 0) {
		catalog_log.SyncUpTo(decision_offset);
	}
	log_owner.OnCatalogLogDecided();
	for (auto &participant : participants) {
		auto &transaction_ref = participant.get();
		auto &db = transaction_ref.transaction.manager.GetDB();
		auto commit_error = transaction_ref.transaction.manager.CommitTransaction(context, transaction_ref.transaction);
		transaction_ref.state = TransactionState::COMMITTED;
		if (commit_error.HasError()) {
			ValidChecker::Invalidate(db, "Failed to apply a transaction whose commit is durable: " +
			                                 commit_error.RawMessage());
			error.Merge(commit_error);
		}
	}
	log_owner.EndCatalogLogCommit();
	return error;
}

ErrorData MetaTransaction::Commit() {
	auto catalog = CatalogLogForCommit();
	if (catalog) {
		return CommitThroughCatalogLog(*catalog);
	}
	ErrorData error;
#ifdef DEBUG
	reference_set_t<AttachedDatabase> committed_tx;
#endif
	// commit transactions in reverse order
	for (idx_t i = all_transactions.size(); i > 0; i--) {
		auto &db = all_transactions[i - 1].get();
		auto entry = transactions.find(db);
		if (entry == transactions.end()) {
			throw InternalException("Could not find transaction corresponding to database in MetaTransaction");
		}

#ifdef DEBUG
		auto already_committed = committed_tx.insert(db).second == false;
		if (already_committed) {
			throw InternalException("All databases inside all_transactions should be unique, invariant broken!");
		}
#endif

		auto &transaction_manager = db.GetTransactionManager();
		auto &transaction_ref = entry->second;
		if (ValidChecker::IsInvalidated(db)) {
			error.Merge(ErrorData(IOException("%s", ValidChecker::InvalidatedMessage(db))));
			continue;
		}
		if (transaction_ref.state != TransactionState::UNCOMMITTED) {
			continue;
		}
		auto &transaction = transaction_ref.transaction;
		try {
			if (!error.HasError()) {
				// Commit the transaction.
				error = transaction_manager.CommitTransaction(context, transaction);
				transaction_ref.state = error.HasError() ? TransactionState::ROLLED_BACK : TransactionState::COMMITTED;
			} else {
				// Rollback due to previous error.
				transaction_manager.RollbackTransaction(transaction);
				transaction_ref.state = TransactionState::ROLLED_BACK;
			}
		} catch (std::exception &ex) {
			error.Merge(ErrorData(ex));
			transaction_ref.state = TransactionState::ROLLED_BACK;
		}
	}
	return error;
}

void MetaTransaction::Rollback() {
	// Rollback all transactions in reverse order.
	ErrorData error;
	for (idx_t i = all_transactions.size(); i > 0; i--) {
		auto &db = all_transactions[i - 1].get();
		auto &transaction_manager = db.GetTransactionManager();
		auto entry = transactions.find(db);
		D_ASSERT(entry != transactions.end());
		auto &transaction_ref = entry->second;
		if (ValidChecker::IsInvalidated(db)) {
			error.Merge(ErrorData(IOException("%s", ValidChecker::InvalidatedMessage(db))));
			continue;
		}
		if (transaction_ref.state != TransactionState::UNCOMMITTED) {
			continue;
		}
		try {
			auto &transaction = transaction_ref.transaction;
			transaction_manager.RollbackTransaction(transaction);
		} catch (std::exception &ex) {
			error.Merge(ErrorData(ex));
		}
		transaction_ref.state = TransactionState::ROLLED_BACK;
	}
	if (error.HasError()) {
		error.Throw();
	}
}

void MetaTransaction::Finalize() {
	// Try to checkpoint any attached databases potentially still held by this transaction.
	for (auto &database : referenced_databases) {
		// If the use count is down to one, then we already detached the database.
		// That means new transactions can no longer obtain a shared pointer to it.
		AttachedDatabase::InvokeCloseIfLastReference(database.second, context);
	}
}

idx_t MetaTransaction::GetActiveQuery() {
	return active_query;
}

void MetaTransaction::SetActiveQuery(transaction_t query_number) {
	lock_guard<mutex> guard(lock);
	active_query = query_number;
	statement_databases.clear();
	for (auto &entry : transactions) {
		entry.second.transaction.active_query = query_number;
	}
}

optional_ptr<AttachedDatabase> MetaTransaction::GetReferencedDatabase(const Identifier &name) {
	lock_guard<mutex> guard(referenced_database_lock);
	auto entry = used_databases.find(name);
	if (entry != used_databases.end()) {
		return entry->second.get();
	}
	return nullptr;
}

shared_ptr<AttachedDatabase> MetaTransaction::GetReferencedDatabaseOwning(const Identifier &name) {
	lock_guard<mutex> guard(referenced_database_lock);
	for (auto &entry : referenced_databases) {
		if (entry.first.get().name == name) {
			return entry.second;
		}
	}
	return nullptr;
}

bool MetaTransaction::ReferencesDatabase(AttachedDatabase &database) {
	lock_guard<mutex> guard(referenced_database_lock);
	return referenced_databases.contains(database);
}

void MetaTransaction::DetachDatabase(AttachedDatabase &database) {
	lock_guard<mutex> guard(referenced_database_lock);
	used_databases.erase(database.GetName());
}

AttachedDatabase &MetaTransaction::UseDatabase(shared_ptr<AttachedDatabase> &database) {
	auto &db_ref = *database;
	lock_guard<mutex> guard(referenced_database_lock);
	auto entry = referenced_databases.find(db_ref);
	if (entry == referenced_databases.end()) {
		used_databases.emplace(db_ref.GetName(), db_ref);
		referenced_databases.emplace(reference<AttachedDatabase>(db_ref), database);
	}
	return db_ref;
}

vector<shared_ptr<AttachedDatabase>> &MetaTransaction::GetStatementDatabases(ClientContext &context) {
	lock_guard<mutex> guard(lock);
	if (statement_databases.empty()) {
		statement_databases = DatabaseManager::Get(context).GetDatabases(context);
	}
	return statement_databases;
}

void MetaTransaction::ModifyDatabase(AttachedDatabase &db, DatabaseModificationType modification) {
	if (IsReadOnly()) {
		throw TransactionException(Exception::InitializeExtraInfo("READ_ONLY", optional_idx()),
		                           "Cannot write to database \"%s\" - transaction is launched in read-only mode",
		                           db.GetName());
	}
	auto &transaction = GetTransaction(db);
	if (transaction.IsReadOnly()) {
		transaction.SetReadWrite();
	}
	transaction.SetModifications(modification);
	if (db.IsSystem() || db.IsTemporary()) {
		// we can always modify the system and temp databases
		return;
	}
	if (!modified_database) {
		modified_database = &db;
		return;
	}
	if (&db != modified_database.get() &&
	    !(db.GetCatalog().UsesCatalogLog() && modified_database->GetCatalog().UsesCatalogLog())) {
		throw TransactionException(
		    Exception::InitializeExtraInfo("CROSS_DATABASE_WRITE", optional_idx()),
		    "Attempting to write to database %s in a transaction that has already modified database %s - a "
		    "single transaction can only write to a single attached database.",
		    db.GetName(), modified_database->GetName());
	}
}

} // namespace duckdb
