#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/create_role_info.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/function/macro_function.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/constraint.hpp"

namespace duckdb {

static string DottedNameToSQL(const string &name) {
	return StringUtil::Join(StringUtil::Split(name, '.'), ".",
	                        [](const string &part) { return SQLIdentifier::ToString(part); });
}

static string PrivilegesToSQL(AclMode privileges, const vector<ColumnPrivilege> &columns) {
	vector<string> items;
	for (auto &named : ACL_PRIVILEGES) {
		if ((privileges & named.mode) != AclMode::NoRights) {
			items.emplace_back(named.keyword);
		}
		vector<string> names;
		for (auto &column : columns) {
			if ((column.privileges & named.mode) != AclMode::NoRights) {
				names.push_back(SQLIdentifier::ToString(column.column));
			}
		}
		if (!names.empty()) {
			items.push_back(string(named.keyword) + " (" + StringUtil::Join(names, ", ") + ")");
		}
	}
	return StringUtil::Join(items, ", ");
}

static string ConfigClauseToSQL(const vector<string> &set_config, const vector<string> &reset_config,
                                bool reset_all_config) {
	if (reset_all_config) {
		return " RESET ALL";
	}
	if (!reset_config.empty()) {
		return " RESET " + Settings::ConfigNameToSQL(reset_config[0]);
	}
	if (set_config.empty()) {
		return string();
	}
	return " SET " + Settings::ConfigToSQL(set_config[0]);
}

//===--------------------------------------------------------------------===//
// ChangeOwnershipInfo
//===--------------------------------------------------------------------===//
ChangeOwnershipInfo::ChangeOwnershipInfo(CatalogType entry_catalog_type, AlterEntryData data, Identifier owner_schema_p,
                                         Identifier owner_name_p)
    : AlterInfo(AlterType::CHANGE_OWNERSHIP, std::move(data.qualified_name), data.if_not_found),
      entry_catalog_type(entry_catalog_type), owner_schema(std::move(owner_schema_p)),
      owner_name(std::move(owner_name_p)) {
}

ChangeOwnershipInfo::ChangeOwnershipInfo() : AlterInfo(AlterType::CHANGE_OWNERSHIP) {
}

CatalogType ChangeOwnershipInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> ChangeOwnershipInfo::Copy() const {
	auto result = make_uniq<ChangeOwnershipInfo>(entry_catalog_type, GetAlterEntryData(), owner_schema, owner_name);
	result->owner_path = owner_path;
	return std::move(result);
}

string ChangeOwnershipInfo::ToString() const {
	string result = "";

	result += "ALTER ";
	result += TypeToString(entry_catalog_type);
	result += " ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " OWNED BY ";
	if (!owner_path.empty()) {
		for (idx_t i = 0; i < owner_path.size(); i++) {
			if (i) {
				result += ".";
			}
			result += SQLIdentifier(owner_path[i].GetIdentifierName());
		}
	} else if (owner_name.empty()) {
		result += "NONE";
	} else {
		result += QualifiedName(GetQualifiedName().Catalog(), owner_schema, owner_name)
		              .ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetCommentInfo
//===--------------------------------------------------------------------===//
SetCommentInfo::SetCommentInfo(CatalogType entry_catalog_type, Identifier entry_catalog_p, Identifier entry_schema_p,
                               Identifier entry_name_p, Value new_comment_value_p, OnEntryNotFound if_not_found)
    : AlterInfo(AlterType::SET_COMMENT,
                QualifiedName(std::move(entry_catalog_p), std::move(entry_schema_p), std::move(entry_name_p)),
                if_not_found),
      entry_catalog_type(entry_catalog_type), comment_value(std::move(new_comment_value_p)) {
}

CatalogType SetCommentInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> SetCommentInfo::Copy() const {
	auto result =
	    make_uniq<SetCommentInfo>(entry_catalog_type, GetQualifiedName().Catalog(), GetQualifiedName().Schema(),
	                              GetQualifiedName().Name(), comment_value, if_not_found);
	result->overload_oid = overload_oid;
	result->func_parameters = func_parameters;
	result->has_func_args = has_func_args;
	result->include_functions = include_functions;
	result->include_procedures = include_procedures;
	return std::move(result);
}

string SetCommentInfo::ToString() const {
	string result = "";

	result += "COMMENT ON ";
	result += entry_catalog_type == CatalogType::MACRO_ENTRY
	              ? ParseInfo::RoutineKeyword(include_functions, include_procedures)
	              : ParseInfo::TypeToString(entry_catalog_type);
	result += " ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (has_func_args) {
		result += "(" + MacroFunction::ParameterTypesToString(func_parameters) + ")";
	}
	result += " IS ";
	result += comment_value.ToSQLString();

	result += ";";
	return result;
}

SetCommentInfo::SetCommentInfo() : AlterInfo(AlterType::SET_COMMENT) {
}

//===--------------------------------------------------------------------===//
// SetSchemaInfo
//===--------------------------------------------------------------------===//
SetSchemaInfo::SetSchemaInfo(CatalogType entry_catalog_type, const AlterEntryData &data, Identifier new_schema_p)
    : AlterInfo(AlterType::SET_SCHEMA, data.qualified_name, data.if_not_found), entry_catalog_type(entry_catalog_type),
      new_schema(std::move(new_schema_p)) {
}

CatalogType SetSchemaInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> SetSchemaInfo::Copy() const {
	auto result = make_uniq<SetSchemaInfo>(entry_catalog_type, GetAlterEntryData(), new_schema);
	result->overload_oid = overload_oid;
	result->func_parameters = func_parameters;
	result->has_func_args = has_func_args;
	result->include_functions = include_functions;
	result->include_procedures = include_procedures;
	return std::move(result);
}

string SetSchemaInfo::ToString() const {
	string result = "ALTER ";
	result += entry_catalog_type == CatalogType::MACRO_ENTRY
	              ? ParseInfo::RoutineKeyword(include_functions, include_procedures)
	              : ParseInfo::TypeToString(entry_catalog_type);
	result += " ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (has_func_args) {
		result += "(" + MacroFunction::ParameterTypesToString(func_parameters) + ")";
	}
	result += " SET SCHEMA ";
	result += SQLIdentifier(new_schema);
	result += ";";
	return result;
}

SetSchemaInfo::SetSchemaInfo() : AlterInfo(AlterType::SET_SCHEMA) {
}

//===--------------------------------------------------------------------===//
// AlterPermissionsInfo
//===--------------------------------------------------------------------===//
AlterPermissionsInfo::AlterPermissionsInfo(CatalogType entry_catalog_type, QualifiedName entry_name)
    : AlterInfo(AlterType::ALTER_PERMISSIONS, std::move(entry_name), OnEntryNotFound::THROW_EXCEPTION),
      entry_catalog_type(entry_catalog_type) {
}

CatalogType AlterPermissionsInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> AlterPermissionsInfo::Copy() const {
	auto result = make_uniq<AlterPermissionsInfo>(entry_catalog_type, GetQualifiedName());
	result->if_not_found = if_not_found;
	result->new_owner = new_owner;
	result->new_owner_id = new_owner_id;
	result->privileges = privileges;
	result->column_privileges = column_privileges;
	result->grantee = grantee;
	result->grantee_id = grantee_id;
	result->granted_by = granted_by;
	result->grantors = grantors;
	result->best_grantor = best_grantor;
	result->revoke = revoke;
	result->with_grant_option = with_grant_option;
	result->option_only = option_only;
	result->cascade = cascade;
	result->default_objtype = default_objtype;
	result->for_role = for_role;
	result->default_schema = default_schema;
	result->target_role = target_role;
	result->default_scope = default_scope;
	result->default_acl_oid = default_acl_oid;
	result->all_in_schema = all_in_schema;
	result->include_functions = include_functions;
	result->include_procedures = include_procedures;
	result->drop_owned_role_id = drop_owned_role_id;
	result->overload_oid = overload_oid;
	result->func_parameters = func_parameters;
	result->has_func_args = has_func_args;
	result->objects = objects;
	result->grantees = grantees;
	result->grantee_ids = grantee_ids;
	result->for_roles = for_roles;
	result->target_roles = target_roles;
	result->default_schemas = default_schemas;
	result->default_scopes = default_scopes;
	return std::move(result);
}

static string TargetKeywordToSQL(const AlterPermissionsInfo &info) {
	if (info.entry_catalog_type == CatalogType::MACRO_ENTRY) {
		return ParseInfo::RoutineKeyword(info.include_functions, info.include_procedures);
	}
	return ParseInfo::TypeToString(info.entry_catalog_type);
}

static string GrantTargetToSQL(const AlterPermissionsInfo &info) {
	switch (info.entry_catalog_type) {
	case CatalogType::VIEW_ENTRY:
		return "TABLE";
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return "FOREIGN SERVER";
	default:
		return TargetKeywordToSQL(info);
	}
}

static string GrantObjectsToSQL(CatalogType type, bool include_functions, bool include_procedures) {
	if (type == CatalogType::MACRO_ENTRY) {
		return ParseInfo::RoutineKeyword(include_functions, include_procedures) + "S";
	}
	return ParseInfo::TypeToString(type) + "S";
}

static string PermissionsTargetToSQL(const AlterPermissionsInfo &info, const QualifiedName &name,
                                     const vector<LogicalType> &func_parameters, bool has_func_args) {
	if (info.all_in_schema) {
		return name.ToString();
	}
	auto target = name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (has_func_args) {
		target += "(" + MacroFunction::ParameterTypesToString(func_parameters) + ")";
	}
	return target;
}

template <class T>
static vector<T> ListedOr(const vector<T> &listed, const T &single) {
	if (listed.empty() && !single.empty()) {
		return {single};
	}
	return listed;
}

string AlterPermissionsInfo::ToString() const {
	vector<string> objects_sql;
	for (auto &object : objects) {
		objects_sql.push_back(PermissionsTargetToSQL(*this, object.name, object.func_parameters, object.has_func_args));
	}
	if (objects.empty()) {
		objects_sql.push_back(PermissionsTargetToSQL(*this, GetQualifiedName(), func_parameters, has_func_args));
	}
	auto targets = StringUtil::Join(objects_sql, ", ");
	if (!new_owner.empty()) {
		return "ALTER " + TargetKeywordToSQL(*this) + " " + targets + " OWNER TO " + new_owner.ToString() + ";";
	}
	string result;
	if (default_objtype != CatalogType::INVALID) {
		result = "ALTER DEFAULT PRIVILEGES ";
		auto roles = ListedOr(for_roles, for_role);
		if (!roles.empty()) {
			result += "FOR ROLE " + RoleSpec::ToString(roles) + " ";
		}
		auto schemas = ListedOr(default_schemas, default_schema);
		if (!schemas.empty()) {
			result += "IN SCHEMA " + StringUtil::Join(schemas, ", ", DottedNameToSQL) + " ";
		}
	}
	result += revoke ? "REVOKE " : "GRANT ";
	if (revoke && option_only) {
		result += "GRANT OPTION FOR ";
	}
	result += PrivilegesToSQL(privileges, column_privileges) + " ON ";
	if (default_objtype != CatalogType::INVALID) {
		result += GrantObjectsToSQL(default_objtype, true, false);
	} else if (all_in_schema) {
		result += "ALL " + GrantObjectsToSQL(entry_catalog_type, include_functions, include_procedures) +
		          " IN SCHEMA " + targets;
	} else {
		result += GrantTargetToSQL(*this) + " " + targets;
	}
	result += revoke ? " FROM " : " TO ";
	result += RoleSpec::ToString(ListedOr(grantees, grantee));
	if (!revoke && with_grant_option) {
		result += " WITH GRANT OPTION";
	}
	if (!granted_by.empty()) {
		result += " GRANTED BY " + granted_by.ToString();
	}
	if (revoke && cascade) {
		result += " CASCADE";
	}
	return result + ";";
}

ReplaceDefinitionInfo::ReplaceDefinitionInfo(unique_ptr<CreateInfo> definition_p)
    : AlterInfo(AlterType::REPLACE_DEFINITION, definition_p->GetQualifiedName(), OnEntryNotFound::THROW_EXCEPTION),
      definition(std::move(definition_p)) {
	new_dependencies = make_uniq<LogicalDependencyList>(definition->dependencies);
}

ReplaceDefinitionInfo::~ReplaceDefinitionInfo() {
}

CatalogType ReplaceDefinitionInfo::GetCatalogType() const {
	return definition->type;
}

unique_ptr<AlterInfo> ReplaceDefinitionInfo::Copy() const {
	auto result = make_uniq<ReplaceDefinitionInfo>(definition->Copy());
	result->if_not_found = if_not_found;
	return std::move(result);
}

string ReplaceDefinitionInfo::ToString() const {
	return definition->ToString();
}

AlterPermissionsInfo::AlterPermissionsInfo()
    : AlterInfo(AlterType::ALTER_PERMISSIONS), entry_catalog_type(CatalogType::INVALID) {
}

//===--------------------------------------------------------------------===//
// AlterRoleInfo
//===--------------------------------------------------------------------===//
AlterRoleInfo::AlterRoleInfo(Identifier role)
    : AlterInfo(AlterType::ALTER_ROLE, QualifiedName(Identifier(), Identifier(), std::move(role)),
                OnEntryNotFound::THROW_EXCEPTION) {
}

AlterRoleInfo::AlterRoleInfo() : AlterInfo(AlterType::ALTER_ROLE) {
}

CatalogType AlterRoleInfo::GetCatalogType() const {
	return CatalogType::ROLE_ENTRY;
}

unique_ptr<AlterInfo> AlterRoleInfo::Copy() const {
	auto result = make_uniq<AlterRoleInfo>(GetQualifiedName().Name());
	result->if_not_found = if_not_found;
	result->set_options = set_options;
	result->clear_options = clear_options;
	result->set_password = set_password;
	result->null_password = null_password;
	result->password = password;
	result->set_conn_limit = set_conn_limit;
	result->conn_limit = conn_limit;
	result->set_valid_until = set_valid_until;
	result->valid_until = valid_until;
	result->new_name = new_name;
	result->reset_all_config = reset_all_config;
	result->reset_config = reset_config;
	result->set_config = set_config;
	result->grant_role = grant_role;
	result->granted_roles = granted_roles;
	result->grantor_ids = grantor_ids;
	result->members = members;
	result->revoke = revoke;
	result->option_only = option_only;
	result->admin_option = admin_option;
	result->inherit_option = inherit_option;
	result->set_option = set_option;
	result->grant_role_id = grant_role_id;
	result->grantor_id = grantor_id;
	result->granted_by = granted_by;
	result->cascade = cascade;
	result->alter_group = alter_group;
	result->role_type = role_type;
	return std::move(result);
}

RoleSpec AlterRoleInfo::Target() const {
	RoleSpec target(GetQualifiedName().Name());
	target.type = role_type;
	return target;
}

static string MembershipToSQL(const AlterRoleInfo &info) {
	auto roles = info.granted_roles.empty() ? info.grant_role.ToString() : RoleSpec::ToString(info.granted_roles);
	auto members = info.members.empty() ? info.Target().ToString() : RoleSpec::ToString(info.members);
	if (info.alter_group) {
		return "ALTER GROUP " + roles + (info.revoke ? " DROP USER " : " ADD USER ") + members + ";";
	}
	string result = info.revoke ? "REVOKE " : "GRANT ";
	if (info.revoke && info.option_only) {
		result += info.admin_option == 0 ? "ADMIN" : info.inherit_option == 0 ? "INHERIT" : "SET";
		result += " OPTION FOR ";
	}
	result += roles + (info.revoke ? " FROM " : " TO ") + members;
	if (!info.revoke) {
		vector<string> options;
		auto option = [&](const char *name, int8_t value) {
			if (value >= 0) {
				options.push_back(string(name) + (value ? " TRUE" : " FALSE"));
			}
		};
		option("ADMIN", info.admin_option);
		option("INHERIT", info.inherit_option);
		option("SET", info.set_option);
		if (!options.empty()) {
			result += " WITH " + StringUtil::Join(options, ", ");
		}
	}
	if (!info.granted_by.empty()) {
		result += " GRANTED BY " + info.granted_by.ToString();
	}
	if (info.revoke && info.cascade) {
		result += " CASCADE";
	}
	return result + ";";
}

string AlterRoleInfo::ToString() const {
	if (!grant_role.empty() || !granted_roles.empty()) {
		return MembershipToSQL(*this);
	}
	string result = "ALTER ROLE " + Target().ToString();
	if (!new_name.empty()) {
		return result + " RENAME TO " + SQLIdentifier::ToString(new_name) + ";";
	}
	auto config = ConfigClauseToSQL(set_config, reset_config, reset_all_config);
	if (!config.empty()) {
		return result + config + ";";
	}
	result += " WITH" + RoleOptionsToSQL(set_options, clear_options);
	if (set_conn_limit) {
		result += " CONNECTION LIMIT " + std::to_string(conn_limit);
	}
	if (set_valid_until) {
		result += " VALID UNTIL " + SQLString::ToString(Value::TIMESTAMPTZ(timestamp_tz_t(valid_until)).ToString());
	}
	if (set_password) {
		result += null_password ? " PASSWORD NULL" : " PASSWORD " + SQLString::ToString(password);
	}
	return result + ";";
}

//===--------------------------------------------------------------------===//
// AlterDatabaseSettingsInfo
//===--------------------------------------------------------------------===//
AlterDatabaseSettingsInfo::AlterDatabaseSettingsInfo(Identifier database)
    : AlterInfo(AlterType::ALTER_DATABASE_SETTINGS, QualifiedName(Identifier(), Identifier(), std::move(database)),
                OnEntryNotFound::THROW_EXCEPTION) {
}

AlterDatabaseSettingsInfo::AlterDatabaseSettingsInfo() : AlterInfo(AlterType::ALTER_DATABASE_SETTINGS) {
}

CatalogType AlterDatabaseSettingsInfo::GetCatalogType() const {
	return CatalogType::DATABASE_ENTRY;
}

unique_ptr<AlterInfo> AlterDatabaseSettingsInfo::Copy() const {
	auto result = make_uniq<AlterDatabaseSettingsInfo>();
	result->SetQualifiedName(GetQualifiedName());
	result->if_not_found = if_not_found;
	result->role = role;
	result->role_id = role_id;
	result->all_roles = all_roles;
	result->reset_all_config = reset_all_config;
	result->reset_config = reset_config;
	result->set_config = set_config;
	result->set_connection_limit = set_connection_limit;
	result->connection_limit = connection_limit;
	result->set_allow_connections = set_allow_connections;
	result->allow_connections = allow_connections;
	return std::move(result);
}

string AlterDatabaseSettingsInfo::ToString() const {
	auto database = SQLIdentifier::ToString(GetQualifiedName().Name());
	auto config = ConfigClauseToSQL(set_config, reset_config, reset_all_config);
	if (all_roles) {
		return "ALTER ROLE ALL IN DATABASE " + database + config + ";";
	}
	if (!role.empty()) {
		return "ALTER ROLE " + role.ToString() + " IN DATABASE " + database + config + ";";
	}
	if (!config.empty()) {
		return "ALTER DATABASE " + database + config + ";";
	}
	string options;
	if (set_allow_connections) {
		options += allow_connections ? " ALLOW_CONNECTIONS TRUE" : " ALLOW_CONNECTIONS FALSE";
	}
	if (set_connection_limit) {
		options += " CONNECTION LIMIT " + std::to_string(connection_limit);
	}
	return "ALTER DATABASE " + database + options + ";";
}

AlterOwnedInfo::AlterOwnedInfo() : AlterInfo(AlterType::ALTER_OWNED) {
}

CatalogType AlterOwnedInfo::GetCatalogType() const {
	return CatalogType::INVALID;
}

unique_ptr<AlterInfo> AlterOwnedInfo::Copy() const {
	auto result = make_uniq<AlterOwnedInfo>();
	result->SetQualifiedName(GetQualifiedName());
	result->roles = roles;
	result->new_owner = new_owner;
	result->cascade = cascade;
	result->role_ids = role_ids;
	result->new_owner_id = new_owner_id;
	result->grantors = grantors;
	return std::move(result);
}

string AlterOwnedInfo::ToString() const {
	if (new_owner.empty()) {
		return "DROP OWNED BY " + RoleSpec::ToString(roles) + (cascade ? " CASCADE;" : ";");
	}
	return "REASSIGN OWNED BY " + RoleSpec::ToString(roles) + " TO " + new_owner.ToString() + ";";
}

//===--------------------------------------------------------------------===//
// AlterTableInfo
//===--------------------------------------------------------------------===//
AlterTableInfo::AlterTableInfo(AlterTableType type) : AlterInfo(AlterType::ALTER_TABLE), alter_table_type(type) {
}

AlterTableInfo::AlterTableInfo(AlterTableType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_TABLE, data.GetQualifiedName(), data.if_not_found), alter_table_type(type) {
}
AlterTableInfo::~AlterTableInfo() {
}

CatalogType AlterTableInfo::GetCatalogType() const {
	return CatalogType::TABLE_ENTRY;
}
//===--------------------------------------------------------------------===//
// RenameColumnInfo
//===--------------------------------------------------------------------===//
RenameColumnInfo::RenameColumnInfo(const AlterEntryData &data, Identifier old_name_p, Identifier new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_COLUMN, data), old_name(std::move(old_name_p)),
      new_name(std::move(new_name_p)) {
}

RenameColumnInfo::RenameColumnInfo() : AlterTableInfo(AlterTableType::RENAME_COLUMN) {
}

RenameColumnInfo::~RenameColumnInfo() {
}

unique_ptr<AlterInfo> RenameColumnInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameColumnInfo>(GetAlterEntryData(), old_name, new_name);
}

string RenameColumnInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME COLUMN ";
	result += SQLIdentifier(old_name);
	result += " TO ";
	result += SQLIdentifier(new_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RenameFieldInfo
//===--------------------------------------------------------------------===//
RenameFieldInfo::RenameFieldInfo(const AlterEntryData &data, vector<Identifier> column_path_p, Identifier new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_FIELD, data), column_path(std::move(column_path_p)),
      new_name(std::move(new_name_p)) {
}

RenameFieldInfo::RenameFieldInfo() : AlterTableInfo(AlterTableType::RENAME_FIELD) {
}

RenameFieldInfo::~RenameFieldInfo() {
}

unique_ptr<AlterInfo> RenameFieldInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameFieldInfo>(GetAlterEntryData(), column_path, new_name);
}

string RenameFieldInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " TO ";
	result += SQLIdentifier(new_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RenameTableInfo
//===--------------------------------------------------------------------===//
RenameTableInfo::RenameTableInfo() : AlterTableInfo(AlterTableType::RENAME_TABLE) {
}

RenameTableInfo::RenameTableInfo(const AlterEntryData &data, Identifier new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_TABLE, data), new_table_name(std::move(new_name_p)) {
}

RenameTableInfo::~RenameTableInfo() {
}

unique_ptr<AlterInfo> RenameTableInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameTableInfo>(GetAlterEntryData(), new_table_name);
}

string RenameTableInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME TO ";
	result += SQLIdentifier(new_table_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AddColumnInfo
//===--------------------------------------------------------------------===//
AddColumnInfo::AddColumnInfo(ColumnDefinition new_column_p)
    : AlterTableInfo(AlterTableType::ADD_COLUMN), new_column(std::move(new_column_p)) {
}

AddColumnInfo::AddColumnInfo(const AlterEntryData &data, ColumnDefinition new_column_p, bool if_column_not_exists_p,
                             AddColumnConstraints add_column_constraints_p)
    : AlterTableInfo(AlterTableType::ADD_COLUMN, data), new_column(std::move(new_column_p)),
      if_column_not_exists(if_column_not_exists_p), add_column_constraints(add_column_constraints_p) {
}

AddColumnInfo::~AddColumnInfo() {
}

unique_ptr<AlterInfo> AddColumnInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddColumnInfo>(GetAlterEntryData(), new_column.Copy(), if_column_not_exists,
	                                                add_column_constraints);
}

string AddColumnInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ADD COLUMN";
	if (if_column_not_exists) {
		result += " IF NOT EXISTS";
	}
	result += " " + SQLIdentifier(this->new_column.GetName());
	result += " " + this->new_column.GetType().ToString();
	if (add_column_constraints.add_not_null) {
		result += " NOT NULL";
	}
	if (add_column_constraints.add_primary_key) {
		result += " PRIMARY KEY";
	} else if (add_column_constraints.add_unique) {
		result += " UNIQUE";
	}
	if (this->new_column.HasDefaultValue()) {
		result += " DEFAULT ";
		result += this->new_column.DefaultValue().ToString();
	}
	if (this->new_column.CompressionType() != CompressionType::COMPRESSION_AUTO) {
		result += " USING COMPRESSION " + CompressionTypeToString(this->new_column.CompressionType());
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AddFieldInfo
//===--------------------------------------------------------------------===//
AddFieldInfo::AddFieldInfo(ColumnDefinition new_field_p)
    : AlterTableInfo(AlterTableType::ADD_FIELD), new_field(std::move(new_field_p)) {
}

AddFieldInfo::AddFieldInfo(const AlterEntryData &data, vector<Identifier> column_path_p, ColumnDefinition new_field_p,
                           bool if_field_not_exists)
    : AlterTableInfo(AlterTableType::ADD_FIELD, data), column_path(std::move(column_path_p)),
      new_field(std::move(new_field_p)), if_field_not_exists(if_field_not_exists) {
}

AddFieldInfo::~AddFieldInfo() {
}

unique_ptr<AlterInfo> AddFieldInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddFieldInfo>(GetAlterEntryData(), column_path, new_field.Copy(),
	                                               if_field_not_exists);
}

string AddFieldInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ADD COLUMN ";
	if (if_field_not_exists) {
		result += "IF NOT EXISTS ";
	}
	for (auto &path : column_path) {
		result += SQLIdentifier(path);
		result += ".";
	}
	result += new_field.ToSQLString();
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RemoveColumnInfo
//===--------------------------------------------------------------------===//
RemoveColumnInfo::RemoveColumnInfo() : AlterTableInfo(AlterTableType::REMOVE_COLUMN) {
}

RemoveColumnInfo::RemoveColumnInfo(const AlterEntryData &data, string removed_column, bool if_column_exists,
                                   bool cascade)
    : AlterTableInfo(AlterTableType::REMOVE_COLUMN, data), removed_column(std::move(removed_column)),
      if_column_exists(if_column_exists), cascade(cascade) {
}
RemoveColumnInfo::~RemoveColumnInfo() {
}

unique_ptr<AlterInfo> RemoveColumnInfo::Copy() const {
	return make_uniq_base<AlterInfo, RemoveColumnInfo>(GetAlterEntryData(), removed_column.GetIdentifierName(),
	                                                   if_column_exists, cascade);
}

string RemoveColumnInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " DROP COLUMN ";
	if (if_column_exists) {
		result += "IF EXISTS ";
	}
	result += SQLIdentifier(removed_column);
	if (cascade) {
		result += " CASCADE";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RemoveFieldInfo
//===--------------------------------------------------------------------===//
RemoveFieldInfo::RemoveFieldInfo() : AlterTableInfo(AlterTableType::REMOVE_FIELD) {
}

RemoveFieldInfo::RemoveFieldInfo(const AlterEntryData &data, vector<Identifier> column_path_p, bool if_column_exists,
                                 bool cascade)
    : AlterTableInfo(AlterTableType::REMOVE_FIELD, data), column_path(std::move(column_path_p)),
      if_column_exists(if_column_exists), cascade(cascade) {
}
RemoveFieldInfo::~RemoveFieldInfo() {
}

unique_ptr<AlterInfo> RemoveFieldInfo::Copy() const {
	return make_uniq_base<AlterInfo, RemoveFieldInfo>(GetAlterEntryData(), column_path, if_column_exists, cascade);
}

string RemoveFieldInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " DROP COLUMN ";
	if (if_column_exists) {
		result += "IF EXISTS ";
	}
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	if (cascade) {
		result += " CASCADE";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// ChangeColumnTypeInfo
//===--------------------------------------------------------------------===//
ChangeColumnTypeInfo::ChangeColumnTypeInfo() : AlterTableInfo(AlterTableType::ALTER_COLUMN_TYPE) {
}

ChangeColumnTypeInfo::ChangeColumnTypeInfo(const AlterEntryData &data, vector<Identifier> column_path,
                                           LogicalType target_type, unique_ptr<ParsedExpression> expression,
                                           idx_t catalog_oid_p)
    : AlterTableInfo(AlterTableType::ALTER_COLUMN_TYPE, data), column_path(std::move(column_path)),
      target_type(std::move(target_type)), expression(std::move(expression)), catalog_oid(catalog_oid_p) {
}
ChangeColumnTypeInfo::~ChangeColumnTypeInfo() {
}

unique_ptr<AlterInfo> ChangeColumnTypeInfo::Copy() const {
	return make_uniq_base<AlterInfo, ChangeColumnTypeInfo>(GetAlterEntryData(), column_path, target_type,
	                                                       expression ? expression->Copy() : nullptr, catalog_oid);
}

string ChangeColumnTypeInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " TYPE ";
	if (target_type.IsValid()) {
		result += target_type.ToString();
	}
	auto &type_info = target_type.GetTypeInfo();
	if (type_info.type == LogicalTypeInfoType::STRING_TYPE_INFO) {
		auto &string_info = type_info.Cast<StringTypeInfo>();
		if (!string_info.collation.empty()) {
			result += " COLLATE " + string_info.collation;
		}
	}
	if (expression) {
		result += " USING ";
		result += expression->ToString();
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetDefaultInfo
//===--------------------------------------------------------------------===//
SetDefaultInfo::SetDefaultInfo() : AlterTableInfo(AlterTableType::SET_DEFAULT) {
}

SetDefaultInfo::SetDefaultInfo(const AlterEntryData &data, vector<Identifier> column_path,
                               unique_ptr<ParsedExpression> new_default)
    : AlterTableInfo(AlterTableType::SET_DEFAULT, data), column_path(std::move(column_path)),
      expression(std::move(new_default)) {
}
SetDefaultInfo::~SetDefaultInfo() {
}

unique_ptr<AlterInfo> SetDefaultInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetDefaultInfo>(GetAlterEntryData(), column_path,
	                                                 expression ? expression->Copy() : nullptr);
}

string SetDefaultInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	if (expression) {
		result += " SET DEFAULT ";
		result += expression->ToString();
	} else {
		result += " DROP DEFAULT";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetColumnCompressionInfo
//===--------------------------------------------------------------------===//
SetColumnCompressionInfo::SetColumnCompressionInfo()
    : AlterTableInfo(AlterTableType::SET_COLUMN_COMPRESSION), compression_type(CompressionType::COMPRESSION_AUTO),
      compression_level(0) {
}

SetColumnCompressionInfo::SetColumnCompressionInfo(const AlterEntryData &data, Identifier column_name_p,
                                                   CompressionType compression_type_p, uint8_t compression_level_p)
    : AlterTableInfo(AlterTableType::SET_COLUMN_COMPRESSION, data), column_name(std::move(column_name_p)),
      compression_type(compression_type_p), compression_level(compression_level_p) {
}

SetColumnCompressionInfo::~SetColumnCompressionInfo() {
}

unique_ptr<AlterInfo> SetColumnCompressionInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetColumnCompressionInfo>(GetAlterEntryData(), column_name, compression_type,
	                                                           compression_level);
}

string SetColumnCompressionInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	result += SQLIdentifier(column_name);
	result += " SET COMPRESSION ";
	if (compression_type == CompressionType::COMPRESSION_AUTO) {
		result += "DEFAULT";
	} else {
		result += CompressionTypeToString(compression_type);
		if (compression_level != 0) {
			result += "(compression_level = " + to_string(compression_level) + ")";
		}
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetNotNullInfo
//===--------------------------------------------------------------------===//
SetNotNullInfo::SetNotNullInfo() : AlterTableInfo(AlterTableType::SET_NOT_NULL) {
}

SetNotNullInfo::SetNotNullInfo(const AlterEntryData &data, vector<Identifier> column_path)
    : AlterTableInfo(AlterTableType::SET_NOT_NULL, data), column_path(std::move(column_path)) {
}
SetNotNullInfo::~SetNotNullInfo() {
}

unique_ptr<AlterInfo> SetNotNullInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetNotNullInfo>(GetAlterEntryData(), column_path);
}

string SetNotNullInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " SET NOT NULL";
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// DropNotNullInfo
//===--------------------------------------------------------------------===//
DropNotNullInfo::DropNotNullInfo() : AlterTableInfo(AlterTableType::DROP_NOT_NULL) {
}

DropNotNullInfo::DropNotNullInfo(const AlterEntryData &data, vector<Identifier> column_path)
    : AlterTableInfo(AlterTableType::DROP_NOT_NULL, data), column_path(std::move(column_path)) {
}
DropNotNullInfo::~DropNotNullInfo() {
}

unique_ptr<AlterInfo> DropNotNullInfo::Copy() const {
	return make_uniq_base<AlterInfo, DropNotNullInfo>(GetAlterEntryData(), column_path);
}

string DropNotNullInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " DROP NOT NULL";
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AlterForeignKeyInfo
//===--------------------------------------------------------------------===//
AlterForeignKeyInfo::AlterForeignKeyInfo() : AlterTableInfo(AlterTableType::FOREIGN_KEY_CONSTRAINT) {
}

AlterForeignKeyInfo::AlterForeignKeyInfo(const AlterEntryData &data, Identifier fk_table, vector<Identifier> pk_columns,
                                         vector<Identifier> fk_columns, vector<PhysicalIndex> pk_keys,
                                         vector<PhysicalIndex> fk_keys, AlterForeignKeyType type_p)
    : AlterTableInfo(AlterTableType::FOREIGN_KEY_CONSTRAINT, data), fk_table(std::move(fk_table)),
      pk_columns(std::move(pk_columns)), fk_columns(std::move(fk_columns)), pk_keys(std::move(pk_keys)),
      fk_keys(std::move(fk_keys)), type(type_p) {
}
AlterForeignKeyInfo::~AlterForeignKeyInfo() {
}

unique_ptr<AlterInfo> AlterForeignKeyInfo::Copy() const {
	return make_uniq_base<AlterInfo, AlterForeignKeyInfo>(GetAlterEntryData(), fk_table, pk_columns, fk_columns,
	                                                      pk_keys, fk_keys, type);
}

string AlterForeignKeyInfo::ToString() const {
	throw NotImplementedException("NOT PARSABLE CURRENTLY");
}

//===--------------------------------------------------------------------===//
// Alter View
//===--------------------------------------------------------------------===//
AlterViewInfo::AlterViewInfo(AlterViewType type) : AlterInfo(AlterType::ALTER_VIEW), alter_view_type(type) {
}

AlterViewInfo::AlterViewInfo(AlterViewType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_VIEW, data.GetQualifiedName(), data.if_not_found), alter_view_type(type) {
}
AlterViewInfo::~AlterViewInfo() {
}

CatalogType AlterViewInfo::GetCatalogType() const {
	return CatalogType::VIEW_ENTRY;
}

//===--------------------------------------------------------------------===//
// RenameViewInfo
//===--------------------------------------------------------------------===//
RenameViewInfo::RenameViewInfo() : AlterViewInfo(AlterViewType::RENAME_VIEW) {
}
RenameViewInfo::RenameViewInfo(const AlterEntryData &data, Identifier new_name_p)
    : AlterViewInfo(AlterViewType::RENAME_VIEW, data), new_view_name(std::move(new_name_p)) {
}
RenameViewInfo::~RenameViewInfo() {
}

unique_ptr<AlterInfo> RenameViewInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameViewInfo>(GetAlterEntryData(), new_view_name);
}

string RenameViewInfo::ToString() const {
	string result = "";
	result += "ALTER VIEW ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME TO ";
	result += SQLIdentifier(new_view_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AlterIndexInfo
//===--------------------------------------------------------------------===//
AlterIndexInfo::AlterIndexInfo(AlterIndexType type) : AlterInfo(AlterType::ALTER_INDEX), alter_index_type(type) {
}

AlterIndexInfo::AlterIndexInfo(AlterIndexType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_INDEX, data.GetQualifiedName(), data.if_not_found), alter_index_type(type) {
}
AlterIndexInfo::~AlterIndexInfo() {
}

CatalogType AlterIndexInfo::GetCatalogType() const {
	return CatalogType::INDEX_ENTRY;
}

//===--------------------------------------------------------------------===//
// SetIndexOptionsInfo
//===--------------------------------------------------------------------===//
SetIndexOptionsInfo::SetIndexOptionsInfo() : AlterIndexInfo(AlterIndexType::SET_INDEX_OPTIONS) {
}
SetIndexOptionsInfo::SetIndexOptionsInfo(const AlterEntryData &data, case_insensitive_map_t<Value> options_p)
    : AlterIndexInfo(AlterIndexType::SET_INDEX_OPTIONS, data), options(std::move(options_p)) {
}
SetIndexOptionsInfo::~SetIndexOptionsInfo() {
}

unique_ptr<AlterInfo> SetIndexOptionsInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetIndexOptionsInfo>(GetAlterEntryData(), options);
}

string SetIndexOptionsInfo::ToString() const {
	string result = "";
	result += "ALTER INDEX ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " SET (";
	idx_t i = 0;
	for (auto &option : options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(option.first) + "=" + option.second.ToSQLString();
		i++;
	}
	result += ");";
	return result;
}

//===--------------------------------------------------------------------===//
// ResetIndexOptionsInfo
//===--------------------------------------------------------------------===//
ResetIndexOptionsInfo::ResetIndexOptionsInfo() : AlterIndexInfo(AlterIndexType::RESET_INDEX_OPTIONS) {
}
ResetIndexOptionsInfo::ResetIndexOptionsInfo(const AlterEntryData &data, identifier_set_t options_p)
    : AlterIndexInfo(AlterIndexType::RESET_INDEX_OPTIONS, data), options(std::move(options_p)) {
}
ResetIndexOptionsInfo::~ResetIndexOptionsInfo() {
}

unique_ptr<AlterInfo> ResetIndexOptionsInfo::Copy() const {
	identifier_set_t options_copy;
	for (auto &option : options) {
		options_copy.emplace(option);
	}
	auto result = make_uniq<ResetIndexOptionsInfo>(GetAlterEntryData(), options_copy);
	result->values = values;
	return std::move(result);
}

string ResetIndexOptionsInfo::ToString() const {
	string result = "";
	result += "ALTER INDEX ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RESET (";
	idx_t i = 0;
	for (auto &option : options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(option.GetIdentifierName());
		i++;
	}
	result += ");";
	return result;
}

//===--------------------------------------------------------------------===//
// AddConstraintInfo
//===--------------------------------------------------------------------===//
AddConstraintInfo::AddConstraintInfo() : AlterTableInfo(AlterTableType::ADD_CONSTRAINT) {
}

AddConstraintInfo::AddConstraintInfo(const AlterEntryData &data, unique_ptr<Constraint> constraint_p)
    : AlterTableInfo(AlterTableType::ADD_CONSTRAINT, data), constraint(std::move(constraint_p)) {
}

AddConstraintInfo::~AddConstraintInfo() {
}

unique_ptr<AlterInfo> AddConstraintInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddConstraintInfo>(GetAlterEntryData(), constraint->Copy());
}

string AddConstraintInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ADD ";
	result += constraint->ToString();
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// DropConstraintInfo
//===--------------------------------------------------------------------===//
DropConstraintInfo::DropConstraintInfo() : AlterTableInfo(AlterTableType::DROP_CONSTRAINT) {
}

DropConstraintInfo::DropConstraintInfo(AlterEntryData data, string constraint_name_p, bool if_constraint_not_found_p,
                                       bool cascade_p)
    : AlterTableInfo(AlterTableType::DROP_CONSTRAINT, std::move(data)), constraint_name(std::move(constraint_name_p)),
      if_constraint_not_found(if_constraint_not_found_p), cascade(cascade_p) {
}

DropConstraintInfo::~DropConstraintInfo() {
}

unique_ptr<AlterInfo> DropConstraintInfo::Copy() const {
	return make_uniq_base<AlterInfo, DropConstraintInfo>(GetAlterEntryData(), constraint_name, if_constraint_not_found,
	                                                     cascade);
}

string DropConstraintInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " DROP CONSTRAINT ";
	if (if_constraint_not_found) {
		result += "IF EXISTS ";
	}
	result += SQLIdentifier(constraint_name);
	if (cascade) {
		result += " CASCADE";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RenameConstraintInfo
//===--------------------------------------------------------------------===//
RenameConstraintInfo::RenameConstraintInfo() : AlterTableInfo(AlterTableType::RENAME_CONSTRAINT) {
}

RenameConstraintInfo::RenameConstraintInfo(const AlterEntryData &data, string old_name_p, string new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_CONSTRAINT, data), old_name(std::move(old_name_p)),
      new_name(std::move(new_name_p)) {
}

RenameConstraintInfo::~RenameConstraintInfo() {
}

unique_ptr<AlterInfo> RenameConstraintInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameConstraintInfo>(GetAlterEntryData(), old_name, new_name);
}

string RenameConstraintInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME CONSTRAINT ";
	result += SQLIdentifier(old_name);
	result += " TO ";
	result += SQLIdentifier(new_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetTriggerFiringInfo
//===--------------------------------------------------------------------===//
SetTriggerFiringInfo::SetTriggerFiringInfo()
    : AlterTableInfo(AlterTableType::SET_TRIGGER_FIRING), firing(TriggerFiring::ORIGIN) {
}

SetTriggerFiringInfo::SetTriggerFiringInfo(const AlterEntryData &data, Identifier trigger_name_p,
                                           TriggerFiring firing_p)
    : AlterTableInfo(AlterTableType::SET_TRIGGER_FIRING, data), trigger_name(std::move(trigger_name_p)),
      firing(firing_p) {
}

SetTriggerFiringInfo::~SetTriggerFiringInfo() {
}

unique_ptr<AlterInfo> SetTriggerFiringInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetTriggerFiringInfo>(GetAlterEntryData(), trigger_name, firing);
}

string SetTriggerFiringInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	switch (firing) {
	case TriggerFiring::ORIGIN:
		result += " ENABLE TRIGGER ";
		break;
	case TriggerFiring::REPLICA:
		result += " ENABLE REPLICA TRIGGER ";
		break;
	case TriggerFiring::ALWAYS:
		result += " ENABLE ALWAYS TRIGGER ";
		break;
	case TriggerFiring::DISABLED:
		result += " DISABLE TRIGGER ";
		break;
	}
	if (trigger_name.empty()) {
		result += "ALL";
	} else {
		result += SQLIdentifier(trigger_name);
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetPartitionedByInfo
//===--------------------------------------------------------------------===//
SetPartitionedByInfo::SetPartitionedByInfo() : AlterTableInfo(AlterTableType::SET_PARTITIONED_BY) {
}

SetPartitionedByInfo::SetPartitionedByInfo(const AlterEntryData &data,
                                           vector<unique_ptr<ParsedExpression>> partition_keys_p)
    : AlterTableInfo(AlterTableType::SET_PARTITIONED_BY, data), partition_keys(std::move(partition_keys_p)) {
}

SetPartitionedByInfo::~SetPartitionedByInfo() {
}

unique_ptr<AlterInfo> SetPartitionedByInfo::Copy() const {
	vector<unique_ptr<ParsedExpression>> copied_partition_keys;
	for (auto &partition_key : partition_keys) {
		copied_partition_keys.push_back(partition_key->Copy());
	}
	return make_uniq_base<AlterInfo, SetPartitionedByInfo>(GetAlterEntryData(), std::move(copied_partition_keys));
}

string SetPartitionedByInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (partition_keys.empty()) {
		result += " RESET PARTITIONED BY";
	} else {
		result += " SET PARTITIONED BY (";
		for (idx_t i = 0; i < partition_keys.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += partition_keys[i]->ToString();
		}
		result += ")";
	}
	return result;
}

//===--------------------------------------------------------------------===//
// SetSortedByInfo
//===--------------------------------------------------------------------===//
SetSortedByInfo::SetSortedByInfo() : AlterTableInfo(AlterTableType::SET_SORTED_BY) {
}

SetSortedByInfo::SetSortedByInfo(const AlterEntryData &data, vector<OrderByNode> orders_p)
    : AlterTableInfo(AlterTableType::SET_SORTED_BY, data), orders(std::move(orders_p)) {
}

SetSortedByInfo::~SetSortedByInfo() {
}

unique_ptr<AlterInfo> SetSortedByInfo::Copy() const {
	vector<OrderByNode> copied_orders;
	for (auto &order_key : orders) {
		copied_orders.emplace_back(order_key.type, order_key.null_order, order_key.expression->Copy());
	}
	return make_uniq_base<AlterInfo, SetSortedByInfo>(GetAlterEntryData(), std::move(copied_orders));
}

string SetSortedByInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (orders.empty()) {
		result += " RESET SORTED BY";
	} else {
		result += " SET SORTED BY (";
		for (idx_t i = 0; i < orders.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += orders[i].ToString();
		}
		result += ")";
	}
	return result;
}

//===--------------------------------------------------------------------===//
// SetTblPropertiesInfo
//===--------------------------------------------------------------------===//
SetTableOptionsInfo::SetTableOptionsInfo() : AlterTableInfo(AlterTableType::SET_TABLE_OPTIONS) {
}

SetTableOptionsInfo::SetTableOptionsInfo(const AlterEntryData &data,
                                         case_insensitive_map_t<unique_ptr<ParsedExpression>> table_options)
    : AlterTableInfo(AlterTableType::SET_TABLE_OPTIONS, data), table_options(std::move(table_options)) {
}

SetTableOptionsInfo::~SetTableOptionsInfo() {
}

unique_ptr<AlterInfo> SetTableOptionsInfo::Copy() const {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> table_options_copy;
	for (auto &option : table_options) {
		table_options_copy.emplace(option.first, option.second->Copy());
	}
	return make_uniq<SetTableOptionsInfo>(GetAlterEntryData(), std::move(table_options_copy));
}

string SetTableOptionsInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " SET (";
	idx_t i = 0;
	for (auto &entry : table_options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(entry.first) + "=" + entry.second->ToString();
		i++;
	}
	result += ")";
	return result;
}

//===--------------------------------------------------------------------===//
// SetTblPropertiesInfo
//===--------------------------------------------------------------------===//
ResetTableOptionsInfo::ResetTableOptionsInfo() : AlterTableInfo(AlterTableType::RESET_TABLE_OPTIONS) {
}

ResetTableOptionsInfo::ResetTableOptionsInfo(const AlterEntryData &data, identifier_set_t table_options)
    : AlterTableInfo(AlterTableType::RESET_TABLE_OPTIONS, data), table_options(std::move(table_options)) {
}

ResetTableOptionsInfo::~ResetTableOptionsInfo() {
}

unique_ptr<AlterInfo> ResetTableOptionsInfo::Copy() const {
	identifier_set_t table_options_copy;
	for (auto &option : table_options) {
		table_options_copy.emplace(option);
	}
	auto result = make_uniq<ResetTableOptionsInfo>(GetAlterEntryData(), table_options_copy);
	result->values = values;
	return std::move(result);
}

string ResetTableOptionsInfo::ToString() const {
	string result = "ALTER TABLE ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RESET (";
	idx_t i = 0;
	for (auto &entry : table_options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(entry.GetIdentifierName());
		i++;
	}
	result += ")";
	return result;
}

} // namespace duckdb
