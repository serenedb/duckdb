//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/read_stream.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/query_context.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/encoding_util.hpp"
#include "duckdb/common/vector.hpp"

#include <cstring>
#include <type_traits>

namespace duckdb {

//! A stream of bytes read in order.
//! A stream that holds its next bytes in memory exposes them as its window: a read the window covers is served
//! from it without a virtual call, and Underflow takes the ones it does not cover. A stream without a buffer keeps
//! the window empty and sees every read in Underflow.
class ReadStream {
public:
	ReadStream() {
		CloseReadWindow();
	}
	ReadStream(const ReadStream &) : ReadStream() {
	}
	ReadStream &operator=(const ReadStream &) {
		return *this;
	}
	virtual ~ReadStream() {
	}

	// Reads a set amount of data from the stream into the specified buffer and moves the stream forward accordingly
	[[gnu::always_inline]] void ReadData(data_ptr_t buffer, idx_t read_size) {
		ReadData(QueryContext(), buffer, read_size);
	}
	[[gnu::always_inline]] void ReadData(QueryContext context, data_ptr_t buffer, idx_t read_size) {
		if (read_size > ReadWindowRoom()) [[unlikely]] {
			Underflow(context, buffer, read_size);
			return;
		}
		memcpy(buffer, read_pos, read_size);
		read_pos += read_size;
	}

	// Reads a type from the stream and moves the stream forward sizeof(T) bytes
	// The type must be a standard layout type
	template <class T>
	[[gnu::always_inline]] T Read() {
		return Read<T>(QueryContext());
	}

	template <class T>
	[[gnu::always_inline]] T Read(QueryContext context) {
		static_assert(std::is_standard_layout<T>(), "Read element must be a standard layout data type");
		T value;
		ReadData(context, data_ptr_cast(&value), sizeof(T));
		return value;
	}

	//! Reads an integer written as LEB128 and moves the stream past it
	template <class T>
	[[gnu::always_inline]] T ReadVarint() {
		// a corrupt encoding is found out one byte after the longest valid one
		if (ReadWindowRoom() <= EncodingUtil::MaxLEB128Size<T>()) [[unlikely]] {
			return ReadVarintSlow<T>();
		}
		T value;
		read_pos += EncodingUtil::DecodeLEB128<T>(read_pos, value);
		return value;
	}

protected:
	//! Takes a read the window does not cover
	virtual void Underflow(QueryContext context, data_ptr_t buffer, idx_t read_size) = 0;

	[[gnu::always_inline]] idx_t ReadWindowRoom() const {
		D_ASSERT(read_pos <= read_end);
		return static_cast<idx_t>(read_end - read_pos);
	}
	//! Makes [begin, begin + size) the bytes read next
	void SetReadWindow(const_data_ptr_t begin, idx_t size) {
		read_pos = begin;
		read_end = begin + size;
	}
	//! Leaves the stream without a window: every read goes to Underflow
	void CloseReadWindow() {
		SetReadWindow(EMPTY_WINDOW, 0);
	}

protected:
	const_data_ptr_t read_pos;
	const_data_ptr_t read_end;

private:
	//! Where a window without bytes points: never null, and the same for every stream, so a stream that moves
	//! takes its window as it is
	static constexpr data_t EMPTY_WINDOW[1] = {};

	template <class T>
	[[gnu::noinline]] T ReadVarintSlow() {
		data_t encoded[EncodingUtil::MaxLEB128Size<T>() + 1] = {};
		for (idx_t size = 0; size < sizeof(encoded); size++) {
			ReadData(encoded + size, 1);
			if (!(encoded[size] & 0x80)) {
				break;
			}
		}
		T value;
		EncodingUtil::DecodeLEB128<T>(encoded, value);
		return value;
	}
};

} // namespace duckdb
