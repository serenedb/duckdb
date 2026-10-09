//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/replication_origin_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/replication_lsn_entry.hpp"
#include "duckdb/parser/parsed_data/create_replication_origin_info.hpp"

namespace duckdb {

class ReplicationOriginCatalogEntry : public ReplicationLsnEntry {
public:
	static constexpr const CatalogType Type = CatalogType::REPLICATION_ORIGIN_ENTRY;
	static constexpr const char *Name = "replication origin";

	ReplicationOriginCatalogEntry(Catalog &catalog, CreateReplicationOriginInfo &info);

	unique_ptr<CreateInfo> GetInfo() const override;
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
};

} // namespace duckdb
