//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/left_right.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace duckdb {

template <typename T>
class LeftRight {
public:
	LeftRight() = default;
	explicit LeftRight(const T &init) : instances {init, init} {
	}

	template <typename Fn>
	decltype(auto) Read(Fn &&fn) const {
		const auto vi = version_index.load(std::memory_order_acquire);
		read_indicator[vi].fetch_add(1, std::memory_order_acq_rel);
		struct Departer {
			std::atomic<int64_t> &indicator;
			~Departer() {
				indicator.fetch_sub(1, std::memory_order_acq_rel);
			}
		} departer {read_indicator[vi]};
		const auto side = left_right.load(std::memory_order_acquire);
		return fn(instances[side]);
	}

	template <typename Fn>
	void Write(Fn &&fn) {
		lock_guard<mutex> guard(writers_lock);
		const auto cur = left_right.load(std::memory_order_relaxed);
		const auto next = cur ^ 1;
		fn(instances[next]);
		left_right.store(next, std::memory_order_release);
		DrainReaders();
		fn(instances[cur]);
	}

private:
	void DrainReaders() {
		const auto prev = version_index.load(std::memory_order_relaxed);
		const auto next = prev ^ 1;
		while (read_indicator[next].load(std::memory_order_acquire) != 0) {
		}
		version_index.store(next, std::memory_order_release);
		while (read_indicator[prev].load(std::memory_order_acquire) != 0) {
		}
	}

	mutable std::array<std::atomic<int64_t>, 2> read_indicator {};
	std::atomic<int> version_index = 0;
	std::atomic<int> left_right = 0;
	mutex writers_lock;
	std::array<T, 2> instances {};
};

} // namespace duckdb
