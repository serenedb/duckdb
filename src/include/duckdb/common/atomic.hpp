//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/atomic.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"

#include <absl/base/config.h>

#include <algorithm>
#include <atomic>

namespace duckdb {

using std::atomic;

template <class T>
inline void StoreRacy(T &target, T value) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
	__atomic_store_n(&target, value, __ATOMIC_RELAXED);
#else
	target = value;
#endif
}

template <class T>
inline T LoadRacy(const T &source) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
	return __atomic_load_n(&source, __ATOMIC_RELAXED);
#else
	return source;
#endif
}

template <class T>
inline void FillRacy(T *target, idx_t count, T value) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
	for (idx_t i = 0; i < count; i++) {
		__atomic_store_n(target + i, value, __ATOMIC_RELAXED);
	}
#else
	std::fill_n(target, count, value);
#endif
}

//! NOTE: When repeatedly trying to atomically set a value in a loop, you can use as the loop condition:
//! * std::atomic_compare_exchange_weak
//! * std::atomic::compare_exchange_weak
//! If not used as a loop condition, use:
//! * std::atomic_compare_exchange_strong
//! * std::atomic::compare_exchange_strong
//! If this is not done correctly, we may get correctness issues when using older compiler versions (see: issue #14389)
//! Performance may be optimized using std::memory_order, but NOT at the cost of correctness.
//! For correct examples of this, see concurrentqueue.h

} // namespace duckdb
