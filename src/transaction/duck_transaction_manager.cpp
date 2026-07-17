#include "duckdb/transaction/duck_transaction_manager.hpp"

#include "duckdb/main/client_data.hpp"

#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/checkpoint/checkpoint_options.hpp"

#include <thread>

namespace duckdb {

void DuckCleanupInfo::Cleanup() {
	for (auto &transaction : transactions) {
		if (transaction->awaiting_cleanup) {
			transaction->Cleanup(lowest_start_time);
		}
	}
}

bool DuckCleanupInfo::ScheduleCleanup() noexcept {
	return !transactions.empty();
}

DuckTransactionManager::DuckTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	// start timestamp starts at two
	current_start_timestamp = 2;
	// transaction ID starts very high:
	// it should be much higher than the current start timestamp
	// if transaction_id < start_timestamp for any set of active transactions
	// uncommitted data could be read by
	current_transaction_id = TRANSACTION_ID_START;
	lowest_active_id = TRANSACTION_ID_START;
	lowest_active_start = MAX_TRANSACTION_ID;
	active_checkpoint = MAX_TRANSACTION_ID;
	if (!db.GetCatalog().IsDuckCatalog()) {
		// Specifically the StorageManager of the DuckCatalog is relied on, with `db.GetStorageManager`
		throw InternalException("DuckTransactionManager should only be created together with a DuckCatalog");
	}
}

DuckTransactionManager::~DuckTransactionManager() {
}

DuckTransactionManager &DuckTransactionManager::Get(AttachedDatabase &db) {
	auto &transaction_manager = TransactionManager::Get(db);
	if (!transaction_manager.IsDuckTransactionManager()) {
		throw InternalException("Calling DuckTransactionManager::Get on non-DuckDB transaction manager");
	}
	return reinterpret_cast<DuckTransactionManager &>(transaction_manager);
}

Transaction &DuckTransactionManager::StartTransaction(ClientContext &context) {
	// obtain the transaction lock during this function
	auto &meta_transaction = MetaTransaction::Get(context);
	unique_lock<mutex> start_lock(start_transaction_lock, std::defer_lock);
	if (!meta_transaction.IsReadOnly()) {
		start_lock.lock();
	}
	lock_guard<mutex> lock(transaction_lock);
	if (current_start_timestamp >= TRANSACTION_ID_START) { // LCOV_EXCL_START
		throw InternalException("Cannot start more transactions, ran out of "
		                        "transaction identifiers!");
	} // LCOV_EXCL_STOP

	// obtain the start time and transaction ID of this transaction
	transaction_t start_time = DurableSnapshotBound(current_start_timestamp++);
	transaction_t transaction_id = current_transaction_id++;
	if (active_transactions.empty()) {
		lowest_active_start = start_time;
		lowest_active_id = transaction_id;
	}

	// create the actual transaction
	auto transaction = make_uniq<DuckTransaction>(*this, context, start_time, transaction_id, last_committed_version);
	auto &transaction_ref = *transaction;

	// store it in the set of active transactions
	active_transactions.push_back(std::move(transaction));
	return transaction_ref;
}

transaction_t DuckTransactionManager::DurableSnapshotBound(transaction_t fresh_start_time) {
	// caller must hold transaction_lock: commits store last_pending_commit in the same critical section that makes
	// them visible, so a snapshot bounded here either misses a commit entirely or sees its durability recorded
	transaction_t durable = last_durable_commit.load(std::memory_order_acquire);
	if (durable >= last_pending_commit.load(std::memory_order_acquire)) {
		return fresh_start_time;
	}
	// commit order equals WAL order equals durability order, so the non-durable commits are exactly the suffix
	// above last_durable_commit: a snapshot just above it sees every durable commit and no non-durable one.
	// Floor at the lowest fresh timestamp: bootstrap catalog entries are committed at timestamp 1 by the system
	// transaction and recreated deterministically on every database open, so they must stay visible even before
	// the first commit is durable, while every real commit id is at least 2 and stays correctly excluded
	return MaxValue<transaction_t>(durable + 1, 2);
}

void DuckTransactionManager::RefreshCheckpointSnapshot(DuckTransaction &transaction) {
	// under transaction_lock every commit at a lower timestamp is fully applied, and RemoveTransaction recomputes
	// lowest_active_start from the live start times, so raising this one is consistent
	lock_guard<mutex> lock(transaction_lock);
	transaction.start_time = current_start_timestamp++;
}

void DuckTransactionManager::SetActiveCheckpoint(transaction_t checkpoint_id) {
	active_checkpoint = checkpoint_id;
}

void DuckTransactionManager::ResetActiveCheckpoint() {
	active_checkpoint = MAX_TRANSACTION_ID;
}

DuckTransactionManager::CheckpointDecision::CheckpointDecision(string reason_p)
    : can_checkpoint(false), reason(std::move(reason_p)) {
}

DuckTransactionManager::CheckpointDecision::CheckpointDecision(CheckpointType type) : can_checkpoint(true), type(type) {
}

DuckTransactionManager::CheckpointDecision::~CheckpointDecision() {
}

bool DuckTransactionManager::HasOtherTransactions(DuckTransaction &transaction) {
	for (auto &active_transaction : active_transactions) {
		if (!RefersToSameObject(*active_transaction, transaction)) {
			return true;
		}
	}
	return false;
}

DuckTransactionManager::CheckpointDecision
DuckTransactionManager::CanCheckpoint(DuckTransaction &transaction, unique_ptr<StorageLockKey> &lock,
                                      const UndoBufferProperties &undo_properties) {
	if (db.IsSystem()) {
		return CheckpointDecision("system transaction");
	}
	if (transaction.IsReadOnly()) {
		return CheckpointDecision("transaction is read-only");
	}
	auto &storage_manager = db.GetStorageManager();
	if (!storage_manager.IsLoaded()) {
		return CheckpointDecision("cannot checkpoint while loading");
	}
	if (!transaction.AutomaticCheckpoint(db, undo_properties)) {
		return CheckpointDecision("no reason to automatically checkpoint");
	}
	if (Settings::Get<DebugSkipCheckpointOnCommitSetting>(db.GetDatabase())) {
		return CheckpointDecision("checkpointing on commit disabled through configuration");
	}
	// let the in-flight group fsyncs land so the WAL truncation below cannot drop a commit that is published but
	// not yet durable, and so committed-but-not-yet-cleaned transactions release their shared checkpoint lock.
	// Purge commits parked by the durable floor (we hold transaction_lock): a parked predecessor keeps its shared
	// checkpoint lock and would fail the exclusive upgrade below on every subsequent commit.
	WaitForInFlightCommits();
	PurgeRecentlyCommittedInternal();
	CleanupTransactions();
	// try to lock the checkpoint lock
	lock = transaction.TryGetCheckpointLock();
	if (!lock) {
		return CheckpointDecision("Failed to obtain checkpoint lock - another thread is writing/checkpointing or "
		                          "another read transaction relies on data that is not yet committed");
	}
	return CheckpointDecision(CheckpointType::FULL_CHECKPOINT);
}

void DuckTransactionManager::WaitForInFlightCommits() {
	// group fsyncs of already-published commits complete on their committers' threads without needing any lock held
	// here, and every published commit raises the durable horizon (RaiseDurableHorizon) on its durability path. The
	// target is snapped once, so the wait is bounded by the fsyncs in flight now, not by later commits.
	transaction_t target = last_pending_commit.load(std::memory_order_acquire);
	transaction_t durable = last_durable_commit.load(std::memory_order_acquire);
	while (durable < target) {
		last_durable_commit.wait(durable, std::memory_order_acquire);
		durable = last_durable_commit.load(std::memory_order_acquire);
	}
}

void DuckTransactionManager::RaiseDurableHorizon(transaction_t commit_id) {
	// durability follows commit = WAL order, so once our fsync covers our marker every lower commit is durable too;
	// the raise-only CAS tolerates fsyncs completing out of commit order.
	transaction_t durable = last_durable_commit.load(std::memory_order_relaxed);
	bool advanced = false;
	while (durable < commit_id) {
		if (last_durable_commit.compare_exchange_weak(durable, commit_id, std::memory_order_acq_rel,
		                                              std::memory_order_relaxed)) {
			advanced = true;
			break;
		}
	}
	// wake any drain parked in WaitForInFlightCommits; atomic notify is free when no one is parked, so the common
	// commit path pays only the CAS above and stays as fast as upstream.
	if (advanced) {
		last_durable_commit.notify_all();
	}
}

DuckTransactionManager::CheckpointDecision
DuckTransactionManager::GetCheckpointType(DuckTransaction &transaction, const UndoBufferProperties &undo_properties) {
	auto &storage_manager = db.GetStorageManager();
	auto checkpoint_type = CheckpointType::FULL_CHECKPOINT;
	bool has_other_transactions = HasOtherTransactions(transaction);
	if (has_other_transactions) {
		if (undo_properties.has_updates || undo_properties.has_dropped_entries) {
			// if we have made updates/catalog changes in this transaction we cannot checkpoint
			// in the presence of other transactions
			string other_transactions;
			for (auto &active_transaction : active_transactions) {
				if (!RefersToSameObject(*active_transaction, transaction)) {
					if (!other_transactions.empty()) {
						other_transactions += ", ";
					}
					other_transactions += "[" + to_string(active_transaction->transaction_id) + "]";
				}
			}
			if (!other_transactions.empty()) {
				// there are other transactions!
				// these active transactions might need data from BEFORE this transaction
				// we might need to change our strategy here based on what changes THIS transaction has made
				if (undo_properties.has_dropped_entries) {
					// this transaction has changed the catalog - we cannot checkpoint
					return CheckpointDecision(
					    "Transaction has dropped catalog entries and there are other transactions "
					    "active\nActive transactions: " +
					    other_transactions);
				}
				// this transaction has performed updates - we cannot checkpoint
				return CheckpointDecision(
				    "Transaction has performed updates and there are other transactions active\nActive transactions: " +
				    other_transactions);
			}
		}
		// otherwise - we need to do a concurrent checkpoint
		checkpoint_type = CheckpointType::CONCURRENT_CHECKPOINT;
	}
	if (storage_manager.InMemory() && !storage_manager.CompressionIsEnabled()) {
		if (checkpoint_type == CheckpointType::CONCURRENT_CHECKPOINT) {
			return CheckpointDecision("Cannot vacuum, and compression is disabled for in-memory table");
		}
		return CheckpointDecision(CheckpointType::VACUUM_ONLY);
	}
	return CheckpointDecision(checkpoint_type);
}

void DuckTransactionManager::Checkpoint(ClientContext &context, bool force) {
	if (ValidChecker::IsInvalidated(db)) {
		throw IOException("%s", ValidChecker::InvalidatedMessage(db));
	}
	auto &storage_manager = db.GetStorageManager();
	auto current = Transaction::TryGet(context, db);
	if (current) {
		if (force) {
			throw TransactionException(
			    "Cannot FORCE CHECKPOINT: the current transaction has been started for this database");
		} else {
			auto &duck_transaction = current->Cast<DuckTransaction>();
			if (duck_transaction.ChangesMade()) {
				throw TransactionException("Cannot CHECKPOINT: the current transaction has transaction local changes");
			}
		}
	}

	unique_ptr<StorageLockKey> lock;
	// let the in-flight group fsyncs land so the WAL truncation cannot drop a published-but-not-durable commit, and
	// run pending cleanups: committed-but-not-yet-cleaned transactions keep their shared checkpoint lock and would
	// block the exclusive acquisition below
	WaitForInFlightCommits();
	PurgeRecentlyCommitted();
	CleanupTransactions();
	if (!force) {
		// not a force checkpoint
		// try to get the checkpoint lock
		lock = checkpoint_lock.TryGetExclusiveLock();
		if (!lock) {
			// we could not manage to get the lock - cancel
			throw TransactionException("Cannot CHECKPOINT: there are other write transactions active. Try using FORCE "
			                           "CHECKPOINT to wait until all active transactions are finished");
		}

	} else {
		// force checkpoint - wait to get an exclusive lock
		// grab the start_transaction_lock to prevent new transactions from starting
		lock_guard<mutex> start_lock(start_transaction_lock);
		// wait until any active transactions are finished
		while (!lock) {
			context.InterruptCheck();
			WaitForInFlightCommits();
			PurgeRecentlyCommitted();
			CleanupTransactions();
			lock = checkpoint_lock.TryGetExclusiveLock();
		}
	}
	// a full checkpoint (chosen below when no active snapshot needs old data) is safe without blocking new
	// transactions: after the drain the durable horizon covers every pending commit and the exclusive checkpoint
	// lock keeps new write commits out, so a snapshot taken during the checkpoint is fresh
	CheckpointOptions options;
	if (GetLastCommit() > LowestActiveStart()) {
		// we cannot do a full checkpoint if any transaction needs to read old data
		options.type = CheckpointType::CONCURRENT_CHECKPOINT;
	}

	storage_manager.CreateCheckpoint(context, options);
}

unique_ptr<StorageLockKey> DuckTransactionManager::SharedCheckpointLock() {
	return checkpoint_lock.GetSharedLock();
}

unique_ptr<StorageLockKey> DuckTransactionManager::TryUpgradeCheckpointLock(StorageLockKey &lock) {
	return checkpoint_lock.TryUpgradeCheckpointLock(lock);
}

unique_ptr<StorageLockKey> DuckTransactionManager::TryGetCheckpointLock() {
	return checkpoint_lock.TryGetExclusiveLock();
}

unique_ptr<StorageLockKey> DuckTransactionManager::SharedVacuumLock() {
	return vacuum_lock.GetSharedLock();
}

unique_ptr<StorageLockKey> DuckTransactionManager::TryGetVacuumLock() {
	return vacuum_lock.TryGetExclusiveLock();
}

transaction_t DuckTransactionManager::GetCommitTimestamp() {
	return current_start_timestamp++;
}

void DuckTransactionManager::RefreshStartTime(Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	if (transaction.ChangesMade()) {
		// the transaction has local changes; moving its snapshot would mix
		// visibility domains
		return;
	}
	// transaction_lock (not start_transaction_lock) guards current_start_timestamp increments
	// (see GetCommitTimestamp) and reads of peer start_time (see RemoveTransaction).
	lock_guard<mutex> lock(transaction_lock);
	// the refreshed snapshot is a snapshot acquisition like StartTransaction: bound it at the durable horizon so a
	// per-statement refresh never observes a commit that is not yet durable
	transaction.start_time = DurableSnapshotBound(current_start_timestamp++);
}

transaction_t DuckTransactionManager::ApplyDurableFloor(transaction_t lowest_start_time) const {
	// While a published commit is not yet durable, DurableSnapshotBound floors NEW snapshots at
	// last_durable_commit + 1 -- below such commits. Treat that floor as an implicit active reader: a
	// version-cleanup horizon above it would destroy catalog/row versions that a snapshot starting a
	// moment later can still legally read (start times are not monotone under the durable bound).
	const auto durable_commit = last_durable_commit.load(std::memory_order_acquire);
	if (durable_commit < last_pending_commit.load(std::memory_order_acquire)) {
		lowest_start_time = MinValue<transaction_t>(lowest_start_time, MaxValue<transaction_t>(durable_commit + 1, 2));
	}
	return lowest_start_time;
}

void DuckTransactionManager::MoveExpiredRecentlyCommitted(transaction_t lowest_start_time,
                                                          DuckCleanupInfo &cleanup_info) {
	idx_t i = 0;
	for (; i < recently_committed_transactions.size(); i++) {
		D_ASSERT(recently_committed_transactions[i]);
		if (recently_committed_transactions[i]->commit_id >= lowest_start_time) {
			// recently_committed_transactions is ordered on commit_id.
			// Thus, if the current commit_id is greater than
			// lowest_start_time, any subsequent commit IDs are also greater.
			break;
		}
		recently_committed_transactions[i]->awaiting_cleanup = true;
		cleanup_info.transactions.push_back(std::move(recently_committed_transactions[i]));
	}
	if (i > 0) {
		auto start = recently_committed_transactions.begin();
		auto end = recently_committed_transactions.begin() + static_cast<int64_t>(i);
		recently_committed_transactions.erase(start, end);
	}
}

void DuckTransactionManager::PurgeRecentlyCommittedInternal() {
	auto cleanup_info = make_uniq<DuckCleanupInfo>();
	auto lowest_start_time = TRANSACTION_ID_START;
	auto lowest_transaction_id = MAX_TRANSACTION_ID;
	for (auto &active : active_transactions) {
		lowest_start_time = MinValue(lowest_start_time, active->start_time);
		lowest_transaction_id = MinValue(lowest_transaction_id, active->transaction_id);
	}
	lowest_start_time = ApplyDurableFloor(lowest_start_time);
	lowest_active_start = lowest_start_time;
	lowest_active_id = lowest_transaction_id;
	cleanup_info->lowest_start_time = lowest_start_time;
	MoveExpiredRecentlyCommitted(lowest_start_time, *cleanup_info);
	if (cleanup_info->ScheduleCleanup()) {
		lock_guard<mutex> q_lock(cleanup_queue_lock);
		cleanup_queue.emplace(std::move(cleanup_info));
	}
}

void DuckTransactionManager::PurgeRecentlyCommitted() {
	lock_guard<mutex> t_lock(transaction_lock);
	PurgeRecentlyCommittedInternal();
}

void DuckTransactionManager::CleanupTransactions() {
	lock_guard<mutex> c_lock(cleanup_lock);
	while (true) {
		unique_ptr<DuckCleanupInfo> top_cleanup_info;
		{
			lock_guard<mutex> q_lock(cleanup_queue_lock);
			if (cleanup_queue.empty()) {
				// all transactions have been cleaned up - done
				return;
			}
			top_cleanup_info = std::move(cleanup_queue.front());
			cleanup_queue.pop();
		}
		if (top_cleanup_info) {
			top_cleanup_info->Cleanup();
		}
	}
}

ErrorData DuckTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	unique_lock<mutex> t_lock(transaction_lock);
	if (!db.IsSystem() && !db.IsTemporary()) {
		if (transaction.ChangesMade()) {
			if (transaction.IsReadOnly()) {
				throw InternalException("Attempting to commit a transaction that is read-only but has made changes - "
				                        "this should not be possible");
			}
		}
	}

	// check if we can checkpoint
	unique_ptr<StorageLockKey> lock;
	auto undo_properties = transaction.GetUndoProperties();
	auto checkpoint_decision = CanCheckpoint(transaction, lock, undo_properties);
	ErrorData error;
	unique_lock<mutex> held_wal_lock;
	// pin the WAL object (captured below while holding the WAL lock) so a concurrent checkpoint that resets it cannot
	// free the object out from under our GroupSync fsync, which runs with the WAL lock released
	shared_ptr<WriteAheadLog> wal_ref;
	// WAL generation of this commit's bytes, captured under the WAL lock for the WAL-ordered commit hook below
	idx_t wal_generation = 0;
	unique_ptr<StorageCommitState> commit_state;
	bool skip_wal_write_due_to_checkpoint = false;
	if (checkpoint_decision.can_checkpoint) {
		// we can perform an automatic checkpoint
		// we have two options:
		// either we write to the WAL, in which case we can perform concurrent commits while running
		// OR we skip writing to the WAL, in which case we cannot perform concurrent commits
		// the reason for this is that if we don't write this transactions' changes to the WAL
		// any failure during checkpoint will cause this transactions' changes to be lost,
		// while later concurrent commits will not be
		// this can cause undefined state, as those commits were made assuming this one was already committed
		if (undo_properties.estimated_size >= Settings::Get<AutoCheckpointSkipWalThresholdSetting>(context)) {
			skip_wal_write_due_to_checkpoint = true;
		}
	}
	bool should_write_to_wal = transaction.ShouldWriteToWAL(db);
	if (should_write_to_wal) {
		auto &storage_manager = db.GetStorageManager().Cast<SingleFileStorageManager>();
		// if we are committing changes and we are not doing a "checkpoint instead of WAL write"
		// we need to write to the WAL to make the changes durable
		// since WAL writes can take a long time - we grab the WAL lock here and unlock the transaction lock
		// read-only transactions can bypass this branch and start/commit while the WAL write is happening
		// unlock the transaction lock while we write to the WAL
		// note: we can only drop the transaction lock if we are NOT checkpointing
		// if we are checkpointing, we have already made certain decisions (e.g. the CheckpointType)
		t_lock.unlock();
		// grab the WAL lock and hold it until the entire commit is finished
		held_wal_lock = storage_manager.GetWALLock();
		wal_ref = storage_manager.GetWALShared();
		// capture the WAL generation under the WAL lock so the WAL-ordered commit hook below sees the exact
		// generation this commit's bytes append to
		wal_generation = storage_manager.GetBlockManager().GetCheckpointIteration();

		// Commit the changes to the WAL.
		if (!skip_wal_write_due_to_checkpoint) {
			error = transaction.WriteToWAL(context, db, commit_state);
		}

		// after we finish writing to the WAL we grab the transaction lock again
		t_lock.lock();
	}
	if (!error.HasError() && checkpoint_decision.can_checkpoint) {
		// now that we have the transaction lock again, new transactions can't start
		// figure out the checkpoint type now
		checkpoint_decision = GetCheckpointType(transaction, undo_properties);
		if (should_write_to_wal && skip_wal_write_due_to_checkpoint && !checkpoint_decision.can_checkpoint) {
			// we have not written to the WAL but we have now realized we can't checkpoint after all
			// in order to commit we need backpeddle and write to the WAL after all
			D_ASSERT(held_wal_lock.owns_lock());
			// unlock the transaction lock while we are writing to the WAL
			t_lock.unlock();
			error = transaction.WriteToWAL(context, db, commit_state);
			t_lock.lock();
			skip_wal_write_due_to_checkpoint = false;
		}
	}
	// in-memory databases don't have a WAL - we estimate how large their changeset is based on the undo properties
	if (!db.IsSystem()) {
		auto &storage_manager = db.GetStorageManager();
		if (storage_manager.InMemory() || db.GetRecoveryMode() == RecoveryMode::NO_WAL_WRITES) {
			storage_manager.AddWALSize(undo_properties.estimated_size);
		}
	}
	// obtain a commit id for the transaction
	CommitInfo info;
	info.commit_id = GetCommitTimestamp();

	// commit the UndoBuffer of the transaction
	if (!error.HasError()) {
		if (HasOtherTransactions(transaction)) {
			info.active_transactions = ActiveTransactionState::OTHER_TRANSACTIONS;
		} else {
			info.active_transactions = ActiveTransactionState::NO_OTHER_TRANSACTIONS;
		}
		error = transaction.Commit(db, info, std::move(commit_state));
		if (!error.HasError() && info.wal_flush_offset > 0) {
			// this commit wrote a WAL flush marker but has not been fsynced yet (that happens with the locks
			// released, below). Record it as pending: until its group fsync raises last_durable_commit, a new
			// snapshot is bounded just below it (see DurableSnapshotBound), so no one observes it before it is
			// durable. Stored under the transaction lock (and the WAL lock), so it is monotonic in commit order.
			last_pending_commit.store(info.commit_id, std::memory_order_release);
		}
		// Let registered client states commit dependent changes (serenedb's out-of-band search-index leg) now that
		// this commit's WAL entries and flush marker are appended, while we still hold the WAL lock (so hooks fire in
		// WAL-append order across the database's commits) and before the group fsync below -- the dependent state
		// gates its own durability on the WAL becoming durable. Only on success; on error the transaction is rolled
		// back below and the dependent changes are discarded via the rollback hook.
		if (!error.HasError() && context.registered_state) {
			for (auto &state : context.registered_state->States()) {
				state->TransactionPreCheckpoint(db, context, wal_generation, info.wal_flush_offset);
			}
		}
	}

	if (error.HasError()) {
		DUCKDB_LOG(context, TransactionLogType, db, "Rollback (after failed commit)", info.commit_id);

		// COMMIT not successful: ROLLBACK.
		checkpoint_decision = CheckpointDecision(error.Message());
		transaction.commit_id = 0;

		auto rollback_error = transaction.Rollback();
		if (rollback_error.HasError()) {
			throw FatalException(
			    "Failed to rollback transaction. Cannot continue operation.\nOriginal Error: %s\nRollback Error: %s",
			    error.Message(), rollback_error.Message());
		}
	} else {
		DUCKDB_LOG(context, TransactionLogType, db, "Commit", info.commit_id);
		last_commit = info.commit_id;

		// check if catalog changes were made
		if (transaction.catalog_version >= TRANSACTION_ID_START) {
			transaction.catalog_version = ++last_committed_version;
		}
	}
	OnCommitCheckpointDecision(checkpoint_decision, transaction);

	if (!checkpoint_decision.can_checkpoint && lock) {
		// we won't checkpoint after all due to an error during commit: unlock the checkpoint lock again
		skip_wal_write_due_to_checkpoint = false;
		lock.reset();
	}

	// Remove the transaction from the active set and gather cleanup information, still under the transaction lock (as
	// upstream): new-snapshot visibility is bounded by last_pending_commit/last_durable_commit, not by active-set
	// membership, so removal need not wait for durability. Doing it here, rather than re-acquiring the transaction
	// lock after the fsync, avoids an extra lock on every commit.
	bool store_transaction = undo_properties.has_updates || undo_properties.has_index_deletes ||
	                         undo_properties.has_catalog_changes || error.HasError();
	auto cleanup_info = RemoveTransaction(transaction, store_transaction);
	if (cleanup_info->ScheduleCleanup()) {
		lock_guard<mutex> q_lock(cleanup_queue_lock);
		cleanup_queue.emplace(std::move(cleanup_info));
	}

	// Release the transaction lock, and (unless we keep it for an in-commit checkpoint) the WAL lock, then make this
	// commit's WAL bytes durable with no locks held: concurrent committers overlap or share a single fsync, and a
	// committer returns only once its flush marker is durable. A snapshot starting during the fsync is still bounded
	// below this commit by DurableSnapshotBound (last_pending_commit was recorded above), so it never observes the
	// commit before it is durable.
	t_lock.unlock();
	if (!skip_wal_write_due_to_checkpoint && held_wal_lock.owns_lock()) {
		held_wal_lock.unlock();
	}
	if (!error.HasError() && wal_ref && info.wal_flush_offset > 0) {
		wal_ref->GroupSync(info.wal_flush_offset);
		// our fsync now covers our flush marker: raise the durable horizon to this commit id (raise-only) and wake
		// any parked drain
		RaiseDurableHorizon(info.commit_id);
	}

	CleanupTransactions();

	// now perform a checkpoint if (1) we are able to checkpoint, and (2) the WAL has reached sufficient size to
	// checkpoint
	if (checkpoint_decision.can_checkpoint) {
		if (!lock || lock->GetType() != StorageLockType::EXCLUSIVE) {
			throw InternalException("Checkpointing requires an exclusive lock to be held");
		}
		// a checkpoint persists every committed transaction and truncates the WAL, so every commit already visible
		// must first be durable in the WAL -- otherwise a crash mid-checkpoint would lose a commit a reader could have
		// observed. Wait for any in-flight group fsyncs to complete before truncating.
		WaitForInFlightCommits();
		// This commit (and any predecessor) parked by the durable floor is durable now: release and clean it, like
		// the pre-floor traverse in RemoveTransaction did. The checkpoint below cannot run with this transaction's
		// own undo outstanding ("Cannot create index with outstanding updates").
		PurgeRecentlyCommitted();
		CleanupTransactions();
		// we can unlock the transaction lock while checkpointing
		// checkpoint the database to disk
		CheckpointOptions options;
		options.action = CheckpointAction::ALWAYS_CHECKPOINT;
		options.type = checkpoint_decision.type;
		options.wal_lock = held_wal_lock.owns_lock() ? &held_wal_lock : nullptr;
		auto &storage_manager = db.GetStorageManager();
		try {
			storage_manager.CreateCheckpoint(context, options);
		} catch (std::exception &ex) {
			// a checkpoint failure here should not result in the commit being turned into a rollback
			// .. UNLESS we have skipped writing to the WAL and there are concurrent transactions active
			if (skip_wal_write_due_to_checkpoint) {
				error.Merge(ErrorData(ex));
			} else {
				// otherwise the failure is dropped here -- log it so it is not silently lost
				DUCKDB_LOG_WARNING(context, "Checkpoint failed on commit for database \"" + db.GetName() +
				                                "\": " + ErrorData(ex).Message());
			}
		}
		// a commit that skipped the WAL (wal_flush_offset == 0) never set last_pending_commit or raised
		// last_durable_commit, yet it is now durable via this checkpoint -- so raise the durable horizon to its id.
		// Otherwise DurableSnapshotBound keeps bounding new snapshots below it while a concurrent WAL commit holds
		// last_pending_commit above last_durable_commit, hiding this commit's rows (including from the committing
		// connection's own next statement). A WAL commit already raised the horizon via GroupSync, so this is a no-op.
		if (!error.HasError()) {
			RaiseDurableHorizon(info.commit_id);
		}
	}

	return error;
}

void DuckTransactionManager::RollbackTransaction(Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();

	DUCKDB_LOG(db.GetDatabase(), TransactionLogType, db, "Rollback", transaction.transaction_id);

	ErrorData error;
	{
		// Obtain the transaction lock and roll back.
		lock_guard<mutex> t_lock(transaction_lock);
		error = transaction.Rollback();

		// Remove the transaction from the list of active transactions and gather cleanup information.
		auto cleanup_info = RemoveTransaction(transaction);
		if (cleanup_info->ScheduleCleanup()) {
			lock_guard<mutex> q_lock(cleanup_queue_lock);
			cleanup_queue.emplace(std::move(cleanup_info));
		}
	}

	CleanupTransactions();

	if (error.HasError()) {
		throw FatalException("Failed to rollback transaction. Cannot continue operation.\nError: %s", error.Message());
	}
}

unique_ptr<DuckCleanupInfo> DuckTransactionManager::RemoveTransaction(DuckTransaction &transaction) noexcept {
	return RemoveTransaction(transaction, transaction.ChangesMade());
}

unique_ptr<DuckCleanupInfo> DuckTransactionManager::RemoveTransaction(DuckTransaction &transaction,
                                                                      bool store_transaction) noexcept {
	auto cleanup_info = make_uniq<DuckCleanupInfo>();

	// Find the transaction in the active transactions,
	// as well as the lowest start time, transaction id, and active query.
	idx_t t_index = active_transactions.size();
	auto lowest_start_time = TRANSACTION_ID_START;
	auto lowest_transaction_id = MAX_TRANSACTION_ID;
	for (idx_t i = 0; i < active_transactions.size(); i++) {
		if (active_transactions[i].get() == &transaction) {
			t_index = i;
			continue;
		}
		lowest_start_time = MinValue(lowest_start_time, active_transactions[i]->start_time);
		lowest_transaction_id = MinValue(lowest_transaction_id, active_transactions[i]->transaction_id);
	}
	lowest_start_time = ApplyDurableFloor(lowest_start_time);
	lowest_active_start = lowest_start_time;
	lowest_active_id = lowest_transaction_id;
	D_ASSERT(t_index != active_transactions.size());

	// Decide if we need to store the transaction, or if we can schedule it for cleanup.
	auto current_transaction = std::move(active_transactions[t_index]);
	if (store_transaction) {
		// If the transaction made any changes, we need to keep it around.
		if (transaction.commit_id != 0) {
			// The transaction was committed.
			// We add it to the list of recently committed transactions.
			recently_committed_transactions.push_back(std::move(current_transaction));
		} else {
			// The transaction was aborted.
			cleanup_info->transactions.push_back(std::move(current_transaction));
		}
	} else if (transaction.ChangesMade()) {
		// We do not need to store the transaction, directly schedule it for cleanup.
		current_transaction->awaiting_cleanup = true;
		cleanup_info->transactions.push_back(std::move(current_transaction));
	}
	cleanup_info->lowest_start_time = lowest_start_time;

	// Remove the transaction from the list of active transactions.
	active_transactions.unsafe_erase_at(t_index);

	// Traverse the recently_committed transactions to see if we can move any
	// to the list of transactions awaiting GC.
	MoveExpiredRecentlyCommitted(lowest_start_time, *cleanup_info);

	return cleanup_info;
}

idx_t DuckTransactionManager::GetCatalogVersion(Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	return transaction.catalog_version;
}

void DuckTransactionManager::PushCatalogEntry(Transaction &transaction_p, duckdb::CatalogEntry &entry,
                                              duckdb::data_ptr_t extra_data, duckdb::idx_t extra_data_size) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	if (!db.IsSystem() && !db.IsTemporary() && transaction.IsReadOnly()) {
		throw InternalException("Attempting to do catalog changes on a transaction that is read-only - "
		                        "this should not be possible");
	}
	transaction.catalog_version = ++last_uncommitted_catalog_version;
	transaction.PushCatalogEntry(entry, extra_data, extra_data_size);
}

void DuckTransactionManager::PushAttach(Transaction &transaction_p, AttachedDatabase &attached_db) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	if (!db.IsSystem()) {
		throw InternalException("Can only ATTACH in the system catalog");
	}
	transaction.catalog_version = ++last_uncommitted_catalog_version;
	transaction.PushAttach(attached_db);
}

} // namespace duckdb
