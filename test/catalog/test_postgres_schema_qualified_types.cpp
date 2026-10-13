#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

class PostgresCompatibleCatalog : public DuckCatalog {
public:
	explicit PostgresCompatibleCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

	SqlCompatibility Compatibility() const override {
		return SqlCompatibility::POSTGRES;
	}
};

struct PostgresCompatibleStorageExtension : StorageExtension {
	PostgresCompatibleStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq<PostgresCompatibleCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("Types qualified with information_schema resolve like pg_catalog ones in PostgreSQL mode", "[catalog]") {
	DBConfig config;
	StorageExtension::Register(config, "postgres_compatible", make_shared_ptr<PostgresCompatibleStorageExtension>());
	DuckDB db(nullptr, &config);
	ExtensionLoader loader(*db.instance, "information_schema_domains");
	loader.RegisterType("sql_identifier", LogicalType::VARCHAR);
	Connection con(db);

	auto result = con.Query("SELECT 'x'::information_schema.sql_identifier");
	REQUIRE(result->HasError());

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS pg (TYPE postgres_compatible)"));
	REQUIRE_NO_FAIL(con.Query("USE pg"));

	result = con.Query("SELECT 'x'::sql_identifier, 'y'::pg_catalog.sql_identifier, "
	                   "current_database()::information_schema.sql_identifier");
	REQUIRE(CHECK_COLUMN(result, 0, {"x"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"y"}));
	REQUIRE(CHECK_COLUMN(result, 2, {"pg"}));

	REQUIRE_FAIL(con.Query("SELECT information_schema.lower('A')"));
}
