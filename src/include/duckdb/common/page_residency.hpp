//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/page_residency.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/unique_ptr.hpp"

#include <bit>
#include <span>

namespace duckdb {

//! What the process knows of the pages of its mapped files that are in memory. Asking the kernel costs a system
//! call, so readers remember the pages they saw or asked for, and trust that until the process may have lost
//! pages: every map's knowledge belongs to an epoch, which ends when a thread sees a major page fault
struct PageResidency {
	static constexpr idx_t PAGE_SIZE = 4ULL * 1024;

	//! Asks the kernel whether every page of [addr, addr + size) is in memory
	DUCKDB_API static bool IsResident(const void *addr, idx_t size) noexcept;
	//! Asks the kernel which pages of [addr, addr + size) are in memory, a byte per page in [pages]; returns the
	//! size of a page, or 0 when it could not ask
	DUCKDB_API static idx_t Query(const void *addr, idx_t size, std::span<unsigned char> pages) noexcept;

	static uint32_t Epoch() noexcept {
		return epoch.load(std::memory_order_relaxed);
	}
	//! Ends the epoch: the pages every map knows may be gone
	DUCKDB_API static void Invalidate() noexcept;
	//! Counts [bytes] the calling thread trusted to be in memory. Once they pass what the thread trusts before it
	//! looks at its page faults again, it ends the epoch when it had a major page fault since it last looked, and
	//! trusts longer while the epoch stays; returns whether this call ended the epoch
	[[gnu::always_inline]] static bool Poll(idx_t bytes) noexcept {
		poll_state.trusted += bytes;
		if (poll_state.trusted < poll_state.interval) [[likely]] {
			return false;
		}
		return PollNow();
	}

private:
	//! How many bytes a thread trusts before it looks at its page faults again. Looking costs a system call; the
	//! most it trusts bounds the pages a reader faults in one by one after the kernel dropped them
	struct PollState {
		static constexpr idx_t MIN_BYTES = 128ULL * 1024;
		static constexpr idx_t MAX_BYTES = 1024ULL * 1024;

		idx_t trusted = 0;
		idx_t interval = MIN_BYTES;
		//! The major page faults the thread had when it last looked
		int64_t faults = 0;
		uint32_t epoch = 0;
	};

	DUCKDB_API static bool PollNow() noexcept;

	static inline atomic<uint32_t> epoch {1};
	static thread_local PollState poll_state;
};

inline thread_local PageResidency::PollState PageResidency::poll_state;

//! A bit per page of a mapped file: set once the page was seen in memory or asked for in the map's epoch
class PageResidencyMap {
public:
	DUCKDB_API void Reset(idx_t pages);

	bool Valid(uint32_t epoch_p) const noexcept {
		return epoch.load(std::memory_order_acquire) == epoch_p;
	}
	//! Makes [epoch_p] the map's epoch, clearing what it knew of an earlier one; false when another thread is
	//! clearing it or it belongs to a later epoch
	DUCKDB_API bool Adopt(uint32_t epoch_p) const noexcept;

	//! The pages set from [first] on, up to [last]
	idx_t Test(idx_t first, idx_t last) const noexcept {
		D_ASSERT(first <= last && last < pages);
		for (auto w = first / BITS, end = last / BITS; w <= end; ++w) {
			const auto cold = ~bits[w].load(std::memory_order_relaxed) & Mask(w, first, last);
			if (cold != 0) {
				return w * BITS + static_cast<idx_t>(std::countr_zero(cold)) - first;
			}
		}
		return last - first + 1;
	}
	bool Test(idx_t page) const noexcept {
		D_ASSERT(page < pages);
		return ((bits[page / BITS].load(std::memory_order_relaxed) >> (page % BITS)) & 1) != 0;
	}

	void Set(idx_t first, idx_t last) const noexcept {
		D_ASSERT(first <= last && last < pages);
		for (auto w = first / BITS, end = last / BITS; w <= end; ++w) {
			const auto mask = Mask(w, first, last);
			if ((bits[w].load(std::memory_order_relaxed) & mask) != mask) {
				bits[w].fetch_or(mask, std::memory_order_relaxed);
			}
		}
	}
	void Set(idx_t page) const noexcept {
		Set(page, page);
	}

private:
	static constexpr idx_t BITS = 64;
	static constexpr uint32_t CLEARING = NumericLimits<uint32_t>::Maximum();

	static uint64_t Mask(idx_t word, idx_t first, idx_t last) noexcept {
		const auto lo = word == first / BITS ? first % BITS : 0;
		const auto hi = word == last / BITS ? last % BITS : BITS - 1;
		return (~uint64_t(0) >> (BITS - 1 - hi + lo)) << lo;
	}

	unique_ptr<atomic<uint64_t>[]> bits;
	idx_t pages = 0;
	mutable atomic<uint32_t> epoch {0};
};

} // namespace duckdb
