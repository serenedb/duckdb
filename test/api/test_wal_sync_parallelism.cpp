#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

constexpr idx_t WRITER_COUNT = 8;
constexpr idx_t COMMITS_PER_WRITER = 20;

class SlowSyncFileSystem : public LocalFileSystem {
public:
	explicit SlowSyncFileSystem(FileSyncParallelism parallelism_p) : parallelism(parallelism_p) {
	}

	FileSyncParallelism SyncParallelism(FileHandle &handle) override {
		return parallelism;
	}

	void FileSync(FileHandle &handle) override {
		if (StringUtil::Contains(handle.GetPath(), ".wal")) {
			auto now = ++syncs_in_flight;
			auto seen = max_syncs_in_flight.load();
			while (now > seen && !max_syncs_in_flight.compare_exchange_weak(seen, now)) {
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			--syncs_in_flight;
		}
		LocalFileSystem::FileSync(handle);
	}

	atomic<idx_t> syncs_in_flight {0};
	atomic<idx_t> max_syncs_in_flight {0};

private:
	FileSyncParallelism parallelism;
};

idx_t RunConcurrentCommits(FileSyncParallelism parallelism, const string &name) {
	auto path = TestCreatePath(name);
	DeleteDatabase(path);
	idx_t max_syncs;
	{
		auto fs = make_uniq<SlowSyncFileSystem>(parallelism);
		auto &slow_fs = *fs;
		DBConfig config;
		config.file_system = make_uniq<VirtualFileSystem>(std::move(fs));
		DuckDB db(path, &config);
		Connection setup(db);
		REQUIRE_NO_FAIL(setup.Query("SET checkpoint_threshold='1TB'"));
		REQUIRE_NO_FAIL(setup.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(writer INTEGER, i INTEGER)"));

		Connection holder(db);
		REQUIRE_NO_FAIL(holder.Query("BEGIN"));
		REQUIRE_NO_FAIL(holder.Query("SELECT count(*) FROM t"));

		atomic<idx_t> failures {0};
		vector<std::thread> writers;
		for (idx_t w = 0; w < WRITER_COUNT; w++) {
			writers.emplace_back([&db, &failures, w]() {
				Connection con(db);
				for (idx_t i = 0; i < COMMITS_PER_WRITER; i++) {
					if (con.Query(StringUtil::Format("INSERT INTO t VALUES (%d, %d)", w, i))->HasError()) {
						failures++;
					}
				}
			});
		}
		for (auto &writer : writers) {
			writer.join();
		}
		REQUIRE(failures == 0);
		REQUIRE_NO_FAIL(holder.Query("ROLLBACK"));
		max_syncs = slow_fs.max_syncs_in_flight;
	}

	DuckDB reopened(path);
	Connection con(reopened);
	auto result = con.Query("SELECT count(*) FROM (SELECT DISTINCT writer, i FROM t)");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(NumericCast<int64_t>(WRITER_COUNT * COMMITS_PER_WRITER))}));
	return max_syncs;
}

} // namespace

TEST_CASE("WAL syncs overlap on a file system that pipelines them", "[api][group_commit]") {
	REQUIRE(RunConcurrentCommits(FileSyncParallelism::PARALLEL, "wal_sync_parallel.db") > 1);
}

TEST_CASE("WAL syncs stay serial on a file system that serializes them", "[api][group_commit]") {
	REQUIRE(RunConcurrentCommits(FileSyncParallelism::SERIAL, "wal_sync_serial.db") == 1);
}
