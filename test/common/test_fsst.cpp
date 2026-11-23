#include "catch.hpp"
#include "duckdb/common/helper.hpp"
#include "fsst.h"
#include "fsst12.h"

#include <algorithm>
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

TEST_CASE("FSST decompresses the start of every string", "[fsst]") {
	const auto strings = FsstCorpus();
	FsstInputs in(strings);
	auto *encoder =
	    duckdb_fsst_create(strings.size(), in.lengths.data(), const_cast<unsigned char **>(in.pointers.data()), 0);
	REQUIRE(encoder);
	vector<unsigned char> out(1 << 20);
	vector<size_t> out_lengths(strings.size());
	vector<unsigned char *> out_strings(strings.size());
	REQUIRE(duckdb_fsst_compress(encoder, strings.size(), in.lengths.data(),
	                             const_cast<unsigned char **>(in.pointers.data()), out.size(), out.data(),
	                             out_lengths.data(), out_strings.data()) == strings.size());
	unsigned char table[FSST_MAXHEADER];
	const auto table_size = duckdb_fsst_export(encoder, table);
	duckdb_fsst_decoder_t decoder;
	REQUIRE(duckdb_fsst_import(&decoder, table, table_size) == table_size);
	vector<unsigned char> buffer(1024);
	for (size_t i = 0; i < strings.size(); i++) {
		const auto &expected = strings[i];
		for (size_t want : {size_t(0), size_t(1), size_t(3), size_t(8), size_t(9), size_t(33), expected.size(),
		                    expected.size() + 5}) {
			const auto written =
			    duckdb_fsst_decompress_prefix(&decoder, out_lengths[i], out_strings[i], want, buffer.data());
			REQUIRE(written <= want + 31);
			const auto have = MinValue(written, expected.size());
			REQUIRE(have >= MinValue(want, expected.size()));
			REQUIRE(string(reinterpret_cast<const char *>(buffer.data()), have) == expected.substr(0, have));
		}
	}
	const unsigned char escape_without_literal[] = {255};
	REQUIRE(duckdb_fsst_decompress_prefix(&decoder, 1, escape_without_literal, 8, buffer.data()) ==
	        DUCKDB_FSST_DECOMPRESS_INVALID_INPUT);
	duckdb_fsst_destroy(encoder);
}

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
			for (size_t want : {size_t(1), size_t(7), size_t(16), expected.size(), expected.size() + 3}) {
				const auto written =
				    duckdb_fsst12_decompress_prefix(decoder, out_lengths[i], out_strings[i], want, buffer.data());
				REQUIRE(written <= want + 15);
				const auto have = MinValue(written, expected.size());
				REQUIRE(have >= MinValue(want, expected.size()));
				REQUIRE(string(reinterpret_cast<const char *>(buffer.data()), have) == expected.substr(0, have));
			}
		}
	}
	const unsigned char one_byte[] = {1};
	REQUIRE(duckdb_fsst12_decompress(&imported, 1, one_byte, buffer.size(), buffer.data()) ==
	        DUCKDB_FSST12_DECOMPRESS_INVALID_INPUT);
	REQUIRE(duckdb_fsst12_decompress_prefix(&imported, 1, one_byte, 8, buffer.data()) ==
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

TEST_CASE("FSST12 decompresses into buffers shorter than the string", "[fsst]") {
	const auto strings = FsstCorpus();
	FsstInputs in(strings);
	auto *encoder = duckdb_fsst12_create(strings.size(), in.lengths.data(), in.pointers.data(), 0);
	REQUIRE(encoder);
	vector<unsigned char> out(1 << 20);
	vector<size_t> out_lengths(strings.size());
	vector<unsigned char *> out_strings(strings.size());
	REQUIRE(duckdb_fsst12_compress(encoder, strings.size(), in.lengths.data(), in.pointers.data(), out.size(),
	                               out.data(), out_lengths.data(), out_strings.data()) == strings.size());
	auto decoder = duckdb_fsst12_decoder(encoder);
	duckdb_fsst12_destroy(encoder);
	constexpr unsigned char CANARY = 0xAB;
	for (size_t i = 0; i < strings.size(); i++) {
		const auto &expected = strings[i];
		for (size_t size : {size_t(0), size_t(1), expected.size() / 2,
		                    expected.size() - MinValue<size_t>(expected.size(), 1), expected.size()}) {
			vector<unsigned char> buffer(size + 16, CANARY);
			const auto written =
			    duckdb_fsst12_decompress(&decoder, out_lengths[i], out_strings[i], size, buffer.data());
			REQUIRE(written == expected.size());
			const auto kept = MinValue(size, expected.size());
			REQUIRE(string(reinterpret_cast<const char *>(buffer.data()), kept) == expected.substr(0, kept));
			REQUIRE(std::all_of(buffer.begin() + kept, buffer.end(), [](unsigned char c) { return c == CANARY; }));
		}
	}
}

TEST_CASE("FSST12 refuses a symbol table that ends after its header", "[fsst]") {
	const auto strings = FsstCorpus();
	FsstInputs in(strings);
	auto *encoder = duckdb_fsst12_create(strings.size(), in.lengths.data(), in.pointers.data(), 0);
	REQUIRE(encoder);
	vector<unsigned char> table(FSST12_MAXHEADER);
	const auto table_size = duckdb_fsst12_export(encoder, table.data());
	duckdb_fsst12_destroy(encoder);
	REQUIRE(table_size > 24);
	duckdb_fsst12_decoder_t decoder;
	REQUIRE(duckdb_fsst12_import(&decoder, table.data(), 24) == DUCKDB_FSST12_IMPORT_OUT_OF_BOUNDS);
}

TEST_CASE("FSST refuses an escape that ends a block of four codes", "[fsst]") {
	const auto strings = FsstCorpus();
	FsstInputs in(strings);
	auto *encoder =
	    duckdb_fsst_create(strings.size(), in.lengths.data(), const_cast<unsigned char **>(in.pointers.data()), 0);
	REQUIRE(encoder);
	unsigned char table[FSST_MAXHEADER];
	const auto table_size = duckdb_fsst_export(encoder, table);
	duckdb_fsst_destroy(encoder);
	duckdb_fsst_decoder_t decoder;
	REQUIRE(duckdb_fsst_import(&decoder, table, table_size) == table_size);
	vector<unsigned char> buffer(64);
	const unsigned char escape_ending_a_block[] = {1, 2, 3, 255};
	REQUIRE(duckdb_fsst_decompress_prefix(&decoder, sizeof(escape_ending_a_block), escape_ending_a_block, 8,
	                                      buffer.data()) == DUCKDB_FSST_DECOMPRESS_INVALID_INPUT);
}
