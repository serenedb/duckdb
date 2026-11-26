#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "duckdb/storage/buffer/buffer_pool.hpp"

namespace duckdb {

BufferPoolReservation::BufferPoolReservation(MemoryTag tag, BufferPool &pool) : tag(tag), pool(pool) {
}

BufferPoolReservation::BufferPoolReservation(BufferPoolReservation &&src) noexcept
    : tag(src.tag), pool(src.pool), account(std::move(src.account)) {
	size = src.size;
	src.size = 0;
}

BufferPoolReservation &BufferPoolReservation::operator=(BufferPoolReservation &&src) noexcept {
	pool.UpdateUsedMemory(tag, -UnsafeNumericCast<int64_t>(size));
	if (account) {
		account->Detach(size);
	}
	tag = src.tag;
	size = src.size;
	account = std::move(src.account);
	src.size = 0;
	return *this;
}

BufferPoolReservation::~BufferPoolReservation() {
	D_ASSERT(size == 0);
}

void BufferPoolReservation::Resize(idx_t new_size) {
	auto delta = UnsafeNumericCast<int64_t>(new_size) - UnsafeNumericCast<int64_t>(size);
	pool.UpdateUsedMemory(tag, delta);
	if (account) {
		if (new_size > size) {
			account->Attach(new_size - size);
		} else {
			account->Detach(size - new_size);
		}
	}
	size = new_size;
}

void BufferPoolReservation::Retag(MemoryTag new_tag) {
	pool.UpdateUsedMemory(tag, -UnsafeNumericCast<int64_t>(size));
	pool.UpdateUsedMemory(new_tag, UnsafeNumericCast<int64_t>(size));
	tag = new_tag;
}

void BufferPoolReservation::Merge(BufferPoolReservation src) {
	if (src.account != account) {
		if (src.account) {
			src.account->Detach(src.size);
		}
		if (account) {
			account->Attach(src.size);
		}
	}
	size += src.size;
	src.size = 0;
}

TempBufferPoolReservation BufferPoolReservation::Split(idx_t split_size) {
	D_ASSERT(split_size <= size);
	TempBufferPoolReservation result(tag, pool, 0);
	result.account = account;
	result.size = split_size;
	size -= split_size;
	return result;
}

} // namespace duckdb
