//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_scheduler_queue.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parallel/task_scheduler_token.hpp"

namespace duckdb {

class Task;

class TaskSchedulerQueue {
public:
	TaskSchedulerQueue();

public:
	void Enqueue(ProducerToken &token, shared_ptr<Task> task);
	//! Dequeues a task enqueued by this producer, return whether a was found and assigned to `task`.
	bool DequeueFromProducer(ProducerToken &token, shared_ptr<Task> &task);
	bool Dequeue(shared_ptr<Task> &task);
	void Charge(SchedulerAccount &account, double cost);
	void RemoveToken(ProducerToken &token);
	idx_t GetTaskCountForProducer(ProducerToken &token) const;

	idx_t GetTasksInQueue() const {
		return queued.load(std::memory_order_relaxed);
	}

private:
	struct AccountList {
		SchedulerAccount *head = nullptr;
		SchedulerAccount *tail = nullptr;
	};

	struct ClassState {
		double weight = 1;
		double vruntime = 0;
		bool active = false;
		AccountList fresh;
		AccountList backlog;
	};

	ClassState &GetClass(SchedulerAccount &account);
	optional_ptr<ClassState> PickClass();
	void Activate(SchedulerAccount &account);
	void Deactivate(SchedulerAccount &account);
	static SchedulerAccount &NextAccount(ClassState &state);
	static void PushBack(AccountList &list, SchedulerAccount &account);
	static void Erase(AccountList &list, SchedulerAccount &account);
	static void Append(ProducerToken &producer);
	static void Unlink(ProducerToken &producer);
	void TakeFront(ProducerToken &producer, shared_ptr<Task> &task);

	array<ClassState, 2> classes;
	double root_vruntime = 0;
	atomic<idx_t> queued {0};
};

} // namespace duckdb
