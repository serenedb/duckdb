#include "catch.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/shared_object_cache.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <condition_variable>
#include <string_view>
#include <thread>

using namespace duckdb; // NOLINT

namespace {

struct SharedTestObject : public ObjectCacheEntry {
	static std::atomic<int> alive;
	static std::atomic<int> builds;
	int value;
	idx_t size;
	SharedTestObject(int value, idx_t size) : value(value), size(size) {
		alive++;
		builds++;
	}
	~SharedTestObject() override {
		alive--;
	}
	static constexpr std::string_view ObjectType() {
		return "SharedTestObject";
	}
	string GetObjectType() override {
		return string(ObjectType());
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return size;
	}
};

std::atomic<int> SharedTestObject::alive {0};
std::atomic<int> SharedTestObject::builds {0};

} // namespace

TEST_CASE("Test SharedObjectCache", "[api][shared_object_cache]") {
	DuckDB db;
	Connection con(db);
	auto &context = *con.context;

	auto &cache = db.instance->GetSharedObjectCache();
	auto &buffer_pool = BufferManager::GetBufferManager(context).GetBufferPool();
	const idx_t initial_memory = buffer_pool.GetUsedMemory();
	SharedTestObject::alive = 0;
	SharedTestObject::builds = 0;

	constexpr idx_t obj_size = 1024 * 1024;
	const auto build = [&]() {
		return make_uniq<SharedTestObject>(42, obj_size);
	};

	SECTION("same key yields the same object, alive while referenced") {
		auto a = cache.GetOrBuild<SharedTestObject>("shared", build);
		auto b = cache.GetOrBuild<SharedTestObject>("shared", build);
		auto c = cache.GetOrBuild<SharedTestObject>("other", build);
		REQUIRE(a.get() == b.get());
		REQUIRE(a.get() != c.get());
		REQUIRE(SharedTestObject::builds == 2);
		REQUIRE(SharedTestObject::alive == 2);
		REQUIRE(cache.GetEntryCount() == 2);
		REQUIRE(cache.GetMemoryUsage() == 2 * obj_size);
		REQUIRE(buffer_pool.GetUsedMemory() == initial_memory + 2 * obj_size);

		// the cache holds only a weak reference: the last holder frees the entry and its reservation
		b.reset();
		c.reset();
		REQUIRE(SharedTestObject::alive == 1);
		REQUIRE(buffer_pool.GetUsedMemory() == initial_memory + obj_size);
		a.reset();
		REQUIRE(SharedTestObject::alive == 0);
		REQUIRE(cache.GetEntryCount() == 0);
		REQUIRE(cache.GetMemoryUsage() == 0);
		REQUIRE(buffer_pool.GetUsedMemory() == initial_memory);

		// a dead key rebuilds on demand
		auto again = cache.GetOrBuild<SharedTestObject>("shared", build);
		REQUIRE(SharedTestObject::builds == 3);
		REQUIRE(SharedTestObject::alive == 1);
	}

	SECTION("Get sees live entries only") {
		auto a = cache.GetOrBuild<SharedTestObject>("shared", build);
		auto found = cache.Get<SharedTestObject>("shared");
		REQUIRE(found.get() == a.get());
		a.reset();
		found.reset();
		REQUIRE(cache.Get<SharedTestObject>("shared") == nullptr);
	}

	SECTION("a throwing build caches nothing") {
		REQUIRE_THROWS(cache.GetOrBuild<SharedTestObject>(
		    "boom", [&]() -> unique_ptr<SharedTestObject> { throw InternalException("build failed"); }));
		REQUIRE(cache.GetEntryCount() == 0);
		auto ok = cache.GetOrBuild<SharedTestObject>("boom", build);
		REQUIRE(ok != nullptr);
	}

	SECTION("builds are single-flight") {
		constexpr idx_t thread_count = 8;
		std::atomic<int> started {0};
		const auto slow_build = [&]() {
			started++;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			return make_uniq<SharedTestObject>(42, obj_size);
		};
		std::vector<std::thread> threads;
		std::array<shared_ptr<SharedTestObject>, thread_count> results;
		for (idx_t idx = 0; idx < thread_count; ++idx) {
			threads.emplace_back(
			    [&, idx]() { results[idx] = cache.GetOrBuild<SharedTestObject>("raced", slow_build); });
		}
		for (auto &thread : threads) {
			thread.join();
		}
		for (idx_t idx = 1; idx < thread_count; ++idx) {
			REQUIRE(results[idx].get() == results[0].get());
		}
		REQUIRE(SharedTestObject::builds == 1);
		REQUIRE(started == 1);
		REQUIRE(SharedTestObject::alive == 1);
		REQUIRE(buffer_pool.GetUsedMemory() == initial_memory + obj_size);
	}

	SECTION("a failed build hands the key to the next waiter") {
		std::mutex m;
		std::condition_variable cv;
		bool release_failure = false;
		std::atomic<int> failing_started {0};

		std::thread failing([&]() {
			REQUIRE_THROWS(cache.GetOrBuild<SharedTestObject>("handoff", [&]() -> unique_ptr<SharedTestObject> {
				failing_started++;
				std::unique_lock<std::mutex> lock(m);
				cv.wait(lock, [&]() { return release_failure; });
				throw InternalException("build failed");
			}));
		});
		while (failing_started == 0) {
			std::this_thread::yield();
		}

		shared_ptr<SharedTestObject> result;
		std::thread waiting([&]() { result = cache.GetOrBuild<SharedTestObject>("handoff", build); });
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		{
			std::lock_guard<std::mutex> lock(m);
			release_failure = true;
		}
		cv.notify_all();
		failing.join();
		waiting.join();
		REQUIRE(result != nullptr);
		REQUIRE(result->value == 42);
		REQUIRE(SharedTestObject::builds == 1);
	}
}
