//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/storage_compatibility.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {
class AttachedDatabase;

class StorageCompatibility {
public:
	static StorageCompatibility FromDatabase(AttachedDatabase &db);
	static StorageCompatibility FromIndex(StorageVersion storage_version_p);
	static StorageCompatibility FromString(const string &input);
	//! The version a database gets when no STORAGE_VERSION was named: duckdb's, so the file stays
	//! readable by duckdb.
	static const StorageCompatibility &DuckDBDefault();
	//! The newest duckdb version. Names a storage format, so it belongs on anything that writes a
	//! database file duckdb should still be able to open.
	static const StorageCompatibility &DuckDBLatest();
	//! The newest version this engine implements. Belongs on in-memory serialization, which has no
	//! compatibility constraint and should use every capability the engine has.
	static const StorageCompatibility &SereneDBLatest();

public:
	bool Compare(StorageVersion property_version) const;
	StorageVersion GetStorageVersionCompatibility() const;

public:
	//! The user provided version
	string duckdb_version;
	//! The max storage version that should be serialized
	StorageVersion storage_version;
	//! Whether this was set by a manual SET/PRAGMA or default
	bool manually_set;

protected:
	StorageCompatibility() = default;
};

} // namespace duckdb
