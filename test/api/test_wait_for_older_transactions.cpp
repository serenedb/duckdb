////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

Catalog &DefaultCatalog(Connection &con) {
	return Catalog::GetCatalog(*con.context, DatabaseManager::GetDefaultDatabase(*con.context));
}

void OpenTransaction(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM t"));
}

struct Waiter {
	explicit Waiter(DuckDB &db) : con(db) {
		OpenTransaction(con);
	}
	~Waiter() {
		if (thread.joinable()) {
			thread.join();
		}
	}
	void Start() {
		auto &catalog = DefaultCatalog(con);
		auto &manager = DuckTransactionManager::Get(catalog.GetAttached());
		auto &transaction = DuckTransaction::Get(*con.context, catalog);
		thread = std::thread([this, &manager, &transaction]() {
			try {
				manager.WaitForOlderTransactions(*con.context, transaction);
				returned = true;
			} catch (InterruptException &) {
				interrupted = true;
			}
		});
	}
	bool ReturnsWithin(std::chrono::milliseconds limit) {
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!returned && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return returned;
	}
	void Join() {
		thread.join();
	}

	Connection con;
	std::thread thread;
	std::atomic<bool> returned {false};
	std::atomic<bool> interrupted {false};
};

constexpr std::chrono::milliseconds STILL_WAITING {300};
constexpr std::chrono::milliseconds RETURNS {10000};

} // namespace

TEST_CASE("WaitForOlderTransactions returns once every older transaction ended", "[api][transaction]") {
	DuckDB db(nullptr);
	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER)"));

	Connection writer(db);
	OpenTransaction(writer);
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO t VALUES (1)"));
	Connection reader(db);
	OpenTransaction(reader);

	Waiter waiter(db);
	waiter.Start();
	REQUIRE(!waiter.ReturnsWithin(STILL_WAITING));
	REQUIRE_NO_FAIL(writer.Query("COMMIT"));
	REQUIRE(!waiter.ReturnsWithin(STILL_WAITING));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));
	REQUIRE(waiter.ReturnsWithin(RETURNS));
	waiter.Join();
	REQUIRE_NO_FAIL(waiter.con.Query("COMMIT"));
}

TEST_CASE("WaitForOlderTransactions throws when its statement is interrupted", "[api][transaction]") {
	DuckDB db(nullptr);
	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER)"));

	Connection writer(db);
	OpenTransaction(writer);
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO t VALUES (1)"));

	Waiter waiter(db);
	waiter.Start();
	REQUIRE(!waiter.ReturnsWithin(STILL_WAITING));
	waiter.con.Interrupt();
	waiter.Join();
	REQUIRE(waiter.interrupted);
	REQUIRE(!waiter.returned);
	REQUIRE_NO_FAIL(writer.Query("COMMIT"));
}

TEST_CASE("Two transactions waiting for older ones do not wait for each other", "[api][transaction]") {
	DuckDB db(nullptr);
	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER)"));

	Waiter first(db);
	Waiter second(db);
	first.Start();
	REQUIRE(!first.ReturnsWithin(STILL_WAITING));
	second.Start();
	REQUIRE(second.ReturnsWithin(RETURNS));
	second.Join();
	REQUIRE(!first.ReturnsWithin(STILL_WAITING));
	REQUIRE_NO_FAIL(second.con.Query("COMMIT"));
	REQUIRE(first.ReturnsWithin(RETURNS));
	first.Join();
	REQUIRE_NO_FAIL(first.con.Query("COMMIT"));
}
