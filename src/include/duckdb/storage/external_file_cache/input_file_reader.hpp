//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/input_file_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/read_stream.hpp"
#include "duckdb/storage/external_file_cache/read_ahead_window.hpp"

#include <span>

namespace duckdb {

//! Reads a file of the external file cache from any position. Its window is the block the reader is in, which it
//! keeps pinned: bytes that lie in one block are read in place, and only a read across two blocks is copied. The
//! blocks behind a reader that goes through the ends of blocks in order are asked for ahead of it (ReadAheadWindow).
//! Over a mapped file, or bytes in memory, the window is all of them
class InputFileReader final : public ReadStream {
public:
	//! Only the block the reader is in is in memory: a pointer into it is valid until the reader leaves the block
	static constexpr bool CONTIGUOUS = false;

public:
	InputFileReader() : begin(read_pos) {
	}
	//! Reads [bytes], which outlive the reader
	explicit InputFileReader(std::span<const data_t> bytes) : begin(bytes.data()) {
		SetReadWindow(bytes.data(), bytes.size());
	}
	//! Reads [file_p], which outlives the reader
	explicit InputFileReader(const InputFile &file_p) : begin(read_pos), file(file_p.Mutable()) {
		if (file_p.Data()) {
			begin = file_p.Data();
			SetReadWindow(begin, file_p.GetSize());
		}
	}
	InputFileReader(InputFileReader &&other) noexcept
	    : begin(other.begin), start(other.start), file(other.file), block(std::move(other.block)), ahead(other.ahead) {
		read_pos = other.read_pos;
		read_end = other.read_end;
		other.DropWindow();
	}
	InputFileReader &operator=(InputFileReader &&other) noexcept {
		if (this != &other) {
			begin = other.begin;
			read_pos = other.read_pos;
			read_end = other.read_end;
			start = other.start;
			file = other.file;
			block = std::move(other.block);
			ahead = other.ahead;
			other.DropWindow();
		}
		return *this;
	}

	//! Whether the reader has a file or bytes to read from
	explicit operator bool() const {
		return file || read_end != begin;
	}
	//! The file the reader reads; null for a reader of bytes
	optional_ptr<const InputFile> File() const {
		return file.get();
	}

	//! The next [size] bytes when they lie in the window, to read in place and pass with Consume; null when they
	//! lie across two blocks or past the end
	[[gnu::always_inline]] const_data_ptr_t Peek(idx_t size) {
		if (size <= ReadWindowRoom()) [[likely]] {
			return read_pos;
		}
		return PeekSlow(size);
	}
	//! Moves past the next [size] bytes of the window
	[[gnu::always_inline]] void Consume(idx_t size) {
		D_ASSERT(size <= ReadWindowRoom());
		read_pos += size;
	}
	//! The bytes from the reader's position to the end of its window
	[[gnu::always_inline]] idx_t Available() const {
		return ReadWindowRoom();
	}

	[[gnu::always_inline]] idx_t Position() const {
		return start + static_cast<idx_t>(read_pos - begin);
	}
	[[gnu::always_inline]] void Seek(idx_t position) {
		if (const auto offset = position - start; offset <= static_cast<idx_t>(read_end - begin)) [[likely]] {
			read_pos = begin + offset;
		} else {
			SeekOutside(position);
		}
	}
	//! Moves past the next [size] bytes
	[[gnu::always_inline]] void Skip(idx_t size) {
		if (size <= ReadWindowRoom()) [[likely]] {
			read_pos += size;
		} else {
			SeekOutside(Position() + size);
		}
	}

	//! Names the offset the reader's user reads up to from where the reader is: the blocks behind the one it is in
	//! are then asked for ahead of it, never past that offset, and its second block comes with the first. They are
	//! asked for where a read runs through the end of a block the reader entered in order, and at the [steps] of a
	//! user that says where it is. Over a mapped file only the steps ask
	void ReadAheadUntil(idx_t until, ReadAheadSteps steps = ReadAheadSteps::NONE) {
		if (InBlocks()) {
			NameEnd(until, steps);
		} else if (steps != ReadAheadSteps::NONE && file) {
			ahead.Arm(Position(), until, InputFile::HINT_CHUNK);
		}
	}
	//! The reader's user reads on from where the reader is (ReadAheadWindow::Advance)
	[[gnu::always_inline]] void Advance() {
		ahead.Advance(*this, Position());
	}

	//! The [size] bytes at [offset], which stay valid while the pin lives (InputFile::Pin)
	[[gnu::always_inline]] FilePin Pin(idx_t offset, idx_t size) const {
		return PinIn(offset, size, false);
	}
	//! Pin for bytes the reader's user reads again and again (InputFile::PinShared)
	[[gnu::always_inline]] FilePin PinShared(idx_t offset, idx_t size) const {
		return PinIn(offset, size, true);
	}
	//! Reads the next [size] bytes into [buffer] for a reader that keeps its own copy and reads them once
	//! (InputFile::ReadOnce)
	DUCKDB_API void ReadOnce(data_ptr_t buffer, idx_t size);

protected:
	//! Reads across the end of the window. A read of at most a value's bytes enters the next block like the reads
	//! before it; a longer one asks for the blocks it needs at once and says nothing about what its reader reads next
	DUCKDB_API void Underflow(QueryContext context, data_ptr_t buffer, idx_t read_size) override;

private:
	//! The pin of the block the window lies in
	struct Block {
		FilePin pin;
		//! The block a reader that goes on in order enters next; the maximum before the first one it enters since
		//! it named its end
		idx_t next = NumericLimits<idx_t>::Maximum();
		//! Whether the reader's user reads in every block up to the end it named
		bool scans = false;
	};

	//! Leaves the reader without a window at its position
	void DropWindow() {
		CloseReadWindow();
		begin = read_pos;
	}
	//! Whether the file lies in blocks, which the reader pins one at a time
	bool InBlocks() const {
		return file && !file->IsMapped();
	}
	Block &GetBlock();
	//! Makes the block at the reader's position the window; false at the end of the file. A read that is not
	//! [exact] may be one of a sequence of reads
	bool Refill(bool exact);
	//! Asks for the blocks behind block [number], which the reader enters, when it goes on from the block before:
	//! it went through that block's end (unless the read is [exact] and the reader named no end), or it scans
	//! and came by a seek
	void ReadAhead(Block &pinned, idx_t number, bool exact);
	[[gnu::noinline]] DUCKDB_API const_data_ptr_t PeekSlow(idx_t size);
	[[gnu::always_inline]] FilePin PinIn(idx_t offset, idx_t size, bool shared) const {
		if (!block) {
			// bytes or a mapping are the window, all of them
			const auto length = static_cast<idx_t>(read_end - begin);
			if (offset <= length && size <= length - offset) [[likely]] {
				return FilePin(begin + offset, size);
			}
		}
		return PinSlow(offset, size, shared);
	}
	[[gnu::noinline]] DUCKDB_API FilePin PinSlow(idx_t offset, idx_t size, bool shared) const;
	[[gnu::noinline]] DUCKDB_API void SeekOutside(idx_t position);
	DUCKDB_API void NameEnd(idx_t until, ReadAheadSteps steps);

private:
	//! The first byte of the window
	const_data_ptr_t begin;
	//! The offset of the window in the file
	idx_t start = 0;
	optional_ptr<InputFile> file;
	unique_ptr<Block> block;
	ReadAheadWindow ahead;
};

} // namespace duckdb
