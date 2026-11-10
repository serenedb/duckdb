#include "catch.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/file_buffer_handle_group.hpp"
#include "test_helpers.hpp"

#include <cstring>

namespace duckdb {

namespace {
ExternalFileCachePin AllocateAndFill(BufferManager &bm, idx_t size, uint8_t fill) {
	auto handle = ExternalFileCache::AllocateUncached(bm.GetDatabase(), size);
	memset(handle.GetDataMutable(), fill, size);
	return handle;
}
} // namespace

TEST_CASE("FileBufferHandleGroup copy with single handle", "[file_buffer_handle_group]") {
	DuckDB db(":memory:");
	auto &bm = BufferManager::GetBufferManager(*db.instance);

	constexpr idx_t BUF_SIZE = 256;
	auto handle = AllocateAndFill(bm, BUF_SIZE, 0);

	for (idx_t i = 0; i < BUF_SIZE; i++) {
		handle.GetDataMutable()[i] = static_cast<uint8_t>(i & 0xFF);
	}

	vector<ExternalFileCachePin> pins;
	pins.push_back(std::move(handle));
	FileBufferHandleGroup group(std::move(pins));

	array<uint8_t, 64> dest {};
	group.CopyTo(dest.data(), dest.size());

	for (idx_t i = 0; i < dest.size(); i++) {
		REQUIRE(dest[i] == static_cast<uint8_t>(i));
	}
}

TEST_CASE("FileBufferHandleGroup copy with single handle with offset", "[file_buffer_handle_group]") {
	DuckDB db(":memory:");
	auto &bm = BufferManager::GetBufferManager(*db.instance);

	constexpr idx_t BUF_SIZE = 256;
	auto handle = AllocateAndFill(bm, BUF_SIZE, 0);
	for (idx_t i = 0; i < BUF_SIZE; i++) {
		handle.GetDataMutable()[i] = static_cast<uint8_t>(i & 0xFF);
	}

	constexpr idx_t OFFSET = 50;
	vector<ExternalFileCachePin> pins;
	handle.Narrow(OFFSET, BUF_SIZE - OFFSET);
	pins.push_back(std::move(handle));
	FileBufferHandleGroup group(std::move(pins));

	array<uint8_t, 32> dest {};
	group.CopyTo(dest.data(), dest.size());

	for (idx_t i = 0; i < dest.size(); i++) {
		REQUIRE(dest[i] == static_cast<uint8_t>(OFFSET + i));
	}
}

TEST_CASE("FileBufferHandleGroup copy partial across handles", "[file_buffer_handle_group]") {
	DuckDB db(":memory:");
	auto &bm = BufferManager::GetBufferManager(*db.instance);

	constexpr idx_t BUF_SIZE = 64;
	constexpr idx_t H1_OFFSET = 20;
	constexpr idx_t H1_LENGTH = BUF_SIZE - H1_OFFSET;  // 44 bytes from first block
	constexpr idx_t H2_LENGTH = 16;                    // 16 bytes from second block
	constexpr idx_t COPY_SIZE = H1_LENGTH + H2_LENGTH; // 60 total

	auto h1 = AllocateAndFill(bm, BUF_SIZE, 0x11);
	auto h2 = AllocateAndFill(bm, BUF_SIZE, 0x22);

	vector<ExternalFileCachePin> pins;
	h1.Narrow(H1_OFFSET, H1_LENGTH);
	h2.Narrow(0, H2_LENGTH);
	pins.push_back(std::move(h1));
	pins.push_back(std::move(h2));
	FileBufferHandleGroup group(std::move(pins));

	array<uint8_t, COPY_SIZE> dest {};
	group.CopyTo(dest.data(), dest.size());

	for (idx_t i = 0; i < H1_LENGTH; i++) {
		REQUIRE(dest[i] == 0x11);
	}
	for (idx_t i = H1_LENGTH; i < COPY_SIZE; i++) {
		REQUIRE(dest[i] == 0x22);
	}
}

} // namespace duckdb
