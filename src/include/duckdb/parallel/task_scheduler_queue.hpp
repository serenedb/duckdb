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
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parallel/task_scheduler_token.hpp"

namespace duckdb {

class Task;

class TaskSchedulerQueue {
public:
	static constexpr idx_t NO_TOP = NumericLimits<uint32_t>::Maximum();
	static constexpr int64_t TURN_NANOS = 10000000;

	TaskSchedulerQueue();
	~TaskSchedulerQueue();

public:
	bool Enqueue(ProducerToken &token, shared_ptr<Task> *tasks, idx_t count, bool keep_claim);
	void Publish(SchedulerAccount &account);
	bool Dequeue(SchedulerAccount &account, shared_ptr<Task> &task);
	bool DequeueFromProducer(ProducerToken &token, shared_ptr<Task> &task);
	void Charge(SchedulerAccount &account, int64_t used_nanos);
	bool HasListed() const;
	bool HasArrivals() const;
	bool TakeArrival();
	void Arrived();
	shared_ptr<SchedulerAccount> Select() DUCKDB_REQUIRES(lock);
	void Refresh() DUCKDB_REQUIRES(lock);
	void Leave(SchedulerAccount &account);
	bool Unclaim(SchedulerAccount &account);
	void Detach(SchedulerAccount &account);
	void Attach(SchedulerAccount &account);
	idx_t GetTaskCountForProducer(ProducerToken &token) const;
	void Forget(SchedulerAccount &account);

	uint64_t Top() const {
		return top.load(std::memory_order_acquire);
	}

	const SchedulerAccount *TopAccount() const {
		return top_account.load(std::memory_order_relaxed);
	}

	int64_t TopKey() const {
		return top_key.load(std::memory_order_relaxed);
	}

	int64_t Floor() const {
		return floor.load(std::memory_order_relaxed);
	}

	bool Reserve(uint64_t expected) {
		return top.compare_exchange_strong(expected, expected + 1, std::memory_order_relaxed);
	}

	bool HasQueuedTasks() const {
		return queued.load(std::memory_order_seq_cst) > 0;
	}

	idx_t GetTasksInQueue() const {
		return NumericCast<idx_t>(MaxValue<int64_t>(queued.load(std::memory_order_relaxed), 0));
	}

	annotated_mutex lock;

private:
	struct Level {
		static constexpr idx_t SLOTS = 128;
		static constexpr idx_t OVERFLOW_BUCKET = SLOTS;
		static constexpr idx_t BELOW_BUCKET = SLOTS + 1;
		static constexpr int64_t SLOT_NANOS = 100000;

		struct Bucket {
			SchedulerAccount *head = nullptr;
			SchedulerAccount *tail = nullptr;
		};

		array<Bucket, SLOTS + 2> buckets;
		uint64_t occupied[2] = {0, 0};
		int64_t base_slot = 0;
		idx_t size = 0;

		optional_ptr<SchedulerAccount> Top();
		void Push(SchedulerAccount &account);
		void Remove(SchedulerAccount &account);
		void Advance(int64_t vruntime);

	private:
		idx_t Position(int64_t key) const;
		idx_t FirstOccupied(idx_t from, idx_t to) const;
	};

	optional_ptr<SchedulerAccount> First() DUCKDB_REQUIRES(lock);
	void Push(SchedulerAccount &account) DUCKDB_REQUIRES(lock);
	void Remove(SchedulerAccount &account) DUCKDB_REQUIRES(lock);
	int64_t Clamp(int64_t vruntime) const DUCKDB_REQUIRES(lock);
	bool Unlist(SchedulerAccount &account);
	void DrainInbox() DUCKDB_REQUIRES(lock);
	void DrainRekeys() DUCKDB_REQUIRES(lock);
	void List(shared_ptr<SchedulerAccount> account) DUCKDB_REQUIRES(lock);
	void Rekey(SchedulerAccount &account);
	void Serve(SchedulerAccount &account);
	void PublishTop() DUCKDB_REQUIRES(lock);

	vector<unique_ptr<Level>> levels DUCKDB_GUARDED_BY(lock);
	vector<uint64_t> occupied_levels DUCKDB_GUARDED_BY(lock);
	idx_t size DUCKDB_GUARDED_BY(lock) = 0;
	uint32_t generation DUCKDB_GUARDED_BY(lock) = 0;
	atomic<uint64_t> top {NO_TOP};
	atomic<const SchedulerAccount *> top_account {nullptr};
	atomic<int64_t> top_key {0};
	atomic<int64_t> floor {0};
	atomic<SchedulerAccount *> inbox {nullptr};
	atomic<SchedulerAccount *> rekeys {nullptr};
	atomic<uint64_t> arrivals {0};
	atomic<uint64_t> arrivals_served {0};
	atomic<uint64_t> arrivals_leaving {0};
	atomic<idx_t> heap_size {0};
	atomic<int64_t> queued {0};
};

} // namespace duckdb
