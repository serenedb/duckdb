#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

struct Probe {
	atomic<idx_t> running {0};
	atomic<idx_t> peak {0};
	atomic<idx_t> done {0};

	void Enter() {
		const auto now = ++running;
		auto seen = peak.load();
		while (now > seen && !peak.compare_exchange_weak(seen, now)) {
		}
	}

	void Leave() {
		--running;
		++done;
	}
};

class ProbeTask : public BaseExecutorTask {
public:
	ProbeTask(TaskExecutor &executor, Probe &probe) : BaseExecutorTask(executor), probe(probe) {
	}

	void ExecuteTask() override {
		probe.Enter();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		probe.Leave();
	}

private:
	Probe &probe;
};

struct Gate {
	mutex lock;
	bool entered = false;
	bool open = false;

	void Pass() {
		lock.Lock();
		entered = true;
		lock.AwaitWithTimeout(absl::Condition(&open), absl::Seconds(30));
		lock.Unlock();
	}

	bool AwaitEntered() {
		lock.Lock();
		const bool done = lock.AwaitWithTimeout(absl::Condition(&entered), absl::Seconds(30));
		lock.Unlock();
		return done;
	}

	void Open() {
		const absl::MutexLock guard(lock);
		open = true;
	}
};

class GateTask : public BaseExecutorTask {
public:
	GateTask(TaskExecutor &executor, Gate &gate) : BaseExecutorTask(executor), gate(gate) {
	}

	void ExecuteTask() override {
		gate.Pass();
	}

private:
	Gate &gate;
};

struct Order {
	mutex lock;
	vector<string> seen;

	void Add(string label) {
		const absl::MutexLock guard(lock);
		seen.push_back(std::move(label));
	}
};

class OrderTask : public BaseExecutorTask {
public:
	OrderTask(TaskExecutor &executor, Order &order, string label)
	    : BaseExecutorTask(executor), order(order), label(std::move(label)) {
	}

	void ExecuteTask() override {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		order.Add(label);
	}

private:
	Order &order;
	string label;
};

} // namespace

TEST_CASE("A dedicated pool runs at most its thread count of tasks at once", "[api][scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	auto pool = scheduler.CreateDedicatedPool(2);
	Probe probe;
	TaskExecutor executor(scheduler, scheduler.CreateAccount(pool));
	for (idx_t i = 0; i < 24; i++) {
		executor.ScheduleTask(make_uniq<ProbeTask>(executor, probe));
	}
	executor.AwaitTasks();
	REQUIRE(probe.done == 24);
	REQUIRE(probe.peak >= 1);
	REQUIRE(probe.peak <= 2);
}

TEST_CASE("Accounts of a dedicated pool share its threads, accounts of two pools do not", "[api][scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	auto first = scheduler.CreateDedicatedPool(1);
	auto second = scheduler.CreateDedicatedPool(1);
	Probe shared;
	Probe other;
	TaskExecutor a(scheduler, scheduler.CreateAccount(first));
	TaskExecutor b(scheduler, scheduler.CreateAccount(first));
	TaskExecutor c(scheduler, scheduler.CreateAccount(second));
	for (idx_t i = 0; i < 8; i++) {
		a.ScheduleTask(make_uniq<ProbeTask>(a, shared));
		b.ScheduleTask(make_uniq<ProbeTask>(b, shared));
		c.ScheduleTask(make_uniq<ProbeTask>(c, other));
	}
	a.AwaitTasks();
	b.AwaitTasks();
	c.AwaitTasks();
	REQUIRE(shared.done == 16);
	REQUIRE(other.done == 8);
	REQUIRE(shared.peak == 1);
	REQUIRE(other.peak == 1);
}

TEST_CASE("A dedicated pool takes its accounts in turn", "[api][scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	auto pool = scheduler.CreateDedicatedPool(1);
	Gate gate;
	Order order;
	TaskExecutor blocker(scheduler, scheduler.CreateAccount(pool));
	blocker.ScheduleTask(make_uniq<GateTask>(blocker, gate));
	REQUIRE(gate.AwaitEntered());
	TaskExecutor many(scheduler, scheduler.CreateAccount(pool));
	for (idx_t i = 0; i < 6; i++) {
		many.ScheduleTask(make_uniq<OrderTask>(many, order, "many"));
	}
	TaskExecutor one(scheduler, scheduler.CreateAccount(pool));
	one.ScheduleTask(make_uniq<OrderTask>(one, order, "one"));
	gate.Open();
	blocker.AwaitTasks();
	many.AwaitTasks();
	one.AwaitTasks();
	REQUIRE(order.seen.size() == 7);
	REQUIRE(order.seen[1] == "one");
}

TEST_CASE("A producer runs its own dedicated tasks while the pool is busy", "[api][scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	auto pool = scheduler.CreateDedicatedPool(1);
	Gate gate;
	TaskExecutor blocker(scheduler, scheduler.CreateAccount(pool));
	blocker.ScheduleTask(make_uniq<GateTask>(blocker, gate));
	REQUIRE(gate.AwaitEntered());
	Probe probe;
	{
		TaskExecutor executor(scheduler, scheduler.CreateAccount(pool));
		for (idx_t i = 0; i < 4; i++) {
			executor.ScheduleTask(make_uniq<ProbeTask>(executor, probe));
		}
		executor.WorkOnTasks();
	}
	REQUIRE(probe.done == 4);
	gate.Open();
	blocker.AwaitTasks();
}

TEST_CASE("Dedicated tasks never enter the shared queue", "[api][scheduler]") {
	DuckDB db(nullptr);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	auto pool = scheduler.CreateDedicatedPool(1);
	Gate gate;
	TaskExecutor blocker(scheduler, scheduler.CreateAccount(pool));
	blocker.ScheduleTask(make_uniq<GateTask>(blocker, gate));
	REQUIRE(gate.AwaitEntered());
	const auto before = scheduler.GetNumberOfTasks();
	Probe probe;
	TaskExecutor executor(scheduler, scheduler.CreateAccount(pool));
	for (idx_t i = 0; i < 4; i++) {
		executor.ScheduleTask(make_uniq<ProbeTask>(executor, probe));
	}
	REQUIRE(scheduler.GetNumberOfTasks() == before);
	REQUIRE(probe.done == 0);
	gate.Open();
	blocker.AwaitTasks();
	executor.AwaitTasks();
	REQUIRE(probe.done == 4);
}
