#include "duckdb/parallel/task_scheduler_queue.hpp"

#include "duckdb/parallel/concurrentqueue.hpp"
#include "duckdb/parallel/task.hpp"

namespace duckdb {

struct SchedulerAccountTasks {
	SchedulerAccountTasks() : queue(0) {
	}

	duckdb_moodycamel::ConcurrentQueue<shared_ptr<Task>> queue;
};

struct SchedulerProducerTasks {
	explicit SchedulerProducerTasks(SchedulerAccountTasks &tasks) : token(tasks.queue) {
	}

	duckdb_moodycamel::ProducerToken token;
};

SchedulerAccount::SchedulerAccount(TaskSchedulerQueue &queue_p, SchedulerClass scheduler_class_p)
    : queue(queue_p), scheduler_class(scheduler_class_p), tasks(make_uniq<SchedulerAccountTasks>()) {
}

SchedulerAccount::~SchedulerAccount() {
	queue.Forget(*this);
}

TaskSchedulerQueue::TaskSchedulerQueue() {
}

TaskSchedulerQueue::~TaskSchedulerQueue() {
	vector<shared_ptr<SchedulerAccount>> listed_accounts;
	{
		const annotated_lock_guard<annotated_mutex> guard(lock);
		DrainInbox();
		DrainRekeys();
		for (auto &level : levels) {
			if (!level) {
				continue;
			}
			for (auto &bucket : level->buckets) {
				for (auto account = bucket.head; account;) {
					auto next = account->bucket_next;
					account->heap_index = DConstants::INVALID_INDEX;
					account->level = DConstants::INVALID_INDEX;
					account->bucket_prev = nullptr;
					account->bucket_next = nullptr;
					listed_accounts.push_back(std::move(account->inbox_self));
					account = next;
				}
			}
		}
		levels.clear();
		occupied_levels.clear();
		size = 0;
	}
}

idx_t TaskSchedulerQueue::Level::Position(int64_t key) const {
	const auto slot = key / SLOT_NANOS;
	if (slot < base_slot) {
		return BELOW_BUCKET;
	}
	if (slot - base_slot >= static_cast<int64_t>(SLOTS)) {
		return OVERFLOW_BUCKET;
	}
	return static_cast<idx_t>(slot) % SLOTS;
}

void TaskSchedulerQueue::Level::Push(SchedulerAccount &account) {
	const auto position = Position(account.key);
	auto &bucket = buckets[position];
	account.bucket_prev = bucket.tail;
	account.bucket_next = nullptr;
	if (bucket.tail) {
		bucket.tail->bucket_next = &account;
	} else {
		bucket.head = &account;
	}
	bucket.tail = &account;
	if (position < SLOTS) {
		occupied[position / 64] |= uint64_t(1) << (position % 64);
	}
	account.heap_index = position;
	size++;
}

void TaskSchedulerQueue::Level::Remove(SchedulerAccount &account) {
	const auto position = account.heap_index;
	auto &bucket = buckets[position];
	if (account.bucket_prev) {
		account.bucket_prev->bucket_next = account.bucket_next;
	} else {
		bucket.head = account.bucket_next;
	}
	if (account.bucket_next) {
		account.bucket_next->bucket_prev = account.bucket_prev;
	} else {
		bucket.tail = account.bucket_prev;
	}
	if (!bucket.head && position < SLOTS) {
		occupied[position / 64] &= ~(uint64_t(1) << (position % 64));
	}
	account.bucket_prev = nullptr;
	account.bucket_next = nullptr;
	account.heap_index = DConstants::INVALID_INDEX;
	size--;
}

idx_t TaskSchedulerQueue::Level::FirstOccupied(idx_t from, idx_t to) const {
	while (from < to) {
		const auto word = from / 64;
		const auto limit = MinValue<idx_t>(to, (word + 1) * 64);
		const auto bits = occupied[word] >> (from % 64);
		if (bits) {
			const auto position = from + static_cast<idx_t>(__builtin_ctzll(bits));
			if (position < limit) {
				return position;
			}
		}
		from = limit;
	}
	return SLOTS;
}

optional_ptr<SchedulerAccount> TaskSchedulerQueue::Level::Top() {
	if (size == 0) {
		return nullptr;
	}
	if (buckets[BELOW_BUCKET].head) {
		return buckets[BELOW_BUCKET].head;
	}
	const auto start = static_cast<idx_t>(base_slot) % SLOTS;
	auto position = FirstOccupied(start, SLOTS);
	if (position == SLOTS) {
		position = FirstOccupied(0, start);
	}
	if (position != SLOTS) {
		return buckets[position].head;
	}
	int64_t lowest = NumericLimits<int64_t>::Maximum();
	for (auto account = buckets[OVERFLOW_BUCKET].head; account; account = account->bucket_next) {
		lowest = MinValue(lowest, account->key);
	}
	Advance(lowest);
	return Top();
}

void TaskSchedulerQueue::Level::Advance(int64_t vruntime) {
	const auto slot = vruntime / SLOT_NANOS;
	if (slot <= base_slot) {
		return;
	}
	base_slot = slot;
	auto &overflow = buckets[OVERFLOW_BUCKET];
	for (auto account = overflow.head; account;) {
		auto next = account->bucket_next;
		if (Position(account->key) != OVERFLOW_BUCKET) {
			Remove(*account);
			Push(*account);
		}
		account = next;
	}
}

optional_ptr<SchedulerAccount> TaskSchedulerQueue::First() {
	for (idx_t word = 0; word < occupied_levels.size(); word++) {
		if (occupied_levels[word]) {
			const auto index = word * 64 + static_cast<idx_t>(__builtin_ctzll(occupied_levels[word]));
			return levels[index]->Top();
		}
	}
	return nullptr;
}

void TaskSchedulerQueue::Push(SchedulerAccount &account) {
	const auto index = account.bound.load(std::memory_order_relaxed);
	if (index >= levels.size()) {
		levels.resize(index + 1);
		occupied_levels.resize(index / 64 + 1);
	}
	if (!levels[index]) {
		levels[index] = make_uniq<Level>();
	}
	levels[index]->Push(account);
	occupied_levels[index / 64] |= uint64_t(1) << (index % 64);
	account.level = index;
	size++;
}

void TaskSchedulerQueue::Remove(SchedulerAccount &account) {
	const auto index = account.level;
	auto &level = *levels[index];
	level.Remove(account);
	if (level.size == 0) {
		occupied_levels[index / 64] &= ~(uint64_t(1) << (index % 64));
	}
	account.level = DConstants::INVALID_INDEX;
	size--;
}

int64_t TaskSchedulerQueue::Clamp(int64_t vruntime) const {
	const auto lowest = floor.load(std::memory_order_relaxed);
	return MinValue(MaxValue(vruntime, lowest), lowest + TURN_NANOS);
}

void TaskSchedulerQueue::List(shared_ptr<SchedulerAccount> account) {
	auto &listed_account = *account;
	auto live = listed_account.vruntime.load(std::memory_order_relaxed);
	while (live != Clamp(live) &&
	       !listed_account.vruntime.compare_exchange_weak(live, Clamp(live), std::memory_order_relaxed)) {
	}
	listed_account.key = Clamp(live);
	listed_account.inbox_self = std::move(account);
	Push(listed_account);
}

void TaskSchedulerQueue::Publish(SchedulerAccount &account) {
	if (account.bound.load(std::memory_order_relaxed) == 0 && !account.arriving.load(std::memory_order_relaxed) &&
	    !account.arriving.exchange(true, std::memory_order_seq_cst)) {
		arrivals.fetch_add(1, std::memory_order_seq_cst);
	}
	account.inbox_self = account.shared_from_this();
	auto head = inbox.load(std::memory_order_relaxed);
	do {
		account.inbox_next = head;
	} while (!inbox.compare_exchange_weak(head, &account, std::memory_order_seq_cst, std::memory_order_relaxed));
}

void TaskSchedulerQueue::Serve(SchedulerAccount &account) {
	if (account.arriving.load(std::memory_order_relaxed) &&
	    account.arriving.exchange(false, std::memory_order_seq_cst)) {
		arrivals_served.fetch_add(1, std::memory_order_seq_cst);
	}
}

void TaskSchedulerQueue::Rekey(SchedulerAccount &account) {
	if (!account.listed.load(std::memory_order_relaxed) || account.rekey_pending.exchange(true)) {
		return;
	}
	account.rekey_self = account.shared_from_this();
	auto head = rekeys.load(std::memory_order_relaxed);
	do {
		account.rekey_next = head;
	} while (!rekeys.compare_exchange_weak(head, &account, std::memory_order_release, std::memory_order_relaxed));
}

bool TaskSchedulerQueue::Unlist(SchedulerAccount &account) {
	account.listed.store(false, std::memory_order_seq_cst);
	return account.queued_tasks.load(std::memory_order_seq_cst) > 0 &&
	       !account.listed.exchange(true, std::memory_order_seq_cst);
}

void TaskSchedulerQueue::Leave(SchedulerAccount &account) {
	account.bound.fetch_sub(1, std::memory_order_relaxed);
	if (!account.claimed.exchange(false, std::memory_order_seq_cst)) {
		Rekey(account);
		return;
	}
	if (account.queued_tasks.load(std::memory_order_seq_cst) > 0 || Unlist(account)) {
		Publish(account);
	}
}

bool TaskSchedulerQueue::Unclaim(SchedulerAccount &account) {
	if (!account.claimed.load(std::memory_order_seq_cst) ||
	    !account.claimed.exchange(false, std::memory_order_seq_cst)) {
		return false;
	}
	if (account.queued_tasks.load(std::memory_order_seq_cst) > 0 || Unlist(account)) {
		Publish(account);
		return true;
	}
	return false;
}

void TaskSchedulerQueue::Detach(SchedulerAccount &account) {
	account.bound.fetch_sub(1, std::memory_order_relaxed);
	Rekey(account);
}

void TaskSchedulerQueue::Attach(SchedulerAccount &account) {
	account.bound.fetch_add(1, std::memory_order_relaxed);
	Serve(account);
	Rekey(account);
}

void TaskSchedulerQueue::DrainInbox() {
	auto head = inbox.exchange(nullptr, std::memory_order_seq_cst);
	SchedulerAccount *reversed = nullptr;
	while (head) {
		auto next = head->inbox_next;
		head->inbox_next = reversed;
		reversed = head;
		head = next;
	}
	while (reversed) {
		auto next = reversed->inbox_next;
		reversed->inbox_next = nullptr;
		Serve(*reversed);
		List(std::move(reversed->inbox_self));
		reversed = next;
	}
}

void TaskSchedulerQueue::DrainRekeys() {
	auto head = rekeys.exchange(nullptr, std::memory_order_acquire);
	while (head) {
		auto next = head->rekey_next;
		head->rekey_next = nullptr;
		auto self = std::move(head->rekey_self);
		head->rekey_pending.store(false, std::memory_order_seq_cst);
		if (head->level != DConstants::INVALID_INDEX && head->level != head->bound.load(std::memory_order_relaxed)) {
			Remove(*head);
			Push(*head);
		}
		head = next;
	}
}

bool TaskSchedulerQueue::HasListed() const {
	return heap_size.load(std::memory_order_relaxed) != 0 || inbox.load(std::memory_order_relaxed) != nullptr;
}

bool TaskSchedulerQueue::HasArrivals() const {
	return arrivals_served.load(std::memory_order_relaxed) + arrivals_leaving.load(std::memory_order_relaxed) <
	       arrivals.load(std::memory_order_relaxed);
}

bool TaskSchedulerQueue::TakeArrival() {
	auto leaving = arrivals_leaving.load(std::memory_order_relaxed);
	while (arrivals_served.load(std::memory_order_relaxed) + leaving < arrivals.load(std::memory_order_relaxed)) {
		if (arrivals_leaving.compare_exchange_weak(leaving, leaving + 1, std::memory_order_relaxed)) {
			return true;
		}
	}
	return false;
}

void TaskSchedulerQueue::Arrived() {
	arrivals_leaving.fetch_sub(1, std::memory_order_relaxed);
}

bool TaskSchedulerQueue::Enqueue(ProducerToken &token, shared_ptr<Task> *tasks, idx_t count, bool keep_claim) {
	auto &account = *token.account;
	auto &queue = account.tasks->queue;
	if (count == 1) {
		queue.enqueue(token.tasks->token, std::move(tasks[0]));
	} else {
		queue.enqueue_bulk(token.tasks->token, std::make_move_iterator(tasks), count);
	}
	account.queued_tasks.fetch_add(NumericCast<int64_t>(count), std::memory_order_seq_cst);
	queued.fetch_add(NumericCast<int64_t>(count), std::memory_order_seq_cst);
	if (!keep_claim) {
		Unclaim(account);
	}
	return !account.listed.exchange(true, std::memory_order_seq_cst);
}

bool TaskSchedulerQueue::Dequeue(SchedulerAccount &account, shared_ptr<Task> &task) {
	if (!account.tasks->queue.try_dequeue(task)) {
		return false;
	}
	account.queued_tasks.fetch_sub(1, std::memory_order_relaxed);
	queued.fetch_sub(1, std::memory_order_relaxed);
	return true;
}

bool TaskSchedulerQueue::DequeueFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	if (!token.account->tasks->queue.try_dequeue_from_producer(token.tasks->token, task)) {
		return false;
	}
	token.account->queued_tasks.fetch_sub(1, std::memory_order_relaxed);
	queued.fetch_sub(1, std::memory_order_relaxed);
	return true;
}

void TaskSchedulerQueue::Charge(SchedulerAccount &account, int64_t used_nanos) {
	account.vruntime.fetch_add(used_nanos, std::memory_order_relaxed);
}

void TaskSchedulerQueue::PublishTop() {
	generation++;
	const auto selected = First();
	uint64_t bound = NO_TOP;
	top_account.store(selected.get(), std::memory_order_relaxed);
	if (selected) {
		top_key.store(selected->key, std::memory_order_relaxed);
		bound = MinValue<uint64_t>(selected->bound.load(std::memory_order_relaxed), NO_TOP - 1);
	}
	top.store((uint64_t(generation) << 32) | bound, std::memory_order_release);
}

shared_ptr<SchedulerAccount> TaskSchedulerQueue::Select() {
	DrainInbox();
	DrainRekeys();
	shared_ptr<SchedulerAccount> selected;
	while (auto first = First()) {
		auto &account = *first;
		const auto queued_tasks = account.queued_tasks.load(std::memory_order_seq_cst);
		if (queued_tasks <= 0) {
			Remove(account);
			auto self = std::move(account.inbox_self);
			if (Unlist(account)) {
				List(std::move(self));
			}
			continue;
		}
		const auto live = account.vruntime.load(std::memory_order_relaxed);
		if (live / Level::SLOT_NANOS > account.key / Level::SLOT_NANOS) {
			Remove(account);
			account.key = live;
			Push(account);
			continue;
		}
		if (account.key > floor.load(std::memory_order_relaxed)) {
			floor.store(account.key, std::memory_order_relaxed);
		}
		Remove(account);
		account.bound.fetch_add(1, std::memory_order_relaxed);
		if (queued_tasks == 1) {
			selected = std::move(account.inbox_self);
			account.claimed.store(true, std::memory_order_seq_cst);
		} else {
			selected = account.shared_from_this();
			Push(account);
		}
		break;
	}
	PublishTop();
	heap_size.store(size, std::memory_order_relaxed);
	return selected;
}

void TaskSchedulerQueue::Refresh() {
	DrainInbox();
	DrainRekeys();
	PublishTop();
	heap_size.store(size, std::memory_order_relaxed);
}

idx_t TaskSchedulerQueue::GetTaskCountForProducer(ProducerToken &token) const {
	return token.account->tasks->queue.size_producer_approx(token.tasks->token);
}

void TaskSchedulerQueue::Forget(SchedulerAccount &account) {
	const auto remaining = account.queued_tasks.load(std::memory_order_relaxed);
	if (remaining > 0) {
		queued.fetch_sub(remaining, std::memory_order_relaxed);
	}
}

ProducerToken::ProducerToken(TaskScheduler &scheduler_p, shared_ptr<SchedulerAccount> account_p)
    : scheduler(scheduler_p), account(std::move(account_p)), tasks(make_uniq<SchedulerProducerTasks>(*account->tasks)) {
}

ProducerToken::~ProducerToken() {
}

} // namespace duckdb
