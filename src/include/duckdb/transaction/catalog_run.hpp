//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/catalog_run.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"

namespace duckdb {

struct CatalogRunEntry {
	CatalogRunEntry(CatalogEntry &entry, unique_ptr<ParseInfo> alter_info)
	    : entry(entry), alter_info(std::move(alter_info)) {
	}

	reference<CatalogEntry> entry;
	unique_ptr<ParseInfo> alter_info;
};

} // namespace duckdb
