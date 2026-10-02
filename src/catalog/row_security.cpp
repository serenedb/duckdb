#include "duckdb/catalog/row_security.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/policy_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/parser/parsed_data/create_policy_info.hpp"

namespace duckdb {

RowSecurity::RowSecurity(Catalog &catalog) {
	if (catalog.IsDuckCatalog()) {
		policies = make_shared_ptr<CatalogSet>(catalog);
	}
}

optional_ptr<RowSecurity> RowSecurity::Get(CatalogEntry &relation) {
	switch (relation.type) {
	case CatalogType::TABLE_ENTRY:
		return relation.Cast<TableCatalogEntry>().GetRowSecurity();
	case CatalogType::VIEW_ENTRY:
		return relation.Cast<ViewCatalogEntry>().GetRowSecurity();
	default:
		return nullptr;
	}
}

StandardEntry &RowSecurity::GetRelation(CatalogTransaction transaction, SchemaCatalogEntry &schema,
                                        idx_t relation_oid) {
	optional_ptr<StandardEntry> result;
	auto &relations = schema.Cast<DuckSchemaEntry>().GetCatalogSet(CatalogType::TABLE_ENTRY);
	relations.Scan(transaction, [&](CatalogEntry &entry) {
		if (entry.oid == relation_oid) {
			result = &entry.Cast<StandardEntry>();
		}
	});
	if (!result) {
		throw IOException("corrupt database file - policy for missing relation %llu in schema \"%s\"", relation_oid,
		                  schema.name.GetIdentifierName());
	}
	return *result;
}

void RowSecurity::Apply(RowSecurityAction action) {
	switch (action) {
	case RowSecurityAction::ENABLE:
		enabled = true;
		break;
	case RowSecurityAction::DISABLE:
		enabled = false;
		break;
	case RowSecurityAction::FORCE:
		forced = true;
		break;
	case RowSecurityAction::NO_FORCE:
		forced = false;
		break;
	}
}

optional_ptr<CatalogEntry> RowSecurity::CreatePolicy(CatalogTransaction transaction, StandardEntry &relation,
                                                     CreatePolicyInfo &info) {
	if (!policies) {
		throw NotImplementedException("Policies are not supported for this catalog");
	}
	info.relation_oid = relation.oid;
	info.temporary = relation.temporary;
	auto policy = make_uniq<PolicyCatalogEntry>(relation.ParentCatalog(), relation.ParentSchema(transaction), info);
	auto entry_name = policy->name;
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && policies->GetEntry(transaction, entry_name)) {
		return nullptr;
	}
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT && policies->GetEntry(transaction, entry_name)) {
		policies->DropEntry(transaction, entry_name, false);
	}
	LogicalDependencyList dependencies;
	if (!policies->CreateEntry(transaction, entry_name, std::move(policy), dependencies)) {
		throw CatalogException("policy \"%s\" for %s \"%s\" already exists", entry_name.GetIdentifierName(),
		                       relation.type == CatalogType::VIEW_ENTRY ? "view" : "table",
		                       relation.name.GetIdentifierName());
	}
	return policies->GetEntry(transaction, entry_name);
}

bool RowSecurity::DropPolicy(CatalogTransaction transaction, const Identifier &name) {
	return policies && policies->DropEntry(transaction, name, false);
}

bool RowSecurity::AlterPolicy(CatalogTransaction transaction, const Identifier &name, AlterInfo &info) {
	return policies && policies->AlterEntry(transaction, name, info);
}

optional_ptr<PolicyCatalogEntry> RowSecurity::GetPolicy(CatalogTransaction transaction, const Identifier &name) const {
	if (!policies) {
		return nullptr;
	}
	auto entry = policies->GetEntry(transaction, name);
	return entry ? &entry->Cast<PolicyCatalogEntry>() : nullptr;
}

void RowSecurity::ScanPolicies(CatalogTransaction transaction,
                               const std::function<void(PolicyCatalogEntry &)> &callback) const {
	if (policies) {
		policies->Scan(transaction, [&](CatalogEntry &entry) { callback(entry.Cast<PolicyCatalogEntry>()); });
	}
}

void RowSecurity::ScanPolicies(const std::function<void(PolicyCatalogEntry &)> &callback) const {
	if (policies) {
		policies->Scan([&](CatalogEntry &entry) { callback(entry.Cast<PolicyCatalogEntry>()); });
	}
}

} // namespace duckdb
