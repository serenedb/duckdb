//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/checked_integer.hpp"

namespace duckdb {
class AttachedDatabase;
class Catalog;
class CatalogEntryRetriever;
class CatalogSet;
class ClientContext;
class DatabaseInstance;
class MetaTransaction;
class TaskScheduler;
struct AttachOptions;
struct AlterInfo;

//! The DatabaseManager is a class that sits at the root of all attached databases
class DatabaseManager {
	friend class Catalog;

public:
	explicit DatabaseManager(DatabaseInstance &db);
	~DatabaseManager();

public:
	static DatabaseManager &Get(DatabaseInstance &db);
	static DatabaseManager &Get(ClientContext &db);
	static DatabaseManager &Get(AttachedDatabase &db);

	DatabaseInstance &GetInstance() {
		return db;
	}
	//! Initializes the system catalog of the attached SYSTEM_DATABASE.
	void InitializeSystemCatalog();
	//! Finalize starting up the system
	void FinalizeStartup();
	//! Get an attached database by its name
	optional_ptr<AttachedDatabase> GetDatabase(ClientContext &context, const Identifier &name);
	shared_ptr<AttachedDatabase> GetDatabase(const Identifier &name);
	//! Attach a new database
	shared_ptr<AttachedDatabase> AttachDatabase(ClientContext &context, AttachInfo &info, AttachOptions &options);

	//! Detach an existing database
	void DetachDatabase(ClientContext &context, const Identifier &name, OnEntryNotFound if_not_found);
	//! Alter operation dispatcher
	void Alter(ClientContext &context, AlterInfo &info);
	//! Rollback the attach of a database
	shared_ptr<AttachedDatabase> DetachInternal(const Identifier &name);
	//! Returns a reference to the system catalog
	Catalog &GetSystemCatalog();

	static Identifier GetDefaultDatabase(ClientContext &context);
	void SetDefaultDatabase(ClientContext &context, const string &new_value);

	//! Inserts a path to name mapping to the database paths map
	InsertDatabasePathResult InsertDatabasePath(const AttachInfo &info, AttachOptions &options);

	//! Returns the database type. This might require checking the header of the file, in which case the file handle is
	//! necessary. We can only grab the file handle, if it is not yet held, even for uncommitted changes. Thus, we have
	//! to lock for this operation.
	void GetDatabaseType(ClientContext &context, AttachInfo &info, const DBConfig &config, AttachOptions &options);
	//! Scans the catalog set and adds each committed database entry, and each database entry of the current
	//! transaction, to a vector holding AttachedDatabase references
	vector<shared_ptr<AttachedDatabase>> GetDatabases(ClientContext &context,
	                                                  const optional_idx max_db_count = optional_idx());
	//! Scans the catalog set and returns each committed database entry
	vector<shared_ptr<AttachedDatabase>> GetDatabases();
	//! Returns the approximate count of attached databases.
	idx_t ApproxDatabaseCount();
	//! Returns the number of remote catalogs currently attached.
	idx_t GetRemoteCatalogCount() const {
		return remote_catalog_count.load();
	}
	//! Removes all databases from the catalog set. This is necessary for the database instance's destructor,
	//! as the database manager has to be alive when destroying the catalog set objects.
	void ResetDatabases();

	transaction_t GetNewQueryNumber() {
		return current_query_number++;
	}
	transaction_t ActiveQueryNumber() const {
		return current_query_number;
	}
	transaction_t GetNewTransactionNumber() {
		return current_transaction_id++;
	}
	transaction_t ActiveTransactionNumber() const {
		return current_transaction_id;
	}
	idx_t NextOid() {
		return NextOids(1);
	}
	//! `count` consecutive object ids, the first of them returned. Every id up to the last is durably spent
	//! before any of them is handed out: an id can name something that outlives the transaction meant to
	//! record it -- a directory, a log shard -- so reissuing one after a crash would collide with what is
	//! already on disk.
	idx_t NextOids(idx_t count) {
		auto first = next_oid.fetch_add(count);
		auto last = first + count - 1;
		if (last >= reserved_oid.load(std::memory_order_acquire)) {
			ReserveOids(last);
		}
		return first;
	}
	//! Raises the allocator past `oid`, for a host reading back ids it did not hand out.
	void RestoreOid(idx_t oid);
	//! Raises the durable horizon, for a host reading back one it recorded.
	void RestoreOidReservation(idx_t horizon);
	idx_t OidReservation() const {
		return reserved_oid.load(std::memory_order_acquire);
	}
	//! Records that every id up to its argument is spent. Set by a host that persists the allocator; without
	//! one the ids are process-local and nothing is written down.
	void SetOidReservationSink(void (*sink)(idx_t)) {
		oid_reservation_sink.store(sink, std::memory_order_release);
	}
	bool HasDefaultDatabase() {
		return !default_database.empty();
	}
	//! Gets a list of all attached database paths
	vector<string> GetAttachedDatabasePaths();

	shared_ptr<AttachedDatabase> GetDatabaseInternal(const lock_guard<mutex> &, const Identifier &name);
	shared_ptr<AttachedDatabase> LookupDatabase(ClientContext &context, const Identifier &name,
	                                            optional_ptr<MetaTransaction> meta_transaction);

private:
	optional_ptr<AttachedDatabase> FinalizeAttach(ClientContext &context, AttachInfo &info,
	                                              shared_ptr<AttachedDatabase> database);
	void ReserveOids(idx_t oid);

private:
	//! How far past the allocator one reservation reaches. Overshooting only costs the ids in between, which
	//! are never reused anyway, so this is one write per a few hundred statements.
	static constexpr idx_t OID_RESERVE_BLOCK = 1024;

	DatabaseInstance &db;
	//! The system database is a special database that holds system entries (e.g. functions)
	shared_ptr<AttachedDatabase> system;
	//! Lock for databases
	mutex databases_lock;
	//! The set of attached databases
	identifier_map_t<shared_ptr<AttachedDatabase>> databases;
	//! The next object id handed out by the NextOid method
	atomic<idx_t> next_oid;
	//! The highest id the sink has been told is spent, and the lock that serializes telling it. Deliberately
	//! not databases_lock: an id is allocated while building objects, which is outside every lock here.
	atomic<idx_t> reserved_oid;
	mutex oid_reservation_lock;
	atomic<void (*)(idx_t)> oid_reservation_sink;
	//! The current query number
	atomic<transaction_t> current_query_number;
	//! The current transaction number
	atomic<transaction_t> current_transaction_id;
	//! Count of remote catalogs currently attached; used to skip the remote pushdown optimizer when zero
	atomic<CheckedInteger<idx_t, InternalException>> remote_catalog_count;
	//! The current default database
	Identifier default_database;
	//! Manager for ensuring we never open the same database file twice in the same program
	shared_ptr<DatabaseFilePathManager> path_manager;

private:
	//! Rename an existing database
	void RenameDatabase(ClientContext &context, const Identifier &old_name, const Identifier &new_name,
	                    OnEntryNotFound if_not_found);
};

} // namespace duckdb
