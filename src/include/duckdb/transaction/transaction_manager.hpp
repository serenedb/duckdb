//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/transaction_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {
class WriteAheadLog;

class AttachedDatabase;
class ClientContext;
class Catalog;
struct ClientLockWrapper;
class DatabaseInstance;
class Transaction;

//! The Transaction Manager is responsible for creating and managing
//! transactions
class TransactionManager {
public:
	explicit TransactionManager(AttachedDatabase &db);
	virtual ~TransactionManager();

	//! Start a new transaction
	virtual Transaction &StartTransaction(ClientContext &context) = 0;
	//! Commit the given transaction. Returns a non-empty error message on failure.
	virtual ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) = 0;
	//! Rollback the given transaction
	virtual void RollbackTransaction(Transaction &transaction) = 0;

	virtual void Checkpoint(ClientContext &context, bool force = false) = 0;

	static TransactionManager &Get(AttachedDatabase &db);

	virtual bool IsDuckTransactionManager() {
		return false;
	}
	//! Move a writeless transaction's read visibility forward to the present
	//! (statement-level snapshots for READ COMMITTED semantics). No-op by
	//! default; managers without MVCC snapshots have nothing to refresh.
	virtual void RefreshStartTime(Transaction &transaction) {
	}
	//! The write-ahead log this manager's catalog changes are recorded in, when that is not the attachment's own.
	//! Two attachments answering with the same log share it, so one transaction may make catalog changes to both:
	//! the single-writable-database rule is there because a commit cannot span two logs, not because of the number
	//! of databases.
	virtual optional_ptr<WriteAheadLog> CatalogLog() {
		return nullptr;
	}
	//! Make everything this transaction wrote to that log durable, and stop holding it. Called inside the commit as
	//! soon as the commit walk is done: ahead of this attachment's own rows, so the two logs can only ever diverge in
	//! the repairable direction, and before anything that waits on another committer.
	virtual void FlushCatalogLog() {
	}

	AttachedDatabase &GetDB() {
		return db;
	}

protected:
	//! The attached database
	AttachedDatabase &db;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
