#include "duckdb/parallel/task_scheduler.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/fault_points.hpp"
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
	bool handoff_claimed = false;
	SchedulerWorker *next_woken = nullptr;
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

TaskScheduler::BlockingScope::BlockingScope() : BlockingScope(true) {
}

TaskScheduler::BlockingScope::BlockingScope(bool wake)
    : worker(current_worker && current_worker->in_task ? current_worker : nullptr) {
	if (!worker || worker->blocking_depth++ > 0) {
		return;
	}
	worker->blocked_since = TimePoint::Now();
	worker->scheduler.ReleaseClaims(*worker, wake);
	if (worker->account) {
		worker->scheduler.queue.Detach(*worker->account);
	}
	if (worker->holds_slot) {
		worker->scheduler.ReleaseSlot(*worker, wake);
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

void TaskScheduler::ScheduleTasks(ProducerToken &producer, std::span<shared_ptr<Task>> tasks) {
	ScheduleTasks(producer, tasks, TaskSchedulerType::REGULAR);
}

void TaskScheduler::ScheduleTask(ProducerToken &token, shared_ptr<Task> task, TaskSchedulerType pool_type) {
	Enqueue(token, &task, 1, pool_type, true);
}

void TaskScheduler::ScheduleTasks(ProducerToken &producer, std::span<shared_ptr<Task>> tasks,
                                  TaskSchedulerType pool_type) {
	Enqueue(producer, tasks.data(), tasks.size(), pool_type, true);
}

void TaskScheduler::ScheduleConcurrentTask(ProducerToken &producer, shared_ptr<Task> task) {
	Enqueue(producer, &task, 1, TaskSchedulerType::REGULAR, false);
}

void TaskScheduler::ScheduleConcurrentTasks(ProducerToken &producer, std::span<shared_ptr<Task>> tasks) {
	Enqueue(producer, tasks.data(), tasks.size(), TaskSchedulerType::REGULAR, false);
}

bool TaskScheduler::StartBlockingTask(shared_ptr<Task> task) {
#ifndef DUCKDB_NO_THREADS
	if (!HasWorkers()) {
		return false;
	}
	task->blocking = true;
	const auto started = task.get();
	blocked.fetch_add(1, std::memory_order_seq_cst);
	{
		const annotated_lock_guard<annotated_mutex> guard(started_lock);
		started_tasks.push_back(std::move(task));
		started_count.fetch_add(1, std::memory_order_seq_cst);
	}
	idx_t index;
	if (idle_count.load(std::memory_order_seq_cst) != 0 && ClaimIdle(index)) {
		idle_count.fetch_sub(1, std::memory_order_relaxed);
		workers[index].load(std::memory_order_acquire)->semaphore.signal();
		return true;
	}
	if (SpawnSpare()) {
		return true;
	}
	{
		const annotated_lock_guard<annotated_mutex> guard(started_lock);
		const auto entry = std::find_if(started_tasks.begin(), started_tasks.end(),
		                                [&](const shared_ptr<Task> &candidate) { return candidate.get() == started; });
		if (entry == started_tasks.end()) {
			return true;
		}
		started_tasks.erase(entry);
		started_count.fetch_sub(1, std::memory_order_relaxed);
	}
	Unblock();
#endif
	return false;
}

void TaskScheduler::Enqueue(ProducerToken &producer, shared_ptr<Task> *tasks, idx_t count, TaskSchedulerType pool_type,
                            bool keep_in_task) {
	if (count == 0) {
		return;
	}
	const auto account = producer.account;
	const bool blocking =
	    pool_type != TaskSchedulerType::REGULAR || account->scheduler_class == SchedulerClass::BACKGROUND;
	bool activated;
	bool keep_claim;
	{
		const annotated_lock_guard<annotated_mutex> producer_guard(producer.producer_lock);
		for (idx_t i = 0; i < count; i++) {
			tasks[i]->token = producer;
			tasks[i]->blocking = blocking;
		}
		keep_claim = !blocking && count == 1 && current_worker && (keep_in_task || !current_worker->in_task) &&
		             !current_worker->rotate && current_worker->account.get() == account.get();
		activated = queue.Enqueue(producer, tasks, count, keep_claim);
		producer.epoch++;
		producer.producer_cv.SignalAll();
	}
	if (activated) {
		queue.Publish(*account);
	}
	SDB_IF_FAILURE("scheduler_enqueue_finds_no_free_slot") {
		return;
	}
	if (!keep_claim) {
		Wake(count, account.get());
	}
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
		worker.semaphore.signal();
		count--;
	}
	while (count > 0 && SpawnSpare()) {
		count--;
	}
#endif
}

void TaskScheduler::ReleaseHandoff(SchedulerWorker &worker) {
	auto account = std::move(worker.handoff);
	if (std::exchange(worker.handoff_claimed, false) && queue.Leave(*account)) {
		Wake(1, account.get());
	}
}

bool TaskScheduler::SpawnSpare() {
	auto current = spares.load(std::memory_order_relaxed);
	while (current < blocked.load(std::memory_order_relaxed) &&
	       current + target.load(std::memory_order_relaxed) < MAX_WORKERS) {
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
	while (force || current > blocked.load(std::memory_order_relaxed) + target.load(std::memory_order_relaxed)) {
		if (spares.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
			worker.retired = true;
			return true;
		}
	}
	return false;
}

void TaskScheduler::RetireIdleSpare() {
#ifndef DUCKDB_NO_THREADS
	for (idx_t word = 0; word < WORKER_WORDS; word++) {
		auto bits = idle_bits[word].load(std::memory_order_seq_cst) & spare_bits[word].load(std::memory_order_relaxed);
		while (bits) {
			const auto bit = static_cast<idx_t>(__builtin_ctzll(bits));
			const auto mask = uint64_t(1) << bit;
			bits &= bits - 1;
			if (!(idle_bits[word].fetch_and(~mask, std::memory_order_seq_cst) & mask)) {
				continue;
			}
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			auto &worker = *workers[word * 64 + bit].load(std::memory_order_acquire);
			if (!worker.spare) {
				RestoreIdle(word * 64 + bit);
				continue;
			}
			if (!RetireSpare(worker, false)) {
				RestoreIdle(word * 64 + bit);
				return;
			}
			worker.semaphore.signal();
			return;
		}
	}
#endif
}

void TaskScheduler::WakeAll() {
#ifndef DUCKDB_NO_THREADS
	for (idx_t word = 0; word < WORKER_WORDS; word++) {
		auto bits = idle_bits[word].exchange(0, std::memory_order_seq_cst);
		while (bits) {
			const auto bit = static_cast<idx_t>(__builtin_ctzll(bits));
			bits &= bits - 1;
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			auto worker = workers[word * 64 + bit].load(std::memory_order_acquire);
			worker->woken.store(true, std::memory_order_release);
			waking.fetch_add(1, std::memory_order_relaxed);
			worker->semaphore.signal();
		}
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
	const auto account = std::move(worker.account);
	worker.bound_used = 0;
	worker.rotate = false;
	if (queue.Leave(*account)) {
		Wake(1, account.get());
	}
}

bool TaskScheduler::Rebind(SchedulerWorker &worker) {
	Leave(worker);
	const bool arrival = worker.arrival_leave;
	if (arrival || (queue.HasListed() && queue.HasQueuedTasks())) {
		worker.account = Dispatch();
	}
	if (arrival) {
		worker.arrival_leave = false;
		queue.Arrived();
	}
	return worker.account != nullptr;
}

bool TaskScheduler::LockToDispatch() {
	if (queue.lock.try_lock()) {
		return true;
	}
	if (lock_waiter.load(std::memory_order_seq_cst) || lock_waiter.exchange(true, std::memory_order_seq_cst)) {
		return false;
	}
	queue.lock.lock();
	lock_waiter.store(false, std::memory_order_seq_cst);
	return true;
}

shared_ptr<SchedulerAccount> TaskScheduler::Dispatch() {
	if (!LockToDispatch()) {
		return nullptr;
	}
	queue.selecting.store(true, std::memory_order_seq_cst);
	auto selected = queue.Select();
	auto woken = HandOffListed(selected ? 1 : 0);
	queue.selecting.store(false, std::memory_order_seq_cst);
	queue.lock.unlock();
	while (woken) {
		std::exchange(woken, woken->next_woken)->semaphore.signal();
	}
	if (queue.HasListed() && queue.HasQueuedTasks()) {
		Wake(1, nullptr);
	}
	return selected;
}

SchedulerWorker *TaskScheduler::HandOffListed(idx_t reserved) {
	SchedulerWorker *woken = nullptr;
#ifndef DUCKDB_NO_THREADS
	while (idle_count.load(std::memory_order_seq_cst) != 0) {
		const auto pending = waking.load(std::memory_order_relaxed);
		const auto busy = running.load(std::memory_order_relaxed) + pending;
		if (busy >= target.load(std::memory_order_relaxed) || queue.GetTasksInQueue() <= pending + reserved) {
			break;
		}
		idx_t index;
		if (!ClaimIdle(index)) {
			break;
		}
		idle_count.fetch_sub(1, std::memory_order_relaxed);
		auto account = queue.Select();
		if (!account) {
			RestoreIdle(index);
			break;
		}
		auto &worker = *workers[index].load(std::memory_order_acquire);
		worker.handoff = std::move(account);
		worker.handoff_claimed = true;
		worker.woken.store(true, std::memory_order_release);
		waking.fetch_add(1, std::memory_order_relaxed);
		worker.next_woken = woken;
		woken = &worker;
	}
#endif
	return woken;
}

void TaskScheduler::RestoreIdle(idx_t index) {
	idle_bits[index / 64].fetch_or(uint64_t(1) << (index % 64), std::memory_order_seq_cst);
	idle_count.fetch_add(1, std::memory_order_seq_cst);
}

void TaskScheduler::ReleaseClaims(SchedulerWorker &worker, bool wake) {
	if (worker.account && queue.Unclaim(*worker.account) && wake) {
		Wake(1, worker.account.get());
	}
}

void TaskScheduler::Unbind(SchedulerWorker &worker) {
	ReleaseHandoff(worker);
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

void TaskScheduler::Arrive(SchedulerWorker &worker) {
	if (worker.woken.load(std::memory_order_relaxed)) {
		worker.woken.store(false, std::memory_order_relaxed);
		waking.fetch_sub(1, std::memory_order_relaxed);
	}
}

bool TaskScheduler::TakeTask(SchedulerWorker &worker, shared_ptr<Task> &task) {
	if (worker.spare && RetireSpare(worker, false)) {
		Arrive(worker);
		Unbind(worker);
		return false;
	}
	if (worker.handoff) {
		if (!worker.account) {
			worker.account = std::move(worker.handoff);
			if (!std::exchange(worker.handoff_claimed, false)) {
				queue.Attach(*worker.account);
			}
		}
		ReleaseHandoff(worker);
	}
	if (worker.rotate) {
		Arrive(worker);
		if (!Rebind(worker)) {
			return false;
		}
	}
	while (true) {
		if (running.load(std::memory_order_relaxed) >= target.load(std::memory_order_relaxed)) {
			Arrive(worker);
			Unbind(worker);
			return false;
		}
		if (worker.account && queue.Dequeue(*worker.account, task)) {
			const auto busy = running.fetch_add(1, std::memory_order_relaxed) + 1;
			Arrive(worker);
			worker.holds_slot = true;
			worker.idle_phase = 0;
			if (queue.GetTasksInQueue() > 0 && waking.load(std::memory_order_relaxed) == 0 &&
			    busy < target.load(std::memory_order_relaxed)) {
				Wake(1, worker.account.get());
			}
			return true;
		}
		Arrive(worker);
		if (!Rebind(worker)) {
			return false;
		}
	}
}

bool TaskScheduler::TakeStarted(shared_ptr<Task> &task) {
	if (started_count.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	const annotated_lock_guard<annotated_mutex> guard(started_lock);
	if (started_tasks.empty()) {
		return false;
	}
	task = std::move(started_tasks.back());
	started_tasks.pop_back();
	started_count.fetch_sub(1, std::memory_order_relaxed);
	return true;
}

void TaskScheduler::RunStarted(SchedulerWorker &worker, shared_ptr<Task> &task) {
	const auto account = worker.account;
	if (account) {
		ReleaseClaims(worker);
		queue.Detach(*account);
	}
	worker.blocking_depth++;
	std::ignore = task->Execute(TaskExecutionMode::PROCESS_ALL);
	worker.blocking_depth--;
	if (account) {
		queue.Attach(*account);
	}
	task.reset();
	Unblock();
}

void TaskScheduler::RunTask(SchedulerWorker &worker, shared_ptr<Task> &task) {
	worker.in_task = true;
	worker.must_yield = false;
	worker.blocked_ticks = 0;
	TaskExecutionResult result;
	const bool blocking = task->blocking;
	if (blocking) {
		ReleaseClaims(worker);
		queue.Detach(*worker.account);
		ReleaseSlot(worker);
		worker.blocking_depth++;
		result = task->Execute(TaskExecutionMode::PROCESS_PARTIAL);
		worker.blocking_depth--;
		queue.Attach(*worker.account);
		worker.released_slot = false;
		Unblock();
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
		ScheduleTask(producer, task, pool_type);
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

void TaskScheduler::ReleaseSlot(SchedulerWorker &worker, bool wake) {
	running.fetch_sub(1, std::memory_order_relaxed);
	worker.holds_slot = false;
	worker.released_slot = true;
	blocked.fetch_add(1, std::memory_order_relaxed);
	if (wake && queue.GetTasksInQueue() > 0) {
		Wake(1, worker.account.get());
	}
}

void TaskScheduler::ReacquireSlot(SchedulerWorker &worker) {
	worker.must_yield = running.fetch_add(1, std::memory_order_relaxed) >= target.load(std::memory_order_relaxed);
	worker.holds_slot = true;
	worker.released_slot = false;
	Unblock();
}

void TaskScheduler::Unblock() {
	const auto still_blocked = blocked.fetch_sub(1, std::memory_order_seq_cst) - 1;
	if (spares.load(std::memory_order_seq_cst) > still_blocked + target.load(std::memory_order_relaxed)) {
		RetireIdleSpare();
	}
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
				if (worker.spare) {
					spare_bits[word].fetch_or(mask, std::memory_order_relaxed);
				}
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
	if (worker.spare) {
		spare_bits[worker.index / 64].fetch_and(~(uint64_t(1) << (worker.index % 64)), std::memory_order_relaxed);
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
	if (worker.retired) {
		return false;
	}
	idle_bits[worker.index / 64].fetch_or(uint64_t(1) << (worker.index % 64), std::memory_order_seq_cst);
	idle_count.fetch_add(1, std::memory_order_seq_cst);
	const auto busy = running.load(std::memory_order_relaxed) + waking.load(std::memory_order_relaxed);
	if (worker.spare && spares.load(std::memory_order_seq_cst) >
	                        blocked.load(std::memory_order_seq_cst) + target.load(std::memory_order_relaxed)) {
		if (ClearIdle(worker.index)) {
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			return !RetireSpare(worker, false);
		}
		worker.semaphore.wait();
	} else if (queue.HasQueuedTasks() && busy < target.load(std::memory_order_relaxed) && queue.HasListed() &&
	           !queue.Selecting() && !lock_waiter.load(std::memory_order_seq_cst)) {
		if (ClearIdle(worker.index)) {
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			return true;
		}
		worker.semaphore.wait();
	} else if (!worker.semaphore.wait(IdleWaitMicros(worker))) {
		if (ClearIdle(worker.index)) {
			idle_count.fetch_sub(1, std::memory_order_relaxed);
			IdleTimeout(worker);
			return true;
		}
		worker.semaphore.wait();
	}
	if (worker.retired) {
		return false;
	}
	SDB_PARK_ONCE_ON_FAILURE("scheduler_woken_worker_pauses");
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
	const auto slice = TimePoint::Now() - worker->slice_start - worker->blocked_ticks;
	if (worker->scheduler.CheckLeave(*worker, slice)) {
		worker->rotate = true;
		return true;
	}
	if (slice >= MinSliceTicks()) {
		if (worker->account->queued_blocking.load(std::memory_order_relaxed) > 0 &&
		    scheduler.running.load(std::memory_order_relaxed) >= scheduler.target.load(std::memory_order_relaxed)) {
			return true;
		}
		worker->scheduler.ReleaseClaims(*worker);
	}
	return false;
}

void TaskScheduler::WorkerLoop(atomic<bool> *marker, bool spare) {
#ifndef DUCKDB_NO_THREADS
	SchedulerWorker worker(*this, spare);
	shared_ptr<Task> task;
	if (!Register(worker)) {
		if (spare) {
			spares.fetch_sub(1, std::memory_order_relaxed);
		}
		while (TakeStarted(task)) {
			RunStarted(worker, task);
		}
		return;
	}
	current_worker = &worker;
	// loop until the marker is set to false
	while (*marker) {
		if (TakeStarted(task)) {
			RunStarted(worker, task);
			continue;
		}
		if (TakeTask(worker, task)) {
			RunTask(worker, task);
			continue;
		}
		if (!*marker || !Park(worker)) {
			break;
		}
	}
	while (TakeStarted(task)) {
		RunStarted(worker, task);
	}
	Arrive(worker);
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
			if (queue.Leave(*account)) {
				Wake(1, account.get());
			}
			return completed_tasks;
		}
		auto execute_result = task->Execute(TaskExecutionMode::PROCESS_ALL);
		if (queue.Leave(*account)) {
			Wake(1, account.get());
		}

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

bool TaskScheduler::HasWorkers() const {
	return target.load(std::memory_order_relaxed) != 0;
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
	if (total_threads > MAX_WORKERS) {
		throw InvalidInputException("Number of threads can't be larger than %d!", MAX_WORKERS);
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
