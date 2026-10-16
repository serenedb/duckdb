//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_scheduler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/common/enums/task_scheduler_type.hpp"
#include "duckdb/parallel/task_scheduler_queue.hpp"
#include "duckdb/parallel/task_scheduler_token.hpp"

namespace duckdb {

class ClientContext;
struct DBConfig;
class DatabaseInstance;
class TaskScheduler;
class TaskSchedulerPool;
struct SchedulerWorker;
struct SchedulerWakeList;
struct SchedulerSlice;

//! The TaskScheduler is responsible for managing tasks and threads
class TaskScheduler {
public:
	explicit TaskScheduler(DatabaseInstance &db);
	~TaskScheduler();

	//! Marks code that waits instead of computing: while it is open, the worker hands its CPU slot to a spare thread.
	//! Used for waits in the kernel or on another thread (fsync, events, futures, condition variables) and for remote
	//! reads (HTTP and S3, AI providers, PostgreSQL and ClickHouse clients). Background work that is not split into
	//! batches (compaction, refresh, cleanup, directory removal, ANN builds) runs as ASYNC tasks, which hold no slot.
	class BlockingScope {
	public:
		BlockingScope();
		~BlockingScope();
		BlockingScope(const BlockingScope &) = delete;
		BlockingScope &operator=(const BlockingScope &) = delete;

	private:
		SchedulerWorker *worker;
	};

	class DriverSlice {
	public:
		DriverSlice();
		~DriverSlice();
		DriverSlice(const DriverSlice &) = delete;
		DriverSlice &operator=(const DriverSlice &) = delete;

	private:
		int64_t previous;
	};

public:
	DUCKDB_API static TaskScheduler &GetScheduler(ClientContext &context);
	DUCKDB_API static TaskScheduler &GetScheduler(DatabaseInstance &db);

	shared_ptr<SchedulerAccount> CreateAccount(SchedulerClass scheduler_class);
	unique_ptr<ProducerToken> CreateProducer();
	unique_ptr<ProducerToken> CreateProducer(shared_ptr<SchedulerAccount> account);
	//! Returns the number of threads
	DUCKDB_API idx_t NumberOfThreads();
	DUCKDB_API static idx_t QueryThreads(ClientContext &context);
	//! Returns the number of async threads
	DUCKDB_API idx_t NumberOfAsyncThreads();
	DUCKDB_API static idx_t QueryAsyncThreads(ClientContext &context);
	DUCKDB_API static TaskSchedulerType QueryAsyncPool(ClientContext &context);

	idx_t GetNumberOfTasks() const;
	idx_t GetTaskCountForProducer(ProducerToken &token) const;

	//! Schedule a task to be executed by the task scheduler in the given pool
	void ScheduleTask(ProducerToken &producer, shared_ptr<Task> task, TaskSchedulerType pool_type);
	void ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks, TaskSchedulerType pool_type);
	//! Helpers for regular tasks
	void ScheduleTask(ProducerToken &producer, shared_ptr<Task> task);
	void ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks);
	//! Fetches a task from a specific producer, returns true if successful or false if no tasks were available
	bool GetTaskFromProducer(ProducerToken &token, shared_ptr<Task> &task);
	//! Fetches a task from a specific producer, returns whether a task was found and assigned to `task`.
	bool GetTaskFromProducerLocked(ProducerToken &token, shared_ptr<Task> &task) DUCKDB_REQUIRES(token.producer_lock);
	//! Run tasks forever until "marker" is set to false, "marker" must remain valid until the thread is joined
	void ExecuteForever(atomic<bool> *marker);
	void ExecuteSpare(atomic<bool> *marker);
	//! Run tasks until `marker` is set to false, `max_tasks` have been completed, or until there are no more tasks
	//! available. Returns the number of tasks that were completed.
	idx_t ExecuteTasks(atomic<bool> *marker, idx_t max_tasks);
	//! Run tasks until `max_tasks` have been completed, or until there are no more tasks available
	void ExecuteTasks(idx_t max_tasks);
	//! Send signals to n threads, signalling for them to wake up and attempt to execute a task
	void Signal(idx_t n);
	DUCKDB_API static bool ShouldYield();
	static bool ShouldYield(TaskExecutionMode mode) {
		return mode == TaskExecutionMode::PROCESS_PARTIAL && ShouldYield();
	}

	//! Sets the amount of background threads to be used for execution, based on the number of total threads
	//! and the number of external threads. External threads, e.g. the main thread, will also be used for execution.
	//! Launches `total_threads - external_threads` background worker threads.
	void SetThreads(idx_t total_threads, idx_t external_threads);
	void SetAsyncThreads(idx_t n);
	void RelaunchThreads();
	//! Stop and join every worker thread in every pool without destroying the scheduler. Unlike SetThreads, this
	//! reaches zero regular workers (RelaunchThreads with destroy=true bypasses the clamp-to-one), so after it returns
	//! from a non-worker thread no worker is left running. Idempotent; the destructor runs the same loop.
	void Join();

	//! Yield to other threads
	static void YieldThread();
	//! Get the number of the CPU on which the calling thread is currently executing.
	//! Fallback to calling thread id if CPU number is not available.
	//! Result do not need to be exact 'return 0' is a valid fallback strategy
	static idx_t GetEstimatedCPUId();

private:
	friend struct ProducerToken;
	friend class TaskSchedulerPool;

	void Enqueue(ProducerToken &producer, shared_ptr<Task> *tasks, idx_t count, TaskSchedulerType pool_type);
	void RemoveProducer(ProducerToken &producer);
	void WorkerLoop(atomic<bool> *marker, bool spare);
	bool AcquireTask(SchedulerWorker &worker, shared_ptr<Task> &task, SchedulerSlice &slice);
	bool TryAcquire(SchedulerWorker &worker, shared_ptr<Task> &task, SchedulerWakeList &wake) DUCKDB_REQUIRES(lock);
	void FlushSlice(SchedulerWorker &worker, SchedulerSlice &slice);
	bool Park(SchedulerWorker &worker);
	void RunTask(SchedulerWorker &worker, shared_ptr<Task> &task, SchedulerSlice &slice);
	void FinishSlice(SchedulerWorker &worker, SchedulerSlice &slice) DUCKDB_REQUIRES(lock);
	void ReleaseSlot(SchedulerWorker &worker);
	void ReacquireSlot(SchedulerWorker &worker);
	void WakeIdle(idx_t count, SchedulerWakeList &wake) DUCKDB_REQUIRES(lock);
	void RemoveIdle(SchedulerWorker &worker) DUCKDB_REQUIRES(lock);
	void Wake(SchedulerWakeList &wake);
	void WakeAll();
	void SetTargetThreads(idx_t count);
	idx_t SpareLimit() const;
	int64_t IdleWaitMicros(SchedulerWorker &worker);
	void IdleTimeout(SchedulerWorker &worker);

private:
	DatabaseInstance &db;
	//! Lock for modifying the thread count
	mutex thread_lock;
	unique_ptr<TaskSchedulerPool> pool;
	mutable annotated_mutex lock;
	TaskSchedulerQueue queue;
	vector<SchedulerWorker *> idle DUCKDB_GUARDED_BY(lock);
	vector<SchedulerWorker *> idle_spares DUCKDB_GUARDED_BY(lock);
	idx_t waking DUCKDB_GUARDED_BY(lock) = 0;
	idx_t blocked DUCKDB_GUARDED_BY(lock) = 0;
	idx_t spares DUCKDB_GUARDED_BY(lock) = 0;
	atomic<idx_t> running {0};
	atomic<idx_t> target {0};
	atomic<idx_t> async_threads {0};
	shared_ptr<SchedulerAccount> background_account;
};

} // namespace duckdb
