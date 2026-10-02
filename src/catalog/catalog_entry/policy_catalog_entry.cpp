#include "duckdb/catalog/catalog_entry/policy_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/alter_policy_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

constexpr const char *PolicyCatalogEntry::Name;

PolicyCatalogEntry::PolicyCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreatePolicyInfo &info)
    : StandardEntry(CatalogType::POLICY_ENTRY, schema, catalog, info.GetPolicyName(), info.oid),
      relation_oid(info.relation_oid), permissive(info.permissive), command(info.command),
      roles(ResolveRoles(info.roles, info.role_names)), using_expr(info.using_expr ? info.using_expr->Copy() : nullptr),
      check_expr(info.check_expr ? info.check_expr->Copy() : nullptr) {
	this->temporary = info.temporary;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
}

vector<idx_t> PolicyCatalogEntry::ResolveRoles(const vector<idx_t> &roles, const vector<Identifier> &role_names) {
	if (!roles.empty()) {
		return roles;
	}
	for (auto &role_name : role_names) {
		if (!StringUtil::CIEquals(role_name.GetIdentifierName(), "PUBLIC")) {
			throw CatalogException("role \"%s\" does not exist", role_name.GetIdentifierName());
		}
	}
	return {ACL_ID_PUBLIC};
}

bool PolicyCatalogEntry::ReferencesColumn(const Identifier &column, const IdentifierEquality &same) const {
	bool found = false;
	for (auto *expr : {using_expr.get(), check_expr.get()}) {
		if (!expr) {
			continue;
		}
		ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
		    *expr, [&](const ColumnRefExpression &colref) { found = found || same(colref.GetColumnName(), column); });
	}
	return found;
}

static bool RenameColumnReferences(ParsedExpression &expr, const Identifier &old_name, const Identifier &new_name) {
	bool renamed = false;
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(expr, [&](ColumnRefExpression &colref) {
		auto &names = colref.ColumnNamesMutable();
		if (names.back() == old_name) {
			names.back() = new_name;
			renamed = true;
		}
	});
	return renamed;
}

unique_ptr<CatalogEntry> PolicyCatalogEntry::AlterEntry(CatalogTransaction transaction, AlterInfo &info) {
	auto create_info = GetInfo();
	auto &policy_info = create_info->Cast<CreatePolicyInfo>();
	if (info.type == AlterType::ALTER_TABLE &&
	    info.Cast<AlterTableInfo>().alter_table_type == AlterTableType::RENAME_COLUMN) {
		auto &rename_info = info.Cast<RenameColumnInfo>();
		bool renamed = false;
		for (auto *expr : {policy_info.using_expr.get(), policy_info.check_expr.get()}) {
			if (expr && RenameColumnReferences(*expr, rename_info.old_name, rename_info.new_name)) {
				renamed = true;
			}
		}
		if (!renamed) {
			return nullptr;
		}
		return make_uniq<PolicyCatalogEntry>(catalog, ParentSchema(transaction), policy_info);
	}
	if (info.type != AlterType::ALTER_POLICY) {
		return CatalogEntry::AlterEntry(transaction, info);
	}
	auto &alter_info = info.Cast<AlterPolicyInfo>();
	switch (alter_info.alter_policy_type) {
	case AlterPolicyType::RENAME:
		policy_info.SetPolicyName(alter_info.new_name);
		break;
	case AlterPolicyType::SET_CLAUSES:
		if (!alter_info.role_names.empty() || !alter_info.roles.empty()) {
			policy_info.roles = ResolveRoles(alter_info.roles, alter_info.role_names);
		}
		if (alter_info.using_expr) {
			policy_info.using_expr = alter_info.using_expr->Copy();
		}
		if (alter_info.check_expr) {
			policy_info.check_expr = alter_info.check_expr->Copy();
		}
		break;
	}
	return make_uniq<PolicyCatalogEntry>(catalog, ParentSchema(transaction), policy_info);
}

unique_ptr<CatalogEntry> PolicyCatalogEntry::Copy(ClientContext &context) const {
	auto info = GetInfo();
	return make_uniq<PolicyCatalogEntry>(catalog, ParentSchema(context), info->Cast<CreatePolicyInfo>());
}

unique_ptr<CreateInfo> PolicyCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreatePolicyInfo>();
	result->SetQualifiedName(QualifiedName(catalog.GetName(), ParentSchemaName(), name));
	result->relation_oid = relation_oid;
	result->permissive = permissive;
	result->command = command;
	result->roles = roles;
	result->using_expr = using_expr ? using_expr->Copy() : nullptr;
	result->check_expr = check_expr ? check_expr->Copy() : nullptr;
	result->temporary = temporary;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->permissions = permissions;
	return std::move(result);
}

} // namespace duckdb
