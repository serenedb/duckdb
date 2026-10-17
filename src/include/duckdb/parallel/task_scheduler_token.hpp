//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_scheduler_token.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

// Forward declarations.
class Task;
class TaskScheduler;
class TaskSchedulerQueue;
struct ProducerToken;

enum class SchedulerClass : uint8_t { FOREGROUND, BACKGROUND };

class SchedulerAccount {
public:
	explicit SchedulerAccount(SchedulerClass scheduler_class);

private:
	friend class TaskSchedulerQueue;
	friend class TaskScheduler;

	const SchedulerClass scheduler_class;
	double deficit;
	SchedulerAccount *prev_active = nullptr;
	SchedulerAccount *next_active = nullptr;
	bool fresh = false;
	ProducerToken *runnable_head = nullptr;
	ProducerToken *runnable_tail = nullptr;
	atomic<idx_t> queued_tasks {0};
};

struct ProducerToken {
public:
	ProducerToken(TaskScheduler &scheduler, shared_ptr<SchedulerAccount> account);
	~ProducerToken();

public:
	annotated_mutex producer_lock;
	absl::CondVar producer_cv;

private:
	friend class TaskSchedulerQueue;
	friend class TaskScheduler;

	TaskScheduler &scheduler;
	shared_ptr<SchedulerAccount> account;
	deque<shared_ptr<Task>> tasks;
	ProducerToken *prev_runnable = nullptr;
	ProducerToken *next_runnable = nullptr;
	atomic<bool> runnable {false};
};

} // namespace duckdb
