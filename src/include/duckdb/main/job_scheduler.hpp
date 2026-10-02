//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/job_scheduler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/job_schedule.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"

#include <functional>
#include <thread>

namespace duckdb {
class AttachedDatabase;
class ClientContext;
class DatabaseInstance;
class DuckCatalog;
class JobCatalogEntry;
struct ProducerToken;

struct JobStatus {
	JobSchedule schedule;
	bool suspended = false;
	bool running = false;
	bool has_next_run = false;
	timestamp_t next_run;
	bool has_last_run = false;
	timestamp_t last_start;
	timestamp_t last_finish;
	bool last_success = false;
	string last_error;
	idx_t run_count = 0;
	idx_t failure_count = 0;
};

struct JobLocation {
	string catalog;
	string schema;
	string name;
};

struct JobRunRecord {
	string catalog;
	string schema;
	string name;
	bool manual = false;
	timestamp_t start;
	timestamp_t finish;
	bool success = false;
	string error;
};

class JobScheduler : public enable_shared_from_this<JobScheduler> {
public:
	explicit JobScheduler(DuckCatalog &catalog);
	~JobScheduler();

	static void StopAll(DatabaseInstance &db);

	void Load(AttachedDatabase &attached, bool has_jobs);
	void JobCreated(AttachedDatabase &attached);
	void CatalogChanged();
	void Execute(JobCatalogEntry &job);
	bool TryGetStatus(idx_t job_oid, JobStatus &result);
	vector<JobRunRecord> GetHistory();
	void Tick();
	void Stop();
	void Wait();

private:
	struct JobState {
		JobLocation location;
		JobSchedule schedule;
		bool suspended = false;
		idx_t generation = 0;
		bool seen = false;
		shared_ptr<ClientContext> running_context;
		JobStatus status;
	};
	struct RunOutcome {
		timestamp_t start;
		timestamp_t finish;
		bool success = false;
		bool skipped = false;
		ErrorData error;
	};

	bool TickOnce(AttachedDatabase &attached, vector<std::function<void()>> &runs, bool &has_wake, timestamp_t &wake);
	bool RequestTickLocked(timestamp_t now);
	void Rescan(AttachedDatabase &attached);
	void Reconcile(JobState &state, JobLocation location, const JobSchedule &schedule, bool suspended, timestamp_t now);
	std::function<void()> MakeRunLocked(idx_t job_oid, JobState &state);
	RunOutcome RunJob(const shared_ptr<AttachedDatabase> &attached, idx_t job_oid, idx_t generation,
	                  const JobLocation &location, bool manual);
	void Finish(idx_t job_oid, idx_t generation, const JobLocation &location, bool manual, const RunOutcome &outcome);
	void Arm(timestamp_t at);
	void Launch(std::function<void()> run);
	void TimerLoop();

private:
	DatabaseInstance &db;
	weak_ptr<AttachedDatabase> database;
	mutex lock;
	absl::CondVar idle;
	bool active = false;
	bool stopped = false;
	bool dirty = false;
	bool ticking = false;
	bool tick_again = false;
	bool has_rescan_retry = false;
	timestamp_t rescan_retry;
	bool armed = false;
	timestamp_t armed_at;
	idx_t in_flight = 0;
	idx_t next_generation = 1;
	map<idx_t, JobState> jobs;
	vector<JobRunRecord> history;
	idx_t history_next = 0;
	unique_ptr<ProducerToken> producer;
	absl::CondVar timer_wakeup;
	bool timer_armed = false;
	timestamp_t timer_at;
	std::thread timer_thread;
};

} // namespace duckdb
