#include "duckdb/catalog/catalog_entry/job_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/alter_job_info.hpp"
#include "duckdb/parser/parsed_data/create_job_info.hpp"

namespace duckdb {

constexpr const char *JobCatalogEntry::Name;

JobCatalogEntry::JobCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateJobInfo &info)
    : StandardEntry(CatalogType::JOB_ENTRY, schema, catalog, info.GetJobName(), info.oid), schedule(info.schedule),
      suspended(info.suspended), body(info.body), search_path(info.search_path) {
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
}

unique_ptr<CatalogEntry> JobCatalogEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
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
	result->body = body;
	result->search_path = search_path;
	result->temporary = temporary;
	result->internal = internal;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->permissions = permissions;
	return std::move(result);
}

string JobCatalogEntry::ToSQL() const {
	return GetInfo()->ToString();
}

unique_ptr<Connection> JobCatalogEntry::Connect(DatabaseInstance &db) const {
	auto context = make_shared_ptr<ClientContext>(db.shared_from_this());
	context->login_role = context->session_role = context->effective_role = permissions.owner;
	auto &path = *ClientData::Get(*context).catalog_search_path;
	if (!temporary) {
		path.Set(CatalogSearchEntry(catalog.GetName(), ParentSchemaName()), CatalogSetPathType::SET_DIRECTLY);
	} else if (!search_path.empty()) {
		path.Set(search_path, CatalogSetPathType::SET_DIRECTLY);
	}
	return make_uniq<Connection>(std::move(context));
}

void JobCatalogEntry::Run(ClientContext &context) const {
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
	auto result = context.Query(body, false);
	if (result->HasError()) {
		result->ThrowError();
	}
}

} // namespace duckdb
