#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/constraints/check_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/alter_job_info.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
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

static void BindAlterTypes(Binder &binder, AlterStatement &stmt, optional_ptr<Catalog> catalog) {
	auto require_usage = [&](const LogicalType &type) {
		if (catalog) {
			RequireTypeUsage(binder.context, binder.CheckAs(), type, *catalog);
		}
	};
	if (stmt.info->type == AlterType::ALTER_TABLE) {
		auto &table_info = stmt.info->Cast<AlterTableInfo>();
		switch (table_info.alter_table_type) {
		case AlterTableType::ADD_COLUMN: {
			auto &add_info = table_info.Cast<AddColumnInfo>();
			binder.BindLogicalType(add_info.new_column.TypeMutable());
			require_usage(add_info.new_column.Type());
		} break;
		case AlterTableType::ADD_FIELD: {
			auto &add_info = table_info.Cast<AddFieldInfo>();
			binder.BindLogicalType(add_info.new_field.TypeMutable());
			require_usage(add_info.new_field.Type());
		} break;
		case AlterTableType::ALTER_COLUMN_TYPE: {
			auto &alter_column_info = table_info.Cast<ChangeColumnTypeInfo>();
			binder.BindLogicalType(alter_column_info.target_type);
			require_usage(alter_column_info.target_type);
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

static void QualifyAlterUserFunctions(Binder &binder, const SchemaCatalogEntry &schema, AlterStatement &stmt) {
	if (stmt.info->type != AlterType::ALTER_TABLE) {
		return;
	}
	auto &table_info = stmt.info->Cast<AlterTableInfo>();
	switch (table_info.alter_table_type) {
	case AlterTableType::ADD_COLUMN:
		binder.QualifyUserFunctions(schema, table_info.Cast<AddColumnInfo>().new_column);
		break;
	case AlterTableType::ADD_FIELD:
		binder.QualifyUserFunctions(schema, table_info.Cast<AddFieldInfo>().new_field);
		break;
	case AlterTableType::SET_DEFAULT: {
		auto &expression = table_info.Cast<SetDefaultInfo>().expression;
		if (expression) {
			binder.QualifyUserFunctions(schema, *expression);
		}
		break;
	}
	case AlterTableType::ADD_CONSTRAINT: {
		auto &constraint = *table_info.Cast<AddConstraintInfo>().constraint;
		if (constraint.type == ConstraintType::CHECK) {
			binder.QualifyUserFunctions(schema, *constraint.Cast<CheckConstraint>().expression);
		}
		break;
	}
	case AlterTableType::ALTER_COLUMN_TYPE:
		binder.QualifyUserFunctions(schema, *table_info.Cast<ChangeColumnTypeInfo>().expression);
		break;
	default:
		break;
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

optional_ptr<CatalogEntry> Binder::LookupRoutine(const QualifiedName &name, OnEntryNotFound if_not_found) {
	for (auto type : {CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY}) {
		if (auto entry = entry_retriever.GetEntry(EntryLookupInfo(type, name), OnEntryNotFound::RETURN_NULL)) {
			return entry;
		}
	}
	return entry_retriever.GetEntry(EntryLookupInfo(CatalogType::MACRO_ENTRY, name), if_not_found);
}

MacroFunction &Binder::ResolveOverload(MacroCatalogEntry &macro, bool has_func_args,
                                       vector<LogicalType> &func_parameters) {
	return *ResolveOverload(macro, has_func_args, func_parameters, OnEntryNotFound::THROW_EXCEPTION);
}

optional_ptr<MacroFunction> Binder::ResolveOverload(MacroCatalogEntry &macro, bool has_func_args,
                                                    vector<LogicalType> &func_parameters,
                                                    OnEntryNotFound if_not_found) {
	auto &name = macro.name.GetIdentifierName();
	if (has_func_args) {
		for (auto &type : func_parameters) {
			BindLogicalType(type);
		}
		for (auto &overload : macro.macros) {
			if (overload->HasParameterTypes(func_parameters)) {
				return overload.get();
			}
		}
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return nullptr;
		}
		auto extra_info = Exception::InitializeExtraInfo("MISSING_ENTRY", optional_idx());
		extra_info["type"] = CatalogTypeToString(macro.type);
		throw CatalogException(extra_info, StringUtil::Format("function %s(%s) does not exist", name,
		                                                      MacroFunction::ParameterTypesToString(func_parameters)));
	}
	if (macro.macros.size() > 1) {
		auto extra_info = Exception::InitializeExtraInfo("AMBIGUOUS_FUNCTION", optional_idx());
		extra_info["hint"] = "Specify the argument list to select the function unambiguously.";
		throw CatalogException(extra_info, StringUtil::Format("function name \"%s\" is not unique", name));
	}
	return macro.macros[0].get();
}

static idx_t RequireRoutineOwner(Binder &binder, CatalogEntry &entry, bool has_func_args,
                                 vector<LogicalType> &func_parameters) {
	const bool names_overload =
	    !entry.internal && (entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY) &&
	    (has_func_args || entry.ParentCatalog().Compatibility() == SqlCompatibility::POSTGRES);
	if (!names_overload) {
		RequireOwner(binder.context, binder.CheckAs(), entry);
		return 0;
	}
	auto &macro = entry.Cast<MacroCatalogEntry>();
	auto &overload = binder.ResolveOverload(macro, has_func_args, func_parameters);
	RequireOwner(binder.CheckAs(), entry, macro.OverloadPermissions(overload).owner);
	return macro.OverloadOid(overload);
}

CatalogEntry &Binder::ResolveDatabaseEntry(CatalogType type, AlterInfo &info) {
	auto &name = info.GetQualifiedName().Name();
	auto &catalog = type == CatalogType::SUBSCRIPTION_ENTRY
	                    ? SubscriptionCatalog(Identifier())
	                    : Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
	auto entry = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context), type, name);
	if (!entry) {
		const bool server = type == CatalogType::FOREIGN_SERVER_ENTRY;
		throw CatalogException(unordered_map<string, string> {{"type", server ? "Foreign Server" : "Subscription"}},
		                       StringUtil::Format("%s \"%s\" does not exist", server ? "server" : "subscription",
		                                          name.GetIdentifierName()));
	}
	info.SetQualifiedName(entry->ParentCatalog().GetName(), Identifier(), entry->name);
	return *entry;
}

void Binder::RequireSubscriptionOwner(CatalogEntry &subscription) {
	RequireOwner(context, CheckAs(), subscription);
	if (!CheckAs().is_superuser && !subscription.GetInfo()->Cast<CreateSubscriptionInfo>().password_required) {
		throw PermissionException(unordered_map<string, string> {{"hint", "Subscriptions with the password_required "
		                                                                  "option set to false may only be created or "
		                                                                  "modified by the superuser."}},
		                          "password_required=false is superuser-only");
	}
}

BoundStatement Binder::Bind(AlterStatement &stmt) {
	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	// Special handling for ALTER DATABASE - doesn't use schema binding
	if (stmt.info->type == AlterType::ALTER_DATABASE) {
		if (!Catalog::GetSystemCatalog(context).IsDuckCatalog()) {
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
			RequireOwner(context, CheckAs(), *schema);
			RequireDatabasePrivilege(context, CheckAs(), info.SchemaCatalog(), AclMode::Create);
		}
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::SET_COMMENT && (stmt.info->GetCatalogType() == CatalogType::DATABASE_ENTRY ||
	                                                  stmt.info->GetCatalogType() == CatalogType::ROLE_ENTRY)) {
		if (Catalog::GetSystemCatalog(context).IsDuckCatalog()) {
			throw NotImplementedException("Adding comments to %ss is not implemented",
			                              stmt.info->GetCatalogType() == CatalogType::DATABASE_ENTRY ? "database"
			                                                                                         : "role");
		}
		CheckSharedObjectComment(*stmt.info);
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		stmt.info->SetQualifiedName(ClusterName(context, stmt.info->GetQualifiedName()));
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_SUBSCRIPTION) {
		auto &subscription = ResolveDatabaseEntry(CatalogType::SUBSCRIPTION_ENTRY, *stmt.info);
		RequireSubscriptionOwner(subscription);
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(subscription.ParentCatalog(), context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::REASSIGN_OWNED || stmt.info->type == AlterType::DROP_OWNED) {
		ResolveOwned(*stmt.info);
		auto &catalog = Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
		stmt.info->SetQualifiedName(catalog.GetName(), Identifier(), Identifier());
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(
		    catalog, context, DatabaseModificationType::ALTER_TABLE | DatabaseModificationType::DROP_CATALOG_ENTRY);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_TABLESPACE) {
		GetStatementProperties().return_type = StatementReturnType::NOTHING;
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_PERMISSIONS || stmt.info->type == AlterType::ALTER_ROLE ||
	    stmt.info->type == AlterType::ALTER_DATABASE_SETTINGS) {
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		BindPrivilegeAlter(*stmt.info, properties);
		if (stmt.info->type == AlterType::ALTER_PERMISSIONS) {
			for (auto &grant : stmt.info->Cast<AlterPermissionsInfo>().additional_grants) {
				BindPrivilegeAlter(*grant, properties);
			}
		} else if (stmt.info->type == AlterType::ALTER_ROLE) {
			for (auto &grant : stmt.info->Cast<AlterRoleInfo>().additional_grants) {
				BindPrivilegeAlter(*grant, properties);
			}
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
		for (auto &type : info.func_parameters) {
			BindLogicalType(type);
		}
		const auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		auto function = LookupRoutine(info.GetQualifiedName(), OnEntryNotFound::RETURN_NULL);
		if (!function) {
			auto missing = Exception::InitializeExtraInfo("MISSING_ENTRY", optional_idx());
			missing["type"] = CatalogTypeToString(CatalogType::MACRO_ENTRY);
			throw CatalogException(missing,
			                       info.has_func_args
			                           ? StringUtil::Format("function %s(%s) does not exist", name,
			                                                MacroFunction::ParameterTypesToString(info.func_parameters))
			                           : StringUtil::Format("could not find a function named \"%s\"", name));
		}
		if (function->internal) {
			ThrowPermissionDenied(*function);
		}
		auto &routine = function->Cast<MacroCatalogEntry>();
		auto &overload = ResolveOverload(routine, info.has_func_args, info.func_parameters);
		RequireOwner(CheckAs(), *function, routine.OverloadPermissions(overload).owner);
		auto create_info = routine.GetInfo();
		const auto position =
		    std::find_if(routine.macros.begin(), routine.macros.end(),
		                 [&](const unique_ptr<MacroFunction> &candidate) { return candidate.get() == &overload; }) -
		    routine.macros.begin();
		auto &macro = *create_info->Cast<CreateMacroInfo>().macros[position];
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
		const bool unqualified = stmt.info->GetQualifiedName().Schema().empty();
		auto function = LookupRoutine(stmt.info->GetQualifiedName(),
		                              unqualified ? stmt.info->if_not_found : OnEntryNotFound::RETURN_NULL);
		if (function && unqualified) {
			stmt.info->SetQualifiedName(function->Cast<StandardEntry>().GetQualifiedName(function->name));
		}
		if (function) {
			auto &rename = stmt.info->Cast<RenameScalarFunctionInfo>();
			rename.overload_oid = RequireRoutineOwner(*this, *function, rename.has_func_args, rename.func_parameters);
			RequireSchemaCreate(context, CheckAs(), function->ParentSchema(context));
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
	} else if (stmt.info->type == AlterType::SET_COMMENT && stmt.info->GetCatalogType() == CatalogType::MACRO_ENTRY) {
		entry = LookupRoutine(stmt.info->GetQualifiedName(), stmt.info->if_not_found);
		if (entry && entry->type == CatalogType::TABLE_MACRO_ENTRY) {
			stmt.info->Cast<SetCommentInfo>().entry_catalog_type = CatalogType::TABLE_MACRO_ENTRY;
		}
	} else {
		// For any other ALTER, we retrieve the catalog entry directly.
		entry = lookup(stmt.info->GetCatalogType(), stmt.info->if_not_found);
	}

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	if (!entry) {
		// Bind types in this binder
		BindAlterTypes(*this, stmt, nullptr);
		RequireResolvedTypeUsage(nullptr);

		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	D_ASSERT(!entry->deleted);
	if (stmt.info->type == AlterType::SET_COMMENT) {
		auto &comment = stmt.info->Cast<SetCommentInfo>();
		comment.overload_oid = RequireRoutineOwner(*this, *entry, comment.has_func_args, comment.func_parameters);
	} else if (stmt.info->type == AlterType::SET_SCHEMA) {
		vector<LogicalType> no_arguments;
		RequireRoutineOwner(*this, *entry, false, no_arguments);
	} else {
		RequireOwner(context, CheckAs(), *entry);
	}
	if (stmt.info->GetNewName()) {
		RequireSchemaCreate(context, CheckAs(), entry->ParentSchema(context));
	}
	if (auto new_schema = stmt.info->GetNewSchema()) {
		EntryLookupInfo schema_lookup(CatalogType::SCHEMA_ENTRY,
		                              QualifiedName(entry->ParentCatalog().GetName(), Identifier(), *new_schema));
		if (auto target = Catalog::GetSchema(entry_retriever, schema_lookup, OnEntryNotFound::RETURN_NULL)) {
			RequireUserSchema(*target, entry->name);
			RequireSchemaCreate(context, CheckAs(), *target);
		}
	}
	auto &catalog = entry->ParentCatalog();

	// Bind types in the same catalog as the entry
	auto type_binder = Binder::CreateBinder(context, *this);
	type_binder->SetSearchPath(catalog, stmt.info->GetQualifiedName().Schema());

	BindAlterTypes(*type_binder, stmt, &catalog);
	RequireResolvedTypeUsage(nullptr);
	if (catalog.Compatibility() == SqlCompatibility::POSTGRES) {
		auto &schema = entry->ParentSchema(context);
		QualifyAlterUserFunctions(*this, schema, stmt);
		if (stmt.info->type == AlterType::SET_SCHEMA && entry->type == CatalogType::TABLE_ENTRY) {
			VerifyMovedTableExpressions(context, entry->Cast<TableCatalogEntry>(),
			                            catalog.GetSchema(context, stmt.info->Cast<SetSchemaInfo>().new_schema));
		}
	}

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
