#include "duckdb/common/page_residency.hpp"

#include "duckdb/common/algorithm.hpp"

#include <algorithm>
#include <utility>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace duckdb {

#ifndef _WIN32

namespace {

idx_t SystemPage() noexcept {
	static const idx_t page_size = static_cast<idx_t>(::sysconf(_SC_PAGESIZE));
	return page_size;
}

//! The start of the first page of [addr, addr + size) and the bytes from there to its end
pair<uintptr_t, idx_t> PageRange(const void *addr, idx_t size) noexcept {
	const auto begin = reinterpret_cast<uintptr_t>(addr);
	const auto aligned = begin & ~(SystemPage() - 1);
	return {aligned, size + (begin - aligned)};
}

} // namespace

#endif

bool PageResidency::IsResident(const void *addr, idx_t size) noexcept {
#ifndef _WIN32
	if (size == 0) {
		return true;
	}
	constexpr idx_t MAX_PAGES = 1024;
	const auto page = SystemPage();
	const auto range = PageRange(addr, size);
#ifdef __APPLE__
	char resident[MAX_PAGES];
#else
	unsigned char resident[MAX_PAGES];
#endif
	for (idx_t done = 0; done < range.second; done += MAX_PAGES * page) {
		const auto len = MinValue(range.second - done, MAX_PAGES * page);
		if (::mincore(reinterpret_cast<void *>(range.first + done), len, resident) != 0 ||
		    !std::all_of(resident, resident + (len + page - 1) / page,
		                 [](unsigned char value) { return (value & 1) != 0; })) {
			return false;
		}
	}
	return true;
#else
	return false;
#endif
}

idx_t PageResidency::Query(const void *addr, idx_t size, std::span<unsigned char> pages) noexcept {
#ifndef _WIN32
	if (size == 0) {
		return 0;
	}
	const auto page = SystemPage();
	const auto range = PageRange(addr, size);
#ifdef __APPLE__
	auto resident = reinterpret_cast<char *>(pages.data());
#else
	auto resident = pages.data();
#endif
	if ((range.second + page - 1) / page > pages.size() ||
	    ::mincore(reinterpret_cast<void *>(range.first), range.second, resident) != 0) {
		return 0;
	}
	return page;
#else
	return 0;
#endif
}

void PageResidency::Invalidate() noexcept {
#ifndef _WIN32
	epoch.fetch_add(1, std::memory_order_relaxed);
#endif
}

bool PageResidency::PollNow() noexcept {
	auto &state = poll_state;
	state.trusted = 0;
	const auto before = Epoch();
#ifdef RUSAGE_THREAD
	rusage usage;
	if (::getrusage(RUSAGE_THREAD, &usage) != 0) {
		Invalidate();
	} else if (usage.ru_majflt != state.faults) {
		state.faults = usage.ru_majflt;
		Invalidate();
	}
#else
	Invalidate();
#endif
	const auto current = Epoch();
	state.interval = std::exchange(state.epoch, current) != current
	                     ? PollState::MIN_BYTES
	                     : MinValue(2 * state.interval, PollState::MAX_BYTES);
	return current != before;
}

void PageResidencyMap::Reset(idx_t pages_p) {
	pages = pages_p;
	bits = unique_ptr<atomic<uint64_t>[]>(new atomic<uint64_t>[(pages_p + BITS - 1) / BITS] {});
	epoch.store(0, std::memory_order_relaxed);
}

bool PageResidencyMap::Adopt(uint32_t epoch_p) const noexcept {
	auto current = epoch.load(std::memory_order_acquire);
	while (current != epoch_p) {
		if (current == CLEARING || current > epoch_p) {
			return false;
		}
		if (epoch.compare_exchange_weak(current, CLEARING, std::memory_order_acq_rel, std::memory_order_acquire)) {
			for (idx_t i = 0, n = (pages + BITS - 1) / BITS; i != n; ++i) {
				bits[i].store(0, std::memory_order_relaxed);
			}
			epoch.store(epoch_p, std::memory_order_release);
			return true;
		}
	}
	return true;
}

} // namespace duckdb
