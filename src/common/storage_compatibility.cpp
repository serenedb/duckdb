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
		throw InvalidInputException("The version string '%s' is not a known storage version, valid options are: %s",
		                            input, StringUtil::Join(candidates, ", "));
	}
	if (IsSereneDBStorageVersion(storage_version) && !IsReadableStorageVersion(storage_version)) {
		throw InvalidInputException(
		    "The storage version '%s' can no longer be read by this version of SereneDB, which reads serenedb "
		    "versions %s through %s",
		    input, StorageVersionInfo::GetStorageVersionString(SERENEDB_VERSION_LOWER),
		    StorageVersionInfo::GetStorageVersionString(SERENEDB_VERSION_UPPER));
	}
	StorageCompatibility result;
	result.duckdb_version = input;
	result.storage_version = storage_version;
	result.manually_set = true;
	return result;
}

const StorageCompatibility &StorageCompatibility::DuckDBDefault() {
#ifdef DUCKDB_ALTERNATIVE_VERIFY
	return DuckDBLatest();
#else
#ifdef DUCKDB_LATEST_STORAGE
	return DuckDBLatest();
#else
	static const StorageCompatibility default_compatibility = [] {
		auto res = FromIndex(StorageVersionInfo::GetStorageVersionDefault());
		res.duckdb_version = "latest";
		res.manually_set = false;
		return res;
	}();
	return default_compatibility;
#endif
#endif
}

const StorageCompatibility &StorageCompatibility::DuckDBLatest() {
	static const StorageCompatibility latest_compatibility = [] {
		auto res = FromString("latest");
		res.manually_set = false;
		return res;
	}();
	return latest_compatibility;
}

const StorageCompatibility &StorageCompatibility::SereneDBLatest() {
	static const StorageCompatibility latest_compatibility = [] {
		auto res = FromString("serenedb_latest");
		res.manually_set = false;
		return res;
	}();
	return latest_compatibility;
}

bool StorageCompatibility::Compare(StorageVersion property_version) const {
	if (IsSereneDBStorageVersion(property_version) && !IsSereneDBStorageVersion(storage_version)) {
		return false;
	}
	return property_version <= storage_version;
}

bool StorageCompatibility::CanPersistRowIdGaps() const {
	return Compare(StorageVersion::V2_0_0);
}

bool StorageCompatibility::CompareVersionString(const string &property_version) const {
	auto property_version_val = GetSerializationVersionDeprecated(property_version.c_str());
	auto deprecated_serialization_version = GetSerializationVersionDeprecated(duckdb_version.c_str());
	return property_version_val <= deprecated_serialization_version;
}

StorageVersion StorageCompatibility::GetStorageVersionCompatibility() const {
	return storage_version;
}

} // namespace duckdb
