#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/memory_file_system.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"

namespace duckdb {

namespace {

constexpr idx_t MEMORY_BLOCK = BlockCache::BLOCK_SIZE;

uint8_t ByteAt(idx_t offset) {
	return static_cast<uint8_t>((offset * 13 + offset / MEMORY_BLOCK) & 0xFF);
}

void WriteFile(FileSystem &fs, const string &path, idx_t size, idx_t chunk = 10000) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	vector<uint8_t> data(chunk);
	for (idx_t written = 0; written < size;) {
		const auto count = MinValue(chunk, size - written);
		for (idx_t i = 0; i < count; i++) {
			data[i] = ByteAt(written + i);
		}
		handle->Write(data.data(), count);
		written += count;
	}
	handle->Sync();
}

bool HoldsBytes(const_data_ptr_t data, idx_t offset, idx_t size) {
	for (idx_t i = 0; i < size; i++) {
		if (data[i] != ByteAt(offset + i)) {
			return false;
		}
	}
	return true;
}

bool ReadsBack(FileSystem &fs, const string &path, idx_t size) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (NumericCast<idx_t>(handle->GetFileSize()) != size) {
		return false;
	}
	vector<uint8_t> data(size);
	handle->Read(data.data(), size, 0);
	if (!HoldsBytes(data.data(), 0, size)) {
		return false;
	}
	const auto offset = size / 3;
	const auto length = MinValue<idx_t>(3 * MEMORY_BLOCK, size - offset);
	vector<uint8_t> middle(length);
	handle->Read(middle.data(), length, offset);
	return HoldsBytes(middle.data(), offset, length);
}

} // namespace

TEST_CASE("Memory files read back what was written", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;
	for (auto fs : {&blocks, &heap}) {
		for (const idx_t size :
		     {idx_t(0), idx_t(1), MEMORY_BLOCK - 1, MEMORY_BLOCK, MEMORY_BLOCK + 1, 5 * MEMORY_BLOCK + 123}) {
			const auto path = string("memory://files/f") + to_string(size);
			WriteFile(*fs, path, size);
			REQUIRE(ReadsBack(*fs, path, size));
		}
	}
	auto &vfs = FileSystem::GetFileSystem(*db.instance);
	WriteFile(vfs, "memory://registered/data.bin", 3 * MEMORY_BLOCK + 7);
	REQUIRE(ReadsBack(vfs, "memory://registered/data.bin", 3 * MEMORY_BLOCK + 7));
}

TEST_CASE("Memory files are written once, from start to end", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	WriteFile(fs, "memory://once.bin", 100);
	REQUIRE_THROWS(fs.OpenFile("memory://once.bin", FileFlags::FILE_FLAGS_WRITE));
	auto handle = fs.OpenFile("memory://other.bin", FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	uint8_t byte = 1;
	REQUIRE_THROWS(handle->Write(QueryContext(), &byte, 1, 10));
	ErrorData missing;
	try {
		fs.OpenFile("memory://missing.bin", FileFlags::FILE_FLAGS_READ);
	} catch (std::exception &ex) {
		missing = ErrorData(ex);
	}
	REQUIRE(missing.HasError());
	REQUIRE(missing.ExtraInfo().at("errno") == std::to_string(ENOENT));
	REQUIRE(
	    !fs.OpenFile("memory://missing.bin", FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS));
}

TEST_CASE("Heap memory files map in place, block files do not", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;

	WriteFile(blocks, "memory://small.bin", MEMORY_BLOCK - 10);
	REQUIRE(blocks.GetBlockFile("memory://small.bin"));
	REQUIRE_THROWS(blocks.MemoryMapFile(OpenFileInfo("memory://small.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions()));

	WriteFile(heap, "memory://heap.bin", 3 * MEMORY_BLOCK);
	auto mapped = heap.MemoryMapFile(OpenFileInfo("memory://heap.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	REQUIRE(mapped);
	REQUIRE(mapped->Size() == 3 * MEMORY_BLOCK);
	REQUIRE(HoldsBytes(mapped->GetData(0, mapped->Size()), 0, 3 * MEMORY_BLOCK));
	REQUIRE(!heap.GetBlockFile("memory://heap.bin"));

	heap.RemoveFile("memory://heap.bin");
	REQUIRE(HoldsBytes(mapped->GetData(0, mapped->Size()), 0, 3 * MEMORY_BLOCK));

	WriteFile(heap, "memory://empty.bin", 0);
	auto empty = heap.MemoryMapFile(OpenFileInfo("memory://empty.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	REQUIRE(empty);
	REQUIRE(empty->Size() == 0);

	auto writer =
	    heap.OpenFile("memory://growing.bin", FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	uint8_t byte = 1;
	writer->Write(&byte, 1);
	REQUIRE_THROWS(heap.MemoryMapFile(OpenFileInfo("memory://growing.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions()));
	writer->Close();
	auto grown = heap.MemoryMapFile(OpenFileInfo("memory://growing.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	REQUIRE(grown->Size() == 1);
}

TEST_CASE("Memory directories list, move and remove files", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	WriteFile(fs, "memory://index/a", 10);
	WriteFile(fs, "memory://index/b", 20);
	WriteFile(fs, "memory://index/sub/c", 30);
	fs.CreateDirectory("memory://index/empty");
	REQUIRE(fs.DirectoryExists("memory://index"));
	REQUIRE(fs.DirectoryExists("memory://index/empty"));
	REQUIRE(!fs.DirectoryExists("memory://other"));

	vector<pair<string, bool>> entries;
	fs.ListFiles("memory://index", [&](const string &name, bool is_dir) { entries.emplace_back(name, is_dir); });
	std::sort(entries.begin(), entries.end());
	REQUIRE(entries == vector<pair<string, bool>> {{"a", false}, {"b", false}, {"empty", true}, {"sub", true}});

	fs.MoveFile("memory://index/a", "memory://index/b");
	REQUIRE(!fs.FileExists("memory://index/a"));
	REQUIRE(ReadsBack(fs, "memory://index/b", 10));

	auto reader = fs.OpenFile("memory://index/b", FileFlags::FILE_FLAGS_READ);
	fs.RemoveFile("memory://index/b");
	REQUIRE(!fs.FileExists("memory://index/b"));
	vector<uint8_t> data(10);
	reader->Read(data.data(), 10, 0);
	REQUIRE(HoldsBytes(data.data(), 0, 10));

	fs.RemoveDirectory("memory://index");
	REQUIRE(!fs.DirectoryExists("memory://index"));
	REQUIRE(!fs.FileExists("memory://index/sub/c"));
}

TEST_CASE("Memory files larger than the block cache spill and read back", "[memory_file_system]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_size='1MB'"));
	REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + TestCreatePath("memory_file_spill") + "'"));
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	WriteFile(fs, "memory://spill/big.bin", 16 * MEMORY_BLOCK + 99, MEMORY_BLOCK / 2 + 1);
	auto &cache = BlockCache::Get(*db.instance);
	REQUIRE(cache.GetStatistics().spilled > 0);
	REQUIRE(ReadsBack(fs, "memory://spill/big.bin", 16 * MEMORY_BLOCK + 99));

	fs.RemoveFile("memory://spill/big.bin");
	REQUIRE(cache.GetStatistics().used <= cache.GetStatistics().capacity);
}

} // namespace duckdb
