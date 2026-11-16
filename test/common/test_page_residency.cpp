#include "catch.hpp"
#include "duckdb/common/page_residency.hpp"
#include "test_helpers.hpp"

#include <cstring>
#include <random>
#include <span>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace duckdb;

TEST_CASE("A page residency map counts the pages set from a page on, within its epoch", "[page_residency]") {
	PageResidencyMap map;
	map.Reset(200);
	REQUIRE_FALSE(map.Valid(7));
	REQUIRE(map.Adopt(7));
	REQUIRE(map.Valid(7));
	REQUIRE(map.Test(0, 199) == 0);

	map.Set(60, 130);
	REQUIRE(map.Test(60, 130) == 71);
	REQUIRE(map.Test(63, 64) == 2);
	REQUIRE(map.Test(128));
	REQUIRE_FALSE(map.Test(59));
	REQUIRE_FALSE(map.Test(131));
	REQUIRE(map.Test(59, 130) == 0);
	REQUIRE(map.Test(60, 131) == 71);
	REQUIRE(map.Test(128, 199) == 3);

	map.Set(199);
	REQUIRE(map.Test(199));
	REQUIRE(map.Test(198, 199) == 0);

	// an earlier epoch does not take the map back
	REQUIRE_FALSE(map.Adopt(6));
	REQUIRE(map.Valid(7));
	REQUIRE(map.Test(60, 130) == 71);

	// a later one clears it
	REQUIRE(map.Adopt(8));
	REQUIRE(map.Valid(8));
	REQUIRE_FALSE(map.Valid(7));
	REQUIRE_FALSE(map.Test(60));
	REQUIRE_FALSE(map.Test(199));
}

TEST_CASE("A page residency map counts leading set pages across its words", "[page_residency]") {
	constexpr idx_t PAGES = 320;
	std::mt19937 rng(42);
	std::vector<std::vector<bool>> patterns;
	for (const double density : {0.0, 0.5, 0.97, 1.0}) {
		std::bernoulli_distribution warm(density);
		patterns.emplace_back(PAGES);
		for (idx_t page = 0; page < PAGES; page++) {
			patterns.back()[page] = warm(rng);
		}
	}
	patterns.emplace_back(PAGES, true);
	for (const idx_t page : {63, 64, 127, 255, 256, 319}) {
		patterns.back()[page] = false;
	}

	PageResidencyMap map;
	map.Reset(PAGES);
	uint32_t epoch = 0;
	for (auto &pattern : patterns) {
		REQUIRE(map.Adopt(++epoch));
		for (idx_t page = 0; page < PAGES; page++) {
			if (pattern[page]) {
				map.Set(page);
			}
		}
		bool matches = true;
		for (idx_t first = 0; first < PAGES; first++) {
			idx_t warm = 0;
			for (idx_t last = first; last < PAGES; last++) {
				if (warm == last - first && pattern[last]) {
					warm++;
				}
				matches = matches && map.Test(first, last) == warm;
			}
		}
		REQUIRE(matches);
	}
}

TEST_CASE("Invalidating page residency ends the epoch", "[page_residency]") {
	const auto epoch = PageResidency::Epoch();
	PageResidency::Invalidate();
	REQUIRE(PageResidency::Epoch() != epoch);
}

#ifndef _WIN32
TEST_CASE("Page residency tells the pages a mapping holds in memory from the ones it does not", "[page_residency]") {
	const auto page = idx_t(::sysconf(_SC_PAGESIZE));
	constexpr idx_t PAGES = 4;
	unsigned char pages[PAGES];
	REQUIRE(PageResidency::IsResident(nullptr, 0));
	REQUIRE(PageResidency::Query(nullptr, 0, pages) == 0);
	auto mapped =
	    static_cast<char *>(::mmap(nullptr, PAGES * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	REQUIRE(mapped != MAP_FAILED);
	REQUIRE_FALSE(PageResidency::IsResident(mapped, PAGES * page));
	memset(mapped, 1, PAGES * page);
	REQUIRE(PageResidency::IsResident(mapped, PAGES * page));
	REQUIRE(PageResidency::Query(mapped, PAGES * page, std::span<unsigned char>(pages, 1)) == 0);
	REQUIRE(PageResidency::Query(mapped, PAGES * page, pages) == page);
	REQUIRE((pages[0] & 1) != 0);
	::munmap(mapped, PAGES * page);
}
#endif

#ifdef RUSAGE_THREAD
TEST_CASE("A page residency poll ends the epoch after its thread read a page in from disk", "[page_residency]") {
	const auto page = idx_t(::sysconf(_SC_PAGESIZE));
	constexpr idx_t PAGES = 64;
	const auto path = TestCreatePath("page_residency_fault.bin");
	const std::vector<char> bytes(PAGES * page, 'x');
	auto fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
	REQUIRE(fd >= 0);
	REQUIRE(::write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size()));
	REQUIRE(::fsync(fd) == 0);
	REQUIRE(::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0);
	auto mapped = static_cast<const volatile char *>(::mmap(nullptr, bytes.size(), PROT_READ, MAP_SHARED, fd, 0));
	REQUIRE(mapped != MAP_FAILED);
	// more than a thread ever trusts, so it takes its page faults so far as seen
	PageResidency::Poll(idx_t(1) << 40);
	rusage before;
	REQUIRE(::getrusage(RUSAGE_THREAD, &before) == 0);
	idx_t read = 0;
	for (idx_t offset = 0; offset < bytes.size(); offset += page) {
		read += mapped[offset] == 'x';
	}
	rusage after;
	REQUIRE(::getrusage(RUSAGE_THREAD, &after) == 0);
	const auto epoch = PageResidency::Epoch();
	const auto ended = PageResidency::Poll(idx_t(1) << 40);
	// a file system that keeps its files in memory reads nothing from disk
	if (after.ru_majflt != before.ru_majflt) {
		REQUIRE(ended);
		REQUIRE(PageResidency::Epoch() != epoch);
	}
	REQUIRE(read == PAGES);
	::munmap(const_cast<char *>(mapped), bytes.size());
	::close(fd);
	::unlink(path.c_str());
}
#endif

TEST_CASE("A page residency poll reports only the evictions its own thread saw", "[page_residency]") {
	const auto poll = [](idx_t bytes) {
		bool evicted = false;
		for (idx_t done = 0; done < bytes; done += PageResidency::PAGE_SIZE) {
			evicted = PageResidency::Poll(PageResidency::PAGE_SIZE) || evicted;
		}
		return evicted;
	};
	while (true) {
		// more than a thread ever trusts, so it looks at its page faults now
		PageResidency::Poll(idx_t(1) << 40);
		const auto epoch = PageResidency::Epoch();
		bool evicted = poll(16ULL * 1024 * 1024);
		// what another thread found is not this thread's eviction
		PageResidency::Invalidate();
		evicted = poll(2ULL * 1024 * 1024) || evicted;
		if (PageResidency::Epoch() != epoch + 1) {
			// a page fault of this thread ended an epoch meanwhile
			continue;
		}
		REQUIRE_FALSE(evicted);
		break;
	}
}
