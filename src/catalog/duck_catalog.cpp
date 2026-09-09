#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
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

DuckCatalog::DuckCatalog(AttachedDatabase &db)
    : Catalog(db), dependency_manager(make_uniq<DependencyManager>(*this)),
      schemas(make_uniq<CatalogSet>(*this, IsSystemCatalog() ? make_uniq<DefaultSchemaGenerator>(*this) : nullptr)),
      roles(make_uniq<CatalogSet>(*this)), databases(make_uniq<CatalogSet>(*this)),
      foreign_servers(make_uniq<CatalogSet>(*this)) {
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
                                                          TableCatalogEntry &table) {
	return make_uniq<DuckIndexEntry>(*this, schema, info, table);
}

unique_ptr<TableCatalogEntry> DuckCatalog::MakeTableEntry(CatalogTransaction transaction, DuckSchemaEntry &schema,
                                                          BoundCreateTableInfo &info) {
	return make_uniq<DuckTableEntry>(*this, schema, info);
}

optional_ptr<CatalogEntry> DuckCatalog::CreateSchemaInternal(CatalogTransaction transaction, CreateSchemaInfo &info) {
	LogicalDependencyList dependencies;

	if (!info.internal && DefaultSchemaGenerator::IsDefaultSchema(info.GetQualifiedName().Schema())) {
		return nullptr;
	}
	auto entry = make_uniq<DuckSchemaEntry>(*this, info);
	auto result = entry.get();
	if (!schemas->CreateEntry(transaction, info.GetQualifiedName().Schema(), std::move(entry), dependencies)) {
		return nullptr;
	}
	return result;
}

optional_ptr<CatalogEntry> DuckCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	D_ASSERT(!info.GetQualifiedName().Schema().empty());
	auto result = CreateSchemaInternal(transaction, info);
	if (!result) {
		switch (info.on_conflict) {
		case OnCreateConflict::ERROR_ON_CONFLICT:
			throw CatalogException::EntryAlreadyExists(CatalogType::SCHEMA_ENTRY, info.GetQualifiedName().Schema());
		case OnCreateConflict::REPLACE_ON_CONFLICT: {
			DropInfo drop_info;
			drop_info.type = CatalogType::SCHEMA_ENTRY;
			drop_info.SetQualifiedName(
			    QualifiedName(info.GetQualifiedName().Catalog(), INVALID_SCHEMA, info.GetQualifiedName().Schema()));
			DropSchema(transaction, drop_info);
			result = CreateSchemaInternal(transaction, info);
			if (!result) {
				throw InternalException("Failed to create schema entry in CREATE_OR_REPLACE");
			}
			break;
		}
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			break;
		default:
			throw InternalException("Unsupported OnCreateConflict for CreateSchema");
		}
		return nullptr;
	}
	return result;
}

void DuckCatalog::DropSchema(CatalogTransaction transaction, DropInfo &info) {
	D_ASSERT(!info.GetQualifiedName().Name().empty());
	if (!schemas->DropEntry(transaction, info.GetQualifiedName().Name(), info.cascade)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingEntry(CatalogType::SCHEMA_ENTRY, info.GetQualifiedName().Name(), string());
		}
	}
}

void DuckCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	DropSchema(GetCatalogTransaction(context), info);
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
	schemas->Scan(GetCatalogTransaction(context),
	              [&](CatalogEntry &entry) { callback(entry.Cast<SchemaCatalogEntry>()); });
}

void DuckCatalog::ScanSchemas(std::function<void(SchemaCatalogEntry &)> callback) {
	schemas->Scan([&](CatalogEntry &entry) { callback(entry.Cast<SchemaCatalogEntry>()); });
}

CatalogSet &DuckCatalog::GetSchemaCatalogSet() {
	return *schemas;
}

optional_ptr<SchemaCatalogEntry> DuckCatalog::LookupSchema(CatalogTransaction transaction,
                                                           const EntryLookupInfo &schema_lookup,
                                                           OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	D_ASSERT(!schema_name.empty());
	auto entry = schemas->GetEntry(transaction, Identifier(schema_name));
	if (!entry) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException(schema_lookup.GetErrorContext(), "Schema with name %s does not exist!", schema_name);
		}
		return nullptr;
	}
	return &entry->Cast<SchemaCatalogEntry>();
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
