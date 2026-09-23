//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/schema_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

class SchemaInfo {
public:
	SchemaInfo(idx_t oid, Identifier name, shared_ptr<SchemaInfo> parent = nullptr)
	    : oid(oid), parent(std::move(parent)), name(std::move(name)) {
	}

	const idx_t oid;
	const shared_ptr<SchemaInfo> parent;

	Identifier Name() const {
		lock_guard<mutex> guard(name_lock);
		return name;
	}
	void SetName(Identifier new_name) {
		lock_guard<mutex> guard(name_lock);
		name = std::move(new_name);
	}
	vector<Identifier> Path() const {
		auto path = parent ? parent->Path() : vector<Identifier>();
		path.push_back(Name());
		return path;
	}

private:
	mutable mutex name_lock;
	Identifier name;
};

} // namespace duckdb
