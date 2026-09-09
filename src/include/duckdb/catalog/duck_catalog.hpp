//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/duck_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

class DuckSchemaEntry;
class InCatalogEntry;
class IndexCatalogEntry;
class StandardEntry;
class TableCatalogEntry;
struct CreateIndexInfo;
struct BoundCreateTableInfo;
struct CreateDatabaseInfo;
struct CreateForeignServerInfo;
struct CreateRoleInfo;
struct CreateTokenizerInfo;
struct DropInfo;

//! The Catalog object represents the catalog of the database.
class DuckCatalog : public Catalog {
public:
	explicit DuckCatalog(AttachedDatabase &db);
	~DuckCatalog() override;

public:
	bool IsDuckCatalog() override;
	void Initialize(bool load_builtin) override;

	string GetCatalogType() override {
		return "duckdb";
	}

	mutex &GetWriteLock() {
		return write_lock;
	}

	// Encryption Functions
	void SetEncryptionKeyId(const string &key_id);
	string &GetEncryptionKeyId();
	void SetIsEncrypted();
	bool GetIsEncrypted();

public:
	DUCKDB_API optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	DUCKDB_API virtual unique_ptr<IndexCatalogEntry> MakeIndexEntry(DuckSchemaEntry &schema, CreateIndexInfo &info,
	                                                                TableCatalogEntry &table);
	DUCKDB_API virtual unique_ptr<TableCatalogEntry>
	MakeTableEntry(CatalogTransaction transaction, DuckSchemaEntry &schema, BoundCreateTableInfo &info);
	DUCKDB_API virtual unique_ptr<InCatalogEntry> MakeRoleEntry(CreateRoleInfo &info);
	DUCKDB_API virtual unique_ptr<InCatalogEntry> MakeDatabaseEntry(CreateDatabaseInfo &info);
	DUCKDB_API virtual unique_ptr<InCatalogEntry> MakeForeignServerEntry(CreateForeignServerInfo &info);
	DUCKDB_API virtual unique_ptr<StandardEntry> MakeTokenizerEntry(DuckSchemaEntry &schema, CreateTokenizerInfo &info);

	DUCKDB_API optional_ptr<CatalogEntry> CreateRole(CatalogTransaction transaction, CreateRoleInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateDatabase(CatalogTransaction transaction, CreateDatabaseInfo &info);
	DUCKDB_API optional_ptr<CatalogEntry> CreateForeignServer(CatalogTransaction transaction,
	                                                          CreateForeignServerInfo &info);
	DUCKDB_API void DropRole(CatalogTransaction transaction, DropInfo &info);
	DUCKDB_API void DropDatabase(CatalogTransaction transaction, DropInfo &info);
	DUCKDB_API void DropForeignServer(CatalogTransaction transaction, DropInfo &info);

	DUCKDB_API void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	DUCKDB_API void ScanSchemas(std::function<void(SchemaCatalogEntry &)> callback);

	DUCKDB_API optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                                         const EntryLookupInfo &schema_lookup,
	                                                         OnEntryNotFound if_not_found) override;

	DUCKDB_API PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                               LogicalCreateTable &op, PhysicalOperator &plan) override;
	DUCKDB_API PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                                        optional_ptr<PhysicalOperator> plan) override;
	DUCKDB_API PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                                        PhysicalOperator &plan) override;
	DUCKDB_API PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                                        PhysicalOperator &plan) override;
	DUCKDB_API PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
	                                           LogicalMergeInto &op, PhysicalOperator &plan) override;
	DUCKDB_API unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, CatalogEntry &table,
	                                                       unique_ptr<LogicalOperator> plan) override;
	DUCKDB_API unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                                         unique_ptr<LogicalOperator> plan,
	                                                         unique_ptr<CreateIndexInfo> create_info,
	                                                         unique_ptr<AlterTableInfo> alter_info) override;

	CatalogSet &GetSchemaCatalogSet();
	CatalogSet &GetCatalogSet(CatalogType type);

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	vector<MetadataBlockInfo> GetMetadataInfo(ClientContext &context) override;

	DUCKDB_API bool InMemory() override;
	DUCKDB_API string GetDBPath() override;
	bool SupportsMultipleDMLCTEs() const override;
	DUCKDB_API bool IsEncrypted() const override;
	DUCKDB_API string GetEncryptionCipher() const override;

	DUCKDB_API optional_idx GetCatalogVersion(ClientContext &context) override;

	optional_ptr<DependencyManager> GetDependencyManager() override;

private:
	DUCKDB_API void DropSchema(CatalogTransaction transaction, DropInfo &info);
	DUCKDB_API void DropSchema(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> CreateSchemaInternal(CatalogTransaction transaction, CreateSchemaInfo &info);
	optional_ptr<CatalogEntry> AddEntry(CatalogTransaction transaction, unique_ptr<InCatalogEntry> entry,
	                                    OnCreateConflict on_conflict);
	void Verify() override;

private:
	//! The DependencyManager manages dependencies between different catalog objects
	unique_ptr<DependencyManager> dependency_manager;
	//! Write lock for the catalog
	mutex write_lock;
	//! The catalog set holding the schemas
	unique_ptr<CatalogSet> schemas;
	unique_ptr<CatalogSet> roles;
	unique_ptr<CatalogSet> databases;
	unique_ptr<CatalogSet> foreign_servers;

	//! Identifies whether the db is encrypted
	bool is_encrypted = false;
	//! If is encrypted, store the encryption key_id
	string encryption_key_id;
};

} // namespace duckdb
