#include "duckdb/common/fault_points.hpp"

#ifdef SDB_FAULT_INJECTION

#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"

#include <algorithm>
#include <atomic>

namespace duckdb {

namespace {

struct Registry {
	absl::Mutex lock;
	absl::flat_hash_set<string> armed ABSL_GUARDED_BY(lock);
	uint64_t clears ABSL_GUARDED_BY(lock) = 0;
	std::atomic<bool> any {false};

	void Refresh() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock) {
		any.store(!armed.empty(), std::memory_order_relaxed);
	}
};

Registry &GetRegistry() {
	static Registry registry;
	return registry;
}

} // namespace

bool FaultPoints::Armed(std::string_view name) noexcept {
	auto &registry = GetRegistry();
	if (!registry.any.load(std::memory_order_relaxed)) {
		return false;
	}
	const absl::ReaderMutexLock guard(&registry.lock);
	return registry.armed.contains(name);
}

bool FaultPoints::Arm(std::string_view name) {
	auto &registry = GetRegistry();
	const absl::MutexLock guard(&registry.lock);
	const bool added = registry.armed.emplace(name).second;
	registry.Refresh();
	return added;
}

bool FaultPoints::Disarm(std::string_view name) {
	auto &registry = GetRegistry();
	const absl::MutexLock guard(&registry.lock);
	const bool removed = registry.armed.erase(name) != 0;
	registry.Refresh();
	return removed;
}

void FaultPoints::Clear() noexcept {
	auto &registry = GetRegistry();
	const absl::MutexLock guard(&registry.lock);
	registry.armed.clear();
	registry.clears++;
	registry.Refresh();
}

vector<string> FaultPoints::List() {
	auto &registry = GetRegistry();
	vector<string> names;
	{
		const absl::ReaderMutexLock guard(&registry.lock);
		names.assign(registry.armed.begin(), registry.armed.end());
	}
	std::sort(names.begin(), names.end());
	return names;
}

void FaultPoints::WaitWhileArmed(std::string_view name) {
	if (!Armed(name)) {
		return;
	}
	auto &registry = GetRegistry();
	const absl::MutexLock guard(&registry.lock);
	const auto disarmed = [&]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(registry.lock) {
		return !registry.armed.contains(name);
	};
	registry.lock.Await(absl::Condition(&disarmed));
}

void FaultPoints::ParkOnce(std::string_view name) {
	if (!Armed(name)) {
		return;
	}
	auto &registry = GetRegistry();
	const absl::MutexLock guard(&registry.lock);
	if (registry.armed.erase(name) == 0) {
		return;
	}
	registry.armed.emplace(string(name) + ":parked");
	const auto release = string(name) + ":release";
	const auto clears = registry.clears;
	const auto released = [&]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(registry.lock) {
		return registry.clears != clears || registry.armed.contains(release);
	};
	registry.lock.Await(absl::Condition(&released));
	registry.armed.erase(release);
	registry.Refresh();
}

} // namespace duckdb

#endif
