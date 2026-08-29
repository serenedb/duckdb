#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/original/std/sstream.hpp"

namespace duckdb {

SchemaCatalogEntry::SchemaCatalogEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info, make_shared_ptr<SchemaIdentity>()) {
}

SchemaCatalogEntry::SchemaCatalogEntry(Catalog &catalog, CreateSchemaInfo &info, shared_ptr<SchemaIdentity> identity_p)
    : InCatalogEntry(CatalogType::SCHEMA_ENTRY, catalog, info.GetQualifiedName().Schema()),
      identity(std::move(identity_p)) {
	this->internal = info.internal;
	this->comment = info.comment;
	this->tags = info.tags;
	identity->Adopt(*this);
}

SchemaIdentity::~SchemaIdentity() {
}

StandardEntry::StandardEntry(CatalogType type, SchemaCatalogEntry &schema, Catalog &catalog, Identifier name)
    : InCatalogEntry(type, catalog, std::move(name)), schema_identity(schema.GetIdentity()) {
}

CatalogTransaction SchemaCatalogEntry::GetCatalogTransaction(ClientContext &context) {
	return CatalogTransaction(catalog, context);
}

optional_ptr<CatalogEntry> SchemaCatalogEntry::CreateIndex(ClientContext &context, CreateIndexInfo &info,
                                                           TableCatalogEntry &table) {
	return CreateIndex(GetCatalogTransaction(context), info, table);
}

SimilarCatalogEntry SchemaCatalogEntry::GetSimilarEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	SimilarCatalogEntry result;
	Scan(transaction.GetContext(), lookup_info.GetCatalogType(), [&](CatalogEntry &entry) {
		auto entry_score = StringUtil::SimilarityRating(entry.name.GetIdentifierName(), lookup_info.GetEntryName());
		if (entry_score > result.score) {
			result.score = entry_score;
			result.name = Identifier(entry.name.GetIdentifierName());
		}
	});
	return result;
}

optional_ptr<CatalogEntry> SchemaCatalogEntry::GetEntry(CatalogTransaction transaction, CatalogType type,
                                                        const Identifier &name) {
	EntryLookupInfo lookup_info(type, QualifiedName(name));
	return LookupEntry(transaction, lookup_info);
}

//! This should not be used, it's only implemented to not put the burden of implementing it on every derived class of
//! SchemaCatalogEntry
CatalogSet::EntryLookup SchemaCatalogEntry::LookupEntryDetailed(CatalogTransaction transaction,
                                                                const EntryLookupInfo &lookup_info) {
	CatalogSet::EntryLookup result;
	result.result = LookupEntry(transaction, lookup_info);
	if (!result.result) {
		result.reason = CatalogSet::EntryLookup::FailureReason::DELETED;
	} else {
		result.reason = CatalogSet::EntryLookup::FailureReason::SUCCESS;
	}
	return result;
}

unique_ptr<CreateInfo> SchemaCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateSchemaInfo>();
	result->SetQualifiedName(QualifiedName({name}, Identifier()));
	result->comment = comment;
	result->tags = tags;
	result->oid = oid;
	return std::move(result);
}

string SchemaCatalogEntry::ToSQL() const {
	auto create_schema_info = GetInfo();
	return create_schema_info->ToString();
}

} // namespace duckdb
