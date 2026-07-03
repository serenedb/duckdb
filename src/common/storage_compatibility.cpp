#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

StorageCompatibility StorageCompatibility::FromDatabase(AttachedDatabase &db) {
	return FromIndex(db.GetStorageManager().GetStorageVersion());
}

StorageCompatibility StorageCompatibility::FromIndex(StorageVersion storage_version_p) {
	StorageCompatibility result;

	result.storage_version = storage_version_p;
	result.duckdb_version = StorageVersionInfo::GetStorageVersionString(storage_version_p);
	result.manually_set = false;
	return result;
}

StorageCompatibility StorageCompatibility::FromString(const string &input) {
	if (input.empty()) {
		throw InvalidInputException("Version string can not be empty");
	}

	auto storage_version = GetStorageVersion(input.c_str());
	if (storage_version == StorageVersion::INVALID) {
		auto candidates = GetStorageCandidates();
		throw InvalidInputException("The version string '%s' is not a known DuckDB version, valid options are: %s",
		                            input, StringUtil::Join(candidates, ", "));
	}
	StorageCompatibility result;
	result.duckdb_version = input;
	result.storage_version = storage_version;
	result.manually_set = true;
	return result;
}

const StorageCompatibility &StorageCompatibility::Default() {
#ifdef DUCKDB_ALTERNATIVE_VERIFY
	return Latest();
#else
#ifdef DUCKDB_LATEST_STORAGE
	return Latest();
#else
	static const StorageCompatibility default_compatibility = [] {
		auto res = FromIndex(StorageVersion::V0_10_2);
		res.duckdb_version = "latest";
		res.manually_set = false;
		return res;
	}();
	return default_compatibility;
#endif
#endif
}

const StorageCompatibility &StorageCompatibility::Latest() {
	static const StorageCompatibility latest_compatibility = [] {
		auto res = FromString("latest");
		res.manually_set = false;
		return res;
	}();
	return latest_compatibility;
}

bool StorageCompatibility::Compare(StorageVersion property_version) const {
	return property_version <= storage_version;
}

StorageVersion StorageCompatibility::GetStorageVersionCompatibility() const {
	return storage_version;
}

} // namespace duckdb
