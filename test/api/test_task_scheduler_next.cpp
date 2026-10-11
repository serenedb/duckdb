#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

bool WaitFlag(const std::atomic<bool> &flag) {
	for (idx_t i = 0; i < 10000 && !flag; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return flag;
}

class RecordThreadTask : public Task {
public:
	TaskExecutionResult Execute(TaskExecutionMode) override {
		thread = std::this_thread::get_id();
		done = true;
		return TaskExecutionResult::TASK_FINISHED;
	}

	std::thread::id thread;
	std::atomic<bool> done {false};
};

class UnfinishedOnceTask : public Task {
public:
	TaskExecutionResult Execute(TaskExecutionMode) override {
		if (runs++ == 0) {
			return TaskExecutionResult::TASK_NOT_FINISHED;
		}
		done = true;
		return TaskExecutionResult::TASK_FINISHED;
	}

	std::atomic<idx_t> runs {0};
	std::atomic<bool> done {false};
};

struct ScheduleNextOptions {
	bool scoped = true;
	bool nested = false;
	bool flush = false;
	const std::atomic<bool> *wait_for = nullptr;
};

class ScheduleNextTask : public Task {
public:
	ScheduleNextTask(TaskScheduler &scheduler_p, ProducerToken &producer_p, shared_ptr<Task> next_p,
	                 ScheduleNextOptions options_p)
	    : scheduler(scheduler_p), producer(producer_p), next(std::move(next_p)), options(options_p) {
	}

	TaskExecutionResult Execute(TaskExecutionMode) override {
		TaskScheduler::ExecutingScope executing;
		thread = std::this_thread::get_id();
		if (options.nested) {
			TaskScheduler::ExecutingScope inner;
			Schedule();
		} else {
			Schedule();
		}
		if (options.flush) {
			TaskScheduler::FlushNextTask();
		}
		if (options.wait_for) {
			waited = WaitFlag(*options.wait_for);
		}
		finished = true;
		return TaskExecutionResult::TASK_FINISHED;
	}

	TaskScheduler &scheduler;
	ProducerToken &producer;
	shared_ptr<Task> next;
	ScheduleNextOptions options;
	std::atomic<bool> waited {false};
	std::atomic<bool> finished {false};
	std::thread::id thread;

private:
	void Schedule() {
		if (options.scoped) {
			TaskScheduler::NextTaskScope scope;
			scheduler.ScheduleTaskNext(producer, std::move(next));
		} else {
			scheduler.ScheduleTaskNext(producer, std::move(next));
		}
	}
};

} // namespace

TEST_CASE("A task scheduled next by a finishing worker runs on that worker", "[api][task_scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	if (scheduler.NumberOfThreads() < 2) {
		return;
	}
	auto producer = scheduler.CreateProducer();
	for (idx_t i = 0; i < 16; i++) {
		auto record = make_shared_ptr<RecordThreadTask>();
		auto first = make_shared_ptr<ScheduleNextTask>(scheduler, *producer, record, ScheduleNextOptions {});
		scheduler.ScheduleTask(*producer, first);
		REQUIRE(WaitFlag(record->done));
		REQUIRE(record->thread == first->thread);
	}
}

TEST_CASE("A task scheduled next by a worker that keeps running is queued", "[api][task_scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	if (scheduler.NumberOfThreads() < 2) {
		return;
	}
	auto producer = scheduler.CreateProducer();
	for (const auto nested : {false, true}) {
		auto record = make_shared_ptr<RecordThreadTask>();
		ScheduleNextOptions options;
		options.scoped = nested;
		options.nested = nested;
		options.wait_for = &record->done;
		auto first = make_shared_ptr<ScheduleNextTask>(scheduler, *producer, record, options);
		scheduler.ScheduleTask(*producer, first);
		REQUIRE(WaitFlag(first->finished));
		REQUIRE(first->waited);
		REQUIRE(record->thread != first->thread);
	}
}

TEST_CASE("A task scheduled next from outside a worker is queued", "[api][task_scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	if (scheduler.NumberOfThreads() < 2) {
		return;
	}
	auto producer = scheduler.CreateProducer();
	auto direct = make_shared_ptr<RecordThreadTask>();
	{
		TaskScheduler::NextTaskScope scope;
		scheduler.ScheduleTaskNext(*producer, direct);
	}
	REQUIRE(WaitFlag(direct->done));
	REQUIRE(direct->thread != std::this_thread::get_id());
}

TEST_CASE("A task scheduled next that is not finished is requeued", "[api][task_scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	if (scheduler.NumberOfThreads() < 2) {
		return;
	}
	auto producer = scheduler.CreateProducer();
	auto unfinished = make_shared_ptr<UnfinishedOnceTask>();
	scheduler.ScheduleTask(*producer,
	                       make_shared_ptr<ScheduleNextTask>(scheduler, *producer, unfinished, ScheduleNextOptions {}));
	REQUIRE(WaitFlag(unfinished->done));
	REQUIRE(unfinished->runs == 2);
}

TEST_CASE("A worker that waits after scheduling next hands the task back first", "[api][task_scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	if (scheduler.NumberOfThreads() < 2) {
		return;
	}
	auto producer = scheduler.CreateProducer();
	auto record = make_shared_ptr<RecordThreadTask>();
	ScheduleNextOptions options;
	options.flush = true;
	options.wait_for = &record->done;
	auto first = make_shared_ptr<ScheduleNextTask>(scheduler, *producer, record, options);
	scheduler.ScheduleTask(*producer, first);
	REQUIRE(WaitFlag(first->finished));
	REQUIRE(first->waited);
	REQUIRE(record->thread != first->thread);
}
