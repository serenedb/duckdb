//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_scheduler_pool.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class TaskScheduler;
class DatabaseInstance;
struct TaskSchedulerThread;
struct TaskSchedulerSpare;

class TaskSchedulerPool {
public:
	explicit TaskSchedulerPool(DatabaseInstance &db);
	~TaskSchedulerPool();

public:
	void SetThreads(idx_t n);
	idx_t NumberOfThreads();
	void RelaunchThreads(TaskScheduler &scheduler, bool destroy);
	bool SpawnSpare(TaskScheduler &scheduler);

private:
	void ReapSpares();
	void StopSpares(TaskScheduler &scheduler);

	DatabaseInstance &db;
	//! The active background threads of the task scheduler
	vector<unique_ptr<TaskSchedulerThread>> threads;
	//! Markers used by the various threads, if the markers are set to "false" the thread execution is stopped
	vector<unique_ptr<atomic<bool>>> markers;
	//! Requested thread count (set by the 'threads' setting)
	atomic<idx_t> requested_thread_count;
	//! The amount of threads currently running
	atomic<idx_t> current_thread_count;
	mutex spare_lock;
	atomic<bool> spares_stopped {false};
	vector<unique_ptr<TaskSchedulerSpare>> spares;
};

}; // namespace duckdb
