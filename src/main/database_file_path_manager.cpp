#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

DatabasePathInfo::DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode)
    : name(name_p.GetIdentifierName()), access_mode(access_mode) {
	attached_databases.insert(manager);
}

idx_t DatabaseFilePathManager::ApproxDatabaseCount() const {
	lock_guard<mutex> path_lock(db_paths_lock);
	return db_paths.size();
}

InsertDatabasePathResult DatabaseFilePathManager::InsertDatabasePath(DatabaseManager &manager, const string &path,
                                                                     const Identifier &name,
                                                                     OnCreateConflict on_conflict,
                                                                     AttachOptions &options) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		throw InternalException("DatabaseFilePathManager::InsertDatabasePath - cannot insert in-memory database");
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.emplace(path, DatabasePathInfo(manager, name, options.access_mode));
	if (!entry.second) {
		auto &existing = entry.first->second;
		// The path is registered but no system currently has it attached: this is a stale entry left
		// behind by a database that has been detached while its cleanup (StoredDatabasePath destruction)
		// is still pending -- e.g. an attached database that is still pinned by a view or index defined
		// over it, or by a transaction that has not finalized yet. That database still owns the file and
		// still writes to it, so we must not open a second one over the same path: hand back the database
		// that is already there and let the caller re-register it under the requested name.
		if (existing.reuse_claimed) {
			// a re-attach of this path is in flight: wait for it to register the database (or to give the
			// claim back), rather than handing the same database out twice
			return InsertDatabasePathResult::ALREADY_EXISTS;
		}
		if (existing.attached_databases.empty()) {
			options.reused_database = existing.database.lock();
			if (options.reused_database) {
				// whether the database is still usable can only be checked while holding a reference to
				// it rather than this lock, so the caller decides and gives the claim back if it does not
				// go through with the re-attach
				existing.reuse_claimed = true;
				return InsertDatabasePathResult::REUSE_EXISTING;
			}
		}
		bool already_exists = false;
		bool attached_in_this_system = false;
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && existing.name == name) {
			already_exists = true;
			attached_in_this_system = existing.attached_databases.find(manager) != existing.attached_databases.end();
		}
		if (options.access_mode == AccessMode::READ_ONLY && existing.access_mode == AccessMode::READ_ONLY) {
			if (attached_in_this_system) {
				return InsertDatabasePathResult::ALREADY_EXISTS;
			}
			// all attaches are in read-only mode - there is no conflict, just increase the reference count
			existing.attached_databases.insert(manager);
			existing.reference_count++;
		} else {
			if (already_exists) {
				if (attached_in_this_system) {
					return InsertDatabasePathResult::ALREADY_EXISTS;
				}
				throw BinderException(
				    "Unique file handle conflict: Cannot attach \"%s\" - the database file \"%s\" is in "
				    "the process of being detached",
				    name, path);
			}
			throw BinderException(
			    "Unique file handle conflict: Cannot attach \"%s\" - the database file \"%s\" is already "
			    "attached by database \"%s\"",
			    name, path, existing.name);
		}
	}
	options.stored_database_path = make_uniq<StoredDatabasePath>(manager, *this, path, name);
	return InsertDatabasePathResult::SUCCESS;
}

void DatabaseFilePathManager::CommitReuse(DatabaseManager &manager, const string &path, const Identifier &name) {
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry == db_paths.end()) {
		return;
	}
	entry->second.name = name.GetIdentifierName();
	entry->second.attached_databases.insert(manager);
	entry->second.reuse_claimed = false;
}

void DatabaseFilePathManager::ReleaseReuse(const string &path) {
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry == db_paths.end()) {
		return;
	}
	entry->second.reuse_claimed = false;
}

void DatabaseFilePathManager::SetDatabase(const string &path, shared_ptr<AttachedDatabase> database) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		entry->second.database = std::move(database);
	}
}

void DatabaseFilePathManager::EraseDatabasePath(const string &path) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		if (entry->second.reference_count <= 1) {
			db_paths.erase(entry);
		} else {
			entry->second.reference_count--;
		}
	}
}

void DatabaseFilePathManager::DetachDatabase(DatabaseManager &manager, const string &path) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		entry->second.attached_databases.erase(manager);
	}
}

} // namespace duckdb
