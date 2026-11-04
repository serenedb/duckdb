#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/alter_job_info.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/parsed_data/create_foreign_server_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_subscription_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/operator/logical_create.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_alter.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> DuckCatalog::BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
                                                           unique_ptr<LogicalOperator> plan,
                                                           unique_ptr<CreateIndexInfo> create_info,
                                                           unique_ptr<AlterTableInfo> alter_info) {
	D_ASSERT(plan->type == LogicalOperatorType::LOGICAL_GET);
	IndexBinder index_binder(binder, binder.context);
	return index_binder.BindCreateIndex(binder.context, std::move(create_info), table_entry, std::move(plan),
	                                    std::move(alter_info));
}

BoundStatement Binder::BindAlterAddIndex(BoundStatement &result, CatalogEntry &entry,
                                         unique_ptr<AlterInfo> alter_info) {
	if (entry.type != CatalogType::TABLE_ENTRY) {
		throw BinderException("Cannot execute the `ALTER TABLE` statement on `%s`, only `Table` entries are accepted.",
		                      CatalogTypeToString(entry.type));
	}
	auto &table_info = alter_info->Cast<AlterTableInfo>();
	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	auto &table = entry.Cast<TableCatalogEntry>();
	auto &column_list = table.GetColumns();

	auto bound_constraint =
	    BindUniqueConstraint(*constraint_info.constraint, table_info.GetQualifiedName().Name(), column_list);
	VerifyConstraintCheckModeStorageVersion(*constraint_info.constraint, table.ParentCatalog(), table.temporary);
	auto &bound_unique = bound_constraint->Cast<BoundUniqueConstraint>();
	const auto existing_pk = table.GetPrimaryKey();
	if (bound_unique.is_primary_key && existing_pk) {
		throw CatalogException("table %s can have only one primary key: %s", table.name, existing_pk->ToString());
	}
	auto &unique_constraint = constraint_info.constraint->Cast<UniqueConstraint>();

	// Create the CreateIndexInfo.
	auto create_index_info = make_uniq<CreateIndexInfo>();
	create_index_info->table = table_info.GetQualifiedName().Name();
	create_index_info->index_type = ART::TYPE_NAME;
	create_index_info->constraint_type = unique_constraint.GetIndexConstraintType();

	for (const auto &physical_index : bound_unique.keys) {
		auto &col = column_list.GetColumn(physical_index);
		unique_ptr<ParsedExpression> parsed =
		    make_uniq<ColumnRefExpression>(col.GetName(), table_info.GetQualifiedName().Name());
		create_index_info->expressions.push_back(parsed->Copy());
		create_index_info->parsed_expressions.push_back(parsed->Copy());
	}

	auto index_name = unique_constraint.GetName(table_info.GetQualifiedName().Name());
	create_index_info->SetIndexName(index_name);
	D_ASSERT(!create_index_info->GetIndexName().empty());

	// Plan the table scan.
	TableDescription table_description(table_info.GetQualifiedName());
	auto table_ref = make_uniq<BaseTableRef>(table_description);
	auto bound_table = Bind(*table_ref);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("can only add an index to a base table");
	}
	auto &get = bound_table.plan->Cast<LogicalGet>();
	get.names = StringsToIdentifiers(column_list.GetColumnNames());

	auto alter_table_info = unique_ptr_cast<AlterInfo, AlterTableInfo>(std::move(alter_info));
	result.plan = table.catalog.BindAlterAddIndex(*this, table, std::move(bound_table.plan),
	                                              std::move(create_index_info), std::move(alter_table_info));
	return std::move(result);
}

static void BindAlterTypes(Binder &binder, AlterStatement &stmt) {
	if (stmt.info->type == AlterType::ALTER_TABLE) {
		auto &table_info = stmt.info->Cast<AlterTableInfo>();
		switch (table_info.alter_table_type) {
		case AlterTableType::ADD_COLUMN: {
			auto &add_info = table_info.Cast<AddColumnInfo>();
			binder.BindLogicalType(add_info.new_column.TypeMutable());
		} break;
		case AlterTableType::ADD_FIELD: {
			auto &add_info = table_info.Cast<AddFieldInfo>();
			binder.BindLogicalType(add_info.new_field.TypeMutable());
		} break;
		case AlterTableType::ALTER_COLUMN_TYPE: {
			auto &alter_column_info = table_info.Cast<ChangeColumnTypeInfo>();
			binder.BindLogicalType(alter_column_info.target_type);
			if (!alter_column_info.expression) {
				// without USING, the column is cast to the target type
				alter_column_info.expression = make_uniq<CastExpression>(
				    alter_column_info.target_type, make_uniq<ColumnRefExpression>(alter_column_info.column_path));
			}
		} break;
		default:
			break;
		}
	}
	if (stmt.info->type == AlterType::ALTER_JOB) {
		auto &job_info = stmt.info->Cast<AlterJobInfo>();
		if (job_info.alter_job_type == AlterJobType::SET_SCHEDULE) {
			binder.BindJobSchedule(job_info.schedule, job_info.parsed_schedule);
		}
	}
}

[[noreturn]] static void ThrowWrongKindForSetSchema(const string &message, const string &hint = string()) {
	auto extra_info = Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx());
	if (!hint.empty()) {
		extra_info["hint"] = hint;
	}
	throw CatalogException(extra_info, message);
}

template <class LOOKUP>
static optional_ptr<CatalogEntry> BindSetSchemaEntry(SetSchemaInfo &info, const LOOKUP &lookup) {
	const auto declared = info.entry_catalog_type;
	vector<CatalogType> kinds {declared};
	if (declared == CatalogType::TABLE_ENTRY) {
		kinds.push_back(CatalogType::SEQUENCE_ENTRY);
	} else if (declared == CatalogType::MACRO_ENTRY) {
		kinds.push_back(CatalogType::TABLE_MACRO_ENTRY);
	}
	for (auto kind : kinds) {
		auto entry = lookup(kind, OnEntryNotFound::RETURN_NULL);
		if (!entry) {
			continue;
		}
		if (declared == CatalogType::VIEW_ENTRY && entry->type != CatalogType::VIEW_ENTRY) {
			ThrowWrongKindForSetSchema(StringUtil::Format("\"%s\" is not a view", entry->name.GetIdentifierName()));
		}
		info.entry_catalog_type = entry->type;
		return entry;
	}
	if (declared == CatalogType::TABLE_ENTRY) {
		if (auto index = lookup(CatalogType::INDEX_ENTRY, OnEntryNotFound::RETURN_NULL)) {
			ThrowWrongKindForSetSchema(
			    StringUtil::Format("cannot change schema of index \"%s\"", index->name.GetIdentifierName()),
			    "Change the schema of the table instead.");
		}
	}
	return lookup(declared, info.if_not_found);
}

static idx_t GranteeId(Binder &binder, const string &name) {
	return name == "public" ? ACL_ID_PUBLIC : binder.RoleSpecId(name);
}

static void RequireGrantable(Binder &binder, CatalogEntry &entry) {
	auto &role = binder.CheckAs();
	auto &permissions = EffectivePermissions(binder.context, entry);
	if (role.Owns(permissions.owner)) {
		return;
	}
	vector<AclItem> defaults;
	std::span<const AclItem> stored = permissions.acl;
	if (stored.empty()) {
		defaults = Permissions::AclDefault(Permissions::AclClass(entry.type), permissions.owner);
		stored = defaults;
	}
	auto held = role.HeldModes(stored) | role.GrantableModes(stored);
	if (entry.type == CatalogType::TABLE_ENTRY) {
		for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
			held |= role.HeldModes(column.Acl()) | role.GrantableModes(column.Acl());
		}
	} else if (entry.type == CatalogType::VIEW_ENTRY) {
		for (auto &acl : entry.Cast<ViewCatalogEntry>().column_acls) {
			held |= role.HeldModes(acl) | role.GrantableModes(acl);
		}
	}
	if (held == AclMode::NoRights) {
		ThrowPermissionDenied(entry);
	}
}

static CatalogEntry &ResolveDatabaseEntry(Binder &binder, CatalogType type, AlterInfo &info) {
	auto &context = binder.context;
	auto &name = info.GetQualifiedName().Name();
	auto &catalog =
	    type == CatalogType::SUBSCRIPTION_ENTRY
	        ? binder.SubscriptionCatalog(Identifier()).Cast<DuckCatalog>()
	        : Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context)).Cast<DuckCatalog>();
	auto entry = catalog.GetCatalogSet(type).GetEntry(catalog.GetCatalogTransaction(context), name);
	if (!entry) {
		const bool server = type == CatalogType::FOREIGN_SERVER_ENTRY;
		throw CatalogException(unordered_map<string, string> {{"type", server ? "Foreign Server" : "Subscription"}},
		                       StringUtil::Format("%s \"%s\" does not exist", server ? "server" : "subscription",
		                                          name.GetIdentifierName()));
	}
	info.SetQualifiedName(entry->ParentCatalog().GetName(), Identifier(), entry->name);
	return *entry;
}

static void RequireSubscriptionOwner(Binder &binder, CatalogEntry &subscription) {
	binder.RequireOwner(subscription);
	if (!binder.CheckAs().is_superuser && !subscription.GetInfo()->Cast<CreateSubscriptionInfo>().password_required) {
		throw PermissionException(unordered_map<string, string> {{"hint", "Subscriptions with the password_required "
		                                                                  "option set to false may only be created or "
		                                                                  "modified by the superuser."}},
		                          "password_required=false is superuser-only");
	}
}

static CatalogEntry &ResolvePermissionsTarget(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &name = info.GetQualifiedName();
	if (info.entry_catalog_type == CatalogType::SCHEMA_ENTRY) {
		auto &schema = Catalog::GetSchema(context, name.Catalog(), name.Name());
		info.SetQualifiedName(schema.ParentCatalog().GetName(), Identifier(), schema.name);
		return schema;
	}
	if (info.entry_catalog_type == CatalogType::FOREIGN_SERVER_ENTRY ||
	    info.entry_catalog_type == CatalogType::SUBSCRIPTION_ENTRY) {
		return ResolveDatabaseEntry(binder, info.entry_catalog_type, info);
	}
	if (info.entry_catalog_type == CatalogType::DATABASE_ENTRY) {
		auto database = binder.ClusterEntry(CatalogType::DATABASE_ENTRY, name.Name());
		if (!database) {
			throw CatalogException(
			    unordered_map<string, string> {{"type", "Database"}},
			    StringUtil::Format("database \"%s\" does not exist", name.Name().GetIdentifierName()));
		}
		return *database;
	}
	auto entry = binder.GetCatalogEntry(EntryLookupInfo(info.entry_catalog_type, name), OnEntryNotFound::RETURN_NULL);
	if (!entry && info.entry_catalog_type == CatalogType::MACRO_ENTRY) {
		entry =
		    binder.GetCatalogEntry(EntryLookupInfo(CatalogType::TABLE_MACRO_ENTRY, name), OnEntryNotFound::RETURN_NULL);
	}
	if (!entry) {
		const bool relation =
		    info.entry_catalog_type == CatalogType::TABLE_ENTRY || info.entry_catalog_type == CatalogType::VIEW_ENTRY;
		throw CatalogException(unordered_map<string, string> {{"type", CatalogTypeToString(info.entry_catalog_type)}},
		                       StringUtil::Format("%s \"%s\" does not exist",
		                                          relation ? "relation" : PermissionKindName(info.entry_catalog_type),
		                                          name.ToString()));
	}
	if (info.entry_catalog_type == CatalogType::VIEW_ENTRY && entry->type != CatalogType::VIEW_ENTRY) {
		throw CatalogException(Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx()),
		                       StringUtil::Format("\"%s\" is not a view", name.Name().GetIdentifierName()));
	}
	info.entry_catalog_type = entry->type == CatalogType::VIEW_ENTRY ? CatalogType::TABLE_ENTRY : entry->type;
	info.SetQualifiedName(QualifiedName::FromCatalogSchema(
	    entry->ParentCatalog().GetName(),
	    entry->ParentSchemaPath(entry->ParentCatalog().GetCatalogTransaction(context)), entry->name));
	return *entry;
}

static void RefuseOwnedSequence(ClientContext &context, CatalogEntry &sequence) {
	auto &catalog = sequence.ParentCatalog();
	auto dependency_manager = catalog.GetDependencyManager();
	if (!dependency_manager) {
		return;
	}
	auto table = dependency_manager->OwnerOf(catalog.GetCatalogTransaction(context), sequence);
	if (!table) {
		return;
	}
	auto &name = sequence.name.GetIdentifierName();
	throw NotImplementedException(
	    unordered_map<string, string> {{"detail", StringUtil::Format("Sequence \"%s\" is linked to table \"%s\".", name,
	                                                                 table->name.GetIdentifierName())}},
	    StringUtil::Format("cannot change owner of sequence \"%s\"", name));
}

static vector<idx_t> CallerGrantors(Binder &binder) {
	vector<idx_t> grantors;
	auto &role = binder.CheckAs();
	if (!role.is_superuser) {
		grantors.push_back(role.role);
		grantors.insert(grantors.end(), role.closure.begin(), role.closure.end());
	}
	return grantors;
}

static void ResolveGrant(Binder &binder, AlterPermissionsInfo &info) {
	auto &role = binder.CheckAs();
	info.grantee_id = GranteeId(binder, info.grantee);
	if (!info.granted_by.empty()) {
		auto granted_by = binder.RoleSpecId(info.granted_by);
		if (!role.MemberOf(granted_by)) {
			throw PermissionException("must be member of role \"%s\"", binder.RoleName(granted_by));
		}
		info.grantors = {granted_by};
	} else {
		info.grantors = CallerGrantors(binder);
	}
	if (info.entry_catalog_type == CatalogType::FOREIGN_DATA_WRAPPER_ENTRY) {
		auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		auto &wrapper = DBConfig::GetConfig(binder.context).GetForeignDataWrapper(name);
		auto database_name = DatabaseManager::GetDefaultDatabase(binder.context);
		auto database = binder.ClusterEntry(CatalogType::DATABASE_ENTRY, database_name);
		if (!database) {
			throw CatalogException(
			    unordered_map<string, string> {{"type", "Database"}},
			    StringUtil::Format("database \"%s\" does not exist", database_name.GetIdentifierName()));
		}
		auto acl = database->permissions.ForeignDataWrapperAcl(wrapper.oid);
		if (!role.Owns(wrapper.owner) && (role.HeldModes(acl) | role.GrantableModes(acl)) == AclMode::NoRights) {
			throw PermissionException("permission denied for foreign-data wrapper %s", name);
		}
		info.foreign_data_wrapper_id = wrapper.oid;
		info.object_owner_id = wrapper.owner;
		info.entry_catalog_type = CatalogType::DATABASE_ENTRY;
		info.SetQualifiedName(Identifier(), Identifier(), database_name);
		return;
	}
	if (info.entry_catalog_type == CatalogType::LANGUAGE_ENTRY) {
		auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		auto &language = DBConfig::GetConfig(binder.context).GetLanguage(name);
		if (!language.trusted) {
			auto extra_info = Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx());
			extra_info["detail"] = "GRANT and REVOKE are not allowed on untrusted languages, because only superusers "
			                       "can use untrusted languages.";
			throw CatalogException(extra_info, StringUtil::Format("language \"%s\" is not trusted", name));
		}
		auto database_name = DatabaseManager::GetDefaultDatabase(binder.context);
		auto database = binder.ClusterEntry(CatalogType::DATABASE_ENTRY, database_name);
		if (!database) {
			throw CatalogException(
			    unordered_map<string, string> {{"type", "Database"}},
			    StringUtil::Format("database \"%s\" does not exist", database_name.GetIdentifierName()));
		}
		auto stored = database->permissions.LanguageAcl(language.oid);
		auto acl = stored.empty() ? Permissions::AclDefault(CatalogType::LANGUAGE_ENTRY, language.owner)
		                          : vector<AclItem>(stored.begin(), stored.end());
		if (!role.Owns(language.owner) && (role.HeldModes(acl) | role.GrantableModes(acl)) == AclMode::NoRights) {
			throw PermissionException("permission denied for language %s", name);
		}
		info.language_id = language.oid;
		info.object_owner_id = language.owner;
		info.entry_catalog_type = CatalogType::DATABASE_ENTRY;
		info.SetQualifiedName(Identifier(), Identifier(), database_name);
		return;
	}
	if (info.entry_catalog_type == CatalogType::PARAMETER_ENTRY) {
		auto &qualified = info.GetQualifiedName();
		auto name = qualified.Name().GetIdentifierName();
		if (!qualified.Schema().empty()) {
			name = qualified.Schema().GetIdentifierName() + "." + name;
		}
		if (!Settings::GetAccess(binder.context, Identifier(name))) {
			throw InvalidInputException("unrecognized configuration parameter \"%s\"", name);
		}
		const auto owner = DBConfig::GetConfig(binder.context).bootstrap_role;
		const Identifier holder(binder.RoleName(owner));
		auto entry = binder.ClusterEntry(CatalogType::ROLE_ENTRY, holder);
		if (!entry) {
			throw NotImplementedException("parameter privileges are not supported by this catalog");
		}
		auto acl = entry->permissions.ParameterAcl(name);
		if (!role.Owns(owner) && (role.HeldModes(acl) | role.GrantableModes(acl)) == AclMode::NoRights) {
			throw PermissionException("permission denied for parameter %s", name);
		}
		info.parameter = StringUtil::Lower(name);
		info.object_owner_id = owner;
		info.entry_catalog_type = CatalogType::ROLE_ENTRY;
		info.SetQualifiedName(Identifier(), Identifier(), holder);
		return;
	}
	if (info.all_in_schema) {
		return;
	}
	auto &target = ResolvePermissionsTarget(binder, info);
	RequireGrantable(binder, target);
	if (!target.internal || IsUnowned(target) ||
	    (target.type != CatalogType::TABLE_ENTRY && target.type != CatalogType::VIEW_ENTRY)) {
		return;
	}
	if (!info.column_privileges.empty()) {
		throw NotImplementedException("column privileges on system catalogs are not supported");
	}
	info.relation_id = target.oid;
	info.relation_acl = target.permissions.acl;
	info.object_owner_id = target.permissions.owner;
	info.entry_catalog_type = CatalogType::DATABASE_ENTRY;
	info.SetQualifiedName(Identifier(), Identifier(), target.ParentCatalog().GetName());
}

static void ResolveOwner(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &entry = ResolvePermissionsTarget(binder, info);
	info.new_owner_id = binder.RoleSpecId(info.new_owner);
	auto &role = binder.CheckAs();
	const bool enforce = !role.is_superuser;
	const bool relation = Catalog::InRelationNamespace(entry.type);
	if (enforce && relation) {
		binder.RequireOwner(entry);
	}
	if (info.new_owner_id == entry.permissions.owner) {
		return;
	}
	if (entry.type == CatalogType::SEQUENCE_ENTRY) {
		RefuseOwnedSequence(context, entry);
	}
	if (!enforce) {
		return;
	}
	if (entry.type == CatalogType::SUBSCRIPTION_ENTRY) {
		RequireSubscriptionOwner(binder, entry);
	} else if (!relation) {
		binder.RequireOwner(entry);
	}
	if (!role.CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", binder.RoleName(info.new_owner_id));
	}
	if (entry.type == CatalogType::SCHEMA_ENTRY || entry.type == CatalogType::SUBSCRIPTION_ENTRY) {
		binder.RequireDatabasePrivilege(entry.ParentCatalog().GetName(), AclMode::Create);
	} else if (entry.type == CatalogType::DATABASE_ENTRY) {
		if (!role.Has(RoleOption::CreateDb)) {
			throw PermissionException("permission denied to change owner of database");
		}
	} else if (entry.type == CatalogType::FOREIGN_SERVER_ENTRY) {
		binder.RequireForeignDataWrapperUsage(context.AccessOf(info.new_owner_id), entry.ParentCatalog().GetName(),
		                                      entry.GetInfo()->Cast<CreateForeignServerInfo>().fdw_name);
	} else {
		auto &schema = entry.ParentSchema(context);
		if (!context.AccessOf(info.new_owner_id).Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Create)) {
			ThrowPermissionDenied(schema);
		}
	}
}

static void ResolveDefaultPrivileges(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	info.target_role = info.for_role.empty() ? context.current_role_id : binder.RoleSpecId(info.for_role);
	if (!binder.CheckAs().MemberOf(info.target_role)) {
		throw PermissionException("permission denied to change default privileges");
	}
	info.grantee_id = GranteeId(binder, info.grantee);
	auto database = DatabaseManager::GetDefaultDatabase(context);
	if (!info.default_schema.empty()) {
		auto &schema = Catalog::GetSchema(context, Identifier(), Identifier(info.default_schema));
		info.default_scope = schema.oid;
		database = schema.ParentCatalog().GetName();
	}
	if (!binder.ClusterEntry(CatalogType::DATABASE_ENTRY, database)) {
		throw CatalogException(unordered_map<string, string> {{"type", "Database"}},
		                       StringUtil::Format("database \"%s\" does not exist", database.GetIdentifierName()));
	}
	info.SetQualifiedName(Identifier(), Identifier(), database);
}

static idx_t ResolveOwnedRole(Binder &binder, const string &name, const string &action, const string &detail) {
	auto role = binder.RoleSpecId(name);
	if (!binder.CheckAs().MemberOf(role)) {
		throw PermissionException(
		    unordered_map<string, string> {{"detail", StringUtil::Format(detail, binder.RoleName(role), action)}},
		    StringUtil::Format("permission denied to %s objects", action));
	}
	return role;
}

static vector<idx_t> ResolveOwnedRoles(Binder &binder, const vector<string> &names, const string &action) {
	vector<idx_t> result;
	for (auto &name : names) {
		result.push_back(ResolveOwnedRole(binder, name, action,
		                                  "Only roles with privileges of role \"%s\" may %s objects owned by it."));
	}
	return result;
}

static void RefuseBootstrapRole(Binder &binder, const vector<idx_t> &roles, const string &action) {
	const auto bootstrap = DBConfig::GetConfig(binder.context).bootstrap_role;
	if (bootstrap != 0 && std::find(roles.begin(), roles.end(), bootstrap) != roles.end()) {
		throw DependencyException("cannot %s owned by role %s because they are required by the database system", action,
		                          binder.RoleName(bootstrap));
	}
}

static void ResolveOwned(Binder &binder, AlterInfo &info) {
	if (info.type == AlterType::DROP_OWNED) {
		auto &drop = info.Cast<DropOwnedInfo>();
		drop.role_ids = ResolveOwnedRoles(binder, drop.roles, "drop");
		drop.grantors = CallerGrantors(binder);
		RefuseBootstrapRole(binder, drop.role_ids, "drop objects");
		return;
	}
	auto &reassign = info.Cast<ReassignOwnedInfo>();
	reassign.role_ids = ResolveOwnedRoles(binder, reassign.roles, "reassign");
	reassign.new_owner_id = ResolveOwnedRole(binder, reassign.new_owner, "reassign",
	                                         "Only roles with privileges of role \"%s\" may %s objects to it.");
	RefuseBootstrapRole(binder, reassign.role_ids, "reassign ownership of objects");
}

static void ResolvePermissions(Binder &binder, AlterPermissionsInfo &info) {
	if (!info.new_owner.empty()) {
		ResolveOwner(binder, info);
	} else if (info.default_objtype != CatalogType::INVALID) {
		ResolveDefaultPrivileges(binder, info);
	} else {
		ResolveGrant(binder, info);
	}
}

BoundStatement Binder::Bind(AlterStatement &stmt) {
	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	// Special handling for ALTER DATABASE - doesn't use schema binding
	if (stmt.info->type == AlterType::ALTER_DATABASE) {
		if (!DBConfig::GetConfig(context).cluster_catalog.empty()) {
			throw NotImplementedException("renaming a database is not supported");
		}
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(Catalog::GetSystemCatalog(context), context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_SCHEMA) {
		// resolve the schema path the same way as CREATE SCHEMA does
		auto &info = stmt.info->Cast<AlterSchemaInfo>();
		info.SetQualifiedName(ResolveCatalog(context, info.GetQualifiedName()));
		auto &catalog = Catalog::GetCatalog(context, info.SchemaCatalog());
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
		auto schema =
		    Catalog::GetSchema(context, info.SchemaCatalog(), info.SchemaPath(), OnEntryNotFound::RETURN_NULL);
		if (schema) {
			RequireOwner(*schema);
			RequireDatabasePrivilege(info.SchemaCatalog(), AclMode::Create);
		}
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::SET_COMMENT && (stmt.info->GetCatalogType() == CatalogType::DATABASE_ENTRY ||
	                                                  stmt.info->GetCatalogType() == CatalogType::ROLE_ENTRY)) {
		if (DBConfig::GetConfig(context).cluster_catalog.empty()) {
			throw NotImplementedException("Adding comments to %ss is not implemented",
			                              stmt.info->GetCatalogType() == CatalogType::DATABASE_ENTRY ? "database"
			                                                                                         : "role");
		}
		CheckSharedObjectComment(*stmt.info);
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		stmt.info->SetQualifiedName(ClusterName(stmt.info->GetQualifiedName()));
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_SUBSCRIPTION) {
		auto &subscription = ResolveDatabaseEntry(*this, CatalogType::SUBSCRIPTION_ENTRY, *stmt.info);
		RequireSubscriptionOwner(*this, subscription);
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(subscription.ParentCatalog(), context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::REASSIGN_OWNED || stmt.info->type == AlterType::DROP_OWNED) {
		ResolveOwned(*this, *stmt.info);
		auto &catalog = Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
		stmt.info->SetQualifiedName(catalog.GetName(), Identifier(), Identifier());
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(
		    catalog, context, DatabaseModificationType::ALTER_TABLE | DatabaseModificationType::DROP_CATALOG_ENTRY);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_PERMISSIONS || stmt.info->type == AlterType::ALTER_ROLE ||
	    stmt.info->type == AlterType::ALTER_DATABASE_SETTINGS) {
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		const auto catalog_type = stmt.info->GetCatalogType();
		if (catalog_type != CatalogType::DATABASE_ENTRY && catalog_type != CatalogType::ROLE_ENTRY &&
		    catalog_type != CatalogType::FOREIGN_DATA_WRAPPER_ENTRY && catalog_type != CatalogType::PARAMETER_ENTRY &&
		    catalog_type != CatalogType::LANGUAGE_ENTRY) {
			stmt.info->SetQualifiedName(BindTableName(stmt.info->GetQualifiedName()));
			auto &catalog = Catalog::GetCatalog(context, stmt.info->GetQualifiedName().Catalog());
			properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
		}
		if (stmt.info->type == AlterType::ALTER_PERMISSIONS) {
			ResolvePermissions(*this, stmt.info->Cast<AlterPermissionsInfo>());
		} else if (stmt.info->type == AlterType::ALTER_DATABASE_SETTINGS) {
			auto &settings = stmt.info->Cast<AlterDatabaseSettingsInfo>();
			if (!settings.role.empty()) {
				settings.role_id = RoleSpecId(settings.role);
			}
			CheckAlterDatabaseSettings(settings);
		} else {
			if (const auto role = RoleSpecKeyword(stmt.info->GetQualifiedName().Name().GetIdentifierName());
			    role.IsValid()) {
				stmt.info->SetQualifiedName(Identifier(), Identifier(), Identifier(RoleName(role.GetIndex())));
			}
			CheckAlterRole(stmt.info->Cast<AlterRoleInfo>());
		}
		const auto target_type = stmt.info->GetCatalogType();
		if (target_type == CatalogType::DATABASE_ENTRY || target_type == CatalogType::ROLE_ENTRY) {
			stmt.info->SetQualifiedName(ClusterName(stmt.info->GetQualifiedName()));
		}
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	// resolve the (possibly nested) catalog/schema qualification of the altered entry
	stmt.info->SetQualifiedName(BindTableName(stmt.info->GetQualifiedName()));

	if (stmt.info->type == AlterType::ALTER_SCALAR_FUNCTION &&
	    stmt.info->Cast<AlterScalarFunctionInfo>().alter_scalar_function_type ==
	        AlterScalarFunctionType::SET_FUNCTION_ATTRIBUTES) {
		auto &info = stmt.info->Cast<AlterFunctionAttributesInfo>();
		for (auto &type : info.parameter_types) {
			BindLogicalType(type);
		}
		const auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		optional_ptr<CatalogEntry> function;
		for (auto type : {CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY}) {
			function =
			    entry_retriever.GetEntry(EntryLookupInfo(type, info.GetQualifiedName()), OnEntryNotFound::RETURN_NULL);
			if (function) {
				break;
			}
		}
		if (!function && info.has_parameter_types) {
			throw CatalogException("function %s(%s) does not exist", name,
			                       MacroFunction::ParameterTypesToString(info.parameter_types));
		}
		if (!function) {
			throw CatalogException("could not find a function named \"%s\"", name);
		}
		if (function->internal) {
			ThrowPermissionDenied(*function);
		}
		RequireOwner(*function);
		auto create_info = function->Cast<MacroCatalogEntry>().GetInfo();
		auto &macros = create_info->Cast<CreateMacroInfo>().macros;
		vector<reference<MacroFunction>> targets;
		for (auto &macro : macros) {
			if (!info.has_parameter_types || macro->HasParameterTypes(info.parameter_types)) {
				targets.push_back(*macro);
			}
		}
		if (targets.empty()) {
			throw CatalogException("function %s(%s) does not exist", name,
			                       MacroFunction::ParameterTypesToString(info.parameter_types));
		}
		if (targets.size() > 1) {
			throw CatalogException(
			    unordered_map<string, string> {
			        {"hint", "Specify the argument list to select the function unambiguously."}},
			    StringUtil::Format("function name \"%s\" is not unique", name));
		}
		auto &macro = targets[0].get();
		if (info.set_strict && macro.strict != info.strict) {
			throw NotImplementedException("ALTER FUNCTION cannot change whether a function is strict");
		}
		if (info.set_security) {
			macro.security_definer = info.security_definer;
		}
		if (info.set_stability) {
			macro.stability = info.stability;
		}
		if (info.replace_config) {
			macro.config = info.config;
			Settings::ResolveCurrentValues(context, macro.config);
		}
		create_info->on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		auto &schema = function->ParentSchema(context);
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(schema.ParentCatalog(), context, DatabaseModificationType::ALTER_TABLE);
		result.plan =
		    make_uniq<LogicalCreate>(LogicalOperatorType::LOGICAL_CREATE_MACRO, std::move(create_info), &schema);
		return result;
	}

	// ALTER FUNCTION ... RENAME TO ... skips entry lookup (scalar-vs-table
	// macro is ambiguous at parse time).
	if (stmt.info->type == AlterType::ALTER_SCALAR_FUNCTION &&
	    stmt.info->Cast<AlterScalarFunctionInfo>().alter_scalar_function_type ==
	        AlterScalarFunctionType::RENAME_SCALAR_FUNCTION) {
		optional_ptr<CatalogEntry> function;
		for (auto type : {CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY}) {
			EntryLookupInfo lookup_info(type, stmt.info->GetQualifiedName());
			function = entry_retriever.GetEntry(lookup_info, OnEntryNotFound::RETURN_NULL);
			if (function) {
				break;
			}
		}
		if (stmt.info->GetQualifiedName().Schema().empty()) {
			if (!function) {
				EntryLookupInfo lookup_info(CatalogType::MACRO_ENTRY, stmt.info->GetQualifiedName());
				function = entry_retriever.GetEntry(lookup_info, stmt.info->if_not_found);
			}
			if (function) {
				stmt.info->SetQualifiedName(function->Cast<StandardEntry>().GetQualifiedName(function->name));
			}
		}
		if (function) {
			RequireOwner(*function);
			RequireSchemaCreate(function->ParentSchema(context));
		}
		auto &catalog = Catalog::GetCatalog(context, stmt.info->GetQualifiedName().Catalog());
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	optional_ptr<CatalogEntry> entry;
	auto lookup = [&](CatalogType type, OnEntryNotFound if_not_found) {
		EntryLookupInfo lookup_info(type, stmt.info->GetQualifiedName());
		return entry_retriever.GetEntry(lookup_info, if_not_found);
	};
	if (stmt.info->type == AlterType::SET_COLUMN_COMMENT) {
		// Extra step for column comments: They can alter a table or a view, and we resolve that here.
		auto &info = stmt.info->Cast<SetColumnCommentInfo>();
		entry = info.TryResolveCatalogEntry(entry_retriever);
		if (entry && info.catalog_entry_type == CatalogType::VIEW_ENTRY) {
			// when running SET COLUMN on a VIEW - ensure the view is bound
			auto &view = entry->Cast<ViewCatalogEntry>();
			view.BindView(context);
		}
	} else if (stmt.info->GetNewName() && (stmt.info->GetCatalogType() == CatalogType::TABLE_ENTRY ||
	                                       stmt.info->GetCatalogType() == CatalogType::INDEX_ENTRY)) {
		auto &target_catalog = Catalog::GetCatalog(context, stmt.info->GetQualifiedName().Catalog());
		const bool rename_spans_tables_and_indexes = target_catalog.Compatibility() == SqlCompatibility::POSTGRES;
		const auto declared = stmt.info->GetCatalogType();
		const auto other = declared == CatalogType::TABLE_ENTRY ? CatalogType::INDEX_ENTRY : CatalogType::TABLE_ENTRY;
		entry = lookup(declared, OnEntryNotFound::RETURN_NULL);
		if (!entry && rename_spans_tables_and_indexes) {
			entry = lookup(other, OnEntryNotFound::RETURN_NULL);
			if (entry) {
				auto data = stmt.info->GetAlterEntryData();
				auto new_name = *stmt.info->GetNewName();
				const auto allow_internal = stmt.info->allow_internal;
				if (other == CatalogType::INDEX_ENTRY) {
					stmt.info = make_uniq<RenameIndexInfo>(data, std::move(new_name));
				} else {
					stmt.info = make_uniq<RenameTableInfo>(data, std::move(new_name));
				}
				stmt.info->allow_internal = allow_internal;
			}
		}
		if (!entry) {
			entry = lookup(declared, stmt.info->if_not_found);
		}
	} else if (stmt.info->type == AlterType::SET_SCHEMA) {
		entry = BindSetSchemaEntry(stmt.info->Cast<SetSchemaInfo>(), lookup);
	} else {
		// For any other ALTER, we retrieve the catalog entry directly.
		entry = lookup(stmt.info->GetCatalogType(), stmt.info->if_not_found);
	}

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	if (!entry) {
		// Bind types in this binder
		BindAlterTypes(*this, stmt);
		RequireTypeUsage(nullptr);

		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	D_ASSERT(!entry->deleted);
	RequireOwner(*entry);
	if (stmt.info->GetNewName()) {
		RequireSchemaCreate(entry->ParentSchema(context));
	}
	auto &catalog = entry->ParentCatalog();

	// Bind types in the same catalog as the entry
	auto type_binder = Binder::CreateBinder(context, *this);
	type_binder->SetSearchPath(catalog, stmt.info->GetQualifiedName().Schema());

	BindAlterTypes(*type_binder, stmt);
	RequireTypeUsage(nullptr);

	if (catalog.IsSystemCatalog()) {
		throw BinderException("Can not comment on System Catalog entries");
	}
	if (!entry->temporary) {
		// We can only alter temporary tables and views in read-only mode.
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
	}
	stmt.info->SetQualifiedName(QualifiedName::FromCatalogSchema(
	    catalog.GetName(), entry->ParentSchemaPath(catalog.GetCatalogTransaction(context)),
	    stmt.info->GetQualifiedName().Name()));

	if (!stmt.info->IsAddUniqueConstraint()) {
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	return BindAlterAddIndex(result, *entry, std::move(stmt.info));
}

} // namespace duckdb
