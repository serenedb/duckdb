//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/memory_account.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

class MemoryAccount {
public:
	static constexpr idx_t UNLIMITED = NumericLimits<idx_t>::Maximum();

	constexpr MemoryAccount() noexcept = default;
	constexpr explicit MemoryAccount(idx_t limit) noexcept : limit(limit) {
	}
	MemoryAccount(const MemoryAccount &) = delete;
	MemoryAccount &operator=(const MemoryAccount &) = delete;

	bool TryAttach(idx_t bytes) noexcept {
		auto now = used.fetch_add(bytes, std::memory_order_relaxed) + bytes;
		if (now > Limit()) {
			used.fetch_sub(bytes, std::memory_order_relaxed);
			return false;
		}
		return true;
	}
	void Attach(idx_t bytes) noexcept {
		used.fetch_add(bytes, std::memory_order_relaxed);
	}
	void Detach(idx_t bytes) noexcept {
		used.fetch_sub(bytes, std::memory_order_relaxed);
	}

	idx_t Used() const noexcept {
		return used.load(std::memory_order_relaxed);
	}
	idx_t Limit() const noexcept {
		return limit.load(std::memory_order_relaxed);
	}
	void SetLimit(idx_t new_limit) noexcept {
		limit.store(new_limit, std::memory_order_relaxed);
	}

private:
	atomic<idx_t> used {0};
	atomic<idx_t> limit {UNLIMITED};
};

} // namespace duckdb
