#include "duckdb/common/serializer/memory_stream.hpp"

#include "duckdb/common/allocator.hpp"

namespace duckdb {

MemoryStream::MemoryStream(Allocator &allocator_p, idx_t capacity_p)
    : allocator(&allocator_p), capacity(capacity_p), data(allocator_p.AllocateData(capacity_p)) {
	D_ASSERT(capacity_p != 0 && IsPowerOfTwo(capacity_p));
}

MemoryStream::MemoryStream(idx_t capacity) : MemoryStream(Allocator::DefaultAllocator(), capacity) {
}

MemoryStream::MemoryStream(data_ptr_t buffer, idx_t capacity_p) : capacity(capacity_p), data(buffer) {
}

MemoryStream::~MemoryStream() {
	if (allocator && data) {
		allocator->FreeData(data, capacity);
	}
}

MemoryStream::MemoryStream(MemoryStream &&other) noexcept {
	// Move the data from the other stream into this stream
	allocator = other.allocator;
	data = other.data;
	capacity = other.capacity;
	Park(other.GetPosition());

	// Reset the other stream
	other.Park(0);
	other.allocator = nullptr;
	other.data = nullptr;
	other.capacity = 0;
}

MemoryStream &MemoryStream::operator=(MemoryStream &&other) noexcept {
	if (this != &other) {
		// Free the current data
		if (allocator) {
			allocator->FreeData(data, capacity);
		}

		// Move the data from the other stream into this stream
		Park(other.GetPosition());
		allocator = other.allocator;
		data = other.data;
		capacity = other.capacity;

		// Reset the other stream
		other.Park(0);
		other.allocator = nullptr;
		other.data = nullptr;
		other.capacity = 0;
	}
	return *this;
}

void MemoryStream::Park(idx_t position) {
	mode = Mode::PARKED;
	CloseWindow();
	CloseReadWindow();
	write_offset = position;
}

void MemoryStream::Overflow(const_data_ptr_t source, idx_t write_size) {
	memcpy(Alloc(write_size), source, write_size);
}

data_ptr_t MemoryStream::Alloc(idx_t size) {
	GrowCapacity(size);
	if (mode != Mode::WRITING) {
		const auto at = GetPosition();
		Park(at);
		mode = Mode::WRITING;
		write_offset = 0;
		SetWindow(data, capacity);
		write_pos += at;
	}
	const auto allocated = write_pos;
	write_pos += size;
	return allocated;
}

void MemoryStream::GrowCapacity(idx_t write_size) {
	const auto at = GetPosition();
	auto new_capacity = capacity;
	while (at + write_size > new_capacity) {
		if (allocator) {
			new_capacity *= 2;
		} else {
			throw SerializationException("Failed to serialize: not enough space in buffer to fulfill write request");
		}
	}
	if (new_capacity != capacity) {
		Park(at);
		data = allocator->ReallocateData(data, capacity, new_capacity);
		capacity = new_capacity;
	}
}

void MemoryStream::Underflow(QueryContext context, data_ptr_t destination, idx_t read_size) {
	const auto at = GetPosition();
	if (at + read_size > capacity) {
		throw SerializationException("Failed to deserialize: not enough data in buffer to fulfill read request");
	}
	if (mode != Mode::READING) {
		Park(at);
		mode = Mode::READING;
		SetReadWindow(data + at, capacity - at);
	}
	memcpy(destination, read_pos, read_size);
	read_pos += read_size;
}

void MemoryStream::Release() {
	allocator = nullptr;
}

void MemoryStream::SetPosition(idx_t position_p) {
	if (position_p > capacity) {
		GrowCapacity(position_p - GetPosition());
	}
	if (mode == Mode::WRITING) {
		write_pos = write_begin + position_p;
	} else {
		Park(position_p);
	}
}

} // namespace duckdb
