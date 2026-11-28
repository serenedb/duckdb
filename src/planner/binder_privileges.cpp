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
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/table_binding.hpp"

#include <absl/algorithm/container.h>

namespace duckdb {

const RoleAccess &Binder::CheckAs() const {
	return acting.privileges ? *acting.privileges : duckdb::Role(context);
}

const RoleAccess &Binder::Role() const {
	return acting.current_user ? *acting.current_user : duckdb::Role(context);
}

optional_idx Binder::BoundRole() const {
	auto &caller = Role();
	return &caller != &duckdb::Role(context) && !caller.IsSystem() ? optional_idx(caller.role) : optional_idx();
}

shared_ptr<const vector<CatalogSearchEntry>> Binder::BoundSearchPath() const {
	auto &path = entry_retriever.FunctionSearchPath();
	if (!path) {
		return nullptr;
	}
	return shared_ptr<const vector<CatalogSearchEntry>>(path, &path->GetSetPaths());
}

const RoleAccess &Binder::FunctionBodyRole(optional_idx owner) {
	return owner.IsValid() ? duckdb::Role(context, owner.GetIndex()) : Role();
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
	Check<AclMode::Execute>(context, Role(), function).Require();
}

void Binder::Execute(const MacroCatalogEntry &function, const MacroFunction &overload) {
	const StatementNaming procedure {"procedure", {&function.name, 1}};
	Check<AclMode::Execute>(context, Role(), {function, function.OverloadPermissions(overload)})
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
	auto &role = Role();
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
		Check<AclMode::Create>(context, CheckAs(), *tablespace).Require();
	}
}

void Binder::BindAsFunctionBody(const FunctionBody &body) {
	if (!CheckAs().IsSystem()) {
		BindAsFunctionBody(FunctionBodyRole(body.owner));
	}
	acting.security_definer |= body.owner.IsValid();
	UseFunctionSearchPath(*body.config);
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
	if (!checks.role || Check<AclMode::Select>(context, *checks.role, relation)) {
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
		auto check = Check<AclMode::Select>(context, *checks.role, PrivilegeTarget::Columns(relation, {}));
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
	Deny(Check<AclMode::Select>(context, *binding.checks.role,
	                            PrivilegeTarget::Columns(*binding.GetStandardEntry(), {&read, 1})),
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
		Check<AclMode::Own>(context, role, *existing).Require();
		return;
	}
	auto &macro = existing->Cast<MacroCatalogEntry>();
	for (auto &overload : macro.macros) {
		Check<AclMode::Own>(context, role, {macro, macro.OverloadPermissions(*overload)}).Require();
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
	Check<AclMode::Create>(context, CheckAs(), schema).Require();
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
				Check<AclMode::Usage>(context, CheckAs(), *entry).Require();
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
