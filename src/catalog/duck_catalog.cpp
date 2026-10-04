#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_database_info.hpp"
#include "duckdb/parser/parsed_data/create_foreign_server_info.hpp"
#include "duckdb/parser/parsed_data/create_role_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_tokenizer_info.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/function/function_list.hpp"
#include "duckdb/common/encryption_state.hpp"

namespace duckdb {

DuckCatalog::DuckCatalog(AttachedDatabase &db, bool case_sensitive_names)
    : Catalog(db), dependency_manager(make_uniq<DependencyManager>(*this)),
      schemas(make_uniq<CatalogSet>(*this, IsSystemCatalog() ? make_uniq<DefaultSchemaGenerator>(*this) : nullptr,
                                    case_sensitive_names)),
      roles(make_uniq<CatalogSet>(*this, nullptr, case_sensitive_names)),
      databases(make_uniq<CatalogSet>(*this, nullptr, case_sensitive_names)),
      foreign_servers(make_uniq<CatalogSet>(*this, nullptr, case_sensitive_names)) {
}

DuckCatalog::~DuckCatalog() {
}

void DuckCatalog::Initialize(bool load_builtin) {
	// first initialize the base system catalogs
	// these are never written to the WAL
	// we start these at 1 because deleted entries default to 0
	auto data = CatalogTransaction::GetSystemTransaction(GetDatabase());

	// create the default schema
	CreateSchemaInfo info;
	info.SetQualifiedName(QualifiedName({Identifier::DefaultSchema()}, Identifier()));
	info.internal = true;
	info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	info.oid = DefaultSchemaOid();
	CreateSchema(data, info);

	if (load_builtin) {
		BuiltinFunctions builtin(data, *this);
		builtin.Initialize();

		// initialize default functions
		FunctionList::RegisterFunctions(*this, data);
	}

	Verify();
}

bool DuckCatalog::IsDuckCatalog() {
	return true;
}

bool DuckCatalog::SupportsMultipleDMLCTEs() const {
	return true;
}

optional_ptr<DependencyManager> DuckCatalog::GetDependencyManager() {
	return dependency_manager.get();
}

//===--------------------------------------------------------------------===//
// Schema
//===--------------------------------------------------------------------===//
unique_ptr<IndexCatalogEntry> DuckCatalog::MakeIndexEntry(DuckSchemaEntry &schema, CreateIndexInfo &info,
                                                          CatalogEntry &relation) {
	return make_uniq<DuckIndexEntry>(*this, schema, info, relation.Cast<TableCatalogEntry>());
}

unique_ptr<TableCatalogEntry> DuckCatalog::MakeTableEntry(CatalogTransaction transaction, DuckSchemaEntry &schema,
                                                          BoundCreateTableInfo &info) {
	return make_uniq<DuckTableEntry>(*this, schema, info);
}

optional_ptr<CatalogEntry> DuckCatalog::CreateSchemaInternal(CatalogTransaction transaction, CreateSchemaInfo &info) {
	LogicalDependencyList dependencies;

	auto parents = info.ParentSchemas();
	if (parents.empty()) {
		// top-level schema
		if (!info.internal && DefaultSchemaGenerator::IsDefaultSchema(info.SchemaName())) {
			return nullptr;
		}
		auto entry = make_uniq<DuckSchemaEntry>(*this, info);
		auto result = entry.get();
		if (!schemas->CreateEntry(transaction, info.SchemaName(), std::move(entry), dependencies)) {
			return nullptr;
		}
		return result;
	}
	EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, info.GetQualifiedName().Parent().Parent());
	auto parent =
	    LookupSchemaPath(transaction, lookup, OnEntryNotFound::THROW_EXCEPTION, [&](const EntryLookupInfo &root) {
		    auto entry = schemas->GetEntry(transaction, root.GetEntryIdentifier());
		    if (!entry) {
			    throw CatalogException("%s is not a catalog or schema", root.GetEntryIdentifier());
		    }
		    return entry;
	    });
	return parent->Cast<DuckSchemaEntry>().CreateSchema(transaction, info);
}

void DuckCatalog::AlterSchema(CatalogTransaction transaction, SchemaCatalogEntry &schema, AlterSchemaInfo &info) {
	switch (info.alter_schema_type) {
	case AlterSchemaType::SET_SCHEMA_OPTIONS:
		throw NotImplementedException("SET (<options>) is not supported for DuckDB schemas");
	case AlterSchemaType::RESET_SCHEMA_OPTIONS:
		throw NotImplementedException("RESET (<options>) is not supported for DuckDB schemas");
	case AlterSchemaType::RENAME_SCHEMA:
		return AlterSchemaEntry(transaction, schema, info);
	default:
		throw InternalException("Unrecognized alter schema type!");
	}
}

optional_ptr<CatalogEntry> DuckCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	D_ASSERT(!info.SchemaName().empty());
	auto result = CreateSchemaInternal(transaction, info);
	if (!result) {
		if (info.ShouldReplaceOnConflict()) {
			DropInfo drop_info;
			drop_info.type = CatalogType::SCHEMA_ENTRY;
			drop_info.SetQualifiedName(info.GetQualifiedName().Parent());
			DropSchema(transaction, drop_info);
			result = CreateSchemaInternal(transaction, info);
			if (!result) {
				throw InternalException("Failed to create schema entry in CREATE_OR_REPLACE");
			}
		}
		return nullptr;
	}
	return result;
}

void DuckCatalog::DropSchema(CatalogTransaction transaction, DropInfo &info) {
	EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, info.GetQualifiedName());
	auto schema = LookupSchema(transaction, lookup, info.if_not_found);
	if (!schema) {
		return;
	}
	D_ASSERT(schema->set);
	if (!schema->set->DropEntry(transaction, schema->name, info.cascade, info.allow_drop_internal)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingEntry(lookup, string());
		}
	}
}

void DuckCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	DropSchema(GetCatalogTransaction(context), info);
}

void DuckCatalog::AlterSchemaEntry(CatalogTransaction transaction, SchemaCatalogEntry &schema, AlterInfo &info) {
	auto parent_path = schema.GetParentSchemaPath(transaction);
	auto &set = parent_path.empty() ? *schemas
	                                : GetSchema(transaction, parent_path, OnEntryNotFound::THROW_EXCEPTION)
	                                      ->Cast<DuckSchemaEntry>()
	                                      .GetCatalogSet(CatalogType::SCHEMA_ENTRY);
	if (!set.AlterEntry(transaction, schema.name, info)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingEntry(CatalogType::SCHEMA_ENTRY, schema.name, string());
		}
	}
}

unique_ptr<InCatalogEntry> DuckCatalog::MakeRoleEntry(CreateRoleInfo &info) {
	throw NotImplementedException("Roles are not supported by this catalog");
}

unique_ptr<InCatalogEntry> DuckCatalog::MakeDatabaseEntry(CreateDatabaseInfo &info) {
	throw NotImplementedException("Database entries are not supported by this catalog");
}

unique_ptr<InCatalogEntry> DuckCatalog::MakeForeignServerEntry(CreateForeignServerInfo &info) {
	throw NotImplementedException("Foreign servers are not supported by this catalog");
}

unique_ptr<StandardEntry> DuckCatalog::MakeTokenizerEntry(DuckSchemaEntry &schema, CreateTokenizerInfo &info) {
	throw NotImplementedException("Text search dictionaries are not supported by this catalog");
}

optional_ptr<CatalogEntry> DuckCatalog::AddEntry(CatalogTransaction transaction, unique_ptr<InCatalogEntry> entry,
                                                 OnCreateConflict on_conflict) {
	auto entry_name = entry->name;
	auto entry_type = entry->type;
	auto result = entry.get();
	auto &set = GetCatalogSet(entry_type);
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && set.GetEntry(transaction, entry_name)) {
		return nullptr;
	}
	if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		auto old_entry = set.GetEntry(transaction, entry_name);
		if (old_entry) {
			entry->permissions = old_entry->permissions;
			(void)set.DropEntry(transaction, entry_name, false, entry->internal);
		}
	}
	if (!set.CreateEntry(transaction, entry_name, std::move(entry), LogicalDependencyList())) {
		if (on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			throw CatalogException::EntryAlreadyExists(entry_type, entry_name);
		}
		return nullptr;
	}
	return result;
}

optional_ptr<CatalogEntry> DuckCatalog::CreateRole(CatalogTransaction transaction, CreateRoleInfo &info) {
	return AddEntry(transaction, MakeRoleEntry(info), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckCatalog::CreateDatabase(CatalogTransaction transaction, CreateDatabaseInfo &info) {
	return AddEntry(transaction, MakeDatabaseEntry(info), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckCatalog::CreateForeignServer(CatalogTransaction transaction,
                                                            CreateForeignServerInfo &info) {
	return AddEntry(transaction, MakeForeignServerEntry(info), info.on_conflict);
}

void DuckCatalog::DropRole(CatalogTransaction transaction, DropInfo &info) {
	D_ASSERT(!info.GetQualifiedName().Name().empty());
	if (!roles->DropEntry(transaction, info.GetQualifiedName().Name(), info.cascade)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingEntry(CatalogType::ROLE_ENTRY, info.GetQualifiedName().Name(), string());
		}
	}
}

void DuckCatalog::DropDatabase(CatalogTransaction transaction, DropInfo &info) {
	D_ASSERT(!info.GetQualifiedName().Name().empty());
	if (!databases->DropEntry(transaction, info.GetQualifiedName().Name(), info.cascade)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingEntry(CatalogType::DATABASE_ENTRY, info.GetQualifiedName().Name(), string());
		}
	}
}

void DuckCatalog::DropForeignServer(CatalogTransaction transaction, DropInfo &info) {
	D_ASSERT(!info.GetQualifiedName().Name().empty());
	if (!foreign_servers->DropEntry(transaction, info.GetQualifiedName().Name(), info.cascade)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingEntry(CatalogType::FOREIGN_SERVER_ENTRY, info.GetQualifiedName().Name(),
			                                     string());
		}
	}
}

CatalogSet &DuckCatalog::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::SCHEMA_ENTRY:
		return *schemas;
	case CatalogType::ROLE_ENTRY:
		return *roles;
	case CatalogType::DATABASE_ENTRY:
		return *databases;
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return *foreign_servers;
	default:
		throw InternalException("Unsupported catalog type in catalog: %s", CatalogTypeToString(type));
	}
}

void DuckCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	// obtain the transaction once (up front) so the nested scan does not re-acquire the meta-transaction lock while
	// holding a catalog set lock
	auto transaction = GetCatalogTransaction(context);
	schemas->Scan(transaction, [&](CatalogEntry &entry) {
		auto &schema = entry.Cast<SchemaCatalogEntry>();
		schema.ScanSchemaTree(transaction, callback);
	});
}

void DuckCatalog::ScanSchemas(std::function<void(SchemaCatalogEntry &)> callback) {
	schemas->Scan([&](CatalogEntry &entry) {
		auto &schema = entry.Cast<SchemaCatalogEntry>();
		schema.ScanSchemaTree(callback);
	});
}

CatalogSet &DuckCatalog::GetSchemaCatalogSet() {
	return *schemas;
}

optional_ptr<SchemaCatalogEntry> DuckCatalog::LookupSchema(CatalogTransaction transaction,
                                                           const EntryLookupInfo &schema_lookup,
                                                           OnEntryNotFound if_not_found) {
	return LookupSchemaPath(transaction, schema_lookup, if_not_found, [&](const EntryLookupInfo &lookup) {
		return schemas->GetEntry(transaction, lookup.GetEntryIdentifier());
	});
}

DatabaseSize DuckCatalog::GetDatabaseSize(ClientContext &context) {
	auto &transaction = DuckTransactionManager::Get(db);
	auto lock = transaction.SharedCheckpointLock();
	return db.GetStorageManager().GetDatabaseSize();
}

vector<MetadataBlockInfo> DuckCatalog::GetMetadataInfo(ClientContext &context) {
	auto &transaction = DuckTransactionManager::Get(db);
	auto lock = transaction.SharedCheckpointLock();
	return db.GetStorageManager().GetMetadataInfo();
}

bool DuckCatalog::InMemory() {
	return db.GetStorageManager().InMemory();
}

string DuckCatalog::GetDBPath() {
	return db.GetStorageManager().GetDBPath();
}

bool DuckCatalog::IsEncrypted() const {
	return IsSystemCatalog() ? false : db.GetStorageManager().IsEncrypted();
}

string DuckCatalog::GetEncryptionCipher() const {
	return IsSystemCatalog() ? string() : EncryptionTypes::CipherToString(db.GetStorageManager().GetCipher());
}

void DuckCatalog::Verify() {
#ifdef D_ASSERT_IS_ENABLED
	DUCKDB_DEBUG_VERIFY_GUARD();
	Catalog::Verify();
	schemas->Verify(*this);
	roles->Verify(*this);
	databases->Verify(*this);
	foreign_servers->Verify(*this);
#endif
}

optional_idx DuckCatalog::GetCatalogVersion(ClientContext &context) {
	auto &transaction_manager = DuckTransactionManager::Get(db);
	auto transaction = GetCatalogTransaction(context);
	D_ASSERT(transaction.transaction);
	return transaction_manager.GetCatalogVersion(*transaction.transaction);
}

//===--------------------------------------------------------------------===//
// Encryption
//===--------------------------------------------------------------------===//
void DuckCatalog::SetEncryptionKeyId(const string &key_id) {
	encryption_key_id = key_id;
}

string &DuckCatalog::GetEncryptionKeyId() {
	return encryption_key_id;
}

void DuckCatalog::SetIsEncrypted() {
	is_encrypted = true;
}

bool DuckCatalog::GetIsEncrypted() {
	return is_encrypted;
}

} // namespace duckdb
