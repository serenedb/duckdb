//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/dependency_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/dependency.hpp"
#include "duckdb/catalog/catalog_entry_map.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/stack.hpp"
#include "duckdb/common/string_util.hpp"

#include <functional>

namespace duckdb {
class DuckCatalog;
class ClientContext;
class DependencyEntry;
class LogicalDependencyList;

// The subject of this dependency
struct DependencySubject {
	CatalogEntryInfo entry;
	//! The type of dependency this is (e.g, ownership)
	DependencySubjectFlags flags;
	//! The oid of the subject entry when the dependency was created
	optional_idx oid;
};

// The entry that relies on the other entry
struct DependencyDependent {
	CatalogEntryInfo entry;
	//! The type of dependency this is (e.g, blocking, non-blocking, ownership)
	DependencyDependentFlags flags;
	//! The pieces of the dependent that bind the subject, when the host recorded them; a cascade may
	//! trim exactly these instead of dropping the dependent whole. Rebuilt with the edges, never persisted.
	vector<DependencyPiece> pieces;
};

//! Every dependency consists of a subject (the entry being depended on) and a dependent (the entry that has the
//! dependency)
struct DependencyInfo {
public:
	static DependencyInfo FromSubject(DependencyEntry &dep);
	static DependencyInfo FromDependent(DependencyEntry &dep);

public:
	DependencyDependent dependent;
	DependencySubject subject;
};

struct MangledEntryName {
public:
	explicit MangledEntryName(const CatalogEntryInfo &info);
	MangledEntryName() = delete;

public:
	//! Format: Type\0Schema\0Name
	Identifier name;

public:
	bool operator==(const MangledEntryName &other) const {
		return other.name == name;
	}
	bool operator!=(const MangledEntryName &other) const {
		return !(*this == other);
	}
};

struct MangledDependencyName {
public:
	MangledDependencyName(const MangledEntryName &from, const MangledEntryName &to);
	MangledDependencyName() = delete;

public:
	//! Format: MangledEntryName\0MangledEntryName
	Identifier name;
};

//! The DependencyManager is in charge of managing dependencies between catalog entries
class DependencyManager {
	friend class CatalogSet;

public:
	explicit DependencyManager(DuckCatalog &catalog);

	//! Scans all dependencies, returning pairs of (object, dependent)
	void Scan(ClientContext &context,
	          const std::function<void(CatalogEntry &, CatalogEntry &, const DependencyDependentFlags &)> &callback);
	//! Removes the (dependent -> subject) dependency edge if present; one direction only, no-op when absent.
	void RemoveDependencyBetween(CatalogTransaction transaction, CatalogEntry &dependent, CatalogEntry &subject);

	void AddOwnership(CatalogTransaction transaction, CatalogEntry &owner, CatalogEntry &entry);

	//! Get the order of entries needed by EXPORT, the objects with no dependencies are exported first
	void ReorderEntries(catalog_entry_vector_t &entries);
	void ReorderEntries(catalog_entry_vector_t &entries, ClientContext &context);

	using dependency_callback_t = const std::function<void(DependencyEntry &)>;
	//! The recorded dependents of one object, for a catalog that draws its own conclusions from them.
	void ScanDependents(CatalogTransaction transaction, const CatalogEntryInfo &info, dependency_callback_t &callback);
	//! Every recorded edge exactly once, as the half that names both of its endpoints -- SourceInfo() is
	//! the subject and EntryInfo() the dependent. Unlike Scan(), which reaches an edge only when its
	//! subject is itself the dependent of something else, this is the whole graph.
	void ScanAllEdges(CatalogTransaction transaction, dependency_callback_t &callback);

	//! One edge and the entry it names as the dependent. The dependent is null when the scanning
	//! transaction cannot resolve it, so a consumer that only counts edges still sees them all.
	using dependent_callback_t = const std::function<void(optional_ptr<CatalogEntry> dependent, DependencyEntry &edge)>;

	//! The dependency managers of every attached catalog that can record foreign edges (minus `skip`),
	//! each with the transaction it is read through. An edge lives in the dependent's own catalog, so a
	//! reader has to visit them all; resolved once, because doing it per object walks the DatabaseManager
	//! per object.
	class Attachments {
	public:
		Attachments(ClientContext &context, optional_ptr<const DependencyManager> skip);

	public:
		//! The recorded dependents of one object, in every accepted attachment.
		void ScanDependents(const CatalogEntryInfo &info, dependent_callback_t &callback);
		//! Every recorded edge of every accepted attachment.
		void ScanAllEdges(dependency_callback_t &callback);

	private:
		struct Attachment {
			optional_ptr<DependencyManager> manager;
			CatalogTransaction transaction;
		};

	private:
		vector<Attachment> attachments;
	};

	//! This manager's recorded dependents of one object, plus those of every accepted sibling attachment.
	//! Without a context there are no siblings to reach: only a live statement can have recorded an edge
	//! in another attachment, and only it carries the transactions those scans need.
	void ScanDependentsEverywhere(CatalogTransaction transaction, const CatalogEntryInfo &info,
	                              dependent_callback_t &callback);

	//! The entry-addressed form of ReplaceSubjects: states the version's references and re-derives the
	//! role edges its permissions imply.
	void AddObject(CatalogTransaction transaction, CatalogEntry &object, const LogicalDependencyList &dependencies);

private:
	DuckCatalog &catalog;
	CatalogSet subjects;
	CatalogSet dependents;

private:
	void ScanSubjects(CatalogTransaction transaction, const CatalogEntryInfo &info, dependency_callback_t &callback);
	//! Replaces everything `object` depends on, leaving the entries that depend on it alone. Addressed by
	//! info rather than by entry: the caller has just written the new version, and an edge belongs to the
	//! object rather than to any one version of it.
	void ReplaceSubjects(CatalogTransaction transaction, const CatalogEntryInfo &object,
	                     const LogicalDependencyList &dependencies);
	bool IsSystemEntry(CatalogEntry &entry) const;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const LogicalDependency &dependency);
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, CatalogEntry &dependency);
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const CatalogEntryInfo &info);
	//! The catalog an info names. Null when it names an attachment this
	//! transaction cannot reach.
	optional_ptr<Catalog> ResolveCatalog(CatalogTransaction transaction, const CatalogEntryInfo &info);
	string CollectDependents(CatalogTransaction transaction, catalog_entry_set_t &entries, CatalogEntryInfo &info,
	                         catalog_entry_set_t &visited);
	void CleanupDependencies(CatalogTransaction transaction, CatalogEntry &entry);
	//! The stated references plus the role edges the entry's permissions imply, minus any self-reference.
	LogicalDependencyList EntrySubjects(CatalogEntry &object, const LogicalDependencyList &dependencies);
	//! Alters a dependent to release exactly the pieces its edges recorded -- the column, the DEFAULT,
	//! the constraint -- so it survives the drop of what it was bound to.
	void TrimDependent(CatalogTransaction transaction, CatalogEntry &dependent, const vector<DependencyPiece> &pieces);

public:
	static Identifier GetSchema(const CatalogEntry &entry);
	static MangledEntryName MangleName(const CatalogEntryInfo &info);
	static MangledEntryName MangleName(const CatalogEntry &entry);
	static CatalogEntryInfo GetLookupProperties(const CatalogEntry &entry);

private:
	void ReorderEntry(CatalogTransaction transaction, CatalogEntry &entry, catalog_entry_set_t &visited,
	                  catalog_entry_vector_t &order);
	void ReorderEntries(catalog_entry_vector_t &entries, CatalogTransaction transaction);
	void VerifyExistence(CatalogTransaction transaction, DependencyEntry &object);
	void VerifyCommitDrop(CatalogTransaction transaction, transaction_t start_time, CatalogEntry &object);
	//! Returns the objects that should be dropped alongside the object
	catalog_entry_set_t CheckDropDependencies(CatalogTransaction transaction, CatalogEntry &object, bool cascade,
	                                          catalog_entry_map_t<vector<DependencyPiece>> &pieces);
	void DropObject(CatalogTransaction transaction, CatalogEntry &object, bool cascade);
	void AlterObject(CatalogTransaction transaction, CatalogEntry &old_obj, CatalogEntry &new_obj, AlterInfo &info);

private:
	void RemoveDependency(CatalogTransaction transaction, const DependencyInfo &info);
	void CreateDependency(CatalogTransaction transaction, DependencyInfo &info);
	void CreateDependencies(CatalogTransaction transaction, const CatalogEntry &object,
	                        const LogicalDependencyList &dependencies);
	void CreateDependencies(CatalogTransaction transaction, const CatalogEntryInfo &object_info,
	                        const LogicalDependencyList &dependencies);
	using dependency_entry_func_t = const std::function<unique_ptr<DependencyEntry>(
	    Catalog &catalog, const DependencyDependent &dependent, const DependencySubject &dependency)>;

	void CreateSubject(CatalogTransaction transaction, const DependencyInfo &info);
	void CreateDependent(CatalogTransaction transaction, const DependencyInfo &info);

	void ScanSetInternal(CatalogTransaction transaction, const CatalogEntryInfo &info, bool subjects,
	                     dependency_callback_t &callback);
	void PrintSubjects(CatalogTransaction transaction, const CatalogEntryInfo &info);
	void PrintDependents(CatalogTransaction transaction, const CatalogEntryInfo &info);
	CatalogSet &Dependents();
	CatalogSet &Subjects();
};

} // namespace duckdb
