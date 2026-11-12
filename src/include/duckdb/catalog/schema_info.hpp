//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/schema_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_versions.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

class SchemaInfo {
public:
	SchemaInfo(idx_t oid, Identifier name, shared_ptr<SchemaInfo> parent = nullptr)
	    : oid(oid), parent(std::move(parent)), versions(make_shared_ptr<CatalogVersions>()),
	      created_name(std::move(name)) {
	}

	const idx_t oid;
	const shared_ptr<SchemaInfo> parent;
	const shared_ptr<CatalogVersions> versions;

	Identifier Name() const {
		auto committed = versions->GetCommitted();
		return committed ? committed->name : created_name;
	}
	Identifier Name(const SnapshotView &view) const {
		auto visible = versions->GetVisible(view);
		return visible ? visible->name : Name();
	}
	vector<Identifier> Path() const {
		auto path = parent ? parent->Path() : vector<Identifier>();
		path.push_back(Name());
		return path;
	}
	vector<Identifier> Path(const SnapshotView &view) const {
		auto path = parent ? parent->Path(view) : vector<Identifier>();
		path.push_back(Name(view));
		return path;
	}
	bool HasPendingVersion() const {
		return versions->HasPending() || (parent && parent->HasPendingVersion());
	}

private:
	const Identifier created_name;
};

} // namespace duckdb
