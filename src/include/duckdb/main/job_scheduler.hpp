//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/job_scheduler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <functional>

namespace duckdb {
class AttachedDatabase;
class ClientContext;
class DatabaseInstance;
class JobCatalogEntry;
class SQLStatement;

using JobKey = pair<idx_t, idx_t>;

struct JobDefinition {
	JobKey key;
	string catalog;
	string schema;
	string name;
	idx_t owner = 0;
	vector<CatalogSearchEntry> search_path;
	shared_ptr<SQLStatement> body;
};

struct JobRunRecord {
	idx_t database_oid = 0;
	string catalog;
	string schema;
	string name;
	bool manual = false;
	timestamp_t start;
	timestamp_t finish;
	bool success = false;
	string error;
};

struct JobStatus {
	JobSchedule schedule;
	bool suspended = false;
	bool running = false;
	timestamp_t next_run;
	idx_t run_count = 0;
	idx_t failure_count = 0;
	JobRunRecord last_run;
};

class JobRuntime {
public:
	virtual ~JobRuntime() = default;
	virtual void RunAt(timestamp_t at, std::function<void()> task) = 0;
};

class JobScheduler : public enable_shared_from_this<JobScheduler> {
public:
	explicit JobScheduler(DatabaseInstance &db);

	void SetRuntime(JobRuntime &runtime);
	void Schedule(JobCatalogEntry &job);
	void Drop(JobCatalogEntry &job);
	void Execute(JobCatalogEntry &job);
	bool TryGetStatus(JobCatalogEntry &job, JobStatus &result);
	vector<JobRunRecord> GetHistory();
	void Stop();

private:
	struct Job {
		weak_ptr<AttachedDatabase> database;
		JobDefinition definition;
		JobStatus status;
		idx_t epoch = 0;
		shared_ptr<ClientContext> context;
	};

	void Run(JobKey key, idx_t epoch);
	ErrorData RunBody(unique_lock<mutex> &guard, JobDefinition job, bool manual);

private:
	DatabaseInstance &db;
	mutex lock;
	absl::CondVar idle;
	bool stopped = false;
	idx_t in_flight = 0;
	idx_t last_epoch = 0;
	optional_ptr<JobRuntime> runtime;
	unordered_map<JobKey, Job> jobs;
	deque<JobRunRecord> history;
};

} // namespace duckdb
