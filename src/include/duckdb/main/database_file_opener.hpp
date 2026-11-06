//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database_file_opener.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/http/http_util.hpp"

namespace duckdb {
class DatabaseInstance;

class DatabaseFileOpener : public FileOpener {
public:
	explicit DatabaseFileOpener(DatabaseInstance &db_p) : db(db_p) {
	}

	Logger &GetLogger() const override {
		return Logger::Get(db);
	}

	SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) override {
		return db.TryGetCurrentSetting(key, result);
	}

	SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result, FileOpenerInfo &) override {
		return db.TryGetCurrentSetting(key, result);
	}

	optional_ptr<ClientContext> TryGetClientContext() override {
		return nullptr;
	}

	optional_ptr<DatabaseInstance> TryGetDatabase() override {
		return &db;
	}
	HTTPUtil &GetHTTPUtil() override {
		return HTTPUtil::Get(*TryGetDatabase());
	}

private:
	DatabaseInstance &db;
};

class DatabaseFileSystem : public OpenerFileSystem {
public:
	explicit DatabaseFileSystem(DatabaseInstance &db_p) : db(db_p), database_opener(db_p) {
	}

	FileSystem &GetFileSystem() const override {
		auto &config = DBConfig::GetConfig(db);
		return *config.file_system;
	}
	optional_ptr<FileOpener> GetOpener() const override {
		return &database_opener;
	}

	//! A remote file opens in the block cache, without asking the store for its metadata when the cache knows a fresh
	//! version of it
	shared_ptr<BlockCacheFile> OpenBlockFile(const string &path) override;
	//! Writing, moving or removing a remote file, or removing a directory it is in, drops its blocks from the block
	//! cache
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener) override;
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener) override;

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener = nullptr) override;

private:
	//! A file on an object store or a web server, which is read through the block cache
	static bool IsRemote(const string &path);
	void ForgetRemoteFile(const string &path);

	DatabaseInstance &db;
	mutable DatabaseFileOpener database_opener;
};

class LocalDatabaseFileSystem : public OpenerFileSystem {
public:
	explicit LocalDatabaseFileSystem(DatabaseInstance &db_p);

	FileSystem &GetFileSystem() const override;
	optional_ptr<FileOpener> GetOpener() const override {
		return &database_opener;
	}

private:
	DatabaseInstance &db;
	unique_ptr<FileSystem> owned_file_system;
	FileSystem &local_fs;
	mutable DatabaseFileOpener database_opener;
};

} // namespace duckdb
