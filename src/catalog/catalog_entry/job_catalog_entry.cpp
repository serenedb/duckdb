#include "duckdb/catalog/catalog_entry/job_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/alter_job_info.hpp"
#include "duckdb/parser/parsed_data/create_job_info.hpp"

namespace duckdb {

constexpr const char *JobCatalogEntry::Name;

JobCatalogEntry::JobCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateJobInfo &info)
    : StandardEntry(CatalogType::JOB_ENTRY, schema, catalog, info.GetJobName(), info.oid), schedule(info.schedule),
      suspended(info.suspended), body(info.body->Copy()), search_path(info.search_path) {
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
}

unique_ptr<CatalogEntry> JobCatalogEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	if (info.type == AlterType::CHANGE_OWNERSHIP) {
		return Copy(context);
	}
	if (info.type != AlterType::ALTER_JOB) {
		return CatalogEntry::AlterEntry(context, info);
	}
	auto &alter = info.Cast<AlterJobInfo>();
	auto result = Copy(context);
	auto &job = result->Cast<JobCatalogEntry>();
	switch (alter.alter_job_type) {
	case AlterJobType::SUSPEND:
		job.suspended = true;
		break;
	case AlterJobType::RESUME:
		job.suspended = false;
		break;
	case AlterJobType::SET_SCHEDULE:
		alter.schedule.Verify();
		job.schedule = alter.schedule;
		break;
	}
	return result;
}

unique_ptr<CatalogEntry> JobCatalogEntry::Copy(ClientContext &context) const {
	auto info = GetInfo();
	return make_uniq<JobCatalogEntry>(catalog, ParentSchema(context), info->Cast<CreateJobInfo>());
}

unique_ptr<CreateInfo> JobCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateJobInfo>();
	result->SetQualifiedName(QualifiedName(catalog.GetName(), ParentSchemaName(), name));
	result->schedule = schedule;
	result->suspended = suspended;
	result->body = body->Copy();
	result->search_path = search_path;
	result->temporary = temporary;
	result->internal = internal;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->permissions = permissions;
	return std::move(result);
}

} // namespace duckdb
