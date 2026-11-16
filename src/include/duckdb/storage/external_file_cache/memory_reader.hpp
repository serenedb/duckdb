//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/external_file_cache/memory_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/storage/external_file_cache/read_ahead_window.hpp"

#include <cstring>
#include <span>
#include <type_traits>

namespace duckdb {

//! Reads bytes that lie in memory in one piece, a mapped file or a buffer, from any position: the reader of the
//! loops that decode them. It is pointers and offsets that copy as such and has no virtual call. Its user knows
//! what the bytes hold, so a value is read without a check against their end; ReadData is checked
class MemoryReader {
public:
	//! Every byte is in memory: a pointer into them stays valid wherever the reader goes
	static constexpr bool CONTIGUOUS = true;

public:
	MemoryReader() = default;
	//! Reads [bytes], which outlive the reader
	explicit MemoryReader(std::span<const data_t> bytes)
	    : begin(bytes.data()), pos(bytes.data()), end(bytes.data() + bytes.size()) {
	}
	//! Reads the mapped [file_p], which outlives the reader
	explicit MemoryReader(const InputFile &file_p)
	    : begin(file_p.Data()), pos(begin), end(begin + file_p.GetSize()), file(&file_p) {
		D_ASSERT(file_p.IsMapped());
	}

	//! Whether the reader has a file or bytes to read from
	explicit operator bool() const {
		return file || begin;
	}
	//! The file the reader reads; null for a reader of bytes
	optional_ptr<const InputFile> File() const {
		return file;
	}

	//! Reads a value without checking that the bytes hold one
	template <class T>
	[[gnu::always_inline]] T Read() {
		static_assert(std::is_standard_layout<T>(), "Read element must be a standard layout data type");
		D_ASSERT(sizeof(T) <= static_cast<idx_t>(end - pos));
		T value;
		memcpy(&value, pos, sizeof(T));
		pos += sizeof(T);
		return value;
	}
	//! Reads the next [size] bytes into [buffer]; throws when the bytes end before them
	[[gnu::always_inline]] void ReadData(data_ptr_t buffer, idx_t size) {
		if (size > static_cast<idx_t>(end - pos)) [[unlikely]] {
			ThrowPastEnd();
		}
		memcpy(buffer, pos, size);
		pos += size;
	}

	//! The first byte
	const_data_ptr_t Data() const {
		return begin;
	}
	//! The byte read next
	const_data_ptr_t Current() const {
		return pos;
	}
	idx_t Position() const {
		return static_cast<idx_t>(pos - begin);
	}
	void Seek(idx_t position) {
		D_ASSERT(position <= static_cast<idx_t>(end - begin));
		pos = begin + position;
	}
	//! Moves past the next [size] bytes
	void Skip(idx_t size) {
		D_ASSERT(size <= static_cast<idx_t>(end - pos));
		pos += size;
	}
	void Consume(idx_t size) {
		Skip(size);
	}
	//! Names the offset the reader's user reads up to from where the reader is. The kernel pages a mapped file in
	//! by itself where it is read in order; a user that reads in [steps] has the pages ahead of it asked for
	void ReadAheadUntil(idx_t until, ReadAheadSteps steps = ReadAheadSteps::NONE) {
		if (steps != ReadAheadSteps::NONE && file) {
			ahead.Arm(Position(), until, InputFile::HINT_CHUNK);
		}
	}
	//! The reader's user reads on from where the reader is (ReadAheadWindow::Advance)
	[[gnu::always_inline]] void Advance() {
		ahead.Advance(*this, Position());
	}

private:
	[[noreturn]] [[gnu::noinline]] void ThrowPastEnd() const {
		InputFile::ThrowPastEnd(file, Position(), static_cast<idx_t>(end - begin));
	}

private:
	const_data_ptr_t begin = nullptr;
	const_data_ptr_t pos = nullptr;
	const_data_ptr_t end = nullptr;
	optional_ptr<const InputFile> file;
	ReadAheadWindow ahead;
};

} // namespace duckdb
