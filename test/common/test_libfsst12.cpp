#include "catch.hpp"
#include "libfsst12.hpp"

#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> Fsst12Corpus() {
	std::vector<std::string> strings;
	uint64_t state = 0x3C6EF372FE94F82BULL;
	for (uint64_t i = 0; i < 2000; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		strings.push_back("https://example.org/catalog/item-" + std::to_string(i % 300) + "/" +
		                  std::to_string(state % 100000) + "?page=" + std::to_string(i % 7));
	}
	strings.emplace_back();
	strings.emplace_back(1, 'x');
	strings.emplace_back(300, 'y');
	return strings;
}

struct Fsst12Batch {
	explicit Fsst12Batch(const std::vector<std::string> &strings) {
		for (auto &s : strings) {
			lengths.push_back(s.size());
			pointers.push_back(reinterpret_cast<const unsigned char *>(s.data()));
		}
	}
	std::vector<unsigned long> lengths;
	std::vector<const unsigned char *> pointers;
};

struct Fsst12Output {
	explicit Fsst12Output(size_t count, size_t size) : buffer(size), lengths(count), strings(count) {
	}
	std::vector<std::string> Compressed(unsigned long count) const {
		std::vector<std::string> compressed;
		for (unsigned long i = 0; i < count; i++) {
			compressed.emplace_back(reinterpret_cast<const char *>(strings[i]), lengths[i]);
		}
		return compressed;
	}
	std::vector<unsigned char> buffer;
	std::vector<unsigned long> lengths;
	std::vector<unsigned char *> strings;
};

} // namespace

TEST_CASE("FSST12 symbols compare their bytes and lengths, and print their bytes", "[fsst]") {
	const char text[] = "abcdefghij";
	const libfsst12::Symbol abc(text, text + 3);
	CHECK(abc.length() == 3);
	CHECK(abc.code() == 4095);
	CHECK(abc.garbageBits() == 40);
	CHECK(abc == libfsst12::Symbol("abc", 3u));
	CHECK_FALSE(abc == libfsst12::Symbol("abx", 3u));
	const char ab_and_zero[] = {'a', 'b', '\0'};
	CHECK_FALSE(libfsst12::Symbol("ab", 2u) == libfsst12::Symbol(ab_and_zero, 3u));
	const libfsst12::Symbol eight(text, text + 10);
	CHECK(eight.length() == 8);
	CHECK(eight.garbageBits() == 0);
	std::ostringstream printed;
	printed << abc << '|' << eight;
	CHECK(printed.str() == "abc|abcdefgh");
}

TEST_CASE("FSST12 counts the hash table slots its symbols of three bytes or more take", "[fsst]") {
	auto map = std::make_unique<libfsst12::SymbolMap>();
	CHECK(map->load() == 0);
	REQUIRE(map->add(libfsst12::Symbol("ab", 2u)));
	CHECK(map->load() == 0);
	for (const char *s : {"abc", "https", "example", "catalog/"}) {
		REQUIRE(map->add(libfsst12::Symbol(s, static_cast<uint32_t>(strlen(s)))));
	}
	CHECK(map->load() == 4);
	CHECK(map->symbolCount == 256 + 5);
	map->clear();
	CHECK(map->load() == 0);
	CHECK(map->symbolCount == 256);
}

TEST_CASE("FSST12 counters back up and restore their counts of single symbols", "[fsst]") {
	auto counters = std::make_unique<libfsst12::Counters>();
	for (int i = 0; i < 300; i++) {
		counters->count1Inc(300);
	}
	counters->count1Inc(4095);
	uint32_t pos = 300;
	CHECK(counters->count1GetNext(pos) == 300);
	CHECK(pos == 300);
	std::vector<uint8_t> backup(2 * FSST_CODE_MAX);
	counters->backup1(backup.data());
	CHECK(backup[300] == 2);
	CHECK(backup[FSST_CODE_MAX + 300] == 44);
	counters->count1Set(300, 0);
	pos = 300;
	CHECK(counters->count1GetNext(pos) == 0);
	counters->restore1(backup.data());
	pos = 300;
	CHECK(counters->count1GetNext(pos) == 300);
	pos = 4095;
	CHECK(counters->count1GetNext(pos) == 1);
}

TEST_CASE("FSST12 builds the table of the 256 single bytes from an empty batch", "[fsst]") {
	const unsigned long no_length = 0;
	const auto *no_string = reinterpret_cast<const unsigned char *>("");
	auto *encoder = duckdb_fsst12_create(0, &no_length, &no_string, 0);
	REQUIRE(encoder);
	std::vector<unsigned char> table(FSST12_MAXHEADER);
	REQUIRE(duckdb_fsst12_export(encoder, table.data()) == 24u + 2u * 256u);
	uint16_t histogram[8];
	memcpy(histogram, table.data() + 8, sizeof(histogram));
	CHECK(histogram[0] == 256);
	for (size_t i = 1; i < 8; i++) {
		CHECK(histogram[i] == 0);
	}
	for (size_t code = 0; code < 256; code++) {
		CHECK(table[24 + 2 * code] == 1);
		CHECK(table[25 + 2 * code] == code);
	}
	const std::string abc = "abc";
	unsigned long length = abc.size();
	const auto *input = reinterpret_cast<const unsigned char *>(abc.data());
	Fsst12Output out(1, 16);
	REQUIRE(duckdb_fsst12_compress(encoder, 1, &length, &input, out.buffer.size(), out.buffer.data(),
	                               out.lengths.data(), out.strings.data()) == 1);
	CHECK(out.Compressed(1)[0] == std::string({'\x61', '\x20', '\x06', '\x63', '\x00'}));
	const auto decoder = duckdb_fsst12_decoder(encoder);
	duckdb_fsst12_destroy(encoder);
	std::vector<unsigned char> decompressed(16);
	REQUIRE(duckdb_fsst12_decompress(&decoder, out.lengths[0], out.strings[0], decompressed.size(),
	                                 decompressed.data()) == 3);
	CHECK(std::string(reinterpret_cast<const char *>(decompressed.data()), 3) == abc);
}

TEST_CASE("FSST12's C++ entry points encode as its C API, whatever their flags", "[fsst]") {
	const auto strings = Fsst12Corpus();
	Fsst12Batch in(strings);
	auto *encoder = duckdb_fsst12_create(strings.size(), in.lengths.data(), in.pointers.data(), 0);
	REQUIRE(encoder);
	auto &e = *reinterpret_cast<libfsst12::Encoder *>(encoder);
	for (size_t size : {size_t(1) << 20, size_t(5000)}) {
		Fsst12Output reference(strings.size(), size);
		const auto count =
		    duckdb_fsst12_compress(encoder, strings.size(), in.lengths.data(), in.pointers.data(), size,
		                           reference.buffer.data(), reference.lengths.data(), reference.strings.data());
		CHECK((size == 5000 ? count < strings.size() : count == strings.size()));
		const auto expected = reference.Compressed(count);
		for (int simd : {0, 3}) {
			for (bool no_suffix_opt : {false, true}) {
				for (bool avoid_branch : {false, true}) {
					Fsst12Output out(strings.size(), size);
					REQUIRE(libfsst12::compressImpl(&e, strings.size(), in.lengths.data(), in.pointers.data(), size,
					                                out.buffer.data(), out.lengths.data(), out.strings.data(),
					                                no_suffix_opt, avoid_branch, simd) == count);
					CHECK(out.Compressed(count) == expected);
				}
			}
			Fsst12Output out(strings.size(), size);
			REQUIRE(libfsst12::compressAuto(&e, strings.size(), in.lengths.data(), in.pointers.data(), size,
			                                out.buffer.data(), out.lengths.data(), out.strings.data(), simd) == count);
			CHECK(out.Compressed(count) == expected);
		}
	}
	duckdb_fsst12_destroy(encoder);
}
