#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_data/alter_domain_info.hpp"
#include <algorithm>
#include <sstream>

namespace duckdb {

constexpr const char *TypeCatalogEntry::Name;

static idx_t TypeOid(Catalog &catalog, const CreateTypeInfo &info) {
	if (info.oid || info.internal || catalog.Compatibility() != SqlCompatibility::POSTGRES) {
		return info.oid;
	}
	return catalog.GetDatabase().GetDatabaseManager().NextOid(2) + 1;
}

TypeCatalogEntry::TypeCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTypeInfo &info)
    : StandardEntry(CatalogType::TYPE_ENTRY, schema, catalog, info.GetTypeName(), TypeOid(catalog, info)),
      // TODO: maybe info.type is enough here
      user_type(info.domain_info && info.domain_info->base_type.id() != LogicalTypeId::INVALID
                    ? info.domain_info->base_type
                    : info.type),
      constructors(info.constructors), domain_info(info.domain_info ? info.domain_info->Copy() : nullptr) {
	if (constructors.functions.empty()) {
		// a type without constructors takes no modifiers and always resolves to its own type
		constructors.AddFunction(TypeConstructor::Identity(name));
	}
	constructors.SetName(name);
	constructors.ApplyToFunctions([&](TypeConstructor &constructor) {
		constructor.SetName(name);
		constructor.SetCatalogName(catalog.GetAttached().GetName());
		constructor.SetSchemaName(schema.name);
	});
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->extension_name = info.extension_name;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
	const bool postgres = catalog.Compatibility() == SqlCompatibility::POSTGRES;
	if (postgres && !internal && user_type.id() == LogicalTypeId::ENUM && !user_type.HasAlias()) {
		user_type = user_type.WithAlias(name.GetIdentifierName());
	}
	if (postgres && !internal) {
		auto extension_info = user_type.HasExtensionInfo() ? make_uniq<ExtensionTypeInfo>(*user_type.GetExtensionInfo())
		                                                   : make_uniq<ExtensionTypeInfo>();
		auto &properties = extension_info->properties;
		if (domain_info) {
			auto base_oid = properties.find(ExtensionTypeInfo::CATALOG_OID_PROPERTY);
			if (!properties.contains(ExtensionTypeInfo::DOMAIN_PROPERTY) && base_oid != properties.end()) {
				properties[ExtensionTypeInfo::BASE_OID_PROPERTY] = base_oid->second;
			}
			properties[ExtensionTypeInfo::DOMAIN_PROPERTY] = Value::BOOLEAN(true);
		} else {
			// TODO: think about aliasing CREATE TYPE x AS d
			properties.erase(ExtensionTypeInfo::DOMAIN_PROPERTY);
			properties.erase(ExtensionTypeInfo::BASE_OID_PROPERTY);
		}
		properties[ExtensionTypeInfo::CATALOG_OID_PROPERTY] = Value::UBIGINT(oid);
		user_type = user_type.WithExtensionInfo(std::move(extension_info));
	}
}

unique_ptr<CatalogEntry> TypeCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateTypeInfo>();
	cast_info.oid = oid;
	auto result = make_uniq<TypeCatalogEntry>(catalog, ParentSchema(context), cast_info);
	return std::move(result);
}

static auto HasConstraintName(const string &constraint_name) {
	return [&constraint_name](const DomainConstraint &constraint) {
		return constraint.name == constraint_name;
	};
}

static string ChooseConstraintName(const string &domain_name, const vector<DomainConstraint> &constraints,
                                   ConstraintType type) {
	auto base = domain_name + "_" + (type == ConstraintType::CHECK ? "check" : "not_null");
	auto candidate = base;
	for (idx_t i = 1; std::any_of(constraints.begin(), constraints.end(), HasConstraintName(candidate)); i++) {
		candidate = base + to_string(i);
	}
	return candidate;
}

unique_ptr<CatalogEntry> TypeCatalogEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	if (info.type != AlterType::ALTER_DOMAIN) {
		return CatalogEntry::AlterEntry(context, info);
	}
	D_ASSERT(domain_info);

	auto &alter = info.Cast<AlterDomainInfo>();
	if (alter.alter_domain_type == AlterDomainType::RENAME) {
		return CatalogEntry::AlterEntry(context, info);
	}
	auto result = Copy(context);
	auto &result_domain_info = result->Cast<TypeCatalogEntry>().domain_info;
	switch (alter.alter_domain_type) {
	case AlterDomainType::SET_DEFAULT:
		result_domain_info->default_value = alter.default_value->Copy();
		break;
	case AlterDomainType::DROP_DEFAULT:
		result_domain_info->default_value.reset();
		break;
	case AlterDomainType::SET_NOT_NULL: {
		auto &constraints = result_domain_info->constraints;
		if (std::any_of(constraints.begin(), constraints.end(), [](const DomainConstraint &constraint) {
			    return constraint.type == ConstraintType::NOT_NULL;
		    })) {
			break;
		}
		alter.constraint.type = ConstraintType::NOT_NULL;
		if (alter.constraint.name.empty()) {
			alter.constraint.name =
			    ChooseConstraintName(name.GetIdentifierName(), constraints, ConstraintType::NOT_NULL);
		}
		constraints.push_back(alter.constraint.Copy());
		break;
	}
	case AlterDomainType::DROP_NOT_NULL:
		std::erase_if(result_domain_info->constraints,
		              [](const DomainConstraint &constraint) { return constraint.type == ConstraintType::NOT_NULL; });
		break;
	case AlterDomainType::ADD_CONSTRAINT: {
		auto &constraints = result_domain_info->constraints;
		if (alter.constraint.name.empty()) {
			alter.constraint.name = ChooseConstraintName(name.GetIdentifierName(), constraints, alter.constraint.type);
		} else if (std::any_of(constraints.begin(), constraints.end(), HasConstraintName(alter.constraint.name))) {
			throw CatalogException("constraint \"%s\" for domain \"%s\" already exists", alter.constraint.name,
			                       name.GetIdentifierName());
		}
		constraints.push_back(alter.constraint.Copy());
		break;
	}
	case AlterDomainType::DROP_CONSTRAINT: {
		auto &constraints = result_domain_info->constraints;
		auto removed = std::erase_if(constraints, HasConstraintName(alter.constraint_name));
		if (removed == 0 && !alter.constraint_if_exists) {
			throw CatalogException("constraint \"%s\" of domain \"%s\" does not exist", alter.constraint_name,
			                       name.GetIdentifierName());
		}
		break;
	}
	case AlterDomainType::RENAME_CONSTRAINT: {
		auto &constraints = result_domain_info->constraints;
		auto it = std::find_if(constraints.begin(), constraints.end(), HasConstraintName(alter.constraint_name));
		if (it == constraints.end()) {
			throw CatalogException("constraint \"%s\" of domain \"%s\" does not exist", alter.constraint_name,
			                       name.GetIdentifierName());
		}
		if (std::any_of(constraints.begin(), constraints.end(), HasConstraintName(alter.new_constraint_name))) {
			throw CatalogException("constraint \"%s\" for domain \"%s\" already exists", alter.new_constraint_name,
			                       name.GetIdentifierName());
		}
		it->name = alter.new_constraint_name;
		break;
	}
	case AlterDomainType::VALIDATE_CONSTRAINT: {
		auto &constraints = result_domain_info->constraints;
		auto it = std::find_if(constraints.begin(), constraints.end(), HasConstraintName(alter.constraint_name));
		if (it == constraints.end()) {
			throw CatalogException("constraint \"%s\" of domain \"%s\" does not exist", alter.constraint_name,
			                       name.GetIdentifierName());
		}
		it->validated = true;
		break;
	}
	case AlterDomainType::RENAME:
		D_ASSERT(false); // already processed before switch
	}
	return result;
}

unique_ptr<CreateInfo> TypeCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateTypeInfo>();
	result->SetQualifiedName(GetQualifiedName(name));
	result->type = user_type;
	result->extension_name = extension_name;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->permissions = permissions;
	result->constructors = constructors;
	result->domain_info = domain_info ? domain_info->Copy() : nullptr;
	return std::move(result);
}

string TypeCatalogEntry::ToSQL() const {
	duckdb::stringstream ss;
	ss << "CREATE TYPE ";
	ss << SQLIdentifier(name);
	ss << " AS ";

	// Strip off the potential alias so ToString doesn't just output the alias
	auto user_type_copy = user_type.WithAlias("");
	D_ASSERT(user_type_copy.GetAlias().empty());

	ss << user_type_copy.ToString();
	ss << ";";
	return ss.str();
}

static optional_ptr<TypeCatalogEntry> FindDomain(ClientContext &context, Catalog &catalog, idx_t oid) {
	auto entry = catalog.LookupOid(catalog.GetCatalogTransaction(context), oid);
	if (!entry || entry->type != CatalogType::TYPE_ENTRY) {
		return nullptr;
	}
	auto &type_entry = entry->Cast<TypeCatalogEntry>();
	if (!type_entry.domain_info) {
		return nullptr;
	}
	return type_entry;
}

optional_ptr<TypeCatalogEntry> TypeCatalogEntry::TryGetDomain(ClientContext &context, const LogicalType &type) {
	auto extension = type.GetExtensionInfo();
	if (!ExtensionTypeInfo::IsDomain(extension)) {
		return nullptr;
	}
	auto oid_it = extension->properties.find(ExtensionTypeInfo::CATALOG_OID_PROPERTY);
	if (oid_it == extension->properties.end()) {
		return nullptr;
	}
	auto oid = oid_it->second.GetValue<idx_t>();

	auto current = DatabaseManager::Get(context).GetDatabase(context, DatabaseManager::TryGetDefaultDatabase(context));
	if (!current) {
		return nullptr;
	}
	return FindDomain(context, current->GetCatalog(), oid);
}

} // namespace duckdb
