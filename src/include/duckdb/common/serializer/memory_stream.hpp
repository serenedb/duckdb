//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/memory_stream.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/serializer/read_stream.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {
class Allocator;

class MemoryStream : public WriteStream, public ReadStream {
private:
	optional_ptr<Allocator> allocator;
	idx_t capacity = 0;
	data_ptr_t data = nullptr;

public:
	static constexpr idx_t DEFAULT_INITIAL_CAPACITY = 512;

	//! Create a new owning MemoryStream with an internal  backing buffer with the specified capacity. The stream will
	//! own the backing buffer, resize it when needed and free its memory when the stream is destroyed
	explicit MemoryStream(Allocator &allocator, idx_t capacity = DEFAULT_INITIAL_CAPACITY);

	//! Create a new owning MemoryStream with an internal  backing buffer with the specified capacity. The stream will
	//! own the backing buffer, resize it when needed and free its memory when the stream is destroyed
	explicit MemoryStream(idx_t capacity = DEFAULT_INITIAL_CAPACITY);

	//! Create a new non-owning MemoryStream over the specified external buffer and capacity. The stream will not take
	//! ownership of the backing buffer, will not attempt to resize it and will not free the memory when the stream
	//! is destroyed
	explicit MemoryStream(data_ptr_t buffer, idx_t capacity);

	//! Cant copy!
	MemoryStream(const MemoryStream &) = delete;
	MemoryStream &operator=(const MemoryStream &) = delete;

	MemoryStream(MemoryStream &&other) noexcept;
	MemoryStream &operator=(MemoryStream &&other) noexcept;

	~MemoryStream() override;

	data_ptr_t Alloc(idx_t size) override;
	//! Make room for a write of [write_size] bytes at the current position.
	//! Throws if the write would exceed the capacity of the stream and the backing buffer is not owned by the stream
	void GrowCapacity(idx_t write_size);

	//! Rewind the stream to the start, keeping the capacity and the backing buffer intact
	void Rewind() {
		if (mode == Mode::WRITING) {
			write_pos = write_begin;
		} else {
			Park(0);
		}
	}

	//! Release ownership of the backing buffer and turn a owning stream into a non-owning one.
	//! The stream will no longer be responsible for freeing the data.
	//! The stream will also no longer attempt to automatically resize the buffer when the capacity is reached.
	void Release();

	//! Get a pointer to the underlying backing buffer
	data_ptr_t GetData() const {
		return data;
	}

	//! Get the current position in the stream
	idx_t GetPosition() const {
		switch (mode) {
		case Mode::WRITING:
			return WindowSize();
		case Mode::READING:
			return capacity - ReadWindowRoom();
		default:
			return write_offset;
		}
	}

	//! Get the capacity of the stream
	idx_t GetCapacity() const {
		return capacity;
	}

	//! Set the position in the stream
	void SetPosition(idx_t position);

protected:
	//! Throws if the write would exceed the capacity of the stream and the backing buffer is not owned by the stream
	void Overflow(const_data_ptr_t buffer, idx_t write_size) override;
	//! Throws if the read would exceed the capacity of the stream
	void Underflow(QueryContext context, data_ptr_t buffer, idx_t read_size) override;

private:
	//! The stream reads and writes at one position. Only the side that moved it last has its window open, so the
	//! other side finds the position out on its next call
	enum class Mode : uint8_t { PARKED, WRITING, READING };

	//! Closes both windows at [position]
	void Park(idx_t position);

	Mode mode = Mode::PARKED;
};

} // namespace duckdb
