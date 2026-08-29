//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/duck_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

//! One place an entry with a given oid is filed: the set the location's slot names -- inside the schema when one
//! is given, at the catalog root otherwise -- and the name it is keyed under there. Kept per placed version node,
//! so the location outlives a rename for readers whose snapshot predates it.
struct EntryOidLocation {
	//! The oid of the schema the entry sits under, or DConstants::INVALID_INDEX for a catalog-level set
	idx_t schema_oid;
	CatalogType slot;
	Identifier name;
	//! Version nodes alive under this filing; the location leaves the map with the last one
	idx_t count;
};

//! The Catalog object represents the catalog of the database.
class DuckCatalog : public Catalog {
public:
	//! `case_sensitive_schemas` keys the schema set by the exact name, for a catalog that folds identifiers
	//! by its own rules before they get here -- the same flag CatalogSet itself takes.
	explicit DuckCatalog(AttachedDatabase &db, bool case_sensitive_schemas = false);
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
	DUCKDB_API void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	//! The same without a client context, over what is committed. This is what the checkpoint enumerates, so a
	//! catalog whose schemas live outside the inherited schema set has to answer here or its storage is never
	//! written.
	DUCKDB_API virtual void ScanSchemas(std::function<void(SchemaCatalogEntry &)> callback);

	DUCKDB_API optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                                         const EntryLookupInfo &schema_lookup,
	                                                         OnEntryNotFound if_not_found) override;

	DUCKDB_API PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                               LogicalCreateTable &op, PhysicalOperator &plan) override;
	//! The insert operator a CREATE TABLE AS load runs through: batch insert when the source is
	//! partitionable and order need not be preserved, parallel streaming insert otherwise.
	DUCKDB_API static PhysicalOperator &PlanCreateTableAsInsert(ClientContext &context, PhysicalPlanGenerator &planner,
	                                                            LogicalCreateTable &op, SchemaCatalogEntry &schema,
	                                                            unique_ptr<BoundCreateTableInfo> info,
	                                                            PhysicalOperator &plan, idx_t estimated_cardinality);
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

	//! The entry `oid` names anywhere in this catalog, resolved under `transaction`'s snapshot, or null.
	//! When an oid is filed in more than one place -- a secondary filing beside the primary one -- the
	//! earlier filing wins, which is the order the placement road writes them in.
	DUCKDB_API optional_ptr<CatalogEntry> GetEntryById(CatalogTransaction transaction, idx_t oid);
	//! The catalog-level set holding entries of `slot`, for a by-id location that names no schema
	DUCKDB_API virtual optional_ptr<CatalogSet> RootEntrySet(CatalogType slot);
	void AddOidLocation(idx_t oid, idx_t schema_oid, CatalogType slot, const Identifier &name);
	void RemoveOidLocation(idx_t oid, idx_t schema_oid, CatalogType slot, const Identifier &name);

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
	void Verify() override;

private:
	//! The DependencyManager manages dependencies between different catalog objects
	unique_ptr<DependencyManager> dependency_manager;
	//! Write lock for the catalog
	mutex write_lock;
	//! The catalog set holding the schemas
	unique_ptr<CatalogSet> schemas;

	//! Identifies whether the db is encrypted
	bool is_encrypted = false;
	//! If is encrypted, store the encryption key_id
	string encryption_key_id;
	//! Every place each oid is currently filed, maintained by the sets at version placement and destruction.
	//! Pure data rather than set pointers, so a location can never dangle: resolution goes through the live
	//! schema each time, and the visible version's own oid is the authority on a hit.
	mutex oid_locations_lock;
	unordered_map<idx_t, vector<EntryOidLocation>> oid_locations;
};

} // namespace duckdb
