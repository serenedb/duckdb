#include "catch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/external_file_cache/memory_reader.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

#include <limits>
#include <span>

using namespace duckdb;

TEST_CASE("A memory stream reads back what was written from the position it is at", "[memory_stream]") {
	MemoryStream stream(8);
	for (uint32_t i = 0; i < 1000; i++) {
		stream.Write<uint32_t>(i);
		stream.Write<data_t>(static_cast<data_t>(i));
	}
	REQUIRE(stream.GetPosition() == 5000);
	REQUIRE(stream.Position() == 5000);
	REQUIRE(stream.GetCapacity() >= 5000);

	stream.Rewind();
	REQUIRE(stream.GetPosition() == 0);
	for (uint32_t i = 0; i < 1000; i++) {
		REQUIRE(stream.Read<uint32_t>() == i);
		REQUIRE(stream.Read<data_t>() == static_cast<data_t>(i));
	}
	REQUIRE(stream.GetPosition() == 5000);

	// a write after reads lands where the reads stopped, and a read after it where the write stopped
	stream.SetPosition(4);
	REQUIRE(stream.Read<data_t>() == 0);
	stream.Write<uint32_t>(0xDEADBEEF);
	REQUIRE(stream.GetPosition() == 9);
	REQUIRE(stream.Read<data_t>() == 1);
	stream.SetPosition(5);
	REQUIRE(stream.Read<uint32_t>() == 0xDEADBEEF);

	// rewinding a stream that writes keeps it writing
	stream.Rewind();
	stream.Write<uint64_t>(42);
	REQUIRE(stream.GetPosition() == 8);
	stream.Rewind();
	REQUIRE(stream.Read<uint64_t>() == 42);

	// a position past the end grows the stream to it
	const auto capacity = stream.GetCapacity();
	stream.SetPosition(capacity + 100);
	REQUIRE(stream.GetCapacity() >= capacity + 100);
	stream.Write<uint32_t>(7);
	stream.SetPosition(capacity + 100);
	REQUIRE(stream.Read<uint32_t>() == 7);
}

TEST_CASE("A memory stream over a borrowed buffer stops at its end", "[memory_stream]") {
	data_t buffer[16];
	MemoryStream stream(buffer, sizeof(buffer));
	stream.Write<uint64_t>(1);
	stream.Write<uint64_t>(2);
	REQUIRE_THROWS_AS(stream.Write<data_t>(3), SerializationException);
	REQUIRE(stream.GetPosition() == 16);
	REQUIRE(stream.GetData() == buffer);
	stream.Rewind();
	REQUIRE(stream.Read<uint64_t>() == 1);
	REQUIRE(stream.Read<uint64_t>() == 2);
	REQUIRE_THROWS_AS(stream.Read<data_t>(), SerializationException);

	MemoryStream moved(std::move(stream));
	REQUIRE(moved.GetPosition() == 16);
	REQUIRE(stream.GetCapacity() == 0);
	moved.SetPosition(8);
	REQUIRE(moved.Read<uint64_t>() == 2);
}

TEST_CASE("A memory stream reads the varints it wrote, at any distance from its end", "[memory_stream]") {
	const uint64_t values[] = {0,
	                           1,
	                           127,
	                           128,
	                           16383,
	                           16384,
	                           0xFFFFFFFFULL,
	                           0x100000000ULL,
	                           1ULL << 56,
	                           1ULL << 63,
	                           std::numeric_limits<uint64_t>::max()};
	MemoryStream stream(8);
	for (const auto value : values) {
		stream.WriteVarint<uint64_t>(value);
		stream.WriteVarint<uint32_t>(static_cast<uint32_t>(value));
		stream.WriteVarint<int64_t>(static_cast<int64_t>(value));
	}
	const auto written = stream.GetPosition();
	// read from a buffer that ends with the last varint, so that the last reads have no bytes to spare
	MemoryStream exact(stream.GetData(), written);
	for (const auto value : values) {
		REQUIRE(exact.ReadVarint<uint64_t>() == value);
		REQUIRE(exact.ReadVarint<uint32_t>() == static_cast<uint32_t>(value));
		REQUIRE(exact.ReadVarint<int64_t>() == static_cast<int64_t>(value));
	}
	REQUIRE(exact.GetPosition() == written);
	REQUIRE_THROWS_AS(exact.ReadVarint<uint64_t>(), SerializationException);

	data_t endless[32];
	memset(endless, 0xFF, sizeof(endless));
	MemoryStream corrupt(endless, sizeof(endless));
	REQUIRE_THROWS_AS(corrupt.ReadVarint<uint64_t>(), IOException);
	MemoryStream short_corrupt(endless, 5);
	REQUIRE_THROWS(short_corrupt.ReadVarint<uint64_t>());
}

TEST_CASE("A memory reader reads bytes that lie in one piece from any position", "[memory_stream]") {
	data_t buffer[64];
	for (idx_t i = 0; i < sizeof(buffer); i++) {
		buffer[i] = static_cast<data_t>(i * 7 + 1);
	}
	MemoryReader none;
	REQUIRE_FALSE(none);
	REQUIRE_FALSE(none.File());
	REQUIRE(none.Position() == 0);
	MemoryReader in {std::span<const data_t>(buffer)};
	REQUIRE(in);
	REQUIRE_FALSE(in.File());
	REQUIRE(in.Data() == buffer);
	REQUIRE(in.Current() == buffer);
	REQUIRE(in.Read<data_t>() == buffer[0]);
	REQUIRE(in.Read<uint32_t>() == Load<uint32_t>(buffer + 1));
	REQUIRE(in.Position() == 5);
	in.Skip(3);
	in.Consume(2);
	in.ReadAheadUntil(sizeof(buffer));
	REQUIRE(in.Position() == 10);
	REQUIRE(in.Current() == buffer + 10);

	// a copy reads on from where the reader is, on its own
	MemoryReader copy(in);
	REQUIRE(copy.Read<uint16_t>() == Load<uint16_t>(buffer + 10));
	REQUIRE(in.Position() == 10);
	in = copy;
	REQUIRE(in.Position() == 12);

	// only a read of bytes is checked against their end
	in.Seek(60);
	data_t tail[8];
	in.ReadData(tail, 4);
	REQUIRE(memcmp(tail, buffer + 60, 4) == 0);
	REQUIRE(in.Position() == sizeof(buffer));
	in.Seek(60);
	REQUIRE_THROWS_AS(in.ReadData(tail, sizeof(tail)), DataCorruptionException);
	REQUIRE(in.Position() == 60);
}
