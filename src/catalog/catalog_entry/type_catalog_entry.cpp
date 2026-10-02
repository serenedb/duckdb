#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/extra_type_info.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include <algorithm>
#include <sstream>

namespace duckdb {

constexpr const char *TypeCatalogEntry::Name;

TypeCatalogEntry::TypeCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTypeInfo &info)
    : StandardEntry(CatalogType::TYPE_ENTRY, schema, catalog, info.GetTypeName(), info.oid), user_type(info.type),
      bind_function(info.bind_function) {
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->extension_name = info.extension_name;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
	const bool postgres = catalog.Compatibility() == SqlCompatibility::POSTGRES;
	if (postgres && !internal && user_type.id() == LogicalTypeId::ENUM && !user_type.HasAlias()) {
		user_type.SetAlias(name.GetIdentifierName());
	}
	if (postgres && !internal) {
		auto type_info = user_type.AuxInfo() ? user_type.AuxInfo()->DeepCopy()
		                                     : make_shared_ptr<ExtraTypeInfo>(ExtraTypeInfoType::GENERIC_TYPE_INFO);
		auto extension_info = type_info->extension_info ? make_uniq<ExtensionTypeInfo>(*type_info->extension_info)
		                                                : make_uniq<ExtensionTypeInfo>();
		extension_info->properties[ExtensionTypeInfo::CATALOG_OID_PROPERTY] = Value::UBIGINT(oid);
		type_info->extension_info = std::move(extension_info);
		user_type = LogicalType(user_type.id(), std::move(type_info));
	}
}

unique_ptr<CatalogEntry> TypeCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateTypeInfo>();
	cast_info.oid = oid;
	auto result = make_uniq<TypeCatalogEntry>(catalog, ParentSchema(context), cast_info);
	return std::move(result);
}

unique_ptr<CreateInfo> TypeCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateTypeInfo>();
	result->SetQualifiedName(QualifiedName(catalog.GetName(), ParentSchemaName(), name));
	result->type = user_type;
	result->extension_name = extension_name;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->permissions = permissions;
	result->bind_function = bind_function;
	return std::move(result);
}

string TypeCatalogEntry::ToSQL() const {
	duckdb::stringstream ss;
	ss << "CREATE TYPE ";
	ss << SQLIdentifier(name);
	ss << " AS ";

	auto user_type_copy = user_type;

	// Strip off the potential alias so ToString doesn't just output the alias
	user_type_copy.SetAlias("");
	D_ASSERT(user_type_copy.GetAlias().empty());

	ss << user_type_copy.ToString();
	ss << ";";
	return ss.str();
}

} // namespace duckdb
