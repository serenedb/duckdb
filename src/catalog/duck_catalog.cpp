#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/function/function_list.hpp"
#include "duckdb/common/encryption_state.hpp"

namespace duckdb {

DuckCatalog::DuckCatalog(AttachedDatabase &db, bool case_sensitive_schemas)
    : Catalog(db), dependency_manager(make_uniq<DependencyManager>(*this)),
      schemas(make_uniq<CatalogSet>(*this, IsSystemCatalog() ? make_uniq<DefaultSchemaGenerator>(*this) : nullptr,
                                    case_sensitive_schemas)) {
	schemas->EnableOidLookup(nullptr, CatalogType::SCHEMA_ENTRY);
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

void DuckCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	schemas->Scan(GetCatalogTransaction(context),
	              [&](CatalogEntry &entry) { callback(entry.Cast<SchemaCatalogEntry>()); });
}

void DuckCatalog::ScanSchemas(std::function<void(SchemaCatalogEntry &)> callback) {
	schemas->Scan([&](CatalogEntry &entry) { callback(entry.Cast<SchemaCatalogEntry>()); });
}

//! Exact rather than the Identifier's case-insensitive equality: a case-sensitive set keys "a" and "A" as two
//! chains, and each version node unregisters under the same exact name it registered with
static bool SameOidLocation(const EntryOidLocation &loc, idx_t schema_oid, CatalogType slot, const Identifier &name) {
	return loc.schema_oid == schema_oid && loc.slot == slot && loc.name.GetIdentifierName() == name.GetIdentifierName();
}

void DuckCatalog::AddOidLocation(idx_t oid, idx_t schema_oid, CatalogType slot, const Identifier &name) {
	lock_guard<mutex> guard(oid_locations_lock);
	auto &locations = oid_locations[oid];
	for (auto &loc : locations) {
		if (SameOidLocation(loc, schema_oid, slot, name)) {
			loc.count++;
			return;
		}
	}
	locations.push_back(EntryOidLocation {schema_oid, slot, name, 1});
}

void DuckCatalog::RemoveOidLocation(idx_t oid, idx_t schema_oid, CatalogType slot, const Identifier &name) {
	lock_guard<mutex> guard(oid_locations_lock);
	auto map_entry = oid_locations.find(oid);
	if (map_entry == oid_locations.end()) {
		return;
	}
	auto &locations = map_entry->second;
	for (idx_t i = 0; i < locations.size(); i++) {
		auto &loc = locations[i];
		if (!SameOidLocation(loc, schema_oid, slot, name)) {
			continue;
		}
		if (--loc.count == 0) {
			locations.erase_at(i);
		}
		break;
	}
	if (locations.empty()) {
		oid_locations.erase(map_entry);
	}
}

optional_ptr<CatalogSet> DuckCatalog::RootEntrySet(CatalogType slot) {
	if (slot == CatalogType::SCHEMA_ENTRY) {
		return schemas.get();
	}
	return nullptr;
}

optional_ptr<CatalogEntry> DuckCatalog::GetEntryById(CatalogTransaction transaction, idx_t oid) {
	vector<EntryOidLocation> locations;
	{
		lock_guard<mutex> guard(oid_locations_lock);
		auto map_entry = oid_locations.find(oid);
		if (map_entry == oid_locations.end()) {
			return nullptr;
		}
		locations = map_entry->second;
	}
	for (auto &loc : locations) {
		optional_ptr<CatalogSet> set;
		if (loc.schema_oid == DConstants::INVALID_INDEX) {
			set = RootEntrySet(loc.slot);
		} else {
			// The schema by its own id: renaming a schema moves nothing under it
			auto schema = GetEntryById(transaction, loc.schema_oid);
			if (!schema || schema->type != CatalogType::SCHEMA_ENTRY) {
				continue;
			}
			set = &schema->Cast<DuckSchemaEntry>().GetCatalogSet(loc.slot);
		}
		if (!set) {
			continue;
		}
		auto entry = set->GetEntry(transaction, loc.name);
		// A location outlives a drop-and-recreate under the same name; the visible version's own oid decides
		if (entry && entry->oid == oid) {
			return entry;
		}
	}
	return nullptr;
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
