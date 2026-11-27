#include "duckdb/planner/binder.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/catalog/privileges.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
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

void Binder::Execute(const CatalogEntry &function) {
	Check(context, CurrentRole(), function, AclMode::Execute).Require();
}

void Binder::Execute(const MacroCatalogEntry &function, const MacroFunction &overload) {
	const StatementNaming procedure {"procedure", {&function.name, 1}};
	Check(context, CurrentRole(), {function, function.OverloadPermissions(overload)}, AclMode::Execute)
	    .Require(overload.is_procedure ? &procedure : nullptr);
}

static bool CallsFunctions(const IndexCatalogEntry &index) {
	bool calls = false;
	const auto visit = [&](const ParsedExpression &expression) {
		ParsedExpressionIterator::VisitExpression<FunctionExpression>(
		    expression, [&](const FunctionExpression &) { calls = true; });
	};
	for (auto &key : index.parsed_expressions) {
		visit(*key);
	}
	if (index.where_clause) {
		visit(*index.where_clause);
	}
	return calls;
}

void Binder::RequireIndexCalls(TableCatalogEntry &table, optional_ptr<const vector<PhysicalIndex>> changed) {
	auto &role = CurrentRole();
	if (role.is_superuser) {
		return;
	}
	auto &columns = table.GetColumns();
	const auto changes = [&](column_t column) {
		return absl::c_any_of(
		    *changed, [&](PhysicalIndex index) { return columns.GetColumn(index).Logical() == LogicalIndex(column); });
	};
	table.ParentCatalog().ScanIndexes(context, table, [&](IndexCatalogEntry &index) {
		if ((!changed || absl::c_any_of(index.column_ids, changes)) && CallsFunctions(index)) {
			global_binder_state->index_key_checks.push_back({table, index, role});
		}
	});
}

void Binder::RequirePlacement(Catalog &catalog, const CreateInfo &info, optional_ptr<const CatalogEntry> indexed) {
	if (auto tablespace = catalog.PlacementTablespace(context, info, indexed)) {
		Check(context, CheckAs(), *tablespace, AclMode::Create).Require();
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

BoundStatement Binder::BindTarget(TableRef &ref, TableRefUse use, AclMode target) {
	return ref.type == TableReferenceType::BASE_TABLE ? Bind(ref.Cast<BaseTableRef>(), use, target) : Bind(ref);
}

RelationChecks Binder::ChecksOf(const StandardEntry &relation) {
	auto &role = CheckAs();
	if (role.is_superuser) {
		return {};
	}
	return {&role, global_binder_state->relation_references++};
}

RelationChecks Binder::ReadChecks(const StandardEntry &relation, RelationChecks checks) {
	if (!checks.role || Check(context, *checks.role, relation, AclMode::Select)) {
		return {};
	}
	return checks;
}

RelationChecks Binder::ReadChecks(const StandardEntry &relation, TableRefUse use, AclMode target) {
	if (use == TableRefUse::UTILITY_TARGET) {
		return {};
	}
	const auto relation_checks = ChecksOf(relation);
	if (relation_checks.role && target != AclMode::NoRights) {
		Deny(Check(context, *relation_checks.role, relation, target), relation_checks);
	}
	auto checks = ReadChecks(relation, relation_checks);
	if (checks.role && use == TableRefUse::READ) {
		auto check = Check(context, *checks.role, PrivilegeTarget::Columns(relation, {}), AclMode::Select);
		if (!check) {
			Deny(check, checks);
			return {};
		}
	}
	return checks;
}

void Binder::Deny(const PrivilegeCheck &check, const RelationChecks &checks) {
	auto &state = *global_binder_state;
	if (!check && (state.relation_check || checks.reference < state.denied_reference)) {
		state.relation_check = check;
		state.denied_reference = checks.reference;
	}
}

void Binder::RequireColumns(const TableCatalogEntry &table, const RelationChecks &checks, AclMode use,
                            std::span<const LogicalIndex> columns) {
	if (checks.role && !Check(context, *checks.role, table, use)) {
		Deny(Check(context, *checks.role, PrivilegeTarget::Columns(table, columns), use), checks);
	}
}

void Binder::RequireColumns(const TableCatalogEntry &table, const RelationChecks &checks, AclMode use,
                            std::span<const Identifier> columns) {
	if (!checks.role || Check(context, *checks.role, table, use)) {
		return;
	}
	if (columns.empty()) {
		Deny(Check(context, *checks.role, PrivilegeTarget::Columns(table, {}), use), checks);
		return;
	}
	for (auto &name : columns) {
		if (table.ColumnExists(name)) {
			const auto column = table.GetColumn(name).Logical();
			Deny(Check(context, *checks.role, PrivilegeTarget::Columns(table, {&column, 1}), use), checks);
		}
	}
}

void Binder::CheckColumnRead(Binding &binding, const Identifier &column) {
	column_t index;
	if (!binding.checks.role || !binding.TryGetBindingIndex(column, index) || index >= VIRTUAL_COLUMN_START) {
		return;
	}
	const LogicalIndex read(index);
	Deny(Check(context, *binding.checks.role, PrivilegeTarget::Columns(*binding.GetStandardEntry(), {&read, 1}),
	           AclMode::Select),
	     binding.checks);
}

void Binder::AdoptPrivilegeChecks(Binder &other) {
	auto &source = *other.global_binder_state;
	D_ASSERT(&source != global_binder_state.get());
	Deny(source.relation_check, {nullptr, source.denied_reference});
	auto &checks = global_binder_state->index_key_checks;
	checks.insert(checks.end(), source.index_key_checks.begin(), source.index_key_checks.end());
	source.relation_check = PrivilegeCheck();
	source.index_key_checks.clear();
}

void Binder::CheckPrivileges() {
	auto &state = *global_binder_state;
	state.relation_check.Require();
	for (auto &check : state.index_key_checks) {
		auto &table = check.table.get();
		auto &index = check.index.get();
		auto keys = Binder::CreateBinder(context, this);
		keys->BindAsFunctionBody(check.role);
		keys->BindStoredDefinition();
		vector<Identifier> names;
		vector<LogicalType> types;
		for (auto &column : table.GetColumns().Logical()) {
			names.push_back(column.Name());
			types.push_back(column.Type());
		}
		keys->bind_context.AddGenericBinding(keys->GenerateTableIndex(), table.name, names, types,
		                                     table.GetColumns().IsCaseSensitive());
		IndexBinder key_binder(*keys, context);
		for (auto &key : index.parsed_expressions) {
			auto expression = QualifyTableExpression(context, table, key->Copy());
			key_binder.BindKey(expression);
		}
		if (index.where_clause) {
			auto predicate = QualifyTableExpression(context, table, index.where_clause->Copy());
			key_binder.Bind(predicate);
		}
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
	auto existing = schema.GetEntry(schema.catalog.GetCatalogTransaction(context), info.type, name);
	if (!existing) {
		return;
	}
	if (existing->type != CatalogType::MACRO_ENTRY && existing->type != CatalogType::TABLE_MACRO_ENTRY) {
		Check(context, role, *existing, AclMode::Own).Require();
		return;
	}
	auto &macro = existing->Cast<MacroCatalogEntry>();
	for (auto &overload : macro.macros) {
		Check(context, role, {macro, macro.OverloadPermissions(*overload)}, AclMode::Own).Require();
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
	auto &schema = relation.ParentSchema(context);
	Check(context, CheckAs(), schema, AclMode::Create).Require();
	RequireReplacedOwner(schema, info, info.GetIndexName());
	RequirePlacement(relation.catalog, info, relation);
}

void Binder::OwnDefinedOverloads(CreateInfo &info, idx_t defined) {
	auto &macros = info.Cast<CreateMacroInfo>().macros;
	for (idx_t i = 0; i < defined; i++) {
		macros[i]->permissions = info.permissions;
	}
}

void Binder::RequireTypeUsage(const LogicalType &type, Catalog &catalog) {
	if (CheckAs().is_superuser) {
		return;
	}
	if (auto extension = type.GetExtensionInfo()) {
		auto oid = extension->properties.find(ExtensionTypeInfo::CATALOG_OID_PROPERTY);
		if (oid != extension->properties.end()) {
			auto entry = catalog.LookupOwner(catalog.GetCatalogTransaction(context), oid->second.GetValue<idx_t>());
			if (entry && entry->type == CatalogType::TYPE_ENTRY) {
				Check(context, CheckAs(), *entry, AclMode::Usage).Require();
			}
			return;
		}
	}
	if (!type.HasParameters()) {
		return;
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return RequireTypeUsage(ListType::GetChildType(type), catalog);
	case LogicalTypeId::ARRAY:
		return RequireTypeUsage(ArrayType::GetChildType(type), catalog);
	case LogicalTypeId::MAP:
		RequireTypeUsage(MapType::KeyType(type), catalog);
		return RequireTypeUsage(MapType::ValueType(type), catalog);
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			RequireTypeUsage(child.second, catalog);
		}
		return;
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			RequireTypeUsage(UnionType::GetMemberType(type, i), catalog);
		}
		return;
	default:
		return;
	}
}

void Binder::RequireStoredTypes(const CreateInfo &info, Catalog &catalog, idx_t defined) {
	const auto require = [&](const LogicalType &type) {
		RequireTypeUsage(type, catalog);
	};
	switch (info.type) {
	case CatalogType::TABLE_ENTRY:
		for (auto &column : info.Cast<CreateTableInfo>().columns.Logical()) {
			require(column.Type());
		}
		return;
	case CatalogType::VIEW_ENTRY:
		for (auto &type : info.Cast<CreateViewInfo>().types) {
			require(type);
		}
		return;
	case CatalogType::TYPE_ENTRY:
		if (auto &type = info.Cast<CreateTypeInfo>().type; type.id() == LogicalTypeId::STRUCT) {
			for (auto &attribute : StructType::GetChildTypes(type)) {
				require(attribute.second);
			}
		}
		return;
	default:
		break;
	}
	auto &macros = info.Cast<CreateMacroInfo>().macros;
	for (idx_t i = 0; i < (defined ? defined : macros.size()); i++) {
		absl::c_for_each(macros[i]->types, require);
		absl::c_for_each(macros[i]->return_types, require);
	}
}

} // namespace duckdb
