//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/type_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/parser/parsed_data/domain_info.hpp"

namespace duckdb {

//! A type catalog entry
class TypeCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::TYPE_ENTRY;
	static constexpr const char *Name = "type";

public:
	//! Create a TypeCatalogEntry and initialize storage for it
	TypeCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTypeInfo &info);

	LogicalType user_type;

	//! The constructors that bind type modifiers to this type. Always holds at least one overload - a type that takes
	//! no modifiers has a single constructor that resolves to user_type.
	TypeConstructorSet constructors;

	//! CREATE DOMAIN: default, collation and constraints; null for a type that is not a domain
	unique_ptr<DomainInfo> domain_info;

public:
	using CatalogEntry::AlterEntry;

	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;

	unique_ptr<CreateInfo> GetInfo() const override;
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;

	string ToSQL() const override;

	static optional_ptr<TypeCatalogEntry> TryGetDomain(ClientContext &context, const LogicalType &type);
};
} // namespace duckdb
