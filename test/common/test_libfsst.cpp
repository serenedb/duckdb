#include "catch.hpp"
#include "libfsst.hpp"

#include <string>
#include <vector>

using std::string;
using std::vector;

namespace {

vector<string> KernelCorpus() {
	vector<string> strings;
	uint64_t state = 0xBB67AE8584CAA73BULL;
	const auto next = [&]() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	};
	for (uint64_t i = 0; i < 1500; i++) {
		string s = "https://example.org/catalog/item-" + std::to_string(i % 300) + "/";
		const auto tail = next() % 60;
		for (uint64_t k = 0; k < tail; k++) {
			s += static_cast<char>(next() % 256);
		}
		strings.push_back(std::move(s));
	}
	strings.emplace_back(511, 'z');
	strings.emplace_back();
	strings.emplace_back(1, 'h');
	return strings;
}

struct Batch {
	explicit Batch(vector<string> &strings) {
		for (auto &s : strings) {
			lengths.push_back(s.size());
			pointers.push_back(reinterpret_cast<libfsst::u8 *>(s.data()));
			total += s.size();
		}
	}
	vector<size_t> lengths;
	vector<libfsst::u8 *> pointers;
	size_t total = 0;
};

vector<string> CompressEachAlone(duckdb_fsst_encoder_t *encoder, vector<string> &strings) {
	vector<string> compressed;
	for (auto &s : strings) {
		size_t length = s.size();
		auto *input = reinterpret_cast<unsigned char *>(s.data());
		vector<unsigned char> out(2 * length + 7);
		size_t out_length = 0;
		unsigned char *out_string = nullptr;
		REQUIRE(duckdb_fsst_compress(encoder, 1, &length, &input, out.size(), out.data(), &out_length, &out_string) ==
		        1);
		compressed.emplace_back(reinterpret_cast<const char *>(out_string), out_length);
	}
	return compressed;
}

vector<string> CompressSimd(duckdb_fsst_encoder_t *encoder, vector<string> &strings, size_t size, int unroll) {
	auto &e = *reinterpret_cast<libfsst::Encoder *>(encoder);
	Batch in(strings);
	vector<libfsst::u8> out(size);
	vector<size_t> out_lengths(strings.size());
	vector<libfsst::u8 *> out_strings(strings.size());
	const auto count =
	    libfsst::compressSIMD(*e.symbolTable, e.simdbuf, strings.size(), in.lengths.data(), in.pointers.data(), size,
	                          out.data(), out_lengths.data(), out_strings.data(), unroll);
	REQUIRE(count <= strings.size());
	vector<string> compressed;
	auto *next = out.data();
	for (size_t i = 0; i < count; i++) {
		REQUIRE(out_strings[i] == next);
		next += out_lengths[i];
		compressed.emplace_back(reinterpret_cast<const char *>(out_strings[i]), out_lengths[i]);
	}
	REQUIRE(next <= out.data() + out.size());
	return compressed;
}

size_t StringsThatFit(const vector<string> &strings, const vector<string> &compressed, size_t size) {
	size_t used = 0;
	for (size_t i = 0; i < strings.size(); i++) {
		if (2 * strings[i].size() + 7 > size - used) {
			return i;
		}
		used += compressed[i].size();
	}
	return strings.size();
}

size_t RoomBefore(const vector<string> &strings, const vector<string> &compressed, size_t stop) {
	size_t size = 2 * strings[stop].size() + 6;
	for (size_t i = 0; i < stop; i++) {
		size += compressed[i].size();
	}
	return size;
}

vector<string> Prefix(const vector<string> &strings, size_t count) {
	return vector<string>(strings.begin(), strings.begin() + static_cast<std::ptrdiff_t>(count));
}

} // namespace

TEST_CASE("FSST encodes SIMD batches as each string alone, at any kernel unroll", "[fsst]") {
	auto strings = KernelCorpus();
	Batch in(strings);
	auto *encoder = duckdb_fsst_create(strings.size(), in.lengths.data(), in.pointers.data(), 0);
	REQUIRE(encoder);
	const auto expected = CompressEachAlone(encoder, strings);

	vector<string> sparse(600);
	for (size_t i = 0; i < 20; i++) {
		sparse[i * 29 + 4] = strings[i * 71];
	}
	sparse[511] = strings[1500];
	REQUIRE(sparse[511].size() == 511);
	const auto sparse_expected = CompressEachAlone(encoder, sparse);
	const auto sparse_size = 2 * Batch(sparse).total + 7;

	auto few = Prefix(strings, 25);
	const auto few_expected = Prefix(expected, 25);
	const auto few_stop = StringsThatFit(few, few_expected, RoomBefore(few, few_expected, 10));
	REQUIRE(few_stop > 0);
	const auto few_room = RoomBefore(few, few_expected, few_stop);
	REQUIRE(StringsThatFit(few, few_expected, few_room) == few_stop);

	const auto stop = StringsThatFit(strings, expected, RoomBefore(strings, expected, 700));
	REQUIRE(stop > 512);
	const auto room = RoomBefore(strings, expected, stop);
	REQUIRE(StringsThatFit(strings, expected, room) == stop);

	for (int unroll : {1, 2, 3}) {
		CHECK(CompressSimd(encoder, sparse, sparse_size, unroll) == sparse_expected);
		CHECK(CompressSimd(encoder, few, few_room, unroll) == Prefix(few_expected, few_stop));
		CHECK(CompressSimd(encoder, few, few_room + 1, unroll) ==
		      Prefix(few_expected, StringsThatFit(few, few_expected, few_room + 1)));
		if (!libfsst::fsst_hasAVX512()) {
			continue;
		}
		CHECK(CompressSimd(encoder, strings, 2 * in.total + 7, unroll) == expected);
		CHECK(CompressSimd(encoder, strings, room, unroll) == Prefix(expected, stop));
	}
	duckdb_fsst_destroy(encoder);
}
