#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/enums/query_result_state.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/query_parameters.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

using namespace duckdb;

namespace {

constexpr idx_t SCAN_TASKS = 2;

struct Meeting {
	mutex lock;
	idx_t started = 0;
	idx_t met = 0;
	bool all_started = false;
	bool done = false;
	string error;

	void Reset() {
		lock_guard<mutex> guard(lock);
		started = 0;
		met = 0;
		all_started = false;
		done = false;
		error.clear();
	}

	void Arrive() {
		lock.Lock();
		all_started = ++started == SCAN_TASKS;
		if (lock.AwaitWithTimeout(absl::Condition(&all_started), absl::Milliseconds(150))) {
			met++;
		}
		lock.Unlock();
	}

	void Finish(string error_p) {
		lock_guard<mutex> guard(lock);
		error = std::move(error_p);
		done = true;
	}

	bool AwaitDone() {
		lock.Lock();
		const bool finished = lock.AwaitWithTimeout(absl::Condition(&done), absl::Seconds(30));
		lock.Unlock();
		return finished;
	}

	string Describe() {
		lock_guard<mutex> guard(lock);
		return StringUtil::Format("tasks started %llu, met the other %llu, error '%s'", started, met, error);
	}

	idx_t Met() {
		lock_guard<mutex> guard(lock);
		return met;
	}

	bool Failed() {
		lock_guard<mutex> guard(lock);
		return !error.empty();
	}
};

Meeting meeting;

struct PairScanState : GlobalTableFunctionState {
	idx_t MaxThreads() const override {
		return SCAN_TASKS;
	}
};

struct PairScanTask : LocalTableFunctionState {
	bool arrived = false;
};

unique_ptr<FunctionData> PairScanBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                      vector<Identifier> &names) {
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("i");
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> PairScanInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PairScanState>();
}

unique_ptr<LocalTableFunctionState> PairScanInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                      GlobalTableFunctionState *) {
	return make_uniq<PairScanTask>();
}

void PairScan(ClientContext &, TableFunctionInput &input, DataChunk &) {
	auto &task = input.local_state->Cast<PairScanTask>();
	if (task.arrived) {
		return;
	}
	task.arrived = true;
	meeting.Arrive();
}

class DriveTask : public Task {
public:
	explicit DriveTask(Connection &con_p) : con(con_p) {
	}

	TaskExecutionResult Execute(TaskExecutionMode) override {
		QueryParameters parameters;
		parameters.caller_drives = true;
		auto result = con.Submit("SELECT count(*) FROM pair_scan()", parameters);
		if (!result->HasError()) {
			for (auto state = result->ExecuteTask(); !IsObservable(state); state = result->ExecuteTask()) {
				if (state != QueryResultState::NOT_READY) {
					result->WaitForTask();
				}
			}
			result->Complete();
		}
		meeting.Finish(result->HasError() ? result->GetError() : string());
		return TaskExecutionResult::TASK_FINISHED;
	}

private:
	Connection &con;
};

} // namespace

TEST_CASE("The task a driver schedules beside its inline task runs on an idle worker", "[api][executor]") {
	meeting.Reset();
	DBConfig config;
	config.options.maximum_threads = SCAN_TASKS + 2;
	DuckDB db(nullptr, &config);
	Connection con(db);
	ExtensionLoader loader(*db.instance, "inline_task_concurrency");
	loader.RegisterFunction(
	    TableFunction("pair_scan", {}, PairScan, PairScanBind, PairScanInitGlobal, PairScanInitLocal));

	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	auto producer = scheduler.CreateProducer(con.context->GetSchedulerAccount());
	scheduler.ScheduleTask(*producer, make_shared_ptr<DriveTask>(con));
	const bool done = meeting.AwaitDone();
	INFO(meeting.Describe());
	REQUIRE(done);
	REQUIRE(!meeting.Failed());
	REQUIRE(meeting.Met() == SCAN_TASKS);
}
