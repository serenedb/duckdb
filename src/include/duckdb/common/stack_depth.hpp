//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/stack_depth.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <absl/base/config.h>

#include <cstdint>
#include <limits>

#ifdef ABSL_HAVE_THREAD_SANITIZER
extern "C" uintptr_t __tsan_testonly_shadow_stack_current_size();
#endif

namespace duckdb {

class StackDepth {
public:
	static void Check() {
		if (DUCKDB_UNLIKELY(Exceeded())) {
			Throw();
		}
	}

	static bool Exceeded() {
#ifdef ABSL_HAVE_THREAD_SANITIZER
		if (__tsan_testonly_shadow_stack_current_size() > TSAN_MAX_FRAMES) {
			return true;
		}
#endif
		return DUCKDB_UNLIKELY(StackPointer() < limit) && StackPointer() < Limit();
	}

private:
	static uintptr_t StackPointer() {
		return reinterpret_cast<uintptr_t>(__builtin_stack_address());
	}

	DUCKDB_API static uintptr_t Limit();
	[[noreturn]] DUCKDB_API static void Throw();

	static constexpr uintptr_t UNREAD = std::numeric_limits<uintptr_t>::max();
#ifdef ABSL_HAVE_THREAD_SANITIZER
	static constexpr uintptr_t TSAN_MAX_FRAMES = 60000;
#endif

	static inline thread_local uintptr_t limit = UNREAD;
};

} // namespace duckdb
