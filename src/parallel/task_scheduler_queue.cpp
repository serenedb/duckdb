#include "duckdb/parallel/task_scheduler_queue.hpp"

#include "duckdb/parallel/task.hpp"

namespace duckdb {

static constexpr double QUANTUM_NANOS = 1000000;

SchedulerAccount::SchedulerAccount(SchedulerClass scheduler_class_p)
    : scheduler_class(scheduler_class_p), deficit(QUANTUM_NANOS) {
}

TaskSchedulerQueue::TaskSchedulerQueue() {
	classes[static_cast<uint8_t>(SchedulerClass::FOREGROUND)].weight = 4;
	classes[static_cast<uint8_t>(SchedulerClass::BACKGROUND)].weight = 1;
}

TaskSchedulerQueue::ClassState &TaskSchedulerQueue::GetClass(SchedulerAccount &account) {
	return classes[static_cast<uint8_t>(account.scheduler_class)];
}

optional_ptr<TaskSchedulerQueue::ClassState> TaskSchedulerQueue::PickClass() {
	auto &foreground = classes[static_cast<uint8_t>(SchedulerClass::FOREGROUND)];
	auto &background = classes[static_cast<uint8_t>(SchedulerClass::BACKGROUND)];
	if (foreground.active && background.active) {
		return background.vruntime < foreground.vruntime ? &background : &foreground;
	}
	if (foreground.active) {
		return &foreground;
	}
	if (background.active) {
		return &background;
	}
	return nullptr;
}

void TaskSchedulerQueue::PushBack(AccountList &list, SchedulerAccount &account) {
	account.prev_active = list.tail;
	account.next_active = nullptr;
	if (list.tail) {
		list.tail->next_active = &account;
	} else {
		list.head = &account;
	}
	list.tail = &account;
}

void TaskSchedulerQueue::Erase(AccountList &list, SchedulerAccount &account) {
	if (account.prev_active) {
		account.prev_active->next_active = account.next_active;
	} else {
		list.head = account.next_active;
	}
	if (account.next_active) {
		account.next_active->prev_active = account.prev_active;
	} else {
		list.tail = account.prev_active;
	}
	account.prev_active = nullptr;
	account.next_active = nullptr;
}

void TaskSchedulerQueue::Activate(SchedulerAccount &account) {
	auto &state = GetClass(account);
	if (!state.active) {
		state.vruntime = MaxValue(state.vruntime, root_vruntime);
		state.active = true;
	}
	account.deficit = MinValue(account.deficit, QUANTUM_NANOS);
	account.fresh = account.deficit > 0;
	PushBack(account.fresh ? state.fresh : state.backlog, account);
}

void TaskSchedulerQueue::Deactivate(SchedulerAccount &account) {
	auto &state = GetClass(account);
	Erase(account.fresh ? state.fresh : state.backlog, account);
	if (!state.fresh.head && !state.backlog.head) {
		state.active = false;
	}
}

SchedulerAccount &TaskSchedulerQueue::NextAccount(ClassState &state) {
	while (true) {
		auto &account = state.fresh.head ? *state.fresh.head : *state.backlog.head;
		if (account.deficit > 0) {
			return account;
		}
		Erase(account.fresh ? state.fresh : state.backlog, account);
		account.fresh = false;
		account.deficit += QUANTUM_NANOS;
		PushBack(state.backlog, account);
	}
}

void TaskSchedulerQueue::Append(ProducerToken &producer) {
	auto &account = *producer.account;
	producer.runnable.store(true, std::memory_order_relaxed);
	producer.prev_runnable = account.runnable_tail;
	producer.next_runnable = nullptr;
	if (account.runnable_tail) {
		account.runnable_tail->next_runnable = &producer;
	} else {
		account.runnable_head = &producer;
	}
	account.runnable_tail = &producer;
}

void TaskSchedulerQueue::Unlink(ProducerToken &producer) {
	auto &account = *producer.account;
	if (producer.prev_runnable) {
		producer.prev_runnable->next_runnable = producer.next_runnable;
	} else {
		account.runnable_head = producer.next_runnable;
	}
	if (producer.next_runnable) {
		producer.next_runnable->prev_runnable = producer.prev_runnable;
	} else {
		account.runnable_tail = producer.prev_runnable;
	}
	producer.prev_runnable = nullptr;
	producer.next_runnable = nullptr;
	producer.runnable.store(false, std::memory_order_release);
}

void TaskSchedulerQueue::Enqueue(ProducerToken &token, shared_ptr<Task> task) {
	auto &account = *token.account;
	token.tasks.push_back(std::move(task));
	if (!token.runnable.load(std::memory_order_relaxed)) {
		const bool account_was_idle = account.runnable_head == nullptr;
		Append(token);
		if (account_was_idle) {
			Activate(account);
		}
	}
	account.queued_tasks.store(account.queued_tasks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	queued.store(queued.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void TaskSchedulerQueue::TakeFront(ProducerToken &producer, shared_ptr<Task> &task) {
	auto &account = *producer.account;
	task = std::move(producer.tasks.front());
	producer.tasks.pop_front();
	account.queued_tasks.store(account.queued_tasks.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
	queued.store(queued.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
}

bool TaskSchedulerQueue::Dequeue(shared_ptr<Task> &task) {
	auto state = PickClass();
	if (!state) {
		return false;
	}
	root_vruntime = state->vruntime;
	auto &account = NextAccount(*state);
	auto &producer = *account.runnable_head;
	TakeFront(producer, task);
	Unlink(producer);
	if (!producer.tasks.empty()) {
		Append(producer);
	} else if (!account.runnable_head) {
		Deactivate(account);
	}
	return true;
}

bool TaskSchedulerQueue::DequeueFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	if (token.tasks.empty()) {
		return false;
	}
	TakeFront(token, task);
	if (token.tasks.empty()) {
		auto &account = *token.account;
		Unlink(token);
		if (!account.runnable_head) {
			Deactivate(account);
		}
	}
	return true;
}

void TaskSchedulerQueue::Charge(SchedulerAccount &account, double cost) {
	auto &state = GetClass(account);
	account.deficit = MaxValue(account.deficit - cost, -QUANTUM_NANOS);
	state.vruntime += cost / state.weight;
}

idx_t TaskSchedulerQueue::GetTaskCountForProducer(ProducerToken &token) const {
	return token.tasks.size();
}

void TaskSchedulerQueue::RemoveToken(ProducerToken &token) {
	if (!token.runnable.load(std::memory_order_relaxed)) {
		return;
	}
	auto &account = *token.account;
	const auto count = token.tasks.size();
	token.tasks.clear();
	account.queued_tasks.store(account.queued_tasks.load(std::memory_order_relaxed) - count, std::memory_order_relaxed);
	queued.store(queued.load(std::memory_order_relaxed) - count, std::memory_order_relaxed);
	Unlink(token);
	if (!account.runnable_head) {
		Deactivate(account);
	}
}

} // namespace duckdb
