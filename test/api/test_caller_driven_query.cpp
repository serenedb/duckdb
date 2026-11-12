#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

namespace {

struct ScanGate {
	mutex lock;
	thread_id driver = ThreadUtil::GetThreadId();
	bool worker_holds_task = false;
	bool open = false;

	void Pass() {
		lock.Lock();
		if (ThreadUtil::GetThreadId() == driver) {
			lock.AwaitWithTimeout(absl::Condition(&worker_holds_task), absl::Seconds(30));
		} else {
			worker_holds_task = true;
			lock.AwaitWithTimeout(absl::Condition(&open), absl::Seconds(30));
		}
		lock.Unlock();
	}

	void Open() {
		lock_guard<mutex> guard(lock);
		open = true;
	}
};

struct ParkedDriver {
	mutex lock;
	bool woken = false;

	std::function<void()> Wake() {
		return [this] {
			lock_guard<mutex> guard(lock);
			woken = true;
		};
	}

	bool WaitWoken() {
		lock.Lock();
		const bool woke = lock.AwaitWithTimeout(absl::Condition(&woken), absl::Seconds(30));
		woken = false;
		lock.Unlock();
		return woke;
	}
};

QueryParameters DrivenQuery() {
	QueryParameters parameters;
	parameters.result_eagerness = ResultEagerness::FORCED;
	parameters.caller_drives = true;
	return parameters;
}

DBConfig &TwoThreads(DBConfig &config) {
	config.options.maximum_threads = 2;
	return config;
}

struct GatedTable {
	GatedTable() : db(nullptr, &TwoThreads(config)), con(db) {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT i FROM range(1000000) t(i)"));
		ExtensionLoader loader(*db.instance, "caller_driven_query");
		loader.RegisterFunction(ScalarFunction("gate", {LogicalType::BIGINT}, LogicalType::BIGINT,
		                                       [this](DataChunk &args, ExpressionState &, Vector &result) {
			                                       gate.Pass();
			                                       result.Reference(args.data[0]);
		                                       }));
	}

	DBConfig config;
	DuckDB db;
	Connection con;
	ScanGate gate;
};

} // namespace

TEST_CASE("A worker hands the next step of a caller-driven query to its parked driver", "[api][executor]") {
	GatedTable table;
	auto &con = table.con;
	auto &gate = table.gate;

	auto result = con.Submit("SELECT sum(gate(i)) FROM t", DrivenQuery());
	REQUIRE(!result->HasError());
	ParkedDriver driver;
	bool parked = false;
	bool woke_to_work = false;
	auto state = result->ExecuteTask(driver.Wake());
	while (!IsObservable(state)) {
		if (state != QueryResultState::NOT_READY) {
			if (!parked) {
				parked = true;
				gate.Open();
			}
			REQUIRE(driver.WaitWoken());
			state = result->ExecuteTask(driver.Wake());
			woke_to_work = woke_to_work || state == QueryResultState::NOT_READY;
			continue;
		}
		state = result->ExecuteTask(driver.Wake());
	}
	REQUIRE(state != QueryResultState::EXECUTION_ERROR);
	REQUIRE(parked);
	REQUIRE(woke_to_work);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::HUGEINT(499999500000));
}

TEST_CASE("A caller-driven query interrupted while its driver is parked ends with an error", "[api][executor]") {
	GatedTable table;
	auto &con = table.con;
	auto &gate = table.gate;

	auto result = con.Submit("SELECT sum(gate(i)) FROM t", DrivenQuery());
	REQUIRE(!result->HasError());
	ParkedDriver driver;
	bool interrupted = false;
	auto state = result->ExecuteTask(driver.Wake());
	while (!IsObservable(state)) {
		if (state != QueryResultState::NOT_READY) {
			if (!interrupted) {
				interrupted = true;
				con.Interrupt();
				gate.Open();
			}
			REQUIRE(driver.WaitWoken());
		}
		state = result->ExecuteTask(driver.Wake());
	}
	REQUIRE(interrupted);
	REQUIRE(state == QueryResultState::EXECUTION_ERROR);
}
