//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/job_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/catalog/standard_entry.hpp"

namespace duckdb {
class Connection;
struct CreateJobInfo;
class DatabaseInstance;

class JobCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::JOB_ENTRY;
	static constexpr const char *Name = "job";

public:
	JobCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateJobInfo &info);

	JobSchedule schedule;
	bool suspended;
	string body;
	vector<CatalogSearchEntry> search_path;

public:
	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	unique_ptr<CreateInfo> GetInfo() const override;
	string ToSQL() const override;

	unique_ptr<Connection> Connect(DatabaseInstance &db) const;
	void Run(ClientContext &context) const;
};

} // namespace duckdb
