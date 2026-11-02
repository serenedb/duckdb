#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <chrono>
#include <fstream>
#include <thread>
#ifdef __linux__
#include <dirent.h>
#endif

using namespace duckdb;

void run_query_multiple_times(duckdb::unique_ptr<string> query, duckdb::unique_ptr<Connection> con) {
	for (int i = 0; i < 10; ++i) {
		auto result = con->Query(*query);
	}
}

void change_thread_counts(duckdb::DuckDB &db) {
	auto con = Connection(db);
	for (int i = 0; i < 10; ++i) {
		con.Query("SET GLOBAL threads=10");
		con.Query("SET GLOBAL threads=1");
	}
}

// NumberOfThreads acquired the same lock as RelaunchThreads
// NumberOfThreads is waiting for the lock
// RelaunchThreads is waiting on the thread to finish, while holding the lock
TEST_CASE("Test deadlock issue between NumberOfThreads and RelaunchThreads", "[api]") {
	duckdb::DuckDB db(nullptr);

	int thread_count = 10;
	std::vector<std::thread> threads(thread_count);

	// This query will hit NumberOfThreads because it uses the RadixPartitionedHashtable
	for (int i = 0; i < thread_count; ++i) {
		auto query = make_uniq<string>(R"(
			WITH dataset AS (
			  SELECT * FROM (VALUES
				(1, 'Alice'),
				(2, 'Bob'),
				(3, 'Alice'),
				(4, 'Carol')
			  ) AS t(id, name)
			)
			SELECT DISTINCT name FROM dataset;
		)");

		threads[i] = std::thread(run_query_multiple_times, std::move(query), make_uniq<Connection>(db));
	}

	// Fire off queries that change the thread count,
	// causing us to relaunch the worker threads on every subsequent query.
	change_thread_counts(db);

	for (int i = 0; i < thread_count; ++i) {
		threads[i].join();
	}
}

TEST_CASE("Test database maximum_threads argument", "[api]") {
	// default is number of hw threads
	// FIXME: not yet
	{
		DuckDB db(nullptr);
		auto file_system = make_uniq<VirtualFileSystem>();
		REQUIRE(db.NumberOfThreads() == DBConfig().GetSystemMaxThreads(*file_system));
	}
	// but we can set another value
	{
		DBConfig config;
		config.options.maximum_threads = 10;
		DuckDB db(nullptr, &config);
		REQUIRE(db.NumberOfThreads() == 10);
	}
	// zero is not erlaubt
	{
		DBConfig config;
		config.options.maximum_threads = 0;
		DuckDB db;
		REQUIRE_THROWS(db = DuckDB(nullptr, &config));
	}
}

TEST_CASE("Test external threads", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = DBConfig::GetConfig(*db.instance);
	auto options = config.GetOptions();

	con.Query("SET GLOBAL threads=13");
	REQUIRE(config.options.maximum_threads == 13);
	REQUIRE(db.NumberOfThreads() == 13);

	auto res = con.Query("SET external_threads=5");
	REQUIRE(res->HasError());
	REQUIRE(StringUtil::Contains(res->GetError(), "cannot be changed"));
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 1);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("RESET GLOBAL threads");
	auto file_system = make_uniq<VirtualFileSystem>();
	REQUIRE(config.options.maximum_threads == DBConfig().GetSystemMaxThreads(*file_system));
	REQUIRE(db.NumberOfThreads() == DBConfig().GetSystemMaxThreads(*file_system));
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Test async threads", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);

	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == scheduler.NumberOfAsyncThreads());

	con.Query("SET GLOBAL async_threads=0");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == 0);

	con.Query("SET GLOBAL async_threads=2");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == 2);

	con.Query("SET async_threads=1");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == 1);
	REQUIRE(TaskScheduler::QueryAsyncPool(*con.context) == TaskSchedulerType::ASYNC);

	con.Query("SET async_threads=0");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == 0);
	REQUIRE(TaskScheduler::QueryAsyncPool(*con.context) == TaskSchedulerType::REGULAR);

	Connection other(db);
	REQUIRE(TaskScheduler::QueryAsyncThreads(*other.context) == 2);

	con.Query("SET async_threads=4");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == 4);

	con.Query("RESET async_threads");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == 2);

	con.Query("RESET GLOBAL async_threads");
	REQUIRE(TaskScheduler::QueryAsyncThreads(*con.context) == scheduler.NumberOfAsyncThreads());
}

static idx_t ColumnDataScanMaxThreads(Connection &con, QueryResult &result,
                                      const OperatorPartitionInfo &partition_info) {
	PhysicalPlan physical_plan(Allocator::Get(*con.context));
	auto &scan = physical_plan.Make<PhysicalColumnDataScan>(result.GetTypes(), PhysicalOperatorType::COLUMN_DATA_SCAN,
	                                                        result.Collection().Count(), result.Collection());
	auto source_state = scan.GetGlobalSourceState(*con.context, partition_info);
	return source_state->MaxThreads();
}

#if STANDARD_VECTOR_SIZE <= 4096
TEST_CASE("Column data scan batch sizes preserve source parallelism", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));

	const idx_t small_count = STANDARD_VECTOR_SIZE * 16;
	auto small_result = con.Query(StringUtil::Format("SELECT i FROM range(%llu) t(i)", small_count));
	REQUIRE(!small_result->HasError());
	auto &small_materialized = *small_result;

	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized, OperatorPartitionInfo::NoPartitionInfo()) == 16);
	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized, OperatorPartitionInfo::BatchIndex()) == 8);
	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized,
	                                 OperatorPartitionInfo::BatchIndex(DEFAULT_ROW_GROUP_SIZE)) == 8);

#if STANDARD_VECTOR_SIZE >= 512
	const idx_t large_count = DEFAULT_ROW_GROUP_SIZE * 8 + STANDARD_VECTOR_SIZE;
	auto large_result = con.Query(StringUtil::Format("SELECT i FROM range(%llu) t(i)", large_count));
	REQUIRE(!large_result->HasError());
	auto &large_materialized = *large_result;
	REQUIRE(ColumnDataScanMaxThreads(con, large_materialized, OperatorPartitionInfo::BatchIndex()) == 9);
	REQUIRE(ColumnDataScanMaxThreads(con, large_materialized,
	                                 OperatorPartitionInfo::BatchIndex(DEFAULT_ROW_GROUP_SIZE)) == 9);
#endif
}
#endif
#endif

#ifdef DUCKDB_NO_THREADS
TEST_CASE("Test scheduling with no threads", "[api]") {
	DuckDB db(nullptr);
	Connection con1(db);
	Connection con2(db);

	const auto query_1 = con1.Submit("SELECT 42");
	const auto query_2 = con2.Submit("SELECT 42");
	// Get the completed pipelines. Because "executeTask" was never called, there should be no completed pipelines.
	auto query_1_pipelines = con1.context->GetExecutor().GetCompletedPipelines();
	REQUIRE((query_1_pipelines == 0));

	// Execute the second query
	query_2->Complete();
	REQUIRE_NO_FAIL(*query_2);

	// And even after that, there should still be no completed pipelines for the first query.
	query_1_pipelines = con1.context->GetExecutor().GetCompletedPipelines();
	REQUIRE((query_1_pipelines == 0));
	query_1->Complete();
	REQUIRE_NO_FAIL(*query_1);
}
#endif

#ifdef __linux__
static idx_t CountProcessThreads() {
	idx_t count = 0;
	auto dir = opendir("/proc/self/task");
	if (!dir) {
		return 0;
	}
	while (auto entry = readdir(dir)) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		std::ifstream comm(string("/proc/self/task/") + entry->d_name + "/comm");
		string name;
		std::getline(comm, name);
		if (name != "jemalloc_bg_thd") {
			count++;
		}
	}
	closedir(dir);
	return count;
}

static bool AwaitThreadsAtMost(idx_t threads) {
	for (idx_t i = 0; i < 3000; i++) {
		if (CountProcessThreads() <= threads) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

static idx_t ThreadsWhileCallsWait(TaskScheduler &scheduler, idx_t calls) {
	mutex lock;
	bool release = false;
	atomic<idx_t> waiting {0};
	TaskExecutor executor(scheduler, scheduler.CreateAccount(SchedulerClass::FOREGROUND));
	for (idx_t i = 0; i < calls; i++) {
		executor.ScheduleTask(
		    [&]() {
			    TaskScheduler::BlockingScope blocking;
			    waiting++;
			    lock.Lock();
			    lock.Await(absl::Condition(&release));
			    lock.Unlock();
		    },
		    "WaitingCall");
	}
	for (idx_t i = 0; i < 3000 && waiting.load() < calls; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(waiting.load() == calls);
	const auto threads = CountProcessThreads();
	{
		lock_guard<mutex> guard(lock);
		release = true;
	}
	executor.WorkOnTasks();
	return threads;
}

TEST_CASE("Waiting calls reuse parked spares and the spares beyond the pool size exit", "[api]") {
	DBConfig config;
	config.options.maximum_threads = 3;
	DuckDB db(nullptr, &config);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	constexpr idx_t pool = 2;
	const auto before = CountProcessThreads();

	REQUIRE(ThreadsWhileCallsWait(scheduler, 8) >= before + 8 - pool);
	REQUIRE(AwaitThreadsAtMost(before + pool));

	REQUIRE(ThreadsWhileCallsWait(scheduler, 2 * pool) < before + 2 * pool);
	REQUIRE(AwaitThreadsAtMost(before + pool));
}

TEST_CASE("Waiting calls that wake no worker start no thread and leave their slots to new work", "[api]") {
	DBConfig config;
	config.options.maximum_threads = 3;
	DuckDB db(nullptr, &config);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	constexpr idx_t pool = 2;
	const auto before = CountProcessThreads();
	mutex lock;
	bool release = false;
	atomic<idx_t> waiting {0};
	atomic<bool> ran {false};
	TaskExecutor executor(scheduler, scheduler.CreateAccount(SchedulerClass::FOREGROUND));
	for (idx_t i = 0; i < pool; i++) {
		executor.ScheduleTask(
		    [&]() {
			    TaskScheduler::BlockingScope blocking {false};
			    waiting++;
			    lock.Lock();
			    lock.Await(absl::Condition(&release));
			    lock.Unlock();
		    },
		    "SilentWait");
	}
	for (idx_t i = 0; i < 3000 && waiting.load() < pool; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(waiting.load() == pool);
	REQUIRE(CountProcessThreads() == before);

	executor.ScheduleTask([&]() { ran = true; }, "NewWork");
	for (idx_t i = 0; i < 3000 && !ran.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(ran.load());
	REQUIRE(waiting.load() == pool);
	{
		lock_guard<mutex> guard(lock);
		release = true;
	}
	executor.WorkOnTasks();
}
#endif

#ifndef DUCKDB_NO_THREADS
class SetFlagTask : public BaseExecutorTask {
public:
	SetFlagTask(TaskExecutor &executor, atomic<bool> &flag_p) : BaseExecutorTask(executor), flag(flag_p) {
	}
	void ExecuteTask() override {
		flag = true;
	}

private:
	atomic<bool> &flag;
};

TEST_CASE("A started task runs at once while every worker waits on a lock", "[api]") {
	DBConfig config;
	config.options.maximum_threads = 2;
	DuckDB db(nullptr, &config);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	mutex lock;
	bool release = false;
	atomic<bool> occupied {false};
	TaskExecutor busy(scheduler, scheduler.CreateAccount(SchedulerClass::FOREGROUND));
	busy.ScheduleTask(
	    [&]() {
		    occupied = true;
		    lock.Lock();
		    lock.Await(absl::Condition(&release));
		    lock.Unlock();
	    },
	    "OccupyWorker");
	for (idx_t i = 0; i < 3000 && !occupied.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(occupied.load());

	atomic<bool> ran {false};
	TaskExecutor waited(scheduler);
	unique_ptr<BaseExecutorTask> task = make_uniq<SetFlagTask>(waited, ran);
	REQUIRE(waited.StartTask(task));
	REQUIRE(!task);
	for (idx_t i = 0; i < 3000 && !ran.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const bool in_time = ran.load();
	{
		lock_guard<mutex> guard(lock);
		release = true;
	}
	busy.WorkOnTasks();
	waited.WorkOnTasks();
	REQUIRE(in_time);
}

TEST_CASE("Without workers a task does not start and stays with its caller", "[api]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	atomic<bool> ran {false};
	TaskExecutor executor(scheduler);
	unique_ptr<BaseExecutorTask> task = make_uniq<SetFlagTask>(executor, ran);
	REQUIRE(!executor.StartTask(task));
	REQUIRE(task);
	executor.WorkOnTasks();
	REQUIRE(!ran.load());
}
#endif
