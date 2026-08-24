#include "duckdb/parallel/task_scheduler_pool.hpp"

#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/storage/block_allocator.hpp"

#ifndef DUCKDB_NO_THREADS
#include "lightweightsemaphore.h"

#include <functional>
#include <stdexcept>
#include <type_traits>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__GNUC__)
#include <sched.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <pthread.h>
#endif
#endif

namespace duckdb {

#ifndef DUCKDB_NO_THREADS
typedef duckdb_moodycamel::LightweightSemaphore lightweight_semaphore_t;

struct LightWeightSemaphoreWrapper {
	lightweight_semaphore_t s;
};
#endif

#ifdef DUCKDB_NO_THREADS

struct TaskSchedulerThread {};

#elif defined(__APPLE__)

// macOS pthreads default to a 512KB stack and ignore RLIMIT_STACK, whereas glibc
// sizes them from it (~8MB). duckdb's recursive optimizer overflows 512KB, and
// std::thread has no stack-size knob, so run workers on pthreads with an 8MB stack.
struct TaskSchedulerThread {
	explicit TaskSchedulerThread(std::function<void()> entry) {
		// Box the entry on the heap so it can pass through pthread's void* arg;
		// the running thread adopts and frees it in Trampoline.
		auto boxed_entry = make_uniq<std::function<void()>>(std::move(entry));
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
		const int rc = pthread_create(&handle, &attr, &Trampoline, boxed_entry.get());
		pthread_attr_destroy(&attr);
		if (rc != 0) {
			throw std::runtime_error("could not create task scheduler thread");
		}
		boxed_entry.release(); // ownership passes to the running thread
	}
	void Join() {
		pthread_join(handle, nullptr);
	}
	bool IsCurrentThread() const {
		return pthread_equal(handle, pthread_self()) != 0;
	}
	void SetAffinity(const vector<int> &, idx_t) {
	}

	static void *Trampoline(void *arg) {
		unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(arg));
		(*fn)();
		return nullptr;
	}
	pthread_t handle;
};

#else

struct TaskSchedulerThread {
	explicit TaskSchedulerThread(std::function<void()> entry) : internal_thread(std::move(entry)) {
	}
	void Join() {
		internal_thread.join();
	}
	bool IsCurrentThread() const {
		return internal_thread.get_id() == std::this_thread::get_id();
	}
	void SetAffinity(const vector<int> &available_cpus, idx_t thread_idx) {
#if defined(__GLIBC__)
		if (thread_idx < available_cpus.size()) {
			const auto cpu_id = available_cpus[thread_idx];
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(cpu_id, &cpuset);
			// if we did not manage to set affinity, the thread just does not have affinity, which is OK
			pthread_setaffinity_np(internal_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
		}
#endif
	}

	thread internal_thread;
};

#endif

TaskSchedulerPool::TaskSchedulerPool(DatabaseInstance &db_p, TaskSchedulerType pool_type_p)
    : db(db_p), pool_type(pool_type_p), requested_thread_count(0),
      current_thread_count(pool_type == TaskSchedulerType::REGULAR ? 1 : 0) {
#ifndef DUCKDB_NO_THREADS
	semaphore = make_uniq<LightWeightSemaphoreWrapper>();
#endif
}

TaskSchedulerPool::~TaskSchedulerPool() {
}

void TaskSchedulerPool::SetThreads(idx_t n) {
	requested_thread_count = n;
}

idx_t TaskSchedulerPool::NumberOfThreads() {
	return current_thread_count.load();
}

void TaskSchedulerPool::Signal(idx_t n) {
#ifndef DUCKDB_NO_THREADS
	if (n == 0) {
		return;
	}
	typedef std::make_signed<std::size_t>::type ssize_t;
	semaphore->s.signal(NumericCast<ssize_t>(n));
#endif
}

#ifndef DUCKDB_NO_THREADS
void TaskSchedulerPool::Wait() {
	semaphore->s.wait();
}

bool TaskSchedulerPool::Wait(int64_t timeout_usecs) {
	return semaphore->s.wait(timeout_usecs);
}
#endif

#ifndef DUCKDB_NO_THREADS
static vector<int> GetProcessCPUMask() {
#if defined(__GLIBC__)
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
		return {};
	}
	vector<int> available_cpus;
	for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
		if (CPU_ISSET(cpu, &cpuset)) {
			available_cpus.push_back(cpu);
		}
	}
	return available_cpus;
#else
	return {};
#endif
}

#endif

#ifndef DUCKDB_NO_THREADS
static void ThreadExecuteTasks(TaskScheduler *scheduler, atomic<bool> *marker, const TaskSchedulerType pool_type) {
	scheduler->ExecuteForever(marker, pool_type);
}
#endif

void TaskSchedulerPool::RelaunchThreads(TaskScheduler &scheduler, bool destroy) {
#ifndef DUCKDB_NO_THREADS
	auto &config = DBConfig::GetConfig(db);
	auto new_thread_count = destroy ? 0 : requested_thread_count.load();

	idx_t external_threads = 0;
	ThreadPinMode pin_thread_mode = ThreadPinMode::AUTO;
	if (!destroy) {
		// If we are destroying, i.e., calling ~TaskScheduler, we don't want to read the settings
		external_threads = Settings::Get<ExternalThreadsSetting>(config);
		pin_thread_mode = Settings::Get<PinThreadsSetting>(db);
	}

	if (threads.size() == new_thread_count) {
		current_thread_count = threads.size() + (pool_type == TaskSchedulerType::REGULAR ? external_threads : 0);
		return;
	}

	// Resolve thread pinning once: it applies both to the kept caller (re-pinned to its new index below) and to the
	// threads spawned afterwards.
	static constexpr idx_t THREAD_PIN_THRESHOLD = 64;
	const auto pin_threads =
	    pool_type == TaskSchedulerType::REGULAR && // Only pin regular threads!
	    (pin_thread_mode == ThreadPinMode::ON ||
	     (pin_thread_mode == ThreadPinMode::AUTO && std::thread::hardware_concurrency() > THREAD_PIN_THRESHOLD));
	const auto available_cpus = pin_threads ? GetProcessCPUMask() : vector<int>();
	// If we have fewer available cores than threads, do not pin and let the OS schedule.
	const auto can_pin = pin_threads && new_thread_count <= available_cpus.size();

	// Stop every worker except the calling thread, detecting it in the same pass. A SET threads runs on the session's
	// own pool worker, which cannot join itself, so it is kept alive and reconciled by the spawn step below. We stop
	// even when increasing so the survivors follow the current affinity mask.
	idx_t self = threads.size();
	idx_t stopped = 0;
	for (idx_t i = 0; i < threads.size(); i++) {
		if (self == threads.size() && threads[i]->IsCurrentThread()) {
			self = i;
			continue;
		}
		*markers[i] = false;
		stopped++;
	}
	Signal(stopped);
	// now join the stopped threads to ensure they are fully stopped before erasing them
	for (idx_t i = 0; i < threads.size(); i++) {
		if (i != self) {
			threads[i]->Join();
		}
	}
	// erase the threads/markers, keeping the calling worker (if any) as thread 0
	if (self < threads.size()) {
		auto kept_thread = std::move(threads[self]);
		auto kept_marker = std::move(markers[self]);
		threads.clear();
		markers.clear();
		// re-pin the survivor to index 0 so the pinned set stays contiguous with the new threads, which pin to
		// 1..N-1 below
		if (can_pin) {
			kept_thread->SetAffinity(available_cpus, 0);
		}
		threads.push_back(std::move(kept_thread));
		markers.push_back(std::move(kept_marker));
	} else {
		threads.clear();
		markers.clear();
	}

	if (threads.size() < new_thread_count) {
		// we are increasing the number of threads: launch them and run tasks on them
		idx_t create_new_threads = new_thread_count - threads.size();

		for (idx_t i = 0; i < create_new_threads; i++) {
			// launch a thread and assign it a cancellation marker
			auto marker = unique_ptr<atomic<bool>>(new atomic<bool>(true));
			unique_ptr<TaskSchedulerThread> thread_wrapper;
			try {
				thread_wrapper =
				    make_uniq<TaskSchedulerThread>([&scheduler, marker_ptr = marker.get(), pool_type = pool_type] {
					    ThreadExecuteTasks(&scheduler, marker_ptr, pool_type);
				    });
				if (can_pin) {
					thread_wrapper->SetAffinity(available_cpus, threads.size());
				}
			} catch (std::exception &ex) {
				// thread constructor failed - this can happen when the system has too many threads allocated
				// in this case we cannot allocate more threads - stop launching them
				break;
			}
			threads.push_back(std::move(thread_wrapper));
			markers.push_back(std::move(marker));
		}
	}
	current_thread_count = threads.size() + (pool_type == TaskSchedulerType::REGULAR ? external_threads : 0);
	BlockAllocator::Get(db).FlushAll();
#endif
}

}; // namespace duckdb
