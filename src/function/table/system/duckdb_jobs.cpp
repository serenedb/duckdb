#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/job_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/job_scheduler.hpp"

namespace duckdb {

namespace {

struct DuckDBJobsData : public GlobalTableFunctionState {
	vector<reference<JobCatalogEntry>> entries;
	idx_t offset = 0;
};

struct DuckDBJobRunsData : public GlobalTableFunctionState {
	vector<JobRunRecord> runs;
	idx_t offset = 0;
};

Value OptionalTimestamp(bool valid, timestamp_t value) {
	return valid ? Value::TIMESTAMPTZ(timestamp_tz_t(value)) : Value(LogicalType::TIMESTAMP_TZ);
}

unique_ptr<FunctionData> DuckDBJobsBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("job_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("job_oid");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("schedule");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schedule_kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schedule_interval");
	return_types.emplace_back(LogicalType::INTERVAL);
	names.emplace_back("schedule_offset");
	return_types.emplace_back(LogicalType::INTERVAL);
	names.emplace_back("suspended");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("running");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("next_run");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("last_start");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("last_finish");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("last_status");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("last_error");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("run_count");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("failure_count");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("body");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBJobsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBJobsData>();
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::JOB_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry.Cast<JobCatalogEntry>()); });
	}
	return std::move(result);
}

void DuckDBJobsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBJobsData>();
	const auto now = Timestamp::GetCurrentTimestamp();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &job = data.entries[data.offset++].get();
		auto &scheduler = job.ParentCatalog().Cast<DuckCatalog>().GetJobScheduler();
		JobStatus status;
		const bool reconciled = scheduler.TryGetStatus(job.oid, status) && status.schedule == job.schedule &&
		                        status.suspended == job.suspended;
		const bool has_next_run = !job.suspended;
		const auto next_run = reconciled && status.has_next_run ? status.next_run : job.schedule.NextRun(now);
		idx_t col = 0;
		output.SetValue(col++, count, Value(job.catalog.GetName()));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(job.catalog.GetOid())));
		output.SetValue(col++, count, Value(job.ParentSchemaName()));
		output.SetValue(col++, count, Value(job.name));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(job.oid)));
		output.SetValue(col++, count, Value(job.schedule.ToString()));
		output.SetValue(col++, count, Value(job.schedule.kind == JobScheduleKind::EVERY ? "EVERY" : "AFTER"));
		output.SetValue(col++, count, job.schedule.interval);
		output.SetValue(col++, count, job.schedule.offset);
		output.SetValue(col++, count, Value::BOOLEAN(job.suspended));
		output.SetValue(col++, count, Value::BOOLEAN(job.temporary));
		output.SetValue(col++, count, Value::BOOLEAN(status.running));
		output.SetValue(col++, count, OptionalTimestamp(has_next_run, next_run));
		output.SetValue(col++, count, OptionalTimestamp(status.has_last_run, status.last_start));
		output.SetValue(col++, count, OptionalTimestamp(status.has_last_run, status.last_finish));
		output.SetValue(col++, count,
		                status.has_last_run ? Value(status.last_success ? "success" : "failed") : Value());
		output.SetValue(col++, count, status.has_last_run && !status.last_success ? Value(status.last_error) : Value());
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(status.run_count)));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(status.failure_count)));
		output.SetValue(col++, count, job.comment);
		output.SetValue(col++, count, Value(job.body));
		output.SetValue(col++, count, Value(job.ToSQL()));
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> DuckDBJobRunsBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("job_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("trigger");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("start_time");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("finish_time");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("error");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBJobRunsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBJobRunsData>();
	for (auto &database : DatabaseManager::Get(context).GetDatabases(context)) {
		auto &catalog = database->GetCatalog();
		if (!catalog.IsDuckCatalog()) {
			continue;
		}
		for (auto &run : catalog.Cast<DuckCatalog>().GetJobScheduler().GetHistory()) {
			result->runs.push_back(std::move(run));
		}
	}
	std::stable_sort(result->runs.begin(), result->runs.end(),
	                 [](const JobRunRecord &left, const JobRunRecord &right) { return left.start < right.start; });
	return std::move(result);
}

void DuckDBJobRunsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBJobRunsData>();
	idx_t count = 0;
	while (data.offset < data.runs.size() && count < STANDARD_VECTOR_SIZE) {
		auto &run = data.runs[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value(run.catalog));
		output.SetValue(col++, count, Value(run.schema));
		output.SetValue(col++, count, Value(run.name));
		output.SetValue(col++, count, Value(run.manual ? "manual" : "schedule"));
		output.SetValue(col++, count, Value::TIMESTAMPTZ(timestamp_tz_t(run.start)));
		output.SetValue(col++, count, Value::TIMESTAMPTZ(timestamp_tz_t(run.finish)));
		output.SetValue(col++, count, Value::BOOLEAN(run.success));
		output.SetValue(col++, count, run.success ? Value() : Value(run.error));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void DuckDBJobsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_jobs", {}, DuckDBJobsFunction, DuckDBJobsBind, DuckDBJobsInit));
}

void DuckDBJobRunsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_job_runs", {}, DuckDBJobRunsFunction, DuckDBJobRunsBind, DuckDBJobRunsInit));
}

struct ExecuteJobBindData : public TableFunctionData {
	explicit ExecuteJobBindData(QualifiedName name_p) : name(std::move(name_p)) {
	}

	QualifiedName name;
};

static unique_ptr<FunctionData> ExecuteJobBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	if (input.inputs[0].IsNull()) {
		throw BinderException("Job name cannot be NULL");
	}
	auto name = QualifiedName::Parse(StringValue::Get(input.inputs[0]));
	Binder::BindSchemaOrCatalog(context, name);
	auto entry = input.binder->EntryRetriever().GetEntry(EntryLookupInfo(CatalogType::JOB_ENTRY, name),
	                                                     OnEntryNotFound::THROW_EXCEPTION);
	return make_uniq<ExecuteJobBindData>(
	    QualifiedName(entry->ParentCatalog().GetName(), entry->ParentSchemaName(), entry->name));
}

static void ExecuteJobFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ExecuteJobBindData>();
	auto &job = Catalog::GetEntry<JobCatalogEntry>(context, bind_data.name);
	job.ParentCatalog().Cast<DuckCatalog>().GetJobScheduler().Execute(job);
}

void ExecuteJobFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("execute_job", {LogicalType::VARCHAR}, ExecuteJobFunction, ExecuteJobBind));
}

} // namespace duckdb
