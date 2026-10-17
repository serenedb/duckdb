#include "duckdb/parallel/task_scheduler.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/time_point.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task_scheduler_pool.hpp"
#include "duckdb/parallel/task_scheduler_queue.hpp"
#include "duckdb/storage/block_allocator.hpp"

#include <absl/container/inlined_vector.h>
#ifndef DUCKDB_NO_THREADS
#include "lightweightsemaphore.h"
#include "duckdb/common/thread.hpp"

#include <thread>
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

static constexpr int64_t MIN_SLICE_NANOS = 1000000;
static constexpr double SLICE_ESTIMATE_NANOS = 1000000;
static constexpr int64_t INITIAL_FLUSH_WAIT = 500000;

struct SchedulerWorker {
	SchedulerWorker(TaskScheduler &scheduler_p, bool spare_p) : scheduler(scheduler_p), spare(spare_p) {
	}

	TaskScheduler &scheduler;
	const bool spare;
#ifndef DUCKDB_NO_THREADS
	duckdb_moodycamel::LightweightSemaphore semaphore;
#endif
	shared_ptr<SchedulerAccount> account;
	idx_t idle_phase = 0;
	int64_t slice_start = 0;
	int64_t blocked_ticks = 0;
	int64_t blocked_since = 0;
	idx_t blocking_depth = 0;
	bool in_task = false;
	bool holds_slot = false;
	bool released_slot = false;
	bool must_yield = false;
	bool idle = false;
	bool woken = false;
};

struct SchedulerWakeList {
	absl::InlinedVector<SchedulerWorker *, 8> workers;
	idx_t spawn = 0;
};

struct SchedulerSlice {
	shared_ptr<SchedulerAccount> account;
	shared_ptr<Task> requeue;
	int64_t used = 0;
	bool pending = false;
};

static thread_local SchedulerWorker *current_worker = nullptr; // NOLINT
static thread_local int64_t driver_slice_start = 0;            // NOLINT

static int64_t MinSliceTicks() {
	static const int64_t ticks = TimePoint::FromNanos(MIN_SLICE_NANOS);
	return ticks;
}

ProducerToken::ProducerToken(TaskScheduler &scheduler_p, shared_ptr<SchedulerAccount> account_p)
    : scheduler(scheduler_p), account(std::move(account_p)) {
}

ProducerToken::~ProducerToken() {
	scheduler.RemoveProducer(*this);
}

TaskScheduler::BlockingScope::BlockingScope() : worker(current_worker) {
	if (!worker || worker->blocking_depth++ > 0) {
		return;
	}
	worker->blocked_since = TimePoint::Now();
	if (worker->holds_slot) {
		worker->scheduler.ReleaseSlot(*worker);
	}
}

TaskScheduler::BlockingScope::~BlockingScope() {
	if (!worker || --worker->blocking_depth > 0) {
		return;
	}
	worker->blocked_ticks += TimePoint::Now() - worker->blocked_since;
	if (worker->released_slot) {
		worker->scheduler.ReacquireSlot(*worker);
	}
}

TaskScheduler::DriverSlice::DriverSlice() : previous(driver_slice_start) {
	if (!current_worker && !previous) {
		driver_slice_start = TimePoint::Now();
	}
}

TaskScheduler::DriverSlice::~DriverSlice() {
	driver_slice_start = previous;
}

TaskScheduler::TaskScheduler(DatabaseInstance &db)
    : db(db), pool(make_uniq<TaskSchedulerPool>(db)),
      background_account(make_shared_ptr<SchedulerAccount>(SchedulerClass::BACKGROUND)) {
}

void TaskScheduler::Join() {
#ifndef DUCKDB_NO_THREADS
	pool->RelaunchThreads(*this, true);
#endif
}

TaskScheduler::~TaskScheduler() {
#ifndef DUCKDB_NO_THREADS
	try {
		Join();
		BlockAllocator::Get(db).FlushAll();
	} catch (...) {
		// nothing we can do in the destructor if this fails
	}
#endif
}

TaskScheduler &TaskScheduler::GetScheduler(ClientContext &context) {
	return GetScheduler(DatabaseInstance::GetDatabase(context));
}

TaskScheduler &TaskScheduler::GetScheduler(DatabaseInstance &db) {
	return db.GetScheduler();
}

shared_ptr<SchedulerAccount> TaskScheduler::CreateAccount(SchedulerClass scheduler_class) {
	return make_shared_ptr<SchedulerAccount>(scheduler_class);
}

unique_ptr<ProducerToken> TaskScheduler::CreateProducer() {
	return CreateProducer(background_account);
}

unique_ptr<ProducerToken> TaskScheduler::CreateProducer(shared_ptr<SchedulerAccount> account) {
	return make_uniq<ProducerToken>(*this, account ? std::move(account) : background_account);
}

void TaskScheduler::RemoveProducer(ProducerToken &producer) {
	if (!producer.runnable.load(std::memory_order_acquire)) {
		return;
	}
	const annotated_lock_guard<annotated_mutex> guard(lock);
	queue.RemoveToken(producer);
}

void TaskScheduler::ScheduleTask(ProducerToken &token, shared_ptr<Task> task) {
	ScheduleTask(token, std::move(task), TaskSchedulerType::REGULAR);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks) {
	ScheduleTasks(producer, tasks, TaskSchedulerType::REGULAR);
}

void TaskScheduler::ScheduleTask(ProducerToken &token, shared_ptr<Task> task, TaskSchedulerType pool_type) {
	Enqueue(token, &task, 1, pool_type);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, vector<shared_ptr<Task>> &tasks,
                                  TaskSchedulerType pool_type) {
	Enqueue(producer, tasks.data(), tasks.size(), pool_type);
}

void TaskScheduler::Enqueue(ProducerToken &producer, shared_ptr<Task> *tasks, idx_t count,
                            TaskSchedulerType pool_type) {
	const bool blocking = pool_type != TaskSchedulerType::REGULAR;
	SchedulerWakeList wake;
	{
		const annotated_lock_guard<annotated_mutex> producer_guard(producer.producer_lock);
		{
			const annotated_lock_guard<annotated_mutex> guard(lock);
			for (idx_t i = 0; i < count; i++) {
				tasks[i]->token = producer;
				tasks[i]->blocking = blocking;
				queue.Enqueue(producer, std::move(tasks[i]));
			}
			WakeIdle(count, wake);
		}
		producer.producer_cv.Signal();
	}
	Wake(wake);
}

bool TaskScheduler::GetTaskFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	const annotated_lock_guard<annotated_mutex> lock(token.producer_lock);
	return GetTaskFromProducerLocked(token, task);
}

bool TaskScheduler::GetTaskFromProducerLocked(ProducerToken &token, shared_ptr<Task> &task) {
	const annotated_lock_guard<annotated_mutex> guard(lock);
	return queue.DequeueFromProducer(token, task);
}

idx_t TaskScheduler::SpareLimit() const {
	return MaxValue<idx_t>(4 * target.load(std::memory_order_relaxed), 8);
}

void TaskScheduler::WakeIdle(idx_t count, SchedulerWakeList &wake) {
	const auto slots = target.load(std::memory_order_relaxed);
	const auto busy = running.load(std::memory_order_relaxed) + waking;
	auto available = slots > busy ? slots - busy : 0;
	const auto queued = queue.GetTasksInQueue();
	count = MinValue<idx_t>(count, queued > waking ? queued - waking : 0);
	for (auto list : {&idle, &idle_spares}) {
		while (count > 0 && available > 0 && !list->empty()) {
			auto &worker = *list->back();
			list->pop_back();
			worker.idle = false;
			worker.woken = true;
			waking++;
			wake.workers.push_back(&worker);
			count--;
			available--;
		}
	}
	while (count > 0 && available > 0 && blocked > 0 && spares < SpareLimit()) {
		spares++;
		wake.spawn++;
		count--;
		available--;
	}
}

void TaskScheduler::RemoveIdle(SchedulerWorker &worker) {
	auto &list = worker.spare ? idle_spares : idle;
	list.erase(std::find(list.begin(), list.end(), &worker));
	worker.idle = false;
}

void TaskScheduler::Wake(SchedulerWakeList &wake) {
#ifndef DUCKDB_NO_THREADS
	for (auto worker : wake.workers) {
		worker->semaphore.signal();
	}
#endif
	for (idx_t i = 0; i < wake.spawn; i++) {
		if (!pool->SpawnSpare(*this)) {
			const annotated_lock_guard<annotated_mutex> guard(lock);
			spares--;
		}
	}
}

void TaskScheduler::WakeAll() {
	SchedulerWakeList wake;
	{
		const annotated_lock_guard<annotated_mutex> guard(lock);
		for (auto list : {&idle, &idle_spares}) {
			for (auto worker : *list) {
				worker->idle = false;
				worker->woken = true;
				waking++;
				wake.workers.push_back(worker);
			}
			list->clear();
		}
	}
	Wake(wake);
}

void TaskScheduler::SetTargetThreads(idx_t count) {
	if (target.load(std::memory_order_relaxed) != count) {
		target.store(count, std::memory_order_relaxed);
	}
}

void TaskScheduler::FinishSlice(SchedulerWorker &worker, SchedulerSlice &slice) {
	if (worker.holds_slot) {
		running.fetch_sub(1, std::memory_order_relaxed);
		worker.holds_slot = false;
	}
	queue.Charge(*slice.account, TimePoint::ToNanos(slice.used) - SLICE_ESTIMATE_NANOS);
	if (slice.requeue) {
		auto &producer = *slice.requeue->token;
		queue.Enqueue(producer, std::move(slice.requeue));
	}
}

bool TaskScheduler::TryAcquire(SchedulerWorker &worker, shared_ptr<Task> &task, SchedulerWakeList &wake) {
	if (worker.woken) {
		worker.woken = false;
		waking--;
	}
	const auto slots = target.load(std::memory_order_relaxed);
	const auto busy = running.load(std::memory_order_relaxed);
	if (busy < slots && worker.spare && !idle.empty()) {
		WakeIdle(1, wake);
	} else if (busy < slots && queue.Dequeue(task)) {
		if (worker.idle) {
			RemoveIdle(worker);
		}
		worker.idle_phase = 0;
		running.fetch_add(1, std::memory_order_relaxed);
		worker.holds_slot = true;
		worker.account = task->token->account;
		queue.Charge(*worker.account, SLICE_ESTIMATE_NANOS);
		if (queue.GetTasksInQueue() > 0) {
			WakeIdle(1, wake);
		}
		return true;
	}
	if (!worker.idle) {
		worker.idle = true;
		(worker.spare ? idle_spares : idle).push_back(&worker);
	}
	return false;
}

bool TaskScheduler::AcquireTask(SchedulerWorker &worker, shared_ptr<Task> &task, SchedulerSlice &slice) {
	SchedulerWakeList wake;
	bool acquired;
	if (slice.requeue) {
		auto &producer = *slice.requeue->token;
		{
			const annotated_lock_guard<annotated_mutex> producer_guard(producer.producer_lock);
			{
				const annotated_lock_guard<annotated_mutex> guard(lock);
				FinishSlice(worker, slice);
				acquired = TryAcquire(worker, task, wake);
			}
			producer.producer_cv.Signal();
		}
	} else {
		const annotated_lock_guard<annotated_mutex> guard(lock);
		if (slice.pending) {
			FinishSlice(worker, slice);
		}
		acquired = TryAcquire(worker, task, wake);
	}
	slice.pending = false;
	slice.account.reset();
	Wake(wake);
	return acquired;
}

void TaskScheduler::FlushSlice(SchedulerWorker &worker, SchedulerSlice &slice) {
	if (!slice.pending) {
		return;
	}
	if (slice.requeue) {
		auto &producer = *slice.requeue->token;
		const annotated_lock_guard<annotated_mutex> producer_guard(producer.producer_lock);
		{
			const annotated_lock_guard<annotated_mutex> guard(lock);
			FinishSlice(worker, slice);
		}
		producer.producer_cv.Signal();
	} else {
		const annotated_lock_guard<annotated_mutex> guard(lock);
		FinishSlice(worker, slice);
	}
	slice.pending = false;
	slice.account.reset();
}

bool TaskScheduler::Park(SchedulerWorker &worker) {
#ifndef DUCKDB_NO_THREADS
	static constexpr int64_t SPARE_IDLE_WAIT = 5000000;
	if (worker.spare) {
		if (worker.semaphore.wait(SPARE_IDLE_WAIT)) {
			return true;
		}
		{
			const annotated_lock_guard<annotated_mutex> guard(lock);
			if (worker.idle) {
				RemoveIdle(worker);
				return false;
			}
		}
		worker.semaphore.wait();
		return true;
	}
	if (!worker.semaphore.wait(IdleWaitMicros(worker))) {
		IdleTimeout(worker);
	}
	return true;
#else
	return false;
#endif
}

int64_t TaskScheduler::IdleWaitMicros(SchedulerWorker &worker) {
	static constexpr int64_t IDLE_WAIT = 1000000;
	if (worker.idle_phase == 0) {
		return INITIAL_FLUSH_WAIT;
	}
	if (worker.idle_phase == 1) {
		auto decay_delay = Allocator::DecayDelay();
		if (decay_delay.IsValid()) {
			return MaxValue<int64_t>(UnsafeNumericCast<int64_t>(decay_delay.GetIndex()) * 1000000 - INITIAL_FLUSH_WAIT,
			                         INITIAL_FLUSH_WAIT);
		}
	}
	return IDLE_WAIT;
}

void TaskScheduler::IdleTimeout(SchedulerWorker &worker) {
	const auto &block_allocator = BlockAllocator::Get(db);
	if (!block_allocator.SupportsFlush() || worker.idle_phase >= 2) {
		worker.idle_phase = 2;
		return;
	}
	if (worker.idle_phase == 0) {
		block_allocator.ThreadFlush(Settings::Get<AllocatorBackgroundThreadsSetting>(db),
		                            StringUtil::ParseFormattedBytes(Settings::Get<AllocatorFlushThresholdSetting>(db)),
		                            pool->NumberOfThreads());
		worker.idle_phase = Allocator::DecayDelay().IsValid() ? 1 : 2;
		return;
	}
	Allocator::ThreadIdle();
	worker.idle_phase = 2;
}

void TaskScheduler::RunTask(SchedulerWorker &worker, shared_ptr<Task> &task, SchedulerSlice &slice) {
	worker.in_task = true;
	worker.must_yield = false;
	worker.blocked_ticks = 0;
	TaskExecutionResult result;
	int64_t used = 0;
	if (task->blocking) {
		ReleaseSlot(worker);
		worker.blocking_depth++;
		result = task->Execute(TaskExecutionMode::PROCESS_PARTIAL);
		worker.blocking_depth--;
		const annotated_lock_guard<annotated_mutex> guard(lock);
		worker.released_slot = false;
		blocked--;
	} else {
		worker.slice_start = TimePoint::Now();
		result = task->Execute(TaskExecutionMode::PROCESS_PARTIAL);
		used = MaxValue<int64_t>(TimePoint::Now() - worker.slice_start - worker.blocked_ticks, 0);
	}
	worker.in_task = false;
	switch (result) {
	case TaskExecutionResult::TASK_FINISHED:
	case TaskExecutionResult::TASK_ERROR:
		task.reset();
		break;
	case TaskExecutionResult::TASK_BLOCKED:
		task->Deschedule();
		task.reset();
		break;
	case TaskExecutionResult::TASK_NOT_FINISHED:
		slice.requeue = std::move(task);
		break;
	}
	slice.account = std::move(worker.account);
	slice.used = used;
	slice.pending = true;
}

void TaskScheduler::ReleaseSlot(SchedulerWorker &worker) {
	SchedulerWakeList wake;
	{
		const annotated_lock_guard<annotated_mutex> guard(lock);
		running.fetch_sub(1, std::memory_order_relaxed);
		worker.holds_slot = false;
		worker.released_slot = true;
		blocked++;
		if (queue.GetTasksInQueue() > 0) {
			WakeIdle(1, wake);
		}
	}
	Wake(wake);
}

void TaskScheduler::ReacquireSlot(SchedulerWorker &worker) {
	const annotated_lock_guard<annotated_mutex> guard(lock);
	worker.must_yield = running.fetch_add(1, std::memory_order_relaxed) >= target.load(std::memory_order_relaxed);
	worker.holds_slot = true;
	worker.released_slot = false;
	blocked--;
}

bool TaskScheduler::ShouldYield() {
	auto worker = current_worker;
	if (!worker) {
		return driver_slice_start && TimePoint::Now() - driver_slice_start >= MinSliceTicks();
	}
	if (!worker->in_task || worker->blocking_depth > 0 || !worker->account) {
		return false;
	}
	if (worker->must_yield) {
		return true;
	}
	if (TimePoint::Now() - worker->slice_start - worker->blocked_ticks < MinSliceTicks()) {
		return false;
	}
	const auto queued = worker->scheduler.queue.GetTasksInQueue();
	return queued > worker->account->queued_tasks.load(std::memory_order_relaxed);
}

void TaskScheduler::WorkerLoop(atomic<bool> *marker, bool spare) {
#ifndef DUCKDB_NO_THREADS
	SchedulerWorker worker(*this, spare);
	current_worker = &worker;
	shared_ptr<Task> task;
	SchedulerSlice slice;
	// loop until the marker is set to false
	while (*marker) {
		if (AcquireTask(worker, task, slice)) {
			RunTask(worker, task, slice);
			continue;
		}
		if (!*marker || !Park(worker)) {
			break;
		}
	}
	FlushSlice(worker, slice);
	{
		const annotated_lock_guard<annotated_mutex> guard(lock);
		if (worker.idle) {
			RemoveIdle(worker);
		}
		if (worker.woken) {
			worker.woken = false;
			waking--;
		}
		if (spare) {
			spares--;
		}
	}
	current_worker = nullptr;
	// this thread will exit, flush all of its outstanding allocations
	const auto &block_allocator = BlockAllocator::Get(db);
	if (block_allocator.SupportsFlush()) {
		block_allocator.ThreadFlush(Settings::Get<AllocatorBackgroundThreadsSetting>(db), 0, pool->NumberOfThreads());
		Allocator::ThreadIdle();
	}
#else
	throw NotImplementedException("DuckDB was compiled without threads! Background thread loop is not allowed.");
#endif
}

void TaskScheduler::ExecuteForever(atomic<bool> *marker) {
	WorkerLoop(marker, false);
}

void TaskScheduler::ExecuteSpare(atomic<bool> *marker) {
	WorkerLoop(marker, true);
}

idx_t TaskScheduler::ExecuteTasks(atomic<bool> *marker, idx_t max_tasks) {
#ifndef DUCKDB_NO_THREADS
	idx_t completed_tasks = 0;
	// loop until the marker is set to false
	while (*marker && completed_tasks < max_tasks) {
		shared_ptr<Task> task;
		{
			const annotated_lock_guard<annotated_mutex> guard(lock);
			if (!queue.Dequeue(task)) {
				return completed_tasks;
			}
		}
		auto execute_result = task->Execute(TaskExecutionMode::PROCESS_ALL);

		switch (execute_result) {
		case TaskExecutionResult::TASK_FINISHED:
		case TaskExecutionResult::TASK_ERROR:
			task.reset();
			completed_tasks++;
			break;
		case TaskExecutionResult::TASK_NOT_FINISHED:
			throw InternalException("Task should not return TASK_NOT_FINISHED in PROCESS_ALL mode");
		case TaskExecutionResult::TASK_BLOCKED:
			task->Deschedule();
			task.reset();
			break;
		}
	}
	return completed_tasks;
#else
	throw NotImplementedException("DuckDB was compiled without threads! Background thread loop is not allowed.");
#endif
}

void TaskScheduler::ExecuteTasks(idx_t max_tasks) {
#ifndef DUCKDB_NO_THREADS
	atomic<bool> marker(true);
	try {
		ExecuteTasks(&marker, max_tasks);
	} catch (...) {
		return;
	}
#else
	throw NotImplementedException("DuckDB was compiled without threads! Background thread loop is not allowed.");
#endif
}

idx_t TaskScheduler::NumberOfThreads() {
	return pool->NumberOfThreads();
}

idx_t TaskScheduler::QueryThreads(ClientContext &context) {
	const auto pool = GetScheduler(context).NumberOfThreads();
	const auto &cap = ClientConfig::GetConfig(context).threads;
	return cap.IsValid() ? MinValue<idx_t>(pool, cap.GetIndex()) : pool;
}

idx_t TaskScheduler::NumberOfAsyncThreads() {
	return async_threads.load(std::memory_order_relaxed);
}

idx_t TaskScheduler::QueryAsyncThreads(ClientContext &context) {
	const auto async_threads = Settings::Get<AsyncThreadsSetting>(context);
	return async_threads < 0 ? GetScheduler(context).NumberOfAsyncThreads() : NumericCast<idx_t>(async_threads);
}

TaskSchedulerType TaskScheduler::QueryAsyncPool(ClientContext &context) {
	return QueryAsyncThreads(context) == 0 ? TaskSchedulerType::REGULAR : TaskSchedulerType::ASYNC;
}

idx_t TaskScheduler::GetNumberOfTasks() const {
	return queue.GetTasksInQueue();
}

idx_t TaskScheduler::GetTaskCountForProducer(ProducerToken &token) const {
	const annotated_lock_guard<annotated_mutex> guard(lock);
	return queue.GetTaskCountForProducer(token);
}

void TaskScheduler::SetThreads(idx_t total_threads, idx_t external_threads) {
	if (total_threads == 0) {
		throw SyntaxException("Number of threads must be positive!");
	}
#ifndef DUCKDB_NO_THREADS
	if (total_threads < external_threads) {
		throw SyntaxException("Number of threads can't be smaller than number of external threads!");
	}
#else
	if (total_threads != external_threads) {
		throw NotImplementedException(
		    "DuckDB was compiled without threads! Setting total_threads != external_threads is not allowed.");
	}
#endif
	pool->SetThreads(total_threads - external_threads);
}

void TaskScheduler::SetAsyncThreads(idx_t n) {
#ifdef DUCKDB_NO_THREADS
	if (n != 0) {
		throw NotImplementedException(
		    "DuckDB was compiled without threads! Setting async threads != 0 is not allowed.");
	}
#endif
	async_threads.store(n, std::memory_order_relaxed);
}

void TaskScheduler::Signal(idx_t n) {
	SchedulerWakeList wake;
	{
		const annotated_lock_guard<annotated_mutex> guard(lock);
		WakeIdle(n, wake);
	}
	Wake(wake);
}

void TaskScheduler::YieldThread() {
#ifndef DUCKDB_NO_THREADS
	std::this_thread::yield();
#endif
}

idx_t TaskScheduler::GetEstimatedCPUId() {
#if defined(__EMSCRIPTEN__)
	// FIXME: Wasm + multithreads can likely be implemented as
	//   return return (idx_t)std::hash<std::thread::id>()(std::this_thread::get_id());
	return 0;
#else
	// this code comes from jemalloc
#if defined(_WIN32)
	return (idx_t)GetCurrentProcessorNumber();
#elif defined(_GNU_SOURCE)
	auto cpu = sched_getcpu();
	if (cpu < 0) {
#ifndef DUCKDB_NO_THREADS
		// fallback to thread id
		return (idx_t)std::hash<std::thread::id>()(std::this_thread::get_id());
#else

		return 0;
#endif
	}
	return (idx_t)cpu;
#elif defined(__aarch64__) && defined(__APPLE__)
	/* Other oses most likely use tpidr_el0 instead */
	uintptr_t c;
	asm volatile("mrs %x0, tpidrro_el0" : "=r"(c)::"memory");
	return (idx_t)(c & ((1 << 3) - 1));
#else
#ifndef DUCKDB_NO_THREADS
	// fallback to thread id
	return (idx_t)std::hash<std::thread::id>()(std::this_thread::get_id());
#else
	return 0;
#endif
#endif
#endif
}

void TaskScheduler::RelaunchThreads() {
	unique_lock<mutex> t(thread_lock, std::try_to_lock);
	if (!t.owns_lock()) {
		return;
	}
	pool->RelaunchThreads(*this, false);
}

} // namespace duckdb
