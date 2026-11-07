#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

struct AlterLog : StorageExtensionInfo {
	vector<string> entries;

	string Take() {
		auto result = StringUtil::Join(entries, "; ");
		entries.clear();
		return result;
	}
};

class AlterLoggingIndexEntry : public DuckIndexEntry {
public:
	AlterLoggingIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
	                       TableCatalogEntry &table, AlterLog &log)
	    : DuckIndexEntry(catalog, schema, create_info, table), log(log) {
	}
	AlterLoggingIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
	                       shared_ptr<IndexDataTableInfo> storage_info, AlterLog &log)
	    : DuckIndexEntry(catalog, schema, create_info, std::move(storage_info)), log(log) {
	}

	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override {
		auto create_info = GetInfo();
		auto result = make_uniq<AlterLoggingIndexEntry>(catalog, ParentSchema(context),
		                                                create_info->Cast<CreateIndexInfo>(), info, log);
		result->initial_index_size = initial_index_size;
		return std::move(result);
	}

	void OnAlter(const AlterInfo &alter) override {
		log.entries.push_back(EnumUtil::ToString(alter.type) + " " + name.GetIdentifierName() + " " +
		                      comment.ToString());
	}

private:
	AlterLog &log;
};

class AlterLoggingCatalog : public DuckCatalog {
public:
	AlterLoggingCatalog(AttachedDatabase &db, AlterLog &log) : DuckCatalog(db), log(log) {
	}

	unique_ptr<IndexCatalogEntry> MakeIndexEntry(DuckSchemaEntry &schema, CreateIndexInfo &info,
	                                             CatalogEntry &relation) override {
		return make_uniq<AlterLoggingIndexEntry>(*this, schema, info, relation.Cast<TableCatalogEntry>(), log);
	}

private:
	AlterLog &log;
};

struct AlterLoggingStorageExtension : StorageExtension {
	explicit AlterLoggingStorageExtension(shared_ptr<AlterLog> log) {
		storage_info = std::move(log);
		attach = [](optional_ptr<StorageExtensionInfo> extension_info, ClientContext &, AttachedDatabase &db,
		            const string &, AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq<AlterLoggingCatalog>(db, static_cast<AlterLog &>(*extension_info));
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("OnAlter runs on the version an ALTER created once its commit is final, and on its replay", "[catalog]") {
	auto path = TestCreatePath("on_alter.db");
	DeleteDatabase(path);
	auto log = make_shared_ptr<AlterLog>();
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	StorageExtension::Register(*config, "alter_logging", make_shared_ptr<AlterLoggingStorageExtension>(log));
	DuckDB db(nullptr, config.get());
	Connection con(db);

	REQUIRE_NO_FAIL(
	    con.Query("ATTACH '" + path + "' AS logged (TYPE alter_logging, STORAGE_VERSION 'serenedb_latest')"));
	REQUIRE_NO_FAIL(con.Query("USE logged"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE INDEX t_idx ON t (i)"));
	REQUIRE(log->Take().empty());

	REQUIRE_NO_FAIL(con.Query("COMMENT ON INDEX t_idx IS 'kept'"));
	REQUIRE(log->Take() == "SET_COMMENT t_idx kept");

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("COMMENT ON INDEX t_idx IS 'rolled back'"));
	REQUIRE(log->Take().empty());
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(log->Take().empty());

	REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = true"));
	REQUIRE_FAIL(con.Query("COMMENT ON INDEX t_idx IS 'failed commit'"));
	REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = false"));
	REQUIRE(log->Take().empty());

	REQUIRE_NO_FAIL(con.Query("ALTER INDEX t_idx RENAME TO t_renamed"));
	REQUIRE(log->Take() == "ALTER_INDEX t_renamed kept");

	REQUIRE_NO_FAIL(con.Query("USE memory"));
	REQUIRE_NO_FAIL(con.Query("DETACH logged"));
	REQUIRE_NO_FAIL(
	    con.Query("ATTACH '" + path + "' AS logged (TYPE alter_logging, STORAGE_VERSION 'serenedb_latest')"));
	REQUIRE(log->Take() == "SET_COMMENT t_idx kept; ALTER_INDEX t_renamed kept");
}
