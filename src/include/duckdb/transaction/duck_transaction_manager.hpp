//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/duck_transaction_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/common/enums/checkpoint_type.hpp"
#include "duckdb/common/queue.hpp"

#include <thread>

namespace duckdb {
class DuckTransactionManager;
class DuckTransaction;
class WriteAheadLog;
struct UndoBufferProperties;

//! CleanupInfo collects transactions awaiting cleanup.
//! This ensures we can clean up after releasing the transaction lock.
struct DuckCleanupInfo {
	//! All transactions in a cleanup info share the same lowest_start_time.
	transaction_t lowest_start_time;
	vector<unique_ptr<DuckTransaction>> transactions;

	void Cleanup();
	bool ScheduleCleanup() noexcept;
};

//! The Transaction Manager is responsible for creating and managing
//! transactions
class DuckTransactionManager : public TransactionManager {
public:
	explicit DuckTransactionManager(AttachedDatabase &db);
	~DuckTransactionManager() override;

public:
	static DuckTransactionManager &Get(AttachedDatabase &db);

	//! Start a new transaction
	Transaction &StartTransaction(ClientContext &context) override;
	//! Commit the given transaction
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	//! Rollback the given transaction
	void RollbackTransaction(Transaction &transaction) override;

	void Checkpoint(ClientContext &context, bool force = false) override;

	transaction_t LowestActiveId() const {
		return lowest_active_id;
	}
	transaction_t LowestActiveStart() const {
		return lowest_active_start;
	}
	transaction_t GetLastCommit() const {
		return last_commit;
	}
	transaction_t GetActiveCheckpoint() const {
		return active_checkpoint;
	}
	void SetActiveCheckpoint(transaction_t checkpoint_id);
	void ResetActiveCheckpoint();
	//! Move a checkpoint transaction's snapshot up to a fresh timestamp: the checkpoint persists every committed
	//! transaction and truncates the WAL, so it must see even commits whose group fsync is still pending (they
	//! become durable via the checkpoint). A durability-bounded snapshot (see DurableSnapshotBound) would silently
	//! drop them. Safe because the checkpoint holds the exclusive checkpoint lock while it runs.
	void RefreshCheckpointSnapshot(DuckTransaction &transaction);

	bool IsDuckTransactionManager() override {
		return true;
	}

	//! Obtains a shared lock to the checkpoint lock
	unique_ptr<StorageLockKey> SharedCheckpointLock();
	//! Try to obtain an exclusive checkpoint lock
	unique_ptr<StorageLockKey> TryGetCheckpointLock();
	unique_ptr<StorageLockKey> TryUpgradeCheckpointLock(StorageLockKey &lock);
	unique_ptr<StorageLockKey> SharedVacuumLock();
	unique_ptr<StorageLockKey> TryGetVacuumLock();

	//! Returns the current version of the catalog (incremented whenever anything changes, not stored between restarts)
	DUCKDB_API idx_t GetCatalogVersion(Transaction &transaction);

	void PushCatalogEntry(Transaction &transaction_p, CatalogEntry &entry, data_ptr_t extra_data = nullptr,
	                      idx_t extra_data_size = 0);
	void PushAttach(Transaction &transaction_p, AttachedDatabase &db);

protected:
	struct CheckpointDecision {
		explicit CheckpointDecision(string reason_p);
		explicit CheckpointDecision(CheckpointType type);
		~CheckpointDecision();

		bool can_checkpoint;
		string reason;
		CheckpointType type;
	};

private:
	//! Generates a new commit timestamp
	transaction_t GetCommitTimestamp();
	//! The snapshot bound for a transaction starting now: normally the passed fresh timestamp, but while commits are
	//! awaiting their group fsync, just above the last durable commit instead -- the new snapshot then excludes
	//! exactly the non-durable suffix, so no transaction can ever observe a commit that a crash could still lose,
	//! and starting a transaction never waits. Must be called with transaction_lock held.
	transaction_t DurableSnapshotBound(transaction_t fresh_start_time);
	//! Remove the given transaction from the list of active transactions
	unique_ptr<DuckCleanupInfo> RemoveTransaction(DuckTransaction &transaction) noexcept;
	//! Remove the given transaction from the list of active transactions
	unique_ptr<DuckCleanupInfo> RemoveTransaction(DuckTransaction &transaction, bool store_transaction) noexcept;

	//! Whether or not we can checkpoint
	CheckpointDecision CanCheckpoint(DuckTransaction &transaction, unique_ptr<StorageLockKey> &checkpoint_lock,
	                                 const UndoBufferProperties &properties);
	//! Get the checkpoint type of an automatic checkpoint
	CheckpointDecision GetCheckpointType(DuckTransaction &transaction, const UndoBufferProperties &undo_properties);

	bool HasOtherTransactions(DuckTransaction &transaction);
	void CleanupTransactions();
	//! Wait until the group fsyncs of all published commits have completed and raised the durable horizon
	void WaitForInFlightCommits();
	//! Raise the durable horizon to (at least) commit_id (raise-only) once its group fsync covers its flush marker,
	//! and wake a waiting drain. Wakeup is skipped entirely when no drain is waiting -- the common path pays only an
	//! atomic load, no lock/notify, so a commit is never slower than upstream.
	void RaiseDurableHorizon(transaction_t commit_id);

private:
	//! The current start timestamp used by transactions
	transaction_t current_start_timestamp;
	//! The current transaction ID used by transactions
	transaction_t current_transaction_id;
	//! The lowest active transaction id
	atomic<transaction_t> lowest_active_id;
	//! The lowest active transaction timestamp
	atomic<transaction_t> lowest_active_start;
	//! The last commit timestamp
	atomic<transaction_t> last_commit;
	//! The currently active checkpoint
	atomic<transaction_t> active_checkpoint;
	//! Set of currently running transactions
	vector<unique_ptr<DuckTransaction>> active_transactions;
	//! Set of recently committed transactions
	vector<unique_ptr<DuckTransaction>> recently_committed_transactions;
	//! The lock used for transaction operations
	mutex transaction_lock;
	//! The checkpoint lock
	StorageLock checkpoint_lock;
	//! The vacuum lock - necessary to start vacuum operations
	StorageLock vacuum_lock;
	//! Lock necessary to start transactions only - used by FORCE CHECKPOINT to prevent new transactions from starting
	mutex start_transaction_lock;
	//! Highest commit id that wrote WAL bytes, stored under transaction_lock in the same critical section that
	//! publishes the commit (so it is monotonic). Durability follows commit order (commit order = WAL order = fsync
	//! coverage order), so together with last_durable_commit it bounds new snapshots at the durable horizon.
	atomic<transaction_t> last_pending_commit = 0;
	//! Highest commit id whose WAL bytes are known durable. Raised (raise-only CAS) by each committer once its group
	//! fsync covers its flush marker; acknowledgements can race out of commit order, hence the max. A drain
	//! (WaitForInFlightCommits) parks on this atomic via C++20 atomic-wait; RaiseDurableHorizon notifies it.
	atomic<transaction_t> last_durable_commit = 0;

	atomic<idx_t> last_uncommitted_catalog_version = {TRANSACTION_ID_START};
	idx_t last_committed_version = 0;

	//! Only one cleanup can be active at any time.
	mutex cleanup_lock;
	//! Changes to the cleanup queue must be synchronized.
	mutex cleanup_queue_lock;
	//! Cleanups have to happen in-order.
	//! E.g., if one transaction drops a table, and another creates a table,
	//! inverting the cleanup order can result in catalog errors.
	queue<unique_ptr<DuckCleanupInfo>> cleanup_queue;

protected:
	virtual void OnCommitCheckpointDecision(const CheckpointDecision &decision, DuckTransaction &transaction) {
	}
};

} // namespace duckdb
