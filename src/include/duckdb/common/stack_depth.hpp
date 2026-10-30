//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/stack_depth.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <cstdint>
#include <limits>

namespace duckdb {

class StackDepth {
public:
	static void Check() {
		if (DUCKDB_UNLIKELY(StackPointer() < limit)) {
			CheckSlow();
		}
	}

	[[noreturn]] DUCKDB_API static void Throw();

private:
	static uintptr_t StackPointer() {
		return reinterpret_cast<uintptr_t>(__builtin_stack_address());
	}

	DUCKDB_API static void CheckSlow();
	static uintptr_t Limit();

	static constexpr idx_t SAFETY_MARGIN = 512 * 1024;

	static inline thread_local uintptr_t limit = std::numeric_limits<uintptr_t>::max();
};

} // namespace duckdb
