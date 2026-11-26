//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/buffer_pool_reservation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/storage/buffer/memory_account.hpp"

namespace duckdb {

enum class BlockState : uint8_t { BLOCK_UNLOADED = 0, BLOCK_LOADED = 1 };

// Forward declaration.
class BufferPool;
struct TempBufferPoolReservation;

struct BufferPoolReservation {
	MemoryTag tag;
	idx_t size {0};
	BufferPool &pool;
	shared_ptr<MemoryAccount> account;

	BufferPoolReservation(MemoryTag tag, BufferPool &pool);
	BufferPoolReservation(const BufferPoolReservation &) = delete;
	BufferPoolReservation &operator=(const BufferPoolReservation &) = delete;

	BufferPoolReservation(BufferPoolReservation &&) noexcept;
	BufferPoolReservation &operator=(BufferPoolReservation &&) noexcept;

	virtual ~BufferPoolReservation();

	void Resize(idx_t new_size);
	void Retag(MemoryTag new_tag);
	void Merge(BufferPoolReservation src);
	TempBufferPoolReservation Split(idx_t split_size);
};

struct TempBufferPoolReservation : BufferPoolReservation {
	TempBufferPoolReservation(MemoryTag tag, BufferPool &pool, idx_t size) : BufferPoolReservation(tag, pool) {
		Resize(size);
	}
	TempBufferPoolReservation(TempBufferPoolReservation &&) = default;
	~TempBufferPoolReservation() override {
		Resize(0);
	}
};

} // namespace duckdb
