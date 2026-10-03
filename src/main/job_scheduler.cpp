#include "duckdb/main/job_scheduler.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/job_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

constexpr idx_t JOB_HISTORY_CAPACITY = 1024;

JobKey KeyOf(JobCatalogEntry &job) {
	return JobKey(job.ParentCatalog().GetOid(), job.oid);
}

JobDefinition DefinitionOf(JobCatalogEntry &job) {
	auto &catalog = job.ParentCatalog();
	JobDefinition result;
	result.key = KeyOf(job);
	result.catalog = catalog.GetName().GetIdentifierName();
	result.schema = job.ParentSchemaName().GetIdentifierName();
	result.name = job.name.GetIdentifierName();
	result.owner = job.permissions.owner;
	result.search_path =
	    job.temporary ? job.search_path
	                  : vector<CatalogSearchEntry> {CatalogSearchEntry(catalog.GetName(), job.ParentSchemaName())};
	result.body = shared_ptr<SQLStatement>(job.body->Copy());
	return result;
}

} // namespace

JobScheduler::JobScheduler(DatabaseInstance &db_p) : db(db_p) {
}

void JobScheduler::SetRuntime(JobRuntime &runtime_p) {
	lock_guard<mutex> guard(lock);
	runtime = runtime_p;
}

void JobScheduler::Schedule(JobCatalogEntry &entry) {
	auto definition = DefinitionOf(entry);
	auto database = entry.ParentCatalog().GetAttached().shared_from_this();
	auto now = Timestamp::GetCurrentTimestamp();
	lock_guard<mutex> guard(lock);
	if (stopped) {
		return;
	}
	if (entry.temporary) {
		erase_if(jobs, [](const pair<const JobKey, Job> &job) { return job.second.database.expired(); });
	}
	auto inserted = jobs.emplace(definition.key, Job());
	auto &job = inserted.first->second;
	auto &status = job.status;
	const bool reschedule =
	    inserted.second || !(status.schedule == entry.schedule) || status.suspended != entry.suspended;
	job.database = database;
	job.definition = std::move(definition);
	status.schedule = entry.schedule;
	status.suspended = entry.suspended;
	if (!reschedule) {
		return;
	}
	job.epoch = ++last_epoch;
	status.next_run = entry.schedule.NextRun(now);
	if (runtime && !entry.suspended) {
		runtime->RunAt(status.next_run, [self = shared_from_this(), key = inserted.first->first, epoch = job.epoch]() {
			self->Run(key, epoch);
		});
	}
}

void JobScheduler::Drop(JobCatalogEntry &entry) {
	lock_guard<mutex> guard(lock);
	auto job = jobs.find(KeyOf(entry));
	if (job == jobs.end()) {
		return;
	}
	if (job->second.context) {
		job->second.context->Interrupt();
	}
	jobs.erase(job);
}

void JobScheduler::Run(JobKey key, idx_t epoch) {
	unique_lock<mutex> guard(lock);
	auto job = jobs.find(key);
	if (stopped || job == jobs.end() || job->second.epoch != epoch) {
		return;
	}
	if (job->second.database.expired()) {
		jobs.erase(job);
		return;
	}
	auto now = Timestamp::GetCurrentTimestamp();
	auto &status = job->second.status;
	if (status.running) {
		status.next_run = status.schedule.NextRun(now);
	} else if (now >= status.next_run) {
		RunBody(guard, job->second.definition, false);
		guard.lock();
		job = jobs.find(key);
		if (stopped || job == jobs.end() || job->second.epoch != epoch) {
			return;
		}
	}
	runtime->RunAt(job->second.status.next_run, [self = shared_from_this(), key, epoch]() { self->Run(key, epoch); });
}

void JobScheduler::Execute(JobCatalogEntry &entry) {
	auto definition = DefinitionOf(entry);
	unique_lock<mutex> guard(lock);
	auto job = jobs.find(definition.key);
	if (job != jobs.end() && job->second.status.running) {
		throw InvalidInputException("Job \"%s\" is already running", definition.name);
	}
	auto error = RunBody(guard, std::move(definition), true);
	if (error.HasError()) {
		error.Throw();
	}
}

ErrorData JobScheduler::RunBody(unique_lock<mutex> &guard, JobDefinition job, bool manual) {
	auto entry = jobs.find(job.key);
	if (entry != jobs.end()) {
		entry->second.status.running = true;
	}
	in_flight++;
	guard.unlock();
	auto start = Timestamp::GetCurrentTimestamp();
	ErrorData error;
	shared_ptr<ClientContext> context;
	try {
		context = make_shared_ptr<ClientContext>(db.shared_from_this());
		context->login_role = context->session_role = context->effective_role = job.owner;
		if (!job.search_path.empty()) {
			ClientData::Get(*context).catalog_search_path->Set(job.search_path, CatalogSetPathType::SET_DIRECTLY);
		}
		Connection connection(context);
		guard.lock();
		entry = jobs.find(job.key);
		if (entry != jobs.end()) {
			entry->second.context = context;
		}
		if (stopped || (entry == jobs.end() && !manual)) {
			context->Interrupt();
		}
		guard.unlock();
		if (context->IsInterrupted()) {
			throw InterruptException();
		}
		auto result = context->Query(job.body->Copy(), false);
		if (result->HasError()) {
			result->ThrowError();
		}
	} catch (std::exception &ex) {
		error = ErrorData(ex);
	}
	JobRunRecord run {job.key.first,
	                  job.catalog,
	                  job.schema,
	                  job.name,
	                  manual,
	                  start,
	                  Timestamp::GetCurrentTimestamp(),
	                  !error.HasError(),
	                  error.HasError() ? error.RawMessage() : string()};
	guard.lock();
	history.push_back(run);
	if (history.size() > JOB_HISTORY_CAPACITY) {
		history.pop_front();
	}
	entry = jobs.find(job.key);
	if (entry != jobs.end()) {
		auto &status = entry->second.status;
		status.running = false;
		status.next_run = status.schedule.NextRun(run.finish);
		status.run_count++;
		if (!run.success) {
			status.failure_count++;
		}
		status.last_run = run;
		entry->second.context.reset();
	}
	if (--in_flight == 0) {
		idle.SignalAll();
	}
	guard.unlock();
	return error;
}

bool JobScheduler::TryGetStatus(JobCatalogEntry &entry, JobStatus &result) {
	lock_guard<mutex> guard(lock);
	auto job = jobs.find(KeyOf(entry));
	if (job == jobs.end()) {
		return false;
	}
	result = job->second.status;
	return true;
}

vector<JobRunRecord> JobScheduler::GetHistory() {
	lock_guard<mutex> guard(lock);
	return vector<JobRunRecord>(history.begin(), history.end());
}

void JobScheduler::Stop() {
	unique_lock<mutex> guard(lock);
	stopped = true;
	for (auto &job : jobs) {
		if (job.second.context) {
			job.second.context->Interrupt();
		}
	}
	while (in_flight > 0) {
		idle.Wait(&lock);
	}
}

} // namespace duckdb
