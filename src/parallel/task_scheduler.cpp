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

static constexpr int64_t MIN_SLICE_NANOS = TaskSchedulerQueue::TURN_NANOS;
static constexpr int64_t INITIAL_FLUSH_WAIT = 500000;

struct SchedulerWorker {
	SchedulerWorker(TaskScheduler &scheduler_p, bool spare_p) : scheduler(scheduler_p), spare(spare_p) {
	}

	TaskScheduler &scheduler;
	const bool spare;
	shared_ptr<SchedulerAccount> account;
	shared_ptr<SchedulerAccount> handoff;
	int64_t bound_used = 0;
	bool rotate = false;
	bool arrival_leave = false;
	idx_t idle_phase = 0;
	int64_t slice_start = 0;
	int64_t blocked_ticks = 0;
	int64_t blocked_since = 0;
	idx_t blocking_depth = 0;
	bool in_task = false;
	bool holds_slot = false;
	bool released_slot = false;
	bool must_yield = false;
	bool retired = false;
	idx_t index = DConstants::INVALID_INDEX;
	atomic<bool> woken {false};
#ifndef DUCKDB_NO_THREADS
	duckdb_moodycamel::LightweightSemaphore semaphore;
#endif
};

static thread_local SchedulerWorker *current_worker = nullptr; // NOLINT
static thread_local int64_t driver_slice_start = 0;            // NOLINT

static int64_t MinSliceTicks() {
	static const int64_t ticks = TimePoint::FromNanos(MIN_SLICE_NANOS);
	return ticks;
}

TaskScheduler::BlockingScope::BlockingScope() : worker(current_worker) {
	if (!worker || worker->blocking_depth++ > 0) {
		return;
	}
	worker->blocked_since = TimePoint::Now();
	worker->scheduler.ReleaseClaims(*worker);
	if (worker->account) {
		worker->scheduler.queue.Detach(*worker->account);
	}
	if (worker->holds_slot) {
		worker->scheduler.ReleaseSlot(*worker);
	}
}

TaskScheduler::BlockingScope::~BlockingScope() {
	if (!worker || --worker->blocking_depth > 0) {
		return;
	}
	worker->blocked_ticks += TimePoint::Now() - worker->blocked_since;
	if (worker->account) {
		worker->scheduler.queue.Attach(*worker->account);
	}
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
    : db(db), pool(make_uniq<TaskSchedulerPool>(db)), background_account(CreateAccount(SchedulerClass::BACKGROUND)) {
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
	return make_shared_ptr<SchedulerAccount>(queue, scheduler_class);
}

unique_ptr<ProducerToken> TaskScheduler::CreateProducer() {
	return CreateProducer(background_account);
}

unique_ptr<ProducerToken> TaskScheduler::CreateProducer(shared_ptr<SchedulerAccount> account) {
	return make_uniq<ProducerToken>(*this, account ? std::move(account) : background_account);
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
	if (count == 0) {
		return;
	}
	const bool blocking =
	    pool_type != TaskSchedulerType::REGULAR || producer.account->scheduler_class == SchedulerClass::BACKGROUND;
	bool activated;
	bool keep_claim;
	{
		const annotated_lock_guard<annotated_mutex> producer_guard(producer.producer_lock);
		for (idx_t i = 0; i < count; i++) {
			tasks[i]->token = producer;
			tasks[i]->blocking = blocking;
		}
		keep_claim = !blocking && count == 1 && current_worker && !current_worker->rotate &&
		             current_worker->account.get() == producer.account.get();
		activated = queue.Enqueue(producer, tasks, count, keep_claim);
		producer.producer_cv.Signal();
	}
	if (activated) {
		queue.Publish(*producer.account);
	}
	Wake(count, producer.account.get());
}

bool TaskScheduler::GetTaskFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	const annotated_lock_guard<annotated_mutex> lock(token.producer_lock);
	return GetTaskFromProducerLocked(token, task);
}

bool TaskScheduler::GetTaskFromProducerLocked(ProducerToken &token, shared_ptr<Task> &task) {
	return queue.DequeueFromProducer(token, task);
}

void TaskScheduler::Wake(idx_t count, optional_ptr<SchedulerAccount> account) {
#ifndef DUCKDB_NO_THREADS
	static thread_local vector<SchedulerWorker *> wake_list;
	const auto slots = target.load(std::memory_order_relaxed);
	const auto pending = waking.load(std::memory_order_relaxed);
	const auto busy = running.load(std::memory_order_relaxed) + pending;
	const auto queued = queue.GetTasksInQueue();
	count = MinValue<idx_t>(count, slots > busy ? slots - busy : 0);
	count = MinValue<idx_t>(count, queued > pending ? queued - pending : 0);
	idx_t index;
	while (count != 0 && idle_count.load(std::memory_order_seq_cst) != 0 && ClaimIdle(index)) {
		idle_count.fetch_sub(1, std::memory_order_relaxed);
		auto &worker = *workers[index].load(std::memory_order_acquire);
		if (account) {
			worker.handoff = account->shared_from_this();
		}
		worker.woken.store(true, std::memory_order_release);
		waking.fetch_add(1, std::memory_order_relaxed);
		wake_list.push_back(&worker);
		count--;
	}
	for (auto worker : wake_list) {
		worker->semaphore.signal();
	}
	wake_list.clear();
	while (count > 0 && SpawnSpare()) {
		count--;
	}
#endif
}

bool TaskScheduler::SpawnSpare() {
	auto current = spares.load(std::memory_order_relaxed);
	while (current < blocked.load(std::memory_order_relaxed)) {
		if (spares.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
			if (!pool->SpawnSpare(*this)) {
				spares.fetch_sub(1, std::memory_order_relaxed);
				return false;
			}
			return true;
		}
	}
	return false;
}

bool TaskScheduler::RetireSpare(SchedulerWorker &worker, bool force) {
	auto current = spares.load(std::memory_order_relaxed);
	while (force || current > blocked.load(std::memory_order_relaxed)) {
		if (spares.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
			worker.retired = true;
			return true;
		}
	}
	return false;
}

void TaskScheduler::WakeAll() {
#ifndef DUCKDB_NO_THREADS
	vector<SchedulerWorker *> wake_list;
	for (idx_t word = 0; word < WORKER_WORDS; word++) {
		auto bits = idle_bits[word].exchange(0, std::memory_order_seq_cst);
		while (bits) {
			const auto bit = static_cast<idx_t>(__builtin_ctzll(bits));
			bits &= bits - 1;
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			auto worker = workers[word * 64 + bit].load(std::memory_order_acquire);
			worker->woken.store(true, std::memory_order_release);
			waking.fetch_add(1, std::memory_order_relaxed);
			wake_list.push_back(worker);
		}
	}
	for (auto worker : wake_list) {
		worker->semaphore.signal();
	}
#endif
}

void TaskScheduler::SetTargetThreads(idx_t count) {
	if (target.load(std::memory_order_relaxed) != count) {
		target.store(count, std::memory_order_relaxed);
	}
}

void TaskScheduler::Leave(SchedulerWorker &worker) {
	if (!worker.account) {
		return;
	}
	queue.Charge(*worker.account, TimePoint::ToNanos(worker.bound_used));
	queue.Leave(*worker.account);
	worker.account.reset();
	worker.bound_used = 0;
	worker.rotate = false;
}

bool TaskScheduler::Rebind(SchedulerWorker &worker) {
	Leave(worker);
	const bool arrival = worker.arrival_leave;
	if (!arrival && (!queue.HasListed() || !queue.HasQueuedTasks())) {
		return false;
	}
	{
		const annotated_lock_guard<annotated_mutex> guard(queue.lock);
		worker.account = queue.Select();
	}
	if (arrival) {
		worker.arrival_leave = false;
		queue.Arrived();
	}
	return worker.account != nullptr;
}

void TaskScheduler::ReleaseClaims(SchedulerWorker &worker) {
	if (worker.account && queue.Unclaim(*worker.account)) {
		Wake(1, worker.account.get());
	}
}

void TaskScheduler::Unbind(SchedulerWorker &worker) {
	Leave(worker);
	if (worker.arrival_leave) {
		{
			const annotated_lock_guard<annotated_mutex> guard(queue.lock);
			queue.Refresh();
		}
		worker.arrival_leave = false;
		queue.Arrived();
	}
}

bool TaskScheduler::TakeTask(SchedulerWorker &worker, shared_ptr<Task> &task) {
	if (worker.spare && RetireSpare(worker, false)) {
		Unbind(worker);
		return false;
	}
	if (worker.handoff) {
		if (!worker.account) {
			worker.account = std::move(worker.handoff);
			queue.Attach(*worker.account);
		}
		worker.handoff.reset();
	}
	if (worker.rotate && !Rebind(worker)) {
		return false;
	}
	while (true) {
		if (running.load(std::memory_order_relaxed) >= target.load(std::memory_order_relaxed)) {
			Unbind(worker);
			return false;
		}
		if (worker.account && queue.Dequeue(*worker.account, task)) {
			const auto busy = running.fetch_add(1, std::memory_order_relaxed) + 1;
			worker.holds_slot = true;
			worker.idle_phase = 0;
			if (queue.GetTasksInQueue() > 0 && waking.load(std::memory_order_relaxed) == 0 &&
			    busy < target.load(std::memory_order_relaxed)) {
				Wake(1, worker.account.get());
			}
			return true;
		}
		if (!Rebind(worker)) {
			return false;
		}
	}
}

void TaskScheduler::RunTask(SchedulerWorker &worker, shared_ptr<Task> &task) {
	worker.in_task = true;
	worker.must_yield = false;
	worker.blocked_ticks = 0;
	TaskExecutionResult result;
	const bool blocking = task->blocking;
	if (blocking) {
		ReleaseSlot(worker);
		worker.blocking_depth++;
		result = task->Execute(TaskExecutionMode::PROCESS_PARTIAL);
		worker.blocking_depth--;
		worker.released_slot = false;
		blocked.fetch_sub(1, std::memory_order_relaxed);
	} else {
		worker.slice_start = TimePoint::Now();
		result = task->Execute(TaskExecutionMode::PROCESS_PARTIAL);
		worker.bound_used += MaxValue<int64_t>(TimePoint::Now() - worker.slice_start - worker.blocked_ticks, 0);
		if (worker.holds_slot) {
			running.fetch_sub(1, std::memory_order_relaxed);
			worker.holds_slot = false;
		}
	}
	worker.in_task = false;
	if (!blocking && worker.bound_used >= MinSliceTicks()) {
		queue.Charge(*worker.account, TimePoint::ToNanos(worker.bound_used));
		worker.bound_used = 0;
	}
	if (!blocking && !worker.rotate) {
		worker.rotate = CheckLeave(worker, 0);
	}
	switch (result) {
	case TaskExecutionResult::TASK_FINISHED:
	case TaskExecutionResult::TASK_ERROR:
		task.reset();
		break;
	case TaskExecutionResult::TASK_BLOCKED:
		task->Deschedule();
		task.reset();
		break;
	case TaskExecutionResult::TASK_NOT_FINISHED: {
		auto &producer = *task->token;
		const auto pool_type = task->blocking ? TaskSchedulerType::ASYNC : TaskSchedulerType::REGULAR;
		Enqueue(producer, &task, 1, pool_type);
		task.reset();
		break;
	}
	}
}

bool TaskScheduler::CheckLeave(SchedulerWorker &worker, int64_t slice) {
	auto &account = *worker.account;
	const auto mine = account.bound.load(std::memory_order_relaxed);
	const auto used = account.vruntime.load(std::memory_order_relaxed) + TimePoint::ToNanos(worker.bound_used + slice);
	if (queue.HasArrivals() && (mine >= 2 || used > queue.Floor() + MIN_SLICE_NANOS) && queue.TakeArrival()) {
		worker.arrival_leave = true;
		return true;
	}
	const auto top = queue.Top();
	const auto top_bound = top & TaskSchedulerQueue::NO_TOP;
	if (top_bound == TaskSchedulerQueue::NO_TOP || queue.TopAccount() == &account) {
		return false;
	}
	if (top_bound + 1 < mine || (top_bound + 1 == mine && used > queue.TopKey() + MIN_SLICE_NANOS)) {
		return queue.Reserve(top);
	}
	return false;
}

void TaskScheduler::ReleaseSlot(SchedulerWorker &worker) {
	running.fetch_sub(1, std::memory_order_relaxed);
	worker.holds_slot = false;
	worker.released_slot = true;
	blocked.fetch_add(1, std::memory_order_relaxed);
	if (queue.GetTasksInQueue() > 0) {
		Wake(1, worker.account.get());
	}
}

void TaskScheduler::ReacquireSlot(SchedulerWorker &worker) {
	worker.must_yield = running.fetch_add(1, std::memory_order_relaxed) >= target.load(std::memory_order_relaxed);
	worker.holds_slot = true;
	worker.released_slot = false;
	blocked.fetch_sub(1, std::memory_order_relaxed);
}

bool TaskScheduler::Register(SchedulerWorker &worker) {
	for (idx_t word = 0; word < WORKER_WORDS; word++) {
		auto bits = registered[word].load(std::memory_order_relaxed);
		while (~bits) {
			const auto bit = static_cast<idx_t>(__builtin_ctzll(~bits));
			const auto mask = uint64_t(1) << bit;
			bits = registered[word].fetch_or(mask, std::memory_order_acq_rel);
			if (!(bits & mask)) {
				worker.index = word * 64 + bit;
				workers[worker.index].store(&worker, std::memory_order_release);
				return true;
			}
		}
	}
	return false;
}

void TaskScheduler::Unregister(SchedulerWorker &worker) {
	if (worker.index == DConstants::INVALID_INDEX) {
		return;
	}
	workers[worker.index].store(nullptr, std::memory_order_relaxed);
	registered[worker.index / 64].fetch_and(~(uint64_t(1) << (worker.index % 64)), std::memory_order_release);
	worker.index = DConstants::INVALID_INDEX;
}

bool TaskScheduler::ClearIdle(idx_t index) {
	const auto mask = uint64_t(1) << (index % 64);
	return idle_bits[index / 64].fetch_and(~mask, std::memory_order_seq_cst) & mask;
}

bool TaskScheduler::ClaimIdle(idx_t &index) {
	const auto hint = last_idle.load(std::memory_order_relaxed);
	if (ClearIdle(hint)) {
		index = hint;
		return true;
	}
	for (idx_t word = 0; word < WORKER_WORDS; word++) {
		auto bits = idle_bits[word].load(std::memory_order_relaxed);
		while (bits) {
			const auto bit = static_cast<idx_t>(__builtin_ctzll(bits));
			const auto mask = uint64_t(1) << bit;
			const auto old = idle_bits[word].fetch_and(~mask, std::memory_order_seq_cst);
			if (old & mask) {
				index = word * 64 + bit;
				return true;
			}
			bits = old & ~mask;
		}
	}
	return false;
}

bool TaskScheduler::Park(SchedulerWorker &worker) {
#ifndef DUCKDB_NO_THREADS
	static constexpr int64_t SPARE_IDLE_WAIT = 5000000;
	if (worker.retired) {
		return false;
	}
	idle_bits[worker.index / 64].fetch_or(uint64_t(1) << (worker.index % 64), std::memory_order_seq_cst);
	idle_count.fetch_add(1, std::memory_order_seq_cst);
	last_idle.store(worker.index, std::memory_order_relaxed);
	const auto busy = running.load(std::memory_order_relaxed) + waking.load(std::memory_order_relaxed);
	if (queue.HasQueuedTasks() && busy < target.load(std::memory_order_relaxed)) {
		if (ClearIdle(worker.index)) {
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			return true;
		}
		worker.semaphore.wait();
	} else if (!worker.semaphore.wait(worker.spare ? SPARE_IDLE_WAIT : IdleWaitMicros(worker))) {
		if (ClearIdle(worker.index)) {
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			if (worker.spare) {
				RetireSpare(worker, true);
				return false;
			}
			IdleTimeout(worker);
			return true;
		}
		worker.semaphore.wait();
	}
	worker.woken.store(false, std::memory_order_relaxed);
	waking.fetch_sub(1, std::memory_order_relaxed);
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

bool TaskScheduler::ShouldYield() {
	auto worker = current_worker;
	if (!worker) {
		return driver_slice_start && TimePoint::Now() - driver_slice_start >= MinSliceTicks();
	}
	if (!worker->in_task || worker->blocking_depth > 0 || !worker->account) {
		return false;
	}
	if (worker->must_yield || worker->rotate) {
		return true;
	}
	auto &scheduler = worker->scheduler;
	if (worker->spare &&
	    scheduler.spares.load(std::memory_order_relaxed) > scheduler.blocked.load(std::memory_order_relaxed)) {
		worker->rotate = true;
		return true;
	}
	const auto slice = TimePoint::Now() - worker->slice_start - worker->blocked_ticks;
	if (worker->scheduler.CheckLeave(*worker, slice)) {
		worker->rotate = true;
		return true;
	}
	if (slice >= MinSliceTicks()) {
		worker->scheduler.ReleaseClaims(*worker);
	}
	return false;
}

void TaskScheduler::WorkerLoop(atomic<bool> *marker, bool spare) {
#ifndef DUCKDB_NO_THREADS
	SchedulerWorker worker(*this, spare);
	if (!Register(worker)) {
		if (spare) {
			spares.fetch_sub(1, std::memory_order_relaxed);
		}
		return;
	}
	current_worker = &worker;
	shared_ptr<Task> task;
	// loop until the marker is set to false
	while (*marker) {
		if (TakeTask(worker, task)) {
			RunTask(worker, task);
			continue;
		}
		if (!*marker || !Park(worker)) {
			break;
		}
	}
	Unbind(worker);
	if (spare && !worker.retired) {
		spares.fetch_sub(1, std::memory_order_relaxed);
	}
	Unregister(worker);
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
		shared_ptr<SchedulerAccount> account;
		if (queue.HasListed()) {
			const annotated_lock_guard<annotated_mutex> guard(queue.lock);
			account = queue.Select();
		}
		if (!account) {
			return completed_tasks;
		}
		if (!queue.Dequeue(*account, task)) {
			queue.Leave(*account);
			return completed_tasks;
		}
		auto execute_result = task->Execute(TaskExecutionMode::PROCESS_ALL);
		queue.Leave(*account);

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
#ifndef DUCKDB_NO_THREADS
	if (n != 0) {
		Wake(n, nullptr);
	}
#endif
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
