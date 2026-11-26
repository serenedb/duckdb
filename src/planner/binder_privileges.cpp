#include "duckdb/planner/binder.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/column_acls.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/catalog/privileges.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/table_binding.hpp"

#include <absl/algorithm/container.h>

namespace duckdb {

struct StatementRule {
	StatementType statement;
	CatalogType object = CatalogType::INVALID;
	RoleOption option = RoleOption::None;
	RoleCapability capability = RoleCapability::None;
	const char *message = "permission denied: superuser required";
	bool named = false;
	const char *detail = nullptr;
	const char *hint = nullptr;
};

static constexpr StatementRule STATEMENT_RULES[] = {
    {.statement = StatementType::EXPORT_STATEMENT},
    {.statement = StatementType::LOAD_STATEMENT},
    {.statement = StatementType::UPDATE_EXTENSIONS_STATEMENT},
    {.statement = StatementType::COPY_DATABASE_STATEMENT},
    {.statement = StatementType::CONNECT_STATEMENT},
    {.statement = StatementType::EXTERNAL_RESOURCE_STATEMENT},
    {.statement = StatementType::DETACH_STATEMENT},
    {.statement = StatementType::ATTACH_STATEMENT},
    {.statement = StatementType::CREATE_STATEMENT, .object = CatalogType::SECRET_ENTRY},
    {.statement = StatementType::DROP_STATEMENT, .object = CatalogType::SECRET_ENTRY},
    {.statement = StatementType::CREATE_STATEMENT,
     .object = CatalogType::TABLESPACE_ENTRY,
     .message = "permission denied to create tablespace \"%s\"",
     .named = true,
     .hint = "Must be superuser to create a tablespace."},
    {.statement = StatementType::CREATE_STATEMENT,
     .object = CatalogType::DATABASE_ENTRY,
     .option = RoleOption::CreateDb,
     .message = "permission denied to create database"},
    {.statement = StatementType::CREATE_STATEMENT,
     .object = CatalogType::SUBSCRIPTION_ENTRY,
     .capability = RoleCapability::CreateSubscription,
     .message = "permission denied to create subscription",
     .detail = "Only roles with privileges of the \"pg_create_subscription\" role may create subscriptions."},
    {.statement = StatementType::CREATE_STATEMENT,
     .object = CatalogType::ROLE_ENTRY,
     .option = RoleOption::CreateRole,
     .message = "permission denied to create role",
     .detail = "Only roles with the CREATEROLE attribute may create roles."},
    {.statement = StatementType::DROP_STATEMENT,
     .object = CatalogType::ROLE_ENTRY,
     .option = RoleOption::CreateRole,
     .message = "permission denied to drop role",
     .detail = "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles."},
};

static const CreateInfo *CreateInfoOf(const SQLStatement &statement) {
	return statement.type == StatementType::CREATE_STATEMENT ? statement.Cast<CreateStatement>().info.get() : nullptr;
}

static CatalogType StatementObject(const SQLStatement &statement) {
	if (auto info = CreateInfoOf(statement)) {
		return info->type;
	}
	return statement.type == StatementType::DROP_STATEMENT ? statement.Cast<DropStatement>().info->type
	                                                       : CatalogType::INVALID;
}

static const StatementRule *StatementRuleOf(const SQLStatement &statement) {
	const auto object = StatementObject(statement);
	const auto rule = absl::c_find_if(STATEMENT_RULES, [&](const StatementRule &rule) {
		return rule.statement == statement.type && rule.object == object;
	});
	return rule == std::end(STATEMENT_RULES) ? nullptr : rule;
}

bool Binder::MayRun(const RoleAccess &role, const SQLStatement &statement) {
	const auto rule = StatementRuleOf(statement);
	return !rule || role.is_superuser || (rule->option != RoleOption::None && role.Has(rule->option)) ||
	       (rule->capability != RoleCapability::None && role.Has(rule->capability));
}

void Binder::RequireStatement(const SQLStatement &statement) {
	if (MayRun(CheckAs(), statement)) {
		return;
	}
	const auto rule = StatementRuleOf(statement);
	const auto message =
	    rule->named
	        ? StringUtil::Format(rule->message, CreateInfoOf(statement)->GetQualifiedName().Name().GetIdentifierName())
	        : string(rule->message);
	if (rule->detail) {
		throw PermissionException::WithDetail(rule->detail, message);
	}
	if (rule->hint) {
		throw PermissionException::WithHint(rule->hint, message);
	}
	throw PermissionException(message);
}

const RoleAccess &Binder::CheckAs() const {
	return acting.privileges ? *acting.privileges : context.CurrentRole();
}

const RoleAccess &Binder::CurrentRole() const {
	return acting.current_user ? *acting.current_user : context.CurrentRole();
}

optional_idx Binder::CallerRole() const {
	auto &caller = CurrentRole();
	return &caller != &context.CurrentRole() && !caller.IsSystem() ? optional_idx(caller.role) : optional_idx();
}

const RoleAccess &Binder::AccessOf(idx_t role) {
	auto entry = global_binder_state->role_access.try_emplace(role);
	if (entry.second) {
		entry.first->second = context.AccessOf(role);
	}
	return entry.first->second;
}

const RoleAccess &Binder::FunctionBodyRole(optional_idx owner) {
	return owner.IsValid() ? AccessOf(owner.GetIndex()) : CurrentRole();
}

void Binder::BindAsFunctionBody(const RoleAccess &role) {
	acting.lookups = &role;
	acting.privileges = &role;
	acting.current_user = &role;
}

void Binder::BindUnchecked() {
	acting.privileges = &RoleAccess::System();
	acting.current_user = &RoleAccess::System();
}

void Binder::BindStoredDefinition() {
	acting.lookups = &RoleAccess::System();
}

void Binder::BindSharedDefinition() {
	BindUnchecked();
	BindStoredDefinition();
}

void Binder::RecordCalls(optional_ptr<RecordedCalls> calls) {
	global_binder_state->recorded_calls = calls;
}

void Binder::Execute(const CatalogEntry &function, optional_ptr<const MacroFunction> overload) {
	if (!Allowed(CurrentRole(), function, AclMode::Execute, overload)) {
		ThrowPermissionDenied(function, overload);
	}
	auto calls = global_binder_state->recorded_calls;
	if (!calls || !overload || PermissionsOf(function, overload).owner == 0) {
		return;
	}
	if (&function.ParentCatalog() != &calls->catalog) {
		throw NotImplementedException("cross-database references are not implemented: \"%s.%s.%s\"",
		                              function.ParentCatalog().GetName().GetIdentifierName(),
		                              function.ParentSchemaName().GetIdentifierName(),
		                              function.name.GetIdentifierName());
	}
	calls->overloads.push_back(function.Cast<MacroCatalogEntry>().OverloadOid(*overload));
}

void Binder::RequireIndexCalls(TableCatalogEntry &table, optional_ptr<const vector<PhysicalIndex>> changed) {
	auto &role = CurrentRole();
	auto &state = *global_binder_state;
	if (role.is_superuser || state.denied_relation || state.denied_function) {
		return;
	}
	auto &columns = table.GetColumns();
	const auto changes = [&](column_t column) {
		return absl::c_any_of(
		    *changed, [&](PhysicalIndex index) { return columns.GetColumn(index).Logical() == LogicalIndex(column); });
	};
	auto &catalog = table.ParentCatalog();
	const auto transaction = catalog.GetCatalogTransaction(context);
	catalog.ScanIndexes(context, table, [&](IndexCatalogEntry &index) {
		if (state.denied_function || (changed && absl::c_none_of(index.column_ids, changes))) {
			return;
		}
		for (const auto oid : index.CalledOverloads()) {
			auto function = catalog.LookupOwner(transaction, oid);
			auto overload = function ? function->Cast<MacroCatalogEntry>().FindOverload(oid) : nullptr;
			if (!overload) {
				throw CatalogException("function with OID %llu does not exist", oid);
			}
			if (!Allowed(role, *function, AclMode::Execute, overload.get())) {
				state.denied_function = function.get();
				state.denied_overload = overload.get();
				return;
			}
		}
	});
}

void Binder::RequirePlacement(Catalog &catalog, const CreateInfo &info, optional_ptr<const CatalogEntry> indexed) {
	if (auto tablespace = catalog.PlacementTablespace(context, info, indexed)) {
		Require(context, CheckAs(), *tablespace, AclMode::Create);
	}
}

bool Binder::ActsOnVisibleTarget(CatalogType type, const QualifiedName &name, OnEntryNotFound if_not_found) {
	auto &role = CheckAs();
	if (role.is_superuser) {
		return true;
	}
	auto internal = CatalogEntryRetriever::Internal(context);
	const auto relation = type == CatalogType::TABLE_ENTRY || type == CatalogType::VIEW_ENTRY ||
	                      type == CatalogType::SEQUENCE_ENTRY || type == CatalogType::INDEX_ENTRY;
	const auto routine = type == CatalogType::MACRO_ENTRY || type == CatalogType::TABLE_MACRO_ENTRY;
	auto &path = name.Path();
	optional_ptr<CatalogEntry> entry;
	switch (type) {
	case CatalogType::SCHEMA_ENTRY:
		if (path.size() < 2) {
			return true;
		}
		entry = internal
		            .GetSchema(path.front(), vector<Identifier>(path.begin() + 1, path.end()), AclMode::NoRights,
		                       OnEntryNotFound::RETURN_NULL)
		            .get();
		break;
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::JOB_ENTRY:
	case CatalogType::TOKENIZER_ENTRY:
		for (auto kind : {type, CatalogType::TABLE_ENTRY, CatalogType::SEQUENCE_ENTRY, CatalogType::INDEX_ENTRY,
		                  CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY}) {
			const auto same_namespace =
			    kind == type ||
			    (relation && (kind == CatalogType::TABLE_ENTRY || kind == CatalogType::SEQUENCE_ENTRY ||
			                  kind == CatalogType::INDEX_ENTRY)) ||
			    (routine && (kind == CatalogType::MACRO_ENTRY || kind == CatalogType::TABLE_MACRO_ENTRY));
			if (!entry && same_namespace) {
				entry = internal.GetEntry(EntryLookupInfo(kind, name), OnEntryNotFound::RETURN_NULL);
			}
		}
		break;
	default:
		if (!IsCatalogLevel(type) ||
		    entry_retriever.GetEntry(EntryLookupInfo(type, name), OnEntryNotFound::RETURN_NULL) ||
		    !internal.GetEntry(EntryLookupInfo(type, name), OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return false;
		}
		throw CatalogException::MissingObject(type, name.Name());
	}
	if (!entry || EntryVisible(context, role, *entry)) {
		return true;
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return false;
	}
	switch (type) {
	case CatalogType::SCHEMA_ENTRY:
		entry_retriever.GetSchema(path.front(), vector<Identifier>(path.begin() + 1, path.end()), AclMode::NoRights,
		                          OnEntryNotFound::THROW_EXCEPTION);
		break;
	default:
		entry_retriever.GetEntry(EntryLookupInfo(type, name), OnEntryNotFound::THROW_EXCEPTION);
		break;
	}
	throw CatalogException::MissingEntry(EntryLookupInfo(type, name), string());
}

void Binder::BindAsFunctionBody(const SelectStatement &statement) {
	if (!statement.function_body) {
		return;
	}
	if (!CheckAs().IsSystem()) {
		BindAsFunctionBody(FunctionBodyRole(statement.function_owner));
	}
	acting.security_definer |= statement.function_owner.IsValid();
	if (statement.function_config) {
		UseFunctionSearchPath(*statement.function_config);
	}
}

BinderScope::BinderScope(Binder &binder_p) : binder(binder_p), acting(binder.acting) {
}

BinderScope::~BinderScope() {
	if (caller_path) {
		binder.EntryRetriever().SwapSearchPath(*caller_path);
	}
	binder.acting = acting;
}

void BinderScope::UseFunctionSearchPath(const vector<string> &config) {
	caller_path = make_uniq<CatalogEntryRetriever>(binder.context);
	binder.EntryRetriever().SwapSearchPath(*caller_path);
	binder.UseFunctionSearchPath(config);
}

void Binder::UseFunctionSearchPath(const vector<string> &config) {
	for (auto &entry : config) {
		auto paths = CatalogSearchEntry::ParseList(string(Settings::ConfigValue(entry)));
		const auto database = DatabaseManager::TryGetDefaultDatabase(context);
		for (auto &path : paths) {
			if (path.GetCatalog().empty()) {
				path.SetCatalog(database);
			}
		}
		entry_retriever.ReplaceSearchPath(std::move(paths));
	}
}

BoundStatement Binder::BindTarget(TableRef &ref, TableRefUse use) {
	return ref.type == TableReferenceType::BASE_TABLE ? Bind(ref.Cast<BaseTableRef>(), use) : Bind(ref);
}

RelationChecks Binder::ChecksOf(const StandardEntry &relation) {
	auto &role = CheckAs();
	if (role.is_superuser) {
		return {};
	}
	return {&role, global_binder_state->relation_references++};
}

RelationChecks Binder::ReadChecks(const StandardEntry &relation, RelationChecks checks) {
	if (!checks.role || checks.role->Can(CatalogType::TABLE_ENTRY, PermissionsOf(relation), AclMode::Select)) {
		return {};
	}
	return checks;
}

RelationChecks Binder::ReadChecks(const StandardEntry &relation, TableRefUse use) {
	if (use == TableRefUse::UTILITY_TARGET) {
		return {};
	}
	auto checks = ReadChecks(relation, ChecksOf(relation));
	if (checks.role && use == TableRefUse::READ && !checks.role->CanColumns(relation, AclMode::Select, {})) {
		Deny(relation, checks);
		return {};
	}
	return checks;
}

void Binder::Deny(const StandardEntry &relation, const RelationChecks &checks) {
	auto &state = *global_binder_state;
	if (!state.denied_relation || checks.reference < state.denied_reference) {
		state.denied_relation = &relation;
		state.denied_reference = checks.reference;
	}
}

void Binder::RequireColumns(const TableCatalogEntry &table, const RelationChecks &checks, AclMode use,
                            std::span<const LogicalIndex> columns) {
	if (!checks.role || checks.role->Can(CatalogType::TABLE_ENTRY, PermissionsOf(table), use)) {
		return;
	}
	const bool granted =
	    columns.empty() ? checks.role->CanColumns(table, use, {}) : absl::c_all_of(columns, [&](LogicalIndex column) {
		    return checks.role->Can(CatalogType::TABLE_ENTRY, table.GetColumn(column).Acl(), use);
	    });
	if (!granted) {
		Deny(table, checks);
	}
}

void Binder::RequireColumns(const TableCatalogEntry &table, const RelationChecks &checks, AclMode use,
                            std::span<const Identifier> columns) {
	if (!checks.role || checks.role->Can(CatalogType::TABLE_ENTRY, PermissionsOf(table), use)) {
		return;
	}
	const bool granted = columns.empty()
	                         ? checks.role->CanColumns(table, use, {})
	                         : absl::c_all_of(columns, [&](const Identifier &name) {
		                           return !table.ColumnExists(name) ||
		                                  checks.role->Can(CatalogType::TABLE_ENTRY, table.GetColumn(name).Acl(), use);
	                           });
	if (!granted) {
		Deny(table, checks);
	}
}

void Binder::RequireColumn(const TableCatalogEntry &table, const RelationChecks &checks, AclMode use,
                           const ColumnDefinition &column) {
	if (checks.role && !checks.role->Can(CatalogType::TABLE_ENTRY, PermissionsOf(table), use) &&
	    !checks.role->Can(CatalogType::TABLE_ENTRY, column.Acl(), use)) {
		Deny(table, checks);
	}
}

void Binder::CheckColumnRead(Binding &binding, const Identifier &column) {
	column_t index;
	if (!binding.checks.role || !binding.TryGetBindingIndex(column, index) || index >= VIRTUAL_COLUMN_START) {
		return;
	}
	auto &relation = *binding.GetStandardEntry();
	if (!binding.checks.role->Can(CatalogType::TABLE_ENTRY, ColumnAclOf(relation, index), AclMode::Select)) {
		Deny(relation, binding.checks);
	}
}

void Binder::AdoptPrivilegeChecks(Binder &other) {
	auto &source = *other.global_binder_state;
	D_ASSERT(&source != global_binder_state.get());
	if (source.denied_relation) {
		Deny(*source.denied_relation, {nullptr, source.denied_reference});
	}
	auto &state = *global_binder_state;
	if (!state.denied_function) {
		state.denied_function = source.denied_function;
		state.denied_overload = source.denied_overload;
	}
	source.denied_relation = nullptr;
	source.denied_function = nullptr;
	source.denied_overload = nullptr;
}

void Binder::CheckPrivileges() {
	auto &state = *global_binder_state;
	if (state.denied_relation) {
		ThrowPermissionDenied(*state.denied_relation);
	}
	if (state.denied_function) {
		ThrowPermissionDenied(*state.denied_function, state.denied_overload);
	}
}

idx_t Binder::CreationOwner(const CreateInfo &info) {
	if (info.type == CatalogType::SEQUENCE_ENTRY) {
		auto &sequence = info.Cast<CreateSequenceInfo>();
		if (sequence.serial) {
			auto table = Catalog::GetEntry(
			    context,
			    EntryLookupInfo(CatalogType::TABLE_ENTRY,
			                    QualifiedName::FromPath({sequence.owned_by.begin(), sequence.owned_by.end() - 1})),
			    OnEntryNotFound::RETURN_NULL);
			if (table) {
				return table->permissions.owner;
			}
		}
	}
	return CheckAs().role;
}

void Binder::RequireReplacedOwner(SchemaCatalogEntry &schema, const CreateInfo &info, const Identifier &name) {
	const bool merges_overloads =
	    info.on_conflict == OnCreateConflict::ALTER_ON_CONFLICT &&
	    (info.type == CatalogType::MACRO_ENTRY || info.type == CatalogType::TABLE_MACRO_ENTRY);
	const bool replaces = info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT ||
	                      (info.on_conflict == OnCreateConflict::ALTER_ON_CONFLICT && !merges_overloads);
	auto &role = CheckAs();
	if (role.is_superuser || !replaces) {
		return;
	}
	if (auto existing = schema.GetEntry(schema.catalog.GetCatalogTransaction(context), info.type, name)) {
		Require(context, role, *existing, AclMode::Own);
	}
}

void Binder::RequireUserSchema(const SchemaCatalogEntry &schema, const Identifier &name) {
	if (schema.internal && (!schema.catalog.IsDuckCatalog() || DefaultSchemaGenerator::IsDefaultSchema(schema.name))) {
		throw PermissionException::WithDetail("System catalog modifications are currently disallowed.",
		                                      "permission denied to create \"%s.%s\"", schema.name.GetIdentifierName(),
		                                      name.GetIdentifierName());
	}
}

void Binder::RefuseSystemTablespace(const CatalogEntry &entry) {
	if (entry.type == CatalogType::TABLESPACE_ENTRY && entry.internal) {
		throw NotImplementedException(
		    unordered_map<string, string> {
		        {"detail", "A system tablespace cannot be renamed, given to another owner or granted on."}},
		    StringUtil::Format("tablespace \"%s\" is a system tablespace", entry.name.GetIdentifierName()));
	}
}

void Binder::RequireIndexCreate(StandardEntry &relation, const CreateIndexInfo &info) {
	auto &role = CheckAs();
	Require(context, role, relation, AclMode::Own);
	auto &schema = relation.ParentSchema(context);
	Require(context, role, schema, AclMode::Create);
	RequireReplacedOwner(schema, info, info.GetIndexName());
	RequirePlacement(relation.catalog, info, relation);
}

void Binder::OwnDefinedOverloads(CreateInfo &info, idx_t defined) {
	auto &macros = info.Cast<CreateMacroInfo>().macros;
	for (idx_t i = 0; i < defined; i++) {
		macros[i]->permissions = info.permissions;
	}
}

void Binder::RequireDefinedTypeUsage(CreateInfo &info, idx_t defined, Catalog &catalog) {
	auto &macros = info.Cast<CreateMacroInfo>().macros;
	for (idx_t i = 0; i < (defined ? defined : macros.size()); i++) {
		for (auto &type : macros[i]->types) {
			RequireTypeUsage(context, CheckAs(), type, catalog);
		}
		for (auto &type : macros[i]->return_types) {
			RequireTypeUsage(context, CheckAs(), type, catalog);
		}
	}
}

} // namespace duckdb
