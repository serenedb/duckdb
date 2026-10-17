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
#include "duckdb/common/mutex.hpp"

namespace duckdb {

// Forward declarations.
class Task;
class TaskScheduler;
class TaskSchedulerQueue;
struct ProducerToken;
struct SchedulerAccountTasks;
struct SchedulerProducerTasks;

enum class SchedulerClass : uint8_t { FOREGROUND, BACKGROUND };

class SchedulerAccount : public enable_shared_from_this<SchedulerAccount> {
public:
	SchedulerAccount(TaskSchedulerQueue &queue, SchedulerClass scheduler_class);
	~SchedulerAccount();

private:
	friend class TaskSchedulerQueue;
	friend class TaskScheduler;
	friend struct ProducerToken;

	TaskSchedulerQueue &queue;
	const SchedulerClass scheduler_class;
	unique_ptr<SchedulerAccountTasks> tasks;
	atomic<int64_t> queued_tasks {0};
	atomic<idx_t> bound {0};
	atomic<bool> listed {false};
	atomic<bool> claimed {false};
	atomic<bool> rekey_pending {false};
	atomic<bool> arriving {false};
	shared_ptr<SchedulerAccount> inbox_self;
	SchedulerAccount *inbox_next = nullptr;
	shared_ptr<SchedulerAccount> rekey_self;
	SchedulerAccount *rekey_next = nullptr;
	atomic<int64_t> vruntime {0};
	int64_t key = 0;
	idx_t level = DConstants::INVALID_INDEX;
	idx_t heap_index = DConstants::INVALID_INDEX;
	SchedulerAccount *bucket_prev = nullptr;
	SchedulerAccount *bucket_next = nullptr;
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
	unique_ptr<SchedulerProducerTasks> tasks;
};

} // namespace duckdb
