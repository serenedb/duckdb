#include "catch.hpp"
#include "duckdb/common/helper.hpp"
#include "fsst12.h"

#include <cstring>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

vector<string> FsstCorpus() {
	vector<string> strings;
	uint64_t state = 0x9E3779B97F4A7C15ULL;
	const auto next = [&]() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	};
	for (uint64_t i = 0; i < 2000; i++) {
		string s = "https://example.org/catalog/item-" + std::to_string(i % 300) + "/";
		const auto tail = next() % 40;
		for (uint64_t k = 0; k < tail; k++) {
			s += static_cast<char>(next() % 256);
		}
		strings.push_back(std::move(s));
	}
	strings.emplace_back();
	strings.emplace_back(1, '\xff');
	strings.emplace_back(200, 'a');
	return strings;
}

struct FsstInputs {
	explicit FsstInputs(const vector<string> &strings) {
		for (auto &s : strings) {
			lengths.push_back(s.size());
			pointers.push_back(reinterpret_cast<const unsigned char *>(s.data()));
		}
	}
	vector<size_t> lengths;
	vector<const unsigned char *> pointers;
};

} // namespace

TEST_CASE("FSST12 round trips through copies of its encoder and decoder", "[fsst]") {
	const auto strings = FsstCorpus();
	FsstInputs in(strings);
	auto *encoder = duckdb_fsst12_create(strings.size(), in.lengths.data(), in.pointers.data(), 0);
	REQUIRE(encoder);
	auto *copy = duckdb_fsst12_duplicate(encoder);
	REQUIRE(copy);
	vector<unsigned char> out(1 << 20);
	vector<size_t> out_lengths(strings.size());
	vector<unsigned char *> out_strings(strings.size());
	REQUIRE(duckdb_fsst12_compress(copy, strings.size(), in.lengths.data(), in.pointers.data(), out.size(), out.data(),
	                               out_lengths.data(), out_strings.data()) == strings.size());
	vector<unsigned char> table(FSST12_MAXHEADER);
	const auto table_size = duckdb_fsst12_export(encoder, table.data());
	duckdb_fsst12_decoder_t imported;
	REQUIRE(duckdb_fsst12_import(&imported, table.data(), table_size) == table_size);
	auto from_encoder = duckdb_fsst12_decoder(encoder);
	vector<unsigned char> buffer(1024);
	for (auto *decoder : {&imported, &from_encoder}) {
		for (size_t i = 0; i < strings.size(); i++) {
			const auto &expected = strings[i];
			const auto size =
			    duckdb_fsst12_decompress(decoder, out_lengths[i], out_strings[i], buffer.size(), buffer.data());
			REQUIRE(string(reinterpret_cast<const char *>(buffer.data()), size) == expected);
		}
	}
	const unsigned char one_byte[] = {1};
	REQUIRE(duckdb_fsst12_decompress(&imported, 1, one_byte, buffer.size(), buffer.data()) ==
	        DUCKDB_FSST12_DECOMPRESS_INVALID_INPUT);
	duckdb_fsst12_destroy(copy);
	duckdb_fsst12_destroy(encoder);
}

TEST_CASE("FSST12 refuses damaged symbol tables", "[fsst]") {
	const auto strings = FsstCorpus();
	FsstInputs in(strings);
	auto *encoder = duckdb_fsst12_create(strings.size(), in.lengths.data(), in.pointers.data(), 0);
	REQUIRE(encoder);
	vector<unsigned char> table(FSST12_MAXHEADER);
	const auto table_size = duckdb_fsst12_export(encoder, table.data());
	duckdb_fsst12_destroy(encoder);
	REQUIRE(table_size > 25);
	duckdb_fsst12_decoder_t decoder;
	REQUIRE(duckdb_fsst12_import(&decoder, table.data(), 23) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
	REQUIRE(duckdb_fsst12_import(&decoder, table.data(), 25) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
	REQUIRE(duckdb_fsst12_import(&decoder, table.data(), table_size - 1) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
	auto damaged = table;
	damaged[4] ^= 0x01;
	REQUIRE(duckdb_fsst12_import(&decoder, damaged.data(), table_size) == DUCKDB_FSST12_IMPORT_VERSION_MISMATCH);
	damaged = table;
	const uint16_t too_many = 4097;
	memcpy(damaged.data() + 8, &too_many, sizeof(too_many));
	REQUIRE(duckdb_fsst12_import(&decoder, damaged.data(), table_size) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
	damaged = table;
	damaged[24] = 0;
	REQUIRE(duckdb_fsst12_import(&decoder, damaged.data(), table_size) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
	damaged[24] = 9;
	REQUIRE(duckdb_fsst12_import(&decoder, damaged.data(), table_size) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
}
