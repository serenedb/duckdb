#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
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
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/alter_job_info.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
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

static bool IsRelation(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::INDEX_ENTRY:
		return true;
	default:
		return false;
	}
}

static RoleAccess AccessOf(ClientContext &context, idx_t role) {
	if (auto access = context.security ? context.security->Access(role) : nullptr) {
		return *access;
	}
	RoleAccess missing;
	missing.role = role;
	return missing;
}

static string RoleName(ClientContext &context, idx_t role) {
	string result;
	auto &cluster_catalog = DBConfig::GetConfig(context).cluster_catalog;
	if (cluster_catalog.empty()) {
		return result;
	}
	auto &cluster = Catalog::GetCatalog(context, cluster_catalog).Cast<DuckCatalog>();
	cluster.GetCatalogSet(CatalogType::ROLE_ENTRY)
	    .Scan(cluster.GetCatalogTransaction(context), [&](CatalogEntry &entry) {
		    if (entry.oid == role) {
			    result = entry.name.GetIdentifierName();
		    }
	    });
	return result;
}

static idx_t GranteeId(Binder &binder, const string &name) {
	return name == "PUBLIC" ? ACL_ID_PUBLIC : binder.RoleId(Identifier(name));
}

static idx_t RoleSpecId(Binder &binder, const string &name) {
	if (StringUtil::CIEquals(name, "CURRENT_USER") || StringUtil::CIEquals(name, "CURRENT_ROLE")) {
		return binder.context.principal_role;
	}
	if (StringUtil::CIEquals(name, "SESSION_USER")) {
		return binder.context.session_role;
	}
	return binder.RoleId(Identifier(name));
}

static void RequireGrantable(Binder &binder, CatalogEntry &entry) {
	auto principal = binder.principal;
	auto &permissions = entry.permissions;
	if (!principal || principal->Owns(permissions.owner)) {
		return;
	}
	vector<AclItem> defaults;
	std::span<const AclItem> stored = permissions.acl;
	if (stored.empty()) {
		defaults = Permissions::AclDefault(Permissions::AclClass(entry.type), permissions.owner);
		stored = defaults;
	}
	auto held = principal->HeldModes(stored) | principal->GrantableModes(stored);
	if (entry.type == CatalogType::TABLE_ENTRY) {
		for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
			held |= principal->HeldModes(column.Acl()) | principal->GrantableModes(column.Acl());
		}
	}
	if (held == AclMode::NoRights) {
		ThrowPermissionDenied(entry);
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
	auto entry = binder.GetCatalogEntry(EntryLookupInfo(info.entry_catalog_type, name), OnEntryNotFound::RETURN_NULL);
	if (!entry && info.entry_catalog_type == CatalogType::MACRO_ENTRY) {
		entry =
		    binder.GetCatalogEntry(EntryLookupInfo(CatalogType::TABLE_MACRO_ENTRY, name), OnEntryNotFound::RETURN_NULL);
	}
	if (!entry) {
		const bool relation = info.entry_catalog_type == CatalogType::TABLE_ENTRY;
		throw CatalogException(unordered_map<string, string> {{"type", CatalogTypeToString(info.entry_catalog_type)}},
		                       StringUtil::Format("%s \"%s\" does not exist",
		                                          relation ? "relation" : PermissionKindName(info.entry_catalog_type),
		                                          name.ToString()));
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

static void ResolveGrant(Binder &binder, AlterPermissionsInfo &info) {
	auto principal = binder.principal;
	info.grantee_id = GranteeId(binder, info.grantee);
	if (!info.granted_by.empty()) {
		auto granted_by = binder.RoleId(Identifier(info.granted_by));
		if (principal && !principal->MemberOf(granted_by)) {
			throw PermissionException("must be member of role \"%s\"", info.granted_by);
		}
		info.grantors = {granted_by};
	} else if (principal && !principal->is_superuser) {
		info.grantors.push_back(principal->role);
		for (auto role : principal->closure) {
			if (role != principal->role) {
				info.grantors.push_back(role);
			}
		}
	}
	if (info.entry_catalog_type == CatalogType::DATABASE_ENTRY) {
		auto &name = info.GetQualifiedName().Name();
		auto database = binder.ClusterEntry(CatalogType::DATABASE_ENTRY, name);
		if (!database) {
			throw CatalogException(unordered_map<string, string> {{"type", "Database"}},
			                       StringUtil::Format("database \"%s\" does not exist", name.GetIdentifierName()));
		}
		RequireGrantable(binder, *database);
		return;
	}
	if (info.entry_catalog_type == CatalogType::FOREIGN_SERVER_ENTRY) {
		auto &name = info.GetQualifiedName().Name();
		auto &catalog = Catalog::GetCatalog(binder.context, DatabaseManager::GetDefaultDatabase(binder.context))
		                    .Cast<DuckCatalog>();
		auto server = catalog.GetCatalogSet(CatalogType::FOREIGN_SERVER_ENTRY)
		                  .GetEntry(catalog.GetCatalogTransaction(binder.context), name);
		if (!server) {
			throw CatalogException(unordered_map<string, string> {{"type", "Foreign Server"}},
			                       StringUtil::Format("server \"%s\" does not exist", name.GetIdentifierName()));
		}
		info.SetQualifiedName(server->ParentCatalog().GetName(), Identifier(), server->name);
		RequireGrantable(binder, *server);
		return;
	}
	if (info.all_in_schema) {
		return;
	}
	RequireGrantable(binder, ResolvePermissionsTarget(binder, info));
}

static void ResolveOwner(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &entry = ResolvePermissionsTarget(binder, info);
	info.new_owner_id = RoleSpecId(binder, info.new_owner);
	auto principal = binder.principal;
	const bool enforce = principal && !principal->is_superuser;
	const bool relation = IsRelation(entry.type);
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
	if (!relation) {
		binder.RequireOwner(entry);
	}
	if (!principal->CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", RoleName(context, info.new_owner_id));
	}
	if (entry.type != CatalogType::SCHEMA_ENTRY) {
		auto &schema = entry.ParentSchema(context);
		if (!AccessOf(context, info.new_owner_id).Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Create)) {
			ThrowPermissionDenied(schema);
		}
	}
}

static void ResolveDefaultPrivileges(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto principal = binder.principal;
	info.target_role = info.for_role.empty() ? context.principal_role : binder.RoleId(Identifier(info.for_role));
	if (principal && !principal->MemberOf(info.target_role)) {
		throw PermissionException("permission denied to change default privileges");
	}
	info.grantee_id = GranteeId(binder, info.grantee);
	auto database = DatabaseManager::GetDefaultDatabase(context);
	if (!info.default_schema.empty()) {
		auto &schema = Catalog::GetSchema(context, Identifier(), Identifier(info.default_schema));
		if (principal && !principal->is_superuser &&
		    !AccessOf(context, info.target_role).Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Create)) {
			ThrowPermissionDenied(schema);
		}
		info.default_scope = schema.oid;
		database = schema.ParentCatalog().GetName();
	}
	if (!binder.ClusterEntry(CatalogType::DATABASE_ENTRY, database)) {
		throw CatalogException(unordered_map<string, string> {{"type", "Database"}},
		                       StringUtil::Format("database \"%s\" does not exist", database.GetIdentifierName()));
	}
	info.SetQualifiedName(Identifier(), Identifier(), database);
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
		auto schema =
		    Catalog::GetSchema(context, info.SchemaCatalog(), info.SchemaPath(), OnEntryNotFound::RETURN_NULL);
		if (schema) {
			RequireOwner(*schema);
			RequireDatabasePrivilege(info.SchemaCatalog(), AclMode::Create);
		}
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_PERMISSIONS || stmt.info->type == AlterType::ALTER_ROLE) {
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		const auto catalog_type = stmt.info->GetCatalogType();
		if (catalog_type != CatalogType::DATABASE_ENTRY && catalog_type != CatalogType::ROLE_ENTRY) {
			stmt.info->SetQualifiedName(BindTableName(stmt.info->GetQualifiedName()));
			auto &catalog = Catalog::GetCatalog(context, stmt.info->GetQualifiedName().Catalog());
			properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
		}
		if (stmt.info->type == AlterType::ALTER_PERMISSIONS) {
			ResolvePermissions(*this, stmt.info->Cast<AlterPermissionsInfo>());
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
