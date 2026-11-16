#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parallel/task_executor.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

constexpr idx_t EXECUTOR_TASKS = 3;

struct Rendezvous {
	mutex lock;
	idx_t started = 0;
	idx_t met = 0;
	bool all_started = false;
	bool drained = false;

	void Reset() {
		lock_guard<mutex> guard(lock);
		started = 0;
		met = 0;
		all_started = false;
		drained = false;
	}

	void Arrive() {
		lock.Lock();
		all_started = ++started == EXECUTOR_TASKS;
		if (lock.AwaitWithTimeout(absl::Condition(&all_started), absl::Milliseconds(150))) {
			met++;
		}
		lock.Unlock();
	}

	void Drained() {
		lock_guard<mutex> guard(lock);
		drained = true;
	}

	bool AwaitDrained() {
		lock.Lock();
		const bool done = lock.AwaitWithTimeout(absl::Condition(&drained), absl::Seconds(30));
		lock.Unlock();
		return done;
	}

	string Describe() {
		lock_guard<mutex> guard(lock);
		return StringUtil::Format("tasks started %llu, met all others %llu", started, met);
	}

	idx_t Met() {
		lock_guard<mutex> guard(lock);
		return met;
	}
};

Rendezvous rendezvous;

class ArriveTask : public BaseExecutorTask {
public:
	explicit ArriveTask(TaskExecutor &executor) : BaseExecutorTask(executor) {
	}

	void ExecuteTask() override {
		rendezvous.Arrive();
	}
};

struct DrainState : GlobalTableFunctionState {
	bool drained = false;
};

unique_ptr<FunctionData> DrainBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                   vector<Identifier> &names) {
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("i");
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DrainInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DrainState>();
}

void DrainExecutor(ClientContext &context, TableFunctionInput &input, DataChunk &) {
	auto &state = input.global_state->Cast<DrainState>();
	if (state.drained) {
		return;
	}
	state.drained = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	TaskExecutor executor(context);
	for (idx_t i = 0; i < EXECUTOR_TASKS; i++) {
		executor.ScheduleTask(make_uniq<ArriveTask>(executor));
	}
	executor.WorkOnTasks();
	rendezvous.Drained();
}

} // namespace

TEST_CASE("Tasks a worker schedules on a TaskExecutor run on idle workers while it drains them", "[api][executor]") {
	rendezvous.Reset();
	DBConfig config;
	config.options.maximum_threads = EXECUTOR_TASKS + 2;
	DuckDB db(nullptr, &config);
	Connection con(db);
	ExtensionLoader loader(*db.instance, "task_executor_concurrency");
	loader.RegisterFunction(TableFunction("drain_executor", {}, DrainExecutor, DrainBind, DrainInitGlobal));

	auto result = con.Submit("SELECT count(*) FROM drain_executor()");
	REQUIRE(!result->HasError());
	const bool drained = rendezvous.AwaitDrained();
	INFO(rendezvous.Describe());
	REQUIRE(drained);
	result->Complete();
	REQUIRE(!result->HasError());
	REQUIRE(rendezvous.Met() == EXECUTOR_TASKS);
}

TEST_CASE("A TaskExecutor keeps no more tasks unfinished than the limit they are scheduled with", "[api][executor]") {
	constexpr idx_t LIMIT = 3;
	constexpr idx_t TASKS = 40;
	for (const idx_t threads : {idx_t(1), idx_t(8)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
		scheduler.SetAsyncThreads(threads == 1 ? 0 : 4);
		TaskExecutor executor(scheduler, TaskSchedulerType::ASYNC);
		atomic<idx_t> running {0};
		atomic<idx_t> most {0};
		atomic<idx_t> ran {0};
		for (idx_t i = 0; i < TASKS; i++) {
			executor.ScheduleTask(
			    [&] {
				    const auto now = running.fetch_add(1) + 1;
				    auto seen = most.load();
				    while (seen < now && !most.compare_exchange_weak(seen, now)) {
				    }
				    std::this_thread::sleep_for(std::chrono::milliseconds(1));
				    running.fetch_sub(1);
				    ran.fetch_add(1);
			    },
			    "LimitedTask", LIMIT);
		}
		executor.Join();
		REQUIRE(ran.load() == TASKS);
		REQUIRE(most.load() <= LIMIT);
		REQUIRE(!executor.HasError());
	}
}

TEST_CASE("A joined TaskExecutor keeps the error of a failed task for its owner", "[api][executor]") {
	DuckDB db(nullptr);
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance), TaskSchedulerType::ASYNC);
	atomic<idx_t> ran {0};
	executor.ScheduleTask([] { throw IOException("the first task fails"); }, "FailingTask");
	executor.Join();
	REQUIRE(executor.HasError());
	executor.ScheduleTask([&] { ran.fetch_add(1); }, "RetiredTask");
	executor.Join();
	REQUIRE(ran.load() == 0);
	REQUIRE_THROWS_AS(executor.WorkOnTasks(), IOException);
}
