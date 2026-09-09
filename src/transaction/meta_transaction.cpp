#include "duckdb/transaction/meta_transaction.hpp"

#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

MetaTransaction::MetaTransaction(ClientContext &context_p, timestamp_t start_timestamp_p,
                                 transaction_t transaction_id_p)
    : context(context_p), start_timestamp(start_timestamp_p), global_transaction_id(transaction_id_p),
      transaction_validity(*context_p.db, ValidChecker::Scope::TRANSACTION), active_query(MAXIMUM_QUERY_ID),
      modified_database(nullptr), is_read_only(false) {
}

MetaTransaction &MetaTransaction::Get(ClientContext &context) {
	return context.transaction.ActiveTransaction();
}

ValidChecker &ValidChecker::Get(MetaTransaction &transaction) {
	return transaction.transaction_validity;
}

void MetaTransaction::RefreshStartTime() {
	// Refreshing takes each attachment's manager locks, so it runs outside this transaction's own lock, off a copy:
	// the map only changes on this transaction's own thread.
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
	// Starting takes the attachment's manager locks, so it runs outside this transaction's own lock: holding one
	// across the other orders them against every path that resolves a transaction while a catalog lock is held, and
	// that order closes into a cycle. Two racers may both start one; the loser rolls its fresh transaction back.
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
	all_transactions.push_back(db);
	transactions.insert(make_pair(reference<AttachedDatabase>(db), TransactionReference(new_transaction)));
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

ErrorData MetaTransaction::Commit() {
	ErrorData error;
#ifdef DEBUG
	reference_set_t<AttachedDatabase> committed_tx;
#endif
	// Commit in reverse order, except that an attachment with no storage of its own goes first: it has no rows and
	// only appends to the catalog log another attachment shares, so committing it first is what makes those records
	// durable ahead of the database whose commit they belong with.
	vector<reference<AttachedDatabase>> order;
	order.reserve(all_transactions.size());
	for (idx_t pass = 0; pass < 2; pass++) {
		for (idx_t i = all_transactions.size(); i > 0; i--) {
			auto &db = all_transactions[i - 1].get();
			const bool storage_less = !db.HasStorageManager() || db.GetStorageManager().InMemory();
			if (storage_less == (pass == 0)) {
				order.emplace_back(db);
			}
		}
	}
	for (auto &next : order) {
		auto &db = next.get();
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

void MetaTransaction::DetachDatabase(AttachedDatabase &database) {
	lock_guard<mutex> guard(referenced_database_lock);
	used_databases.erase(database.GetName());
}

AttachedDatabase &MetaTransaction::UseDatabase(shared_ptr<AttachedDatabase> &database) {
	auto &db_ref = *database;
	lock_guard<mutex> guard(referenced_database_lock);
	auto entry = referenced_databases.find(db_ref);
	if (entry == referenced_databases.end()) {
		// The name index answers name lookups; a reference is keyed by identity. A name whose holder
		// changed under this transaction (a concurrent DROP and CREATE) keeps the database that claimed
		// it first, and the other stays reachable as the object it is.
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
	// An attachment with no storage of its own is not a second writer: it has no log to commit to, and what it
	// changes is recorded in a log another attachment shares. The rule protects the absence of a commit across two
	// logs, not the number of databases.
	if (!db.HasStorageManager() || db.GetStorageManager().InMemory()) {
		auto log = db.GetTransactionManager().CatalogLog();
		auto shared = !modified_database || modified_database->GetTransactionManager().CatalogLog().get() == log.get();
		if (log && shared) {
			return;
		}
	}
	if (!modified_database) {
		modified_database = &db;
		return;
	}
	if (&db != modified_database.get()) {
		throw TransactionException(
		    Exception::InitializeExtraInfo("CROSS_DATABASE_WRITE", optional_idx()),
		    "Attempting to write to database \"%s\" in a transaction that has already modified database \"%s\" - a "
		    "single transaction can only write to a single attached database.",
		    db.GetName(), modified_database->GetName());
	}
}

} // namespace duckdb
