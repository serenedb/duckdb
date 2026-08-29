#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_entry.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_subject_entry.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_dependent_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/constraints/check_constraint.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/catalog/dependency_catalog_set.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"

#include "duckdb/common/printer.hpp"

namespace duckdb {

static void AssertMangledName(const string &mangled_name, idx_t expected_null_bytes) {
#ifdef DEBUG
	idx_t nullbyte_count = 0;
	for (auto &ch : mangled_name) {
		nullbyte_count += ch == '\0';
	}
	D_ASSERT(nullbyte_count == expected_null_bytes);
#endif
}

MangledEntryName::MangledEntryName(const CatalogEntryInfo &info) {
	if (info.oid != 0) {
		// An id-addressed subject keys by the id alone: whatever the name-keyed
		// fields hold does not change which object it names.
		this->name = Identifier(CatalogTypeToString(CatalogType::INVALID) + '\0' + '\0' + to_string(info.oid) + '\0');
		AssertMangledName(this->name.GetIdentifierName(), 3);
		return;
	}
	auto &type = info.type;
	auto &schema = info.schema;
	auto &name = info.name;
	auto &catalog = info.catalog;

	this->name = Identifier(CatalogTypeToString(type) + '\0' + schema + '\0' + name + '\0' + catalog);
	AssertMangledName(this->name.GetIdentifierName(), 3);
}

MangledDependencyName::MangledDependencyName(const MangledEntryName &from, const MangledEntryName &to) {
	this->name = Identifier(from.name + '\0' + to.name);
	AssertMangledName(this->name.GetIdentifierName(), 7);
}

DependencyManager::DependencyManager(DuckCatalog &catalog) : catalog(catalog), subjects(catalog), dependents(catalog) {
}

Identifier DependencyManager::GetSchema(const CatalogEntry &entry) {
	if (entry.type == CatalogType::SCHEMA_ENTRY) {
		return entry.name;
	}
	auto schema = entry.TryGetParentSchema();
	return schema ? schema->name : Identifier();
}

MangledEntryName DependencyManager::MangleName(const CatalogEntryInfo &info) {
	return MangledEntryName(info);
}

MangledEntryName DependencyManager::MangleName(const CatalogEntry &entry) {
	if (entry.type == CatalogType::DEPENDENCY_ENTRY) {
		auto &dependency_entry = entry.Cast<DependencyEntry>();
		return dependency_entry.EntryMangledName();
	}
	return MangleName(GetLookupProperties(entry));
}

DependencyInfo DependencyInfo::FromSubject(DependencyEntry &dep) {
	return DependencyInfo {/*dependent = */ dep.Dependent(),
	                       /*subject = */ dep.Subject()};
}

DependencyInfo DependencyInfo::FromDependent(DependencyEntry &dep) {
	return DependencyInfo {/*dependent = */ dep.Dependent(),
	                       /*subject = */ dep.Subject()};
}

// ----------- DEPENDENCY_MANAGER -----------

bool DependencyManager::IsSystemEntry(CatalogEntry &entry) const {
	if (entry.internal) {
		return true;
	}

	switch (entry.type) {
	case CatalogType::DEPENDENCY_ENTRY:
	case CatalogType::RENAMED_ENTRY:
		return true;
	default:
		return false;
	}
}

CatalogSet &DependencyManager::Dependents() {
	return dependents;
}

CatalogSet &DependencyManager::Subjects() {
	return subjects;
}

void DependencyManager::ScanSetInternal(CatalogTransaction transaction, const CatalogEntryInfo &info,
                                        bool scan_subjects, dependency_callback_t &callback) {
	catalog_entry_set_t other_entries;

	auto cb = [&](CatalogEntry &other) {
		D_ASSERT(other.type == CatalogType::DEPENDENCY_ENTRY);
		auto &other_entry = other.Cast<DependencyEntry>();
#ifdef DEBUG
		auto side = other_entry.Side();
		if (scan_subjects) {
			D_ASSERT(side == DependencyEntryType::SUBJECT);
		} else {
			D_ASSERT(side == DependencyEntryType::DEPENDENT);
		}

#endif

		other_entries.insert(other_entry);
		callback(other_entry);
	};

	if (scan_subjects) {
		DependencyCatalogSet subjects(Subjects(), info);
		subjects.Scan(transaction, cb);
	} else {
		DependencyCatalogSet dependents(Dependents(), info);
		dependents.Scan(transaction, cb);
	}

#ifdef DEBUG
	// Verify some invariants
	// Every dependency should have a matching dependent in the other set
	// And vice versa
	auto mangled_name = MangleName(info);

	if (scan_subjects) {
		for (auto &entry : other_entries) {
			auto other_info = GetLookupProperties(entry);
			DependencyCatalogSet other_dependents(Dependents(), other_info);

			// Verify that the other half of the dependency also exists
			auto dependent = other_dependents.GetEntryDetailed(transaction, mangled_name);
			D_ASSERT(dependent.reason != CatalogSet::EntryLookup::FailureReason::NOT_PRESENT);
		}
	} else {
		for (auto &entry : other_entries) {
			auto other_info = GetLookupProperties(entry);
			DependencyCatalogSet other_subjects(Subjects(), other_info);

			// Verify that the other half of the dependent also exists
			auto subject = other_subjects.GetEntryDetailed(transaction, mangled_name);
			D_ASSERT(subject.reason != CatalogSet::EntryLookup::FailureReason::NOT_PRESENT);
		}
	}
#endif
}

void DependencyManager::ScanDependents(CatalogTransaction transaction, const CatalogEntryInfo &info,
                                       dependency_callback_t &callback) {
	ScanSetInternal(transaction, info, false, callback);
}

void DependencyManager::ScanSubjects(CatalogTransaction transaction, const CatalogEntryInfo &info,
                                     dependency_callback_t &callback) {
	ScanSetInternal(transaction, info, true, callback);
}

void DependencyManager::ScanAllEdges(CatalogTransaction transaction, dependency_callback_t &callback) {
	dependents.Scan(transaction, [&](CatalogEntry &entry) {
		D_ASSERT(entry.type == CatalogType::DEPENDENCY_ENTRY);
		callback(entry.Cast<DependencyEntry>());
	});
}

DependencyManager::Attachments::Attachments(ClientContext &context, optional_ptr<const DependencyManager> skip) {
	// Filter before opening any transaction: opening one can block behind another session's commit,
	// and a database this scan will never read must not be kept alive across that wait. A catalog is
	// consulted for foreign edges exactly when it can record them in the first place.
	vector<shared_ptr<AttachedDatabase>> accepted;
	for (auto &db : DatabaseManager::Get(context).GetDatabases(context)) {
		if (db->IsClosed()) {
			continue;
		}
		auto &db_catalog = db->GetCatalog();
		auto manager = db_catalog.GetDependencyManager();
		if (!manager || manager.get() == skip.get() || !db_catalog.SupportsForeignDependencies()) {
			continue;
		}
		accepted.push_back(std::move(db));
	}
	for (auto &db : accepted) {
		auto &db_catalog = db->GetCatalog();
		attachments.push_back(
		    Attachment {db_catalog.GetDependencyManager(), db_catalog.GetCatalogTransaction(context)});
	}
}

void DependencyManager::Attachments::ScanDependents(const CatalogEntryInfo &info, dependent_callback_t &callback) {
	for (auto &attachment : attachments) {
		auto &manager = *attachment.manager;
		auto transaction = attachment.transaction;
		manager.ScanDependents(transaction, info,
		                       [&](DependencyEntry &dep) { callback(manager.LookupEntry(transaction, dep), dep); });
	}
}

void DependencyManager::Attachments::ScanAllEdges(dependency_callback_t &callback) {
	for (auto &attachment : attachments) {
		attachment.manager->ScanAllEdges(attachment.transaction, callback);
	}
}

void DependencyManager::ScanDependentsEverywhere(CatalogTransaction transaction, const CatalogEntryInfo &info,
                                                 dependent_callback_t &callback) {
	ScanDependents(transaction, info, [&](DependencyEntry &dep) { callback(LookupEntry(transaction, dep), dep); });
	if (!transaction.context) {
		return;
	}
	// This manager was already scanned through the transaction the caller holds, which is not the one a
	// sibling attachment reads through.
	Attachments(*transaction.context, this).ScanDependents(info, callback);
}

void DependencyManager::RemoveDependency(CatalogTransaction transaction, const DependencyInfo &info) {
	auto &dependent = info.dependent;
	auto &subject = info.subject;

	// The dependents of the dependency (target)
	DependencyCatalogSet dependents(Dependents(), subject.entry);
	// The subjects of the dependencies of the dependent
	DependencyCatalogSet subjects(Subjects(), dependent.entry);

	auto dependent_mangled = MangledEntryName(dependent.entry);
	auto subject_mangled = MangledEntryName(subject.entry);

	auto dependent_p = dependents.GetEntry(transaction, dependent_mangled);
	if (dependent_p) {
		// 'dependent' is no longer inhibiting the deletion of 'dependency'
		dependents.DropEntry(transaction, dependent_mangled, false);
	}
	auto subject_p = subjects.GetEntry(transaction, subject_mangled);
	if (subject_p) {
		// 'dependency' is no longer required by 'dependent'
		subjects.DropEntry(transaction, subject_mangled, false);
	}
}

void DependencyManager::CreateSubject(CatalogTransaction transaction, const DependencyInfo &info) {
	auto &from = info.dependent.entry;

	DependencyCatalogSet set(Subjects(), from);
	auto dep = make_uniq_base<DependencyEntry, DependencySubjectEntry>(catalog, info);
	auto entry_name = dep->EntryMangledName();

	//! Add to the list of objects that 'dependent' has a dependency on
	set.CreateEntry(transaction, entry_name, std::move(dep));
}

void DependencyManager::CreateDependent(CatalogTransaction transaction, const DependencyInfo &info) {
	auto &from = info.subject.entry;

	DependencyCatalogSet set(Dependents(), from);
	auto dep = make_uniq_base<DependencyEntry, DependencyDependentEntry>(catalog, info);
	auto entry_name = dep->EntryMangledName();

	//! Add to the list of object that depend on 'subject'
	set.CreateEntry(transaction, entry_name, std::move(dep));
}

void DependencyManager::CreateDependency(CatalogTransaction transaction, DependencyInfo &info) {
	auto subject_entry = LookupEntry(transaction, info.subject.entry);
	info.subject.oid = subject_entry ? subject_entry->oid : optional_idx();

	DependencyCatalogSet subjects(Subjects(), info.dependent.entry);
	DependencyCatalogSet dependents(Dependents(), info.subject.entry);

	auto subject_mangled = MangleName(info.subject.entry);
	auto dependent_mangled = MangleName(info.dependent.entry);

	auto &dependent_flags = info.dependent.flags;
	auto &subject_flags = info.subject.flags;

	auto existing_subject = subjects.GetEntry(transaction, subject_mangled);
	auto existing_dependent = dependents.GetEntry(transaction, dependent_mangled);

	// Inherit the existing flags and drop the existing entry if present
	if (existing_subject) {
		auto &existing = existing_subject->Cast<DependencyEntry>();
		auto existing_flags = existing.Subject().flags;
		if (existing_flags != subject_flags) {
			subject_flags.Apply(existing_flags);
		}
		subjects.DropEntry(transaction, subject_mangled, false, false);
	}
	if (existing_dependent) {
		auto &existing = existing_dependent->Cast<DependencyEntry>();
		auto existing_flags = existing.Dependent().flags;
		if (existing_flags != dependent_flags) {
			dependent_flags.Apply(existing_flags);
		}
		dependents.DropEntry(transaction, dependent_mangled, false, false);
	}

	// Create an entry in the dependents map of the object that is the target of the dependency
	CreateDependent(transaction, info);
	// Create an entry in the subjects map of the object that is targeting another entry
	CreateSubject(transaction, info);
}

void DependencyManager::CreateDependencies(CatalogTransaction transaction, const CatalogEntryInfo &object_info,
                                           const LogicalDependencyList &dependencies) {
	DependencyDependentFlags blocking;
	if (object_info.type != CatalogType::INDEX_ENTRY) {
		// indexes do not require CASCADE to be dropped, they are simply always dropped along with the table
		blocking.SetBlocking();
	}

	// add the object to the dependents_map of each object that it depends on
	for (auto &dependency : dependencies.Set()) {
		// An edge the dependent states as automatic is one it falls with, whatever its other references do
		auto dependency_flags = dependency.automatic ? DependencyDependentFlags() : blocking;
		DependencyInfo info {
		    /*dependent = */ DependencyDependent {object_info, dependency_flags, dependency.pieces},
		    /*subject = */ DependencySubject {dependency.entry, DependencySubjectFlags(), optional_idx()}};
		CreateDependency(transaction, info);
	}
}

void DependencyManager::CreateDependencies(CatalogTransaction transaction, const CatalogEntry &object,
                                           const LogicalDependencyList &dependencies) {
	CreateDependencies(transaction, GetLookupProperties(object), dependencies);
}

void DependencyManager::ReplaceSubjects(CatalogTransaction transaction, const CatalogEntryInfo &object,
                                        const LogicalDependencyList &dependencies) {
	// A diff, not a rewrite. Each edge is a versioned entry of its own, so chaining a version on one this
	// call does not change would turn an unrelated concurrent write of that same edge into a write-write
	// conflict -- which is the whole reason the graph is per-edge rather than one adjacency set per object.
	vector<DependencyInfo> to_remove;
	LogicalDependencyList existing;
	ScanSubjects(transaction, object, [&](DependencyEntry &dep) {
		LogicalDependency held(nullptr, dep.EntryInfo(), dep.EntryInfo().catalog);
		// An ownership edge is structural (ALTER SEQUENCE ... OWNED BY) and is never stated in a dependency
		// list, so absence from one does not mean it was retired.
		if (!dep.Subject().flags.IsOwnership()) {
			auto stated = dependencies.Set().find(held);
			// A piece change is an edge change: the edge is versioned whole, so a dependent that now
			// binds the subject through different pieces retires the old edge and writes a new one.
			if (stated == dependencies.Set().end() || !(stated->pieces == dep.Dependent().pieces)) {
				to_remove.push_back(DependencyInfo::FromSubject(dep));
				return;
			}
		}
		existing.AddDependency(held);
	});
	for (auto &dep : to_remove) {
		RemoveDependency(transaction, dep);
	}
	LogicalDependencyList added;
	for (auto &dependency : dependencies.Set()) {
		if (!existing.Set().count(dependency)) {
			added.AddDependency(dependency);
		}
	}
	CreateDependencies(transaction, object, added);
}

LogicalDependencyList DependencyManager::EntrySubjects(CatalogEntry &object,
                                                       const LogicalDependencyList &dependencies) {
	auto props = GetLookupProperties(object);
	LogicalDependencyList subjects = dependencies;
	// Every principal the entry's permissions name is a subject too: dropping a role
	// that still owns or holds a grant on this entry has to see the edge. Never itself:
	// a role's own defacl self-edge is the caller's to state.
	auto add_role = [&](idx_t role) {
		if (role == 0 || role == props.oid) {
			return;
		}
		CatalogEntryInfo info {CatalogType::INVALID, Identifier(), Identifier(), Identifier(), role};
		subjects.AddDependency(LogicalDependency(nullptr, std::move(info), Identifier()));
	};
	add_role(object.permissions.owner);
	for (auto &item : object.permissions.acl) {
		add_role(item.grantee);
		add_role(item.grantor);
	}
	for (auto &column : object.permissions.column_acl) {
		for (auto &item : column.acl) {
			add_role(item.grantee);
			add_role(item.grantor);
		}
	}
	return subjects;
}

void DependencyManager::AddObject(CatalogTransaction transaction, CatalogEntry &object,
                                  const LogicalDependencyList &dependencies) {
	if (IsSystemEntry(object)) {
		// Don't do anything for this
		return;
	}
	// The list is what the object depends on now, not something to add to what it depended on before: a
	// new version of an entry states its references in full, so one it no longer has must go. For a first
	// create there is nothing to remove and this is the plain add it always was.
	ReplaceSubjects(transaction, GetLookupProperties(object), EntrySubjects(object, dependencies));
}

static bool CascadeDrop(bool cascade, const DependencyDependentFlags &flags) {
	if (cascade) {
		return true;
	}
	if (flags.IsOwnedBy()) {
		// We are owned by this object, while it exists we can not be dropped without cascade.
		return false;
	}
	return !flags.IsBlocking();
}

CatalogEntryInfo DependencyManager::GetLookupProperties(const CatalogEntry &entry) {
	if (entry.type == CatalogType::DEPENDENCY_ENTRY) {
		auto &dependency_entry = entry.Cast<DependencyEntry>();
		return dependency_entry.EntryInfo();
	}
	return entry.ParentCatalog().GetDependencyInfo(entry);
}

optional_ptr<Catalog> DependencyManager::ResolveCatalog(CatalogTransaction transaction, const CatalogEntryInfo &info) {
	// Empty is how every dependency written before cross-catalog support looked,
	// and it always meant this catalog.
	if (info.catalog.empty() || info.catalog == catalog.GetName()) {
		return &catalog;
	}
	if (transaction.context) {
		return Catalog::GetCatalogEntry(*transaction.context, info.catalog);
	}
	// Checkpoint and boot run on a system transaction with no context, so the
	// attachment has to be reached through the instance instead.
	auto attached = DatabaseManager::Get(catalog.GetAttached().GetDatabase()).GetDatabase(info.catalog);
	if (!attached) {
		return nullptr;
	}
	return &attached->GetCatalog();
}

optional_ptr<CatalogEntry> DependencyManager::LookupEntry(CatalogTransaction transaction,
                                                          const CatalogEntryInfo &info) {
	// An entry in another attached catalog resolves there, not here. A miss is a
	// miss, not a fallback to this catalog: a same-named schema is common and
	// resolving into it would answer with the wrong object.
	auto owner = ResolveCatalog(transaction, info);
	if (!owner) {
		return nullptr;
	}

	// A CatalogTransaction is bound to one attachment; handing this one to a peer
	// catalog would have it read through a transaction that is not its own.
	auto owner_transaction = transaction;
	if (owner.get() != &catalog.Cast<Catalog>()) {
		owner_transaction = transaction.context
		                        ? owner->GetCatalogTransaction(*transaction.context)
		                        : CatalogTransaction::GetSystemTransaction(owner->GetAttached().GetDatabase());
	}
	return owner->GetDependencyEntry(owner_transaction, info);
}

optional_ptr<CatalogEntry> DependencyManager::LookupEntry(CatalogTransaction transaction, CatalogEntry &dependency) {
	if (dependency.type != CatalogType::DEPENDENCY_ENTRY) {
		return &dependency;
	}
	return LookupEntry(transaction, GetLookupProperties(dependency));
}

void DependencyManager::CleanupDependencies(CatalogTransaction transaction, CatalogEntry &object) {
	// Collect the dependencies
	vector<DependencyInfo> to_remove;

	auto info = GetLookupProperties(object);
	ScanSubjects(transaction, info,
	             [&](DependencyEntry &dep) { to_remove.push_back(DependencyInfo::FromSubject(dep)); });
	ScanDependents(transaction, info,
	               [&](DependencyEntry &dep) { to_remove.push_back(DependencyInfo::FromDependent(dep)); });

	// Remove the dependency entries
	for (auto &dep : to_remove) {
		RemoveDependency(transaction, dep);
	}
}

void DependencyManager::RemoveDependencyBetween(CatalogTransaction transaction, CatalogEntry &dependent,
                                                CatalogEntry &subject) {
	if (IsSystemEntry(dependent) || IsSystemEntry(subject)) {
		return;
	}
	auto dependent_info = GetLookupProperties(dependent);
	auto subject_info = GetLookupProperties(subject);
	auto matches = [](const CatalogEntryInfo &a, const CatalogEntryInfo &b) {
		return a.type == b.type && a.schema == b.schema && a.name == b.name;
	};
	vector<DependencyInfo> to_remove;
	ScanSubjects(transaction, dependent_info, [&](DependencyEntry &dep) {
		if (matches(dep.EntryInfo(), subject_info)) {
			to_remove.push_back(DependencyInfo::FromSubject(dep));
		}
	});
	for (auto &dep : to_remove) {
		RemoveDependency(transaction, dep);
	}
}

static string EntryToString(CatalogEntryInfo &info) {
	auto type = info.type;
	switch (type) {
	case CatalogType::TABLE_ENTRY: {
		return StringUtil::Format("table \"%s\"", info.name);
	}
	case CatalogType::SCHEMA_ENTRY: {
		return StringUtil::Format("schema \"%s\"", info.name);
	}
	case CatalogType::VIEW_ENTRY: {
		return StringUtil::Format("view \"%s\"", info.name);
	}
	case CatalogType::INDEX_ENTRY: {
		return StringUtil::Format("index \"%s\"", info.name);
	}
	case CatalogType::SEQUENCE_ENTRY: {
		return StringUtil::Format("sequence \"%s\"", info.name);
	}
	case CatalogType::COLLATION_ENTRY: {
		return StringUtil::Format("collation \"%s\"", info.name);
	}
	case CatalogType::COORDINATE_SYSTEM_ENTRY: {
		return StringUtil::Format("coordinate system \"%s\"", info.name);
	}
	case CatalogType::TYPE_ENTRY: {
		return StringUtil::Format("type \"%s\"", info.name);
	}
	case CatalogType::TABLE_FUNCTION_ENTRY: {
		return StringUtil::Format("table function \"%s\"", info.name);
	}
	case CatalogType::SCALAR_FUNCTION_ENTRY: {
		return StringUtil::Format("scalar function \"%s\"", info.name);
	}
	case CatalogType::AGGREGATE_FUNCTION_ENTRY: {
		return StringUtil::Format("aggregate function \"%s\"", info.name);
	}
	case CatalogType::PRAGMA_FUNCTION_ENTRY: {
		return StringUtil::Format("pragma function \"%s\"", info.name);
	}
	case CatalogType::COPY_FUNCTION_ENTRY: {
		return StringUtil::Format("copy function \"%s\"", info.name);
	}
	case CatalogType::MACRO_ENTRY: {
		return StringUtil::Format("macro function \"%s\"", info.name);
	}
	case CatalogType::TABLE_MACRO_ENTRY: {
		return StringUtil::Format("table macro function \"%s\"", info.name);
	}
	case CatalogType::SECRET_ENTRY: {
		return StringUtil::Format("secret \"%s\"", info.name);
	}
	case CatalogType::SECRET_TYPE_ENTRY: {
		return StringUtil::Format("secret type \"%s\"", info.name);
	}
	case CatalogType::SECRET_FUNCTION_ENTRY: {
		return StringUtil::Format("secret function \"%s\"", info.name);
	}
	case CatalogType::TRIGGER_ENTRY: {
		return StringUtil::Format("trigger \"%s\"", info.name);
	}
	default:
		// An error path must not raise an internal error over a kind this list has not caught up with.
		return StringUtil::Format("%s \"%s\"", StringUtil::Lower(CatalogTypeToString(type)), info.name);
	};
}

//! The entry's own identity, for messages: a catalog that addresses its dependency edges by a mangled
//! stable id (GetDependencyInfo) must not leak that mangling into what a refusal shows.
static CatalogEntryInfo EntryDisplayInfo(const CatalogEntry &entry) {
	return CatalogEntryInfo {entry.type, DependencyManager::GetSchema(entry), entry.name,
	                         Identifier(entry.ParentCatalog().GetName())};
}

string DependencyManager::CollectDependents(CatalogTransaction transaction, catalog_entry_set_t &entries,
                                            CatalogEntryInfo &info, catalog_entry_set_t &visited) {
	string result;
	// In name order, not set order: which parent a shared dependent is attributed to is decided by who
	// visits it first, and the message must read the same on every run.
	vector<reference<CatalogEntry>> ordered(entries.begin(), entries.end());
	std::sort(ordered.begin(), ordered.end(), [](const reference<CatalogEntry> &a, const reference<CatalogEntry> &b) {
		return a.get().name.GetIdentifierName() < b.get().name.GetIdentifierName();
	});
	for (auto &entry : ordered) {
		D_ASSERT(!IsSystemEntry(entry.get()));
		// Dependencies can be mutual (a view over a function whose body reads the view), so an entry
		// already reported is not walked again.
		if (visited.count(entry)) {
			continue;
		}
		visited.insert(entry);
		auto other_info = GetLookupProperties(entry);
		auto display_info = EntryDisplayInfo(entry);
		result += StringUtil::Format("%s depends on %s.\n", EntryToString(display_info), EntryToString(info));
		catalog_entry_set_t entry_dependents;
		ScanDependents(transaction, other_info, [&](DependencyEntry &dep) {
			auto child = LookupEntry(transaction, dep);
			if (!child) {
				return;
			}
			if (!CascadeDrop(false, dep.Dependent().flags)) {
				entry_dependents.insert(*child);
			}
		});
		if (!entry_dependents.empty()) {
			result += CollectDependents(transaction, entry_dependents, display_info, visited);
		}
	}
	return result;
}

void DependencyManager::VerifyExistence(CatalogTransaction transaction, DependencyEntry &object) {
	auto &subject = object.Subject();

	CatalogEntryInfo info;
	if (subject.flags.IsOwnership()) {
		info = object.SourceInfo();
	} else {
		info = object.EntryInfo();
	}

	auto &type = info.type;
	auto &schema = info.schema;
	auto &name = info.name;

	auto &duck_catalog = catalog.Cast<DuckCatalog>();
	auto &schema_catalog_set = duck_catalog.GetSchemaCatalogSet();

	CatalogSet::EntryLookup lookup_result;
	lookup_result = schema_catalog_set.GetEntryDetailed(transaction, schema);

	if (type != CatalogType::SCHEMA_ENTRY && lookup_result.result) {
		auto &schema_entry = lookup_result.result->Cast<SchemaCatalogEntry>();
		EntryLookupInfo lookup_info(type, QualifiedName(name));
		lookup_result = schema_entry.LookupEntryDetailed(transaction, lookup_info);
	}

	if (lookup_result.reason == CatalogSet::EntryLookup::FailureReason::DELETED) {
		throw DependencyException("Could not commit creation of dependency, subject \"%s\" has been deleted",
		                          object.SourceInfo().name);
	}
	// The subject still exists by name - check if it is the same object the dependency was created against
	if (!subject.flags.IsOwnership() && subject.oid.IsValid() && lookup_result.result &&
	    lookup_result.result->oid != subject.oid.GetIndex()) {
		throw DependencyException(
		    "Could not commit creation of dependency, subject \"%s\" was dropped and re-created by another transaction",
		    object.EntryInfo().name);
	}
}

void DependencyManager::VerifyCommitDrop(CatalogTransaction transaction, transaction_t start_time,
                                         CatalogEntry &object) {
	if (IsSystemEntry(object)) {
		return;
	}
	auto info = GetLookupProperties(object);
	ScanDependents(transaction, info, [&](DependencyEntry &dep) {
		auto dep_committed_at = dep.timestamp.load();
		if (dep_committed_at >= start_time) {
			// In the event of a CASCADE, the dependency drop has not committed yet
			// so we would be halted by the existence of a dependency we are already dropping unless we check the
			// timestamp
			//
			// Which differentiates between objects that we were already aware of (and will subsequently be dropped) and
			// objects that were introduced inbetween, which should cause this error:
			throw DependencyException(
			    "Could not commit DROP of \"%s\" because a dependency was created after the transaction started",
			    object.name);
		}
	});
	ScanSubjects(transaction, info, [&](DependencyEntry &dep) {
		auto dep_committed_at = dep.timestamp.load();
		if (!dep.Dependent().flags.IsOwnedBy()) {
			return;
		}
		D_ASSERT(dep.Subject().flags.IsOwnership());
		if (dep_committed_at >= start_time) {
			// Same as above, objects that are owned by the object that is being dropped will be dropped as part of this
			// transaction. Only objects that were introduced by other transactions, that this transaction could not
			// see, should cause this error:
			throw DependencyException(
			    "Could not commit DROP of \"%s\" because a dependency was created after the transaction started",
			    object.name);
		}
	});
}

catalog_entry_set_t DependencyManager::CheckDropDependencies(CatalogTransaction transaction, CatalogEntry &object,
                                                             bool cascade,
                                                             catalog_entry_map_t<vector<DependencyPiece>> &pieces) {
	if (IsSystemEntry(object)) {
		// Don't do anything for this
		return catalog_entry_set_t();
	}

	catalog_entry_set_t to_drop;
	catalog_entry_set_t blocking_dependents;

	auto info = GetLookupProperties(object);
	auto collect = [&](optional_ptr<CatalogEntry> entry, DependencyEntry &dep) {
		// It makes no sense to have a schema depend on anything
		D_ASSERT(dep.EntryInfo().type != CatalogType::SCHEMA_ENTRY);
		if (!entry) {
			return;
		}

		if (!CascadeDrop(cascade, dep.Dependent().flags)) {
			// no cascade and there are objects that depend on this object: throw error
			blocking_dependents.insert(*entry);
		} else {
			to_drop.insert(*entry);
			auto &dependent_pieces = dep.Dependent().pieces;
			auto &collected = pieces[*entry];
			collected.insert(collected.end(), dependent_pieces.begin(), dependent_pieces.end());
		}
	};
	// An edge lives in the dependent's own catalog, so a dependent another attachment holds recorded
	// its edge there.
	ScanDependentsEverywhere(transaction, info, collect);
	if (!blocking_dependents.empty()) {
		string error_string = StringUtil::Format(
		    "Cannot drop entry \"%s\" because there are entries that depend on it.\n", object.name.GetIdentifierName());
		auto object_info = EntryDisplayInfo(object);
		catalog_entry_set_t visited;
		// Sorted: the sets iterate in address order, and an error message must read the same on every run.
		auto lines = StringUtil::Split(CollectDependents(transaction, blocking_dependents, object_info, visited), '\n');
		std::sort(lines.begin(), lines.end());
		for (auto &line : lines) {
			if (line.empty()) {
				continue;
			}
			error_string += line + "\n";
		}
		error_string += "Use DROP...CASCADE to drop all dependents.";
		// The count travels beside the message: a host that words the refusal its own way needs the
		// number of blocking dependents, not the prose naming them.
		throw DependencyException({{"blocking_dependents", to_string(blocking_dependents.size())}}, error_string);
	}

	// Look through all the entries that 'object' depends on
	ScanSubjects(transaction, info, [&](DependencyEntry &dep) {
		auto flags = dep.Subject().flags;
		if (flags.IsOwnership()) {
			// We own this object, it should be dropped along with the table
			auto entry = LookupEntry(transaction, dep);
			if (entry) {
				to_drop.insert(*entry);
			}
		}
	});
	return to_drop;
}

void DependencyManager::DropObject(CatalogTransaction transaction, CatalogEntry &object, bool cascade) {
	if (IsSystemEntry(object)) {
		// Don't do anything for this
		return;
	}

	// Check if there are any entries that block the DROP because they still depend on the object
	catalog_entry_map_t<vector<DependencyPiece>> pieces;
	auto to_drop = CheckDropDependencies(transaction, object, cascade, pieces);
	CleanupDependencies(transaction, object);

	// A table dependent is altered rather than dropped (DropDependent), and altering it is refused while
	// another dependent still binds it -- so everything that falls, falls first.
	catalog_entry_vector_t ordered;
	for (auto &entry : to_drop) {
		if (entry.get().type != CatalogType::TABLE_ENTRY) {
			ordered.push_back(entry);
		}
	}
	for (auto &entry : to_drop) {
		if (entry.get().type == CatalogType::TABLE_ENTRY) {
			ordered.push_back(entry);
		}
	}
	const vector<DependencyPiece> no_pieces;
	for (auto &entry : ordered) {
		auto &dep_catalog = entry.get().ParentCatalog();
		auto collected = pieces.find(entry.get());
		const bool has_pieces = collected != pieces.end() && !collected->second.empty();
		// A table bound through sub-objects survives in altered form: each recorded piece becomes the
		// alter the statement would have been, and only a dependent bound whole is dropped whole.
		if (has_pieces && entry.get().type == CatalogType::TABLE_ENTRY) {
			TrimDependent(transaction, entry.get(), collected->second);
			continue;
		}
		// The dependent's own catalog first: a host dependent can need more than the entry removed.
		if (dep_catalog.DropDependent(transaction, object, entry.get(), cascade,
		                              collected != pieces.end() ? collected->second : no_pieces)) {
			continue;
		}
		auto set = entry.get().set;
		D_ASSERT(set);
		if (&dep_catalog == &catalog) {
			set->DropEntry(transaction, entry.get().name, cascade);
			continue;
		}
		// A dependent another attachment holds: its set wants a transaction of its own catalog, which
		// only a live statement carries -- and only a live statement can have created such an edge.
		if (transaction.context) {
			set->DropEntry(dep_catalog.GetCatalogTransaction(*transaction.context), entry.get().name, cascade);
		}
	}
}

void DependencyManager::TrimDependent(CatalogTransaction transaction, CatalogEntry &dependent,
                                      const vector<DependencyPiece> &pieces) {
	auto &set = *dependent.set;
	AlterEntryData data(QualifiedName(Identifier(), GetSchema(dependent), dependent.name),
	                    OnEntryNotFound::THROW_EXCEPTION);
	for (auto &piece : pieces) {
		// Resolved from the live version each time: an earlier piece's alter supersedes the entry the
		// walk collected. A sub-object a previous alter already took is simply gone -- duplicate pieces
		// dedupe themselves.
		auto live = set.GetEntry(transaction, dependent.name);
		if (!live || live->type != CatalogType::TABLE_ENTRY) {
			return;
		}
		auto &table = live->Cast<TableCatalogEntry>();
		auto column_name = [&](idx_t catalog_oid) -> optional_ptr<const ColumnDefinition> {
			for (auto &column : table.GetColumns().Logical()) {
				if (column.CatalogOid() == catalog_oid) {
					return &column;
				}
			}
			return nullptr;
		};
		unique_ptr<AlterInfo> step;
		switch (piece.kind) {
		case DependencyPieceKind::COLUMN_TYPE: {
			auto column = column_name(piece.sub_object);
			if (column) {
				step = make_uniq<RemoveColumnInfo>(data, column->Name().GetIdentifierName(),
				                                   /*if_column_exists=*/false, /*cascade=*/true);
			}
			break;
		}
		case DependencyPieceKind::COLUMN_DEFAULT: {
			auto column = column_name(piece.sub_object);
			if (column) {
				step = make_uniq<SetDefaultInfo>(data, column->Name(), nullptr);
			}
			break;
		}
		case DependencyPieceKind::CHECK:
		case DependencyPieceKind::FOREIGN_KEY: {
			for (auto &constraint : table.GetConstraints()) {
				if (constraint->oid != piece.sub_object) {
					continue;
				}
				// A CHECK declared without a name is dropped by its expression text, the same key
				// DROP CONSTRAINT falls back to.
				auto key = constraint->constraint_name;
				if (key.empty() && constraint->type == ConstraintType::CHECK) {
					key = constraint->Cast<CheckConstraint>().expression->ToString();
				}
				step = make_uniq<DropConstraintInfo>(data, std::move(key),
				                                     /*if_constraint_not_found=*/true, /*cascade=*/false);
				break;
			}
			break;
		}
		case DependencyPieceKind::NONE:
			break;
		}
		if (step) {
			dependent.ParentCatalog().AlterDependent(transaction, dependent, *step);
		}
	}
}

void DependencyManager::ReorderEntries(catalog_entry_vector_t &entries, ClientContext &context) {
	auto transaction = catalog.GetCatalogTransaction(context);
	// Read all the entries visible to this snapshot
	ReorderEntries(entries, transaction);
}

void DependencyManager::ReorderEntries(catalog_entry_vector_t &entries) {
	// Read all committed entries
	CatalogTransaction transaction(catalog.GetDatabase(), TRANSACTION_ID_START - 1, TRANSACTION_ID_START - 1);
	ReorderEntries(entries, transaction);
}

void DependencyManager::ReorderEntry(CatalogTransaction transaction, CatalogEntry &entry, catalog_entry_set_t &visited,
                                     catalog_entry_vector_t &order) {
	auto resolved = LookupEntry(transaction, entry);
	if (!resolved) {
		return;
	}
	auto &catalog_entry = *resolved;
	// We use this in CheckpointManager, it has the highest commit ID, allowing us to read any committed data
	bool allow_internal = transaction.start_time == TRANSACTION_ID_START - 1;
	if (visited.count(catalog_entry) || (!allow_internal && catalog_entry.internal)) {
		// Already seen and ordered appropriately
		return;
	}

	// Check if there are any entries that this entry depends on, those are written first
	catalog_entry_vector_t dependents;
	auto info = GetLookupProperties(entry);
	ScanSubjects(transaction, info, [&](DependencyEntry &dep) { dependents.push_back(dep); });
	for (auto &dep : dependents) {
		ReorderEntry(transaction, dep, visited, order);
	}

	// Then write the entry
	visited.insert(catalog_entry);
	order.push_back(catalog_entry);
}

void DependencyManager::ReorderEntries(catalog_entry_vector_t &entries, CatalogTransaction transaction) {
	catalog_entry_vector_t reordered;
	catalog_entry_set_t visited;
	for (auto &entry : entries) {
		ReorderEntry(transaction, entry, visited, reordered);
	}
	// If this would fail, that means there are more entries that we somehow reached through the dependency manager
	// but those entries should not actually be visible to this transaction
	D_ASSERT(entries.size() == reordered.size());
	entries.clear();
	entries = reordered;
}

void DependencyManager::AlterObject(CatalogTransaction transaction, CatalogEntry &old_obj, CatalogEntry &new_obj,
                                    AlterInfo &alter_info) {
	if (IsSystemEntry(new_obj)) {
		D_ASSERT(IsSystemEntry(old_obj));
		// Don't do anything for this
		return;
	}

	const auto old_info = GetLookupProperties(old_obj);
	const auto new_info = GetLookupProperties(new_obj);
	// An edge is keyed by the addresses of its two ends, so an address that did not move leaves every edge
	// pointing where it already pointed. Re-creating one chains a new version of it, which turns a concurrent
	// write of that same edge into a write-write conflict between two alters that share nothing.
	const bool moved = !(old_info == new_info);

	vector<DependencyInfo> dependencies;
	// Other entries that depend on us
	ScanDependents(transaction, old_info, [&](DependencyEntry &dep) {
		// It makes no sense to have a schema depend on anything
		D_ASSERT(dep.EntryInfo().type != CatalogType::SCHEMA_ENTRY);

		const auto rebinds = catalog.DependentsResolveByName() && alter_info.DependentCanRebind();
		const auto reshaped = catalog.ReshapesOwnedDependents() && CascadeDrop(false, dep.Dependent().flags);
		if (!rebinds && !reshaped && alter_info.BreaksDependent(dep.EntryInfo().type)) {
			throw DependencyException("Cannot alter entry \"%s\" because there are entries that "
			                          "depend on it.",
			                          old_obj.name.GetIdentifierName());
		}

		if (!moved) {
			return;
		}
		auto dep_info = DependencyInfo::FromDependent(dep);
		dep_info.subject.entry = new_info;
		dependencies.emplace_back(dep_info);
	});

	// Keep old dependencies
	bool has_new_dependencies = alter_info.new_dependencies.get();
	if (!moved) {
		// Only what this object references can have changed, and only the edges that differ are rewritten.
		if (has_new_dependencies) {
			ReplaceSubjects(transaction, new_info, EntrySubjects(new_obj, *alter_info.new_dependencies));
		}
		return;
	}
	ScanSubjects(transaction, old_info, [&](DependencyEntry &dep) {
		if (has_new_dependencies && !dep.Subject().flags.IsOwnership()) {
			// The alter provided updated dependencies - skip old non-ownership subject dependencies
			// as they will be replaced by the new dependencies
			return;
		}
		auto entry = LookupEntry(transaction, dep);
		if (!entry) {
			return;
		}

		auto dep_info = DependencyInfo::FromSubject(dep);
		dep_info.dependent.entry = new_info;
		dependencies.emplace_back(dep_info);
	});

	if (has_new_dependencies || !(old_obj.name == new_obj.name)) {
		// The dependencies have changed (e.g. SET DEFAULT) or the name has changed
		// We need to recreate the dependency links
		CleanupDependencies(transaction, old_obj);
	}

	if (has_new_dependencies) {
		// Add the new dependencies
		CreateDependencies(transaction, new_obj, EntrySubjects(new_obj, *alter_info.new_dependencies));
	}

	// Reinstate any old dependencies
	for (auto &dep : dependencies) {
		CreateDependency(transaction, dep);
	}
}

void DependencyManager::Scan(
    ClientContext &context,
    const std::function<void(CatalogEntry &, CatalogEntry &, const DependencyDependentFlags &)> &callback) {
	auto transaction = catalog.GetCatalogTransaction(context);
	lock_guard<mutex> write_lock(catalog.GetWriteLock());

	// All the objects registered in the dependency manager
	catalog_entry_set_t entries;
	dependents.Scan(transaction, [&](CatalogEntry &set) {
		auto entry = LookupEntry(transaction, set);
		if (!entry) {
			return;
		}
		entries.insert(*entry);
	});

	// For every registered entry, get the dependents
	for (auto &entry : entries) {
		auto entry_info = GetLookupProperties(entry);
		// Scan all the dependents of the entry
		ScanDependents(transaction, entry_info, [&](DependencyEntry &dependent) {
			auto dep = LookupEntry(transaction, dependent);
			if (!dep) {
				return;
			}
			auto &dependent_entry = *dep;
			callback(entry, dependent_entry, dependent.Dependent().flags);
		});
	}
}

void DependencyManager::AddOwnership(CatalogTransaction transaction, CatalogEntry &owner, CatalogEntry &entry) {
	if (IsSystemEntry(entry) || IsSystemEntry(owner)) {
		return;
	}

	// If the owner is already owned by something else, throw an error
	const auto owner_info = GetLookupProperties(owner);
	ScanDependents(transaction, owner_info, [&](DependencyEntry &dep) {
		if (dep.Dependent().flags.IsOwnedBy()) {
			throw DependencyException("%s can not become the owner, it is already owned by %s", owner.name,
			                          dep.EntryInfo().name);
		}
	});

	// If the entry is the owner of another entry, throw an error
	auto entry_info = GetLookupProperties(entry);
	ScanSubjects(transaction, entry_info, [&](DependencyEntry &other) {
		auto dependent_entry = LookupEntry(transaction, other);
		if (!dependent_entry) {
			return;
		}
		auto &dep = *dependent_entry;

		auto flags = other.Dependent().flags;
		if (!flags.IsOwnedBy()) {
			return;
		}
		throw DependencyException("%s already owns %s. Cannot have circular dependencies", entry.name, dep.name);
	});

	// If the entry is already owned, throw an error
	ScanDependents(transaction, entry_info, [&](DependencyEntry &other) {
		auto dependent_entry = LookupEntry(transaction, other);
		if (!dependent_entry) {
			return;
		}

		auto &dep = *dependent_entry;
		auto flags = other.Subject().flags;
		if (!flags.IsOwnership()) {
			return;
		}
		if (&dep != &owner) {
			throw DependencyException("%s is already owned by %s", entry.name, dep.name);
		}
	});

	DependencyInfo info {
	    /*dependent = */ DependencyDependent {GetLookupProperties(owner), DependencyDependentFlags().SetOwnedBy()},
	    /*subject = */ DependencySubject {GetLookupProperties(entry), DependencySubjectFlags().SetOwnership(),
	                                      optional_idx()}};
	CreateDependency(transaction, info);
}

static string FormatString(const MangledEntryName &mangled) {
	auto input = mangled.name.GetIdentifierName();
	for (size_t i = 0; i < input.size(); i++) {
		if (input[i] == '\0') {
			input[i] = '_';
		}
	}
	return input;
}

void DependencyManager::PrintSubjects(CatalogTransaction transaction, const CatalogEntryInfo &info) {
	auto name = MangleName(info);
	Printer::Print(StringUtil::Format("Subjects of %s", FormatString(name)));
	auto subjects = DependencyCatalogSet(Subjects(), info);
	subjects.Scan(transaction, [&](CatalogEntry &dependency) {
		auto &dep = dependency.Cast<DependencyEntry>();
		auto &entry_info = dep.EntryInfo();
		auto type = entry_info.type;
		auto schema = entry_info.schema;
		auto name = entry_info.name;
		Printer::Print(StringUtil::Format("Schema: %s | Name: %s | Type: %s | Dependent type: %s | Subject type: %s",
		                                  schema, name, CatalogTypeToString(type), dep.Dependent().flags.ToString(),
		                                  dep.Subject().flags.ToString()));
	});
}

void DependencyManager::PrintDependents(CatalogTransaction transaction, const CatalogEntryInfo &info) {
	auto name = MangleName(info);
	Printer::Print(StringUtil::Format("Dependents of %s", FormatString(name)));
	auto dependents = DependencyCatalogSet(Dependents(), info);
	dependents.Scan(transaction, [&](CatalogEntry &dependent) {
		auto &dep = dependent.Cast<DependencyEntry>();
		auto &entry_info = dep.EntryInfo();
		auto type = entry_info.type;
		auto schema = entry_info.schema;
		auto name = entry_info.name;
		Printer::Print(StringUtil::Format("Schema: %s | Name: %s | Type: %s | Dependent type: %s | Subject type: %s",
		                                  schema, name, CatalogTypeToString(type), dep.Dependent().flags.ToString(),
		                                  dep.Subject().flags.ToString()));
	});
}

} // namespace duckdb
