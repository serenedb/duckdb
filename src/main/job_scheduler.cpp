#include "duckdb/main/job_scheduler.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/job_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parser/parsed_data/create_job_info.hpp"

namespace duckdb {

namespace {

constexpr idx_t JOB_HISTORY_CAPACITY = 1024;
constexpr int64_t JOB_RESCAN_RETRY_MICROS = 1000000;

struct FoundJob {
	idx_t job_oid;
	JobLocation location;
	JobSchedule schedule;
	bool suspended;
};

JobLocation LocationOf(JobCatalogEntry &job) {
	return JobLocation {job.ParentCatalog().GetName().GetIdentifierName(), job.ParentSchemaName().GetIdentifierName(),
	                    job.name.GetIdentifierName()};
}

void CollectJobs(ClientContext &context, Catalog &catalog, vector<FoundJob> &found) {
	catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		schema.Scan(context, CatalogType::JOB_ENTRY, [&](CatalogEntry &entry) {
			auto &job = entry.Cast<JobCatalogEntry>();
			found.push_back(FoundJob {job.oid, LocationOf(job), job.schedule, job.suspended});
		});
	});
}

class JobRunTask : public Task {
public:
	explicit JobRunTask(std::function<void()> run_p) : run(std::move(run_p)) {
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		run();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "JobRunTask";
	}

private:
	std::function<void()> run;
};

} // namespace

JobScheduler::JobScheduler(DuckCatalog &catalog)
    : db(catalog.GetDatabase()), producer(TaskScheduler::GetScheduler(catalog.GetDatabase()).CreateProducer()) {
}

JobScheduler::~JobScheduler() {
	Stop();
	Wait();
}

void JobScheduler::StopAll(DatabaseInstance &db) {
	vector<shared_ptr<JobScheduler>> schedulers;
	for (auto &database : DatabaseManager::Get(db).GetDatabases()) {
		auto &catalog = database->GetCatalog();
		if (!catalog.IsDuckCatalog()) {
			continue;
		}
		auto &scheduler = catalog.Cast<DuckCatalog>().GetJobScheduler();
		scheduler.Stop();
		schedulers.push_back(scheduler.shared_from_this());
	}
	for (auto &scheduler : schedulers) {
		scheduler->Wait();
	}
}

bool JobScheduler::RequestTickLocked(timestamp_t now) {
	if (armed && armed_at <= now) {
		return false;
	}
	armed = true;
	armed_at = now;
	return true;
}

void JobScheduler::Load(AttachedDatabase &attached, bool has_jobs) {
	auto now = Timestamp::GetCurrentTimestamp();
	{
		lock_guard<mutex> guard(lock);
		if (stopped) {
			return;
		}
		database = attached.shared_from_this();
		if (!has_jobs) {
			return;
		}
		active = true;
		dirty = true;
		if (!RequestTickLocked(now)) {
			return;
		}
	}
	Arm(now);
}

void JobScheduler::JobCreated(AttachedDatabase &attached) {
	lock_guard<mutex> guard(lock);
	if (stopped) {
		return;
	}
	if (database.expired()) {
		database = attached.shared_from_this();
	}
	active = true;
}

void JobScheduler::CatalogChanged() {
	auto now = Timestamp::GetCurrentTimestamp();
	{
		lock_guard<mutex> guard(lock);
		if (!active || stopped) {
			return;
		}
		dirty = true;
		if (!RequestTickLocked(now)) {
			return;
		}
	}
	Arm(now);
}

void JobScheduler::Stop() {
	lock_guard<mutex> guard(lock);
	stopped = true;
	for (auto &entry : jobs) {
		if (entry.second.running_context) {
			entry.second.running_context->Interrupt();
		}
	}
	timer_wakeup.SignalAll();
}

void JobScheduler::Wait() {
	std::thread timer;
	{
		unique_lock<mutex> guard(lock);
		while (in_flight > 0) {
			idle.Wait(&lock);
		}
		timer = std::move(timer_thread);
	}
	if (!timer.joinable()) {
		return;
	}
	if (timer.get_id() == std::this_thread::get_id()) {
		timer.detach();
	} else {
		timer.join();
	}
}

void JobScheduler::Arm(timestamp_t at) {
	lock_guard<mutex> guard(lock);
	if (stopped) {
		return;
	}
	if (!timer_armed || at < timer_at) {
		timer_armed = true;
		timer_at = at;
	}
	if (!timer_thread.joinable()) {
		timer_thread = std::thread([self = shared_from_this()]() { self->TimerLoop(); });
	}
	timer_wakeup.SignalAll();
}

void JobScheduler::Launch(std::function<void()> run) {
	auto &scheduler = TaskScheduler::GetScheduler(db);
	if (scheduler.NumberOfAsyncThreads() == 0) {
		run();
		return;
	}
	scheduler.ScheduleTask(*producer, make_shared_ptr<JobRunTask>(std::move(run)), TaskSchedulerType::ASYNC);
}

void JobScheduler::TimerLoop() {
	unique_lock<mutex> guard(lock);
	while (!stopped) {
		if (!timer_armed) {
			timer_wakeup.Wait(&lock);
			continue;
		}
		auto now = Timestamp::GetCurrentTimestamp();
		if (now < timer_at) {
			timer_wakeup.WaitWithTimeout(&lock, absl::Microseconds(timer_at.value - now.value));
			continue;
		}
		timer_armed = false;
		guard.unlock();
		Tick();
		guard.lock();
	}
}

void JobScheduler::Reconcile(JobState &state, JobLocation location, const JobSchedule &schedule, bool suspended,
                             timestamp_t now) {
	const bool fresh = state.generation == 0;
	const bool reschedule = fresh || !(state.schedule == schedule) || state.suspended != suspended;
	if (fresh) {
		state.generation = next_generation++;
	}
	state.location = std::move(location);
	state.schedule = schedule;
	state.suspended = suspended;
	state.seen = true;
	if (!reschedule || state.status.running) {
		return;
	}
	state.status.has_next_run = !suspended;
	if (!suspended) {
		state.status.next_run = schedule.NextRun(now);
	}
}

void JobScheduler::Rescan(AttachedDatabase &attached) {
	vector<FoundJob> found;
	try {
		Connection con(db);
		con.context->RunFunctionInTransaction([&]() { CollectJobs(*con.context, attached.GetCatalog(), found); });
	} catch (std::exception &) {
		lock_guard<mutex> guard(lock);
		has_rescan_retry = true;
		rescan_retry =
		    Timestamp::FromEpochMicroSeconds(Timestamp::GetCurrentTimestamp().value + JOB_RESCAN_RETRY_MICROS);
		return;
	}
	lock_guard<mutex> guard(lock);
	auto now = Timestamp::GetCurrentTimestamp();
	for (auto &entry : jobs) {
		entry.second.seen = false;
	}
	for (auto &job : found) {
		Reconcile(jobs[job.job_oid], std::move(job.location), job.schedule, job.suspended, now);
	}
	for (auto it = jobs.begin(); it != jobs.end();) {
		if (it->second.seen) {
			++it;
			continue;
		}
		if (it->second.running_context) {
			it->second.running_context->Interrupt();
		}
		it = jobs.erase(it);
	}
}

std::function<void()> JobScheduler::MakeRunLocked(idx_t job_oid, JobState &state) {
	state.status.running = true;
	in_flight++;
	return [self = shared_from_this(), weak = database, job_oid, generation = state.generation,
	        location = state.location]() {
		auto attached = weak.lock();
		auto outcome = self->RunJob(attached, job_oid, generation, location, false);
		self->Finish(job_oid, generation, location, false, outcome);
	};
}

void JobScheduler::Tick() {
	shared_ptr<AttachedDatabase> attached;
	{
		lock_guard<mutex> guard(lock);
		if (ticking) {
			tick_again = true;
			return;
		}
		attached = database.lock();
		if (stopped || !active || !attached) {
			return;
		}
		ticking = true;
	}
	vector<std::function<void()>> runs;
	bool has_wake = false;
	timestamp_t wake;
	while (TickOnce(*attached, runs, has_wake, wake)) {
	}
	for (auto &run : runs) {
		Launch(std::move(run));
	}
	if (has_wake) {
		Arm(wake);
	}
}

bool JobScheduler::TickOnce(AttachedDatabase &attached, vector<std::function<void()>> &runs, bool &has_wake,
                            timestamp_t &wake) {
	bool rescan;
	{
		lock_guard<mutex> guard(lock);
		auto now = Timestamp::GetCurrentTimestamp();
		if (armed && armed_at <= now) {
			armed = false;
		}
		if (has_rescan_retry && rescan_retry <= now) {
			has_rescan_retry = false;
			dirty = true;
		}
		rescan = dirty;
		dirty = false;
		tick_again = false;
	}
	if (rescan) {
		Rescan(attached);
	}
	lock_guard<mutex> guard(lock);
	if (stopped) {
		ticking = false;
		has_wake = false;
		return false;
	}
	if (tick_again || dirty) {
		return true;
	}
	auto now = Timestamp::GetCurrentTimestamp();
	has_wake = has_rescan_retry;
	wake = rescan_retry;
	for (auto &entry : jobs) {
		auto &state = entry.second;
		if (state.status.running || state.suspended || !state.status.has_next_run) {
			continue;
		}
		if (state.status.next_run <= now) {
			runs.push_back(MakeRunLocked(entry.first, state));
			continue;
		}
		if (!has_wake || state.status.next_run < wake) {
			wake = state.status.next_run;
			has_wake = true;
		}
	}
	if (has_wake && armed && armed_at <= wake) {
		has_wake = false;
	}
	if (has_wake) {
		armed = true;
		armed_at = wake;
	}
	ticking = false;
	return false;
}

JobScheduler::RunOutcome JobScheduler::RunJob(const shared_ptr<AttachedDatabase> &attached, idx_t job_oid,
                                              idx_t generation, const JobLocation &location, bool manual) {
	RunOutcome outcome;
	outcome.start = Timestamp::GetCurrentTimestamp();
	try {
		if (!attached) {
			throw CatalogException("Job \"%s\" does not exist", location.name);
		}
		Connection lookup(db);
		lookup.BeginTransaction();
		auto &catalog = attached->GetCatalog();
		auto transaction = catalog.GetCatalogTransaction(*lookup.context);
		auto schema = catalog.GetSchema(transaction, Identifier(location.schema), OnEntryNotFound::RETURN_NULL);
		auto entry =
		    schema ? schema->GetEntry(transaction, CatalogType::JOB_ENTRY, Identifier(location.name)) : nullptr;
		if (!entry || entry->oid != job_oid || (!manual && entry->Cast<JobCatalogEntry>().suspended)) {
			if (!manual) {
				lookup.Rollback();
				outcome.skipped = true;
				outcome.finish = Timestamp::GetCurrentTimestamp();
				return outcome;
			}
			throw CatalogException("Job \"%s\" does not exist", location.name);
		}
		auto info = entry->Cast<JobCatalogEntry>().GetInfo();
		JobCatalogEntry job(catalog, *schema, info->Cast<CreateJobInfo>());
		auto con = job.Connect(db);
		lookup.Commit();
		{
			lock_guard<mutex> guard(lock);
			auto state = jobs.find(job_oid);
			if (stopped || state == jobs.end() || state->second.generation != generation) {
				con->context->Interrupt();
			} else {
				state->second.running_context = con->context;
			}
		}
		con->BeginTransaction();
		job.Run(*con->context);
		con->Commit();
		outcome.success = true;
	} catch (std::exception &ex) {
		outcome.error = ErrorData(ex);
	}
	outcome.finish = Timestamp::GetCurrentTimestamp();
	return outcome;
}

void JobScheduler::Finish(idx_t job_oid, idx_t generation, const JobLocation &location, bool manual,
                          const RunOutcome &outcome) {
	bool has_arm = false;
	timestamp_t arm_at;
	{
		lock_guard<mutex> guard(lock);
		auto entry = jobs.find(job_oid);
		const bool current = entry != jobs.end() && entry->second.generation == generation;
		if (outcome.skipped) {
			if (current) {
				entry->second.running_context.reset();
				entry->second.status.running = false;
				entry->second.status.has_next_run = false;
			}
			dirty = true;
			arm_at = outcome.finish;
			has_arm = RequestTickLocked(arm_at);
		} else {
			JobRunRecord record;
			record.catalog = location.catalog;
			record.schema = location.schema;
			record.name = location.name;
			record.manual = manual;
			record.start = outcome.start;
			record.finish = outcome.finish;
			record.success = outcome.success;
			if (!outcome.success) {
				record.error = outcome.error.RawMessage();
			}
			if (history.size() < JOB_HISTORY_CAPACITY) {
				history.push_back(record);
			} else {
				history[history_next] = record;
			}
			history_next = (history_next + 1) % JOB_HISTORY_CAPACITY;
			if (current) {
				auto &state = entry->second;
				auto &status = state.status;
				state.running_context.reset();
				status.running = false;
				status.has_last_run = true;
				status.last_start = outcome.start;
				status.last_finish = outcome.finish;
				status.last_success = outcome.success;
				status.last_error = record.error;
				status.run_count++;
				if (!outcome.success) {
					status.failure_count++;
				}
				status.has_next_run = !state.suspended;
				if (!state.suspended) {
					status.next_run = state.schedule.NextRun(outcome.finish);
					if (!armed || status.next_run < armed_at) {
						armed = true;
						armed_at = status.next_run;
						arm_at = status.next_run;
						has_arm = true;
					}
				}
			}
		}
		in_flight--;
		if (in_flight == 0) {
			idle.SignalAll();
		}
		if (stopped) {
			has_arm = false;
		}
	}
	if (has_arm) {
		Arm(arm_at);
	}
}

void JobScheduler::Execute(JobCatalogEntry &job) {
	auto location = LocationOf(job);
	auto attached = job.ParentCatalog().GetAttached().shared_from_this();
	auto now = Timestamp::GetCurrentTimestamp();
	bool has_arm = false;
	idx_t generation;
	{
		lock_guard<mutex> guard(lock);
		if (stopped) {
			throw InvalidInputException("Job \"%s\" cannot run: the database is shutting down", location.name);
		}
		if (database.expired()) {
			database = attached;
		}
		auto &state = jobs[job.oid];
		if (state.generation == 0) {
			Reconcile(state, location, job.schedule, job.suspended, now);
			active = true;
			dirty = true;
			has_arm = RequestTickLocked(now);
		}
		if (state.status.running) {
			throw InvalidInputException("Job \"%s\" is already running", location.name);
		}
		state.status.running = true;
		in_flight++;
		generation = state.generation;
	}
	if (has_arm) {
		Arm(now);
	}
	auto outcome = RunJob(attached, job.oid, generation, location, true);
	Finish(job.oid, generation, location, true, outcome);
	if (!outcome.success) {
		outcome.error.Throw();
	}
}

bool JobScheduler::TryGetStatus(idx_t job_oid, JobStatus &result) {
	lock_guard<mutex> guard(lock);
	auto entry = jobs.find(job_oid);
	if (entry == jobs.end()) {
		return false;
	}
	result = entry->second.status;
	result.schedule = entry->second.schedule;
	result.suspended = entry->second.suspended;
	return true;
}

vector<JobRunRecord> JobScheduler::GetHistory() {
	lock_guard<mutex> guard(lock);
	vector<JobRunRecord> result;
	result.reserve(history.size());
	auto start = history.size() < JOB_HISTORY_CAPACITY ? 0 : history_next;
	for (idx_t i = 0; i < history.size(); i++) {
		result.push_back(history[(start + i) % history.size()]);
	}
	return result;
}

} // namespace duckdb
