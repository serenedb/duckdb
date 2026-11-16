//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/read_ahead_window.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

namespace duckdb {

//! Whether the user of a reader says where it is at every step (Advance), on top of what the reader sees itself
enum class ReadAheadSteps : uint8_t {
	//! It does not: it reads the bytes in order, and the reader sees a read run through the end of a block
	NONE,
	//! It does, and may skip bytes: more is asked for while it reads more than it skips
	SKIPPING,
	//! It does, and reads in every block up to the end it named: a seek of its into the next block goes on like a
	//! read through the end of the block before
	SCANNING
};

//! The bytes of a file asked for ahead of a reader that reads it in order. Each time the reader comes within half a
//! window of the end of what was asked, the next window is asked for, twice the one before, never past the offset
//! the reader stops at. Everything that reads ahead of a sequence of reads keeps one: a request sized by guesses
//! of several kinds is asked for several times, or past what its reader reads
class ReadAheadWindow {
public:
	//! Starts a window of [first] bytes at [at] for a reader that reads no further than [stop_p]
	void Arm(idx_t at, idx_t stop_p, idx_t first) noexcept {
		last = at;
		end = at;
		stop = stop_p;
		run = 0;
		skipped = 0;
		window = first;
		asked = 0;
	}
	//! The offset no window passes
	idx_t Stop() const noexcept {
		return stop;
	}
	void Until(idx_t stop_p) noexcept {
		stop = stop_p;
	}

	//! The user of [reader] is at [at], where it reads next: counts what it read and what it skipped since its
	//! step before, and asks the reader's file for the next window once the user is within half of it of the end of
	//! what was asked, unless it skipped more than it read
	template <class READER>
	[[gnu::always_inline]] void Advance(const READER &reader, idx_t at) noexcept {
		if (at > last) {
			const auto gap = at - last;
			(gap < PageResidency::PAGE_SIZE ? run : skipped) += gap;
		}
		last = at;
		if (at + window / 2 >= end && end < stop) [[unlikely]] {
			Grow(*reader.File().get(), at);
		}
	}
	//! The reader is at [at] and reads on in order, which its caller knows: the bytes to ask for now, [least] or
	//! more unless the reader stops before, growing up to [max]; none until the reader is within half of the window
	//! asked last of its end
	[[gnu::always_inline]] FileRange Extend(idx_t at, idx_t max, idx_t least = 0) noexcept {
		if (at + asked / 2 < end || end >= stop) [[likely]] {
			return {};
		}
		return Next(at, max, least);
	}
	//! The reader went to [at], out of its order: nothing counts as asked behind it, and its next window is
	//! [first] bytes at most
	void Restart(idx_t at, idx_t first) noexcept {
		end = MinValue(end, at);
		window = MinValue(window, first);
		asked = 0;
	}
	//! The reader went on past bytes it left unread: its next window is half as large, [first] bytes or more
	void Halve(idx_t first) noexcept {
		window = MaxValue(first, window / 2);
	}
	//! The bytes from [at] on were not asked for, the bytes in flight being at their budget: they are asked for again,
	//! by a window that starts over from the least size its reader asks for, and no step asks before that
	void Rewind(idx_t at) noexcept {
		end = at;
		asked = 0;
		window = 0;
	}

private:
	FileRange Next(idx_t at, idx_t max, idx_t least) noexcept {
		const auto from = MaxValue(at, end);
		if (from >= stop) {
			return {};
		}
		const auto size = MaxValue(window, least);
		asked = MinValue(size, stop - from);
		end = from + asked;
		window = MinValue(2 * size, max);
		return {from, asked};
	}
	[[gnu::noinline]] void Grow(const InputFile &file, idx_t at) noexcept {
		if (window == 0 || run < PageResidency::PAGE_SIZE || 2 * run < skipped) {
			return;
		}
		auto max = InputFile::MAX_HINT;
		if (!file.IsMapped()) {
			// a window larger than a mapped file's is asked for as late as a reader asks for its own (Extend):
			// asked earlier, it shares the requests in flight with the reads a query waits for
			if (window > InputFile::MAX_HINT && at + asked / 2 < end) {
				return;
			}
			max = ExternalFileCache::MAX_READ_AHEAD;
		}
		const auto range = Next(at, max, 0);
		if (range.size == 0) {
			return;
		}
		if (const auto asked = file.ReadAhead(range.offset, range.size, 0); asked < range.offset + range.size) {
			Rewind(asked);
		}
	}

private:
	//! Where the reader's user was at its last step
	idx_t last = 0;
	//! The end of the bytes asked for so far
	idx_t end = 0;
	idx_t stop = 0;
	//! Bytes the user read in steps closer than a page, and the bytes of its longer steps
	idx_t run = 0;
	idx_t skipped = 0;
	//! The bytes the next request asks for, and the bytes the last one asked for
	idx_t window = 0;
	idx_t asked = 0;
};

} // namespace duckdb
