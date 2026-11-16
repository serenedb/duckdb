//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/write_stream.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/encoding_util.hpp"
#include "duckdb/common/vector.hpp"

#include <cstring>
#include <type_traits>

namespace duckdb {

//! A stream of bytes written in order.
//! A stream that owns a buffer exposes the unwritten part of it as its window: a write that fits goes straight into
//! the window without a virtual call, and Overflow takes the ones that do not. A stream without a buffer keeps the
//! window empty and sees every write in Overflow.
class WriteStream {
public:
	WriteStream() {
		CloseWindow();
	}
	WriteStream(const WriteStream &) : WriteStream() {
	}
	WriteStream &operator=(const WriteStream &) {
		return *this;
	}
	virtual ~WriteStream() {
	}

	// Writes a set amount of data from the specified buffer into the stream and moves the stream forward accordingly
	[[gnu::always_inline]] void WriteData(const_data_ptr_t buffer, idx_t write_size) {
		if (write_size > WindowRoom()) [[unlikely]] {
			Overflow(buffer, write_size);
			return;
		}
		if (write_size == 0) {
			return;
		}
		memcpy(write_pos, buffer, write_size);
		write_pos += write_size;
	}

	virtual data_ptr_t Alloc(idx_t size) {
		return nullptr;
	}

	// Writes a type into the stream and moves the stream forward sizeof(T) bytes
	// The type must be a standard layout type
	template <class T>
	[[gnu::always_inline]] void Write(T element) {
		static_assert(std::is_standard_layout<T>(), "Write element must be a standard layout data type");
		if (WindowRoom() < sizeof(T)) [[unlikely]] {
			WriteSlow(element);
			return;
		}
		memcpy(write_pos, &element, sizeof(T));
		write_pos += sizeof(T);
	}

	//! Writes an integer as LEB128 and moves the stream forward by its encoded size
	template <class T>
	[[gnu::always_inline]] void WriteVarint(T value) {
		if (WindowRoom() < EncodingUtil::MaxLEB128Size<T>()) [[unlikely]] {
			WriteVarintSlow(value);
			return;
		}
		write_pos += EncodingUtil::EncodeLEB128<T>(write_pos, value);
	}

	//! The number of bytes written so far, in a stream that keeps a window
	idx_t Position() const {
		return write_offset + WindowSize();
	}

protected:
	//! Takes a write the window has no room for
	virtual void Overflow(const_data_ptr_t buffer, idx_t write_size) = 0;

	[[gnu::always_inline]] idx_t WindowRoom() const {
		D_ASSERT(write_pos <= write_end);
		return static_cast<idx_t>(write_end - write_pos);
	}
	//! The number of bytes written into the window
	idx_t WindowSize() const {
		return static_cast<idx_t>(write_pos - write_begin);
	}
	//! Makes [begin, begin + size) the window, with nothing written into it yet
	void SetWindow(data_ptr_t begin, idx_t size) {
		write_begin = begin;
		write_pos = begin;
		write_end = begin + size;
	}
	//! Leaves the stream without a window: every write goes to Overflow
	void CloseWindow() {
		SetWindow(data_ptr_cast(this), 0);
	}

protected:
	data_ptr_t write_begin;
	data_ptr_t write_pos;
	data_ptr_t write_end;
	//! The number of bytes written before the window
	idx_t write_offset = 0;

private:
	template <class T>
	[[gnu::noinline]] void WriteSlow(T element) {
		Overflow(const_data_ptr_cast(&element), sizeof(T));
	}

	template <class T>
	[[gnu::noinline]] void WriteVarintSlow(T value) {
		data_t encoded[EncodingUtil::MaxLEB128Size<T>()];
		WriteData(encoded, EncodingUtil::EncodeLEB128<T>(encoded, value));
	}
};

} // namespace duckdb
