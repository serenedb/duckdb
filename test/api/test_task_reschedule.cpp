#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parallel/interrupt.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

constexpr idx_t SCAN_TASKS = 4;

struct ResumeWindow {
	mutex lock;
	idx_t tasks = 0;
	idx_t resumed = 0;
	bool all_resumed = false;
	bool could_park = true;
	bool closed = false;
	idx_t resumed_in_window = 0;

	void Reset() {
		lock_guard<mutex> guard(lock);
		tasks = 0;
		resumed = 0;
		all_resumed = false;
		could_park = true;
		closed = false;
		resumed_in_window = 0;
	}

	void StartTask() {
		lock_guard<mutex> guard(lock);
		tasks++;
	}

	void Resume() {
		lock_guard<mutex> guard(lock);
		all_resumed = ++resumed == SCAN_TASKS - 1;
	}

	void CannotPark() {
		lock_guard<mutex> guard(lock);
		could_park = false;
	}

	void AwaitResumed() {
		lock.Lock();
		lock.AwaitWithTimeout(absl::Condition(&all_resumed), absl::Milliseconds(200));
		resumed_in_window = resumed;
		closed = true;
		lock.Unlock();
	}

	bool AwaitClosed() {
		lock.Lock();
		const bool done = lock.AwaitWithTimeout(absl::Condition(&closed), absl::Seconds(30));
		lock.Unlock();
		return done;
	}

	string Describe() {
		lock_guard<mutex> guard(lock);
		return StringUtil::Format("tasks %llu, could park %d, resumed while the releasing task waited %llu", tasks,
		                          could_park, resumed_in_window);
	}

	idx_t ResumedInWindow() {
		lock_guard<mutex> guard(lock);
		return resumed_in_window;
	}
};

ResumeWindow window;

struct ParkingScanState : GlobalTableFunctionState {
	idx_t MaxThreads() const override {
		return SCAN_TASKS;
	}

	idx_t arrived = 0;
};

struct ParkingScanTask : LocalTableFunctionState {
	bool parked = false;
};

unique_ptr<FunctionData> ParkingScanBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                         vector<Identifier> &names) {
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("i");
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> ParkingScanInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ParkingScanState>();
}

unique_ptr<LocalTableFunctionState> ParkingScanInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                         GlobalTableFunctionState *) {
	window.StartTask();
	return make_uniq<ParkingScanTask>();
}

void ParkingScan(ClientContext &, TableFunctionInput &input, DataChunk &) {
	auto &task = input.local_state->Cast<ParkingScanTask>();
	if (task.parked) {
		window.Resume();
		return;
	}
	if (!input.interrupt_state || !input.blockable ||
	    input.results_execution_mode != AsyncResultsExecutionMode::TASK_EXECUTOR) {
		window.CannotPark();
		return;
	}
	auto &state = input.global_state->Cast<ParkingScanState>();
	{
		annotated_lock_guard<annotated_mutex> guard(input.blockable->lock);
		if (++state.arrived < SCAN_TASKS) {
			task.parked = input.blockable->BlockTask(*input.interrupt_state);
			if (!task.parked) {
				window.CannotPark();
				return;
			}
			input.async_result = AsyncResultType::BLOCKED;
			return;
		}
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	{
		annotated_lock_guard<annotated_mutex> guard(input.blockable->lock);
		input.blockable->UnblockTasks();
	}
	window.AwaitResumed();
}

} // namespace

TEST_CASE("Tasks a running task unblocks resume on idle workers while it keeps running", "[api][executor]") {
	window.Reset();
	DBConfig config;
	config.options.maximum_threads = SCAN_TASKS + 1;
	DuckDB db(nullptr, &config);
	Connection con(db);
	ExtensionLoader loader(*db.instance, "task_reschedule");
	loader.RegisterFunction(
	    TableFunction("parking_scan", {}, ParkingScan, ParkingScanBind, ParkingScanInitGlobal, ParkingScanInitLocal));

	auto result = con.Submit("SELECT count(*) FROM parking_scan()");
	REQUIRE(!result->HasError());
	const bool closed = window.AwaitClosed();
	INFO(window.Describe());
	REQUIRE(closed);
	result->Complete();
	REQUIRE(!result->HasError());
	REQUIRE(window.ResumedInWindow() == SCAN_TASKS - 1);
}
