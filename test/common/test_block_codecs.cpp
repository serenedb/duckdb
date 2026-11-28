#include "catch.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/formats/block_codecs.hpp"

#include <span>

using namespace duckdb;

namespace {

const BlockCodec ALL_CODECS[] = {BlockCodec::DEFLATE, BlockCodec::GZIP,      BlockCodec::SNAPPY, BlockCodec::ZSTD,
                                 BlockCodec::LZ4,     BlockCodec::LZ4_FRAME, BlockCodec::BROTLI};

string BlockInput() {
	string input;
	for (idx_t i = 0; i < 50000; i++) {
		input += to_string(i * 7919 % 100003) + (i % 5 == 0 ? "\n" : ",");
	}
	return input;
}

string Compress(BlockCompressor &compressor, std::span<const std::span<const data_t>> inputs, bool finish) {
	idx_t total = 0;
	for (auto &input : inputs) {
		total += input.size();
	}
	string output(compressor.MaxCompressedSize(total), '\0');
	output.resize(compressor.Compress(inputs, finish, data_ptr_cast(&output[0]), output.size()));
	return output;
}

string Compress(BlockCodec codec, const string &input) {
	BlockCompressor compressor(codec);
	const std::span<const data_t> whole(const_data_ptr_cast(input.data()), input.size());
	return Compress(compressor, std::span<const std::span<const data_t>>(&whole, 1), true);
}

bool Decompress(BlockDecompressor &decompressor, BlockCodec codec, const string &compressed, idx_t compressed_size,
                string &output, string &error) {
	return decompressor.TryDecompress(codec, const_data_ptr_cast(compressed.data()), compressed_size,
	                                  data_ptr_cast(&output[0]), output.size(), error);
}

bool DecompressGrowing(BlockDecompressor &decompressor, BlockCodec codec, const string &compressed, AllocatedData &out,
                       string &output, string &error) {
	idx_t size = 0;
	if (!decompressor.TryDecompress(codec, const_data_ptr_cast(compressed.data()), compressed.size(),
	                                Allocator::DefaultAllocator(), 0, out, size, error)) {
		return false;
	}
	output.assign(const_char_ptr_cast(out.get()), size);
	return true;
}

string HadoopLz4Frame(idx_t decompressed, const string &input) {
	const auto block = Compress(BlockCodec::LZ4, input);
	const uint32_t sizes[] = {BSwapIfLE(NumericCast<uint32_t>(decompressed)),
	                          BSwapIfLE(NumericCast<uint32_t>(block.size()))};
	return string(const_char_ptr_cast(sizes), sizeof(sizes)) + block;
}

} // namespace

TEST_CASE("Block codecs decompress what they compress into its exact size and nothing else", "[formats]") {
	const auto input = BlockInput();
	for (auto codec : ALL_CODECS) {
		CAPTURE(static_cast<int>(codec));
		const auto compressed = Compress(codec, input);
		REQUIRE(compressed.size() < input.size());

		BlockDecompressor decompressor;
		string error;
		string output(input.size(), '\0');
		REQUIRE(Decompress(decompressor, codec, compressed, compressed.size(), output, error));
		REQUIRE(output == input);

		string shorter(input.size() - 1, '\0');
		REQUIRE_FALSE(Decompress(decompressor, codec, compressed, compressed.size(), shorter, error));
		REQUIRE_FALSE(error.empty());

		error.clear();
		string longer(input.size() + 1, '\0');
		REQUIRE_FALSE(Decompress(decompressor, codec, compressed, compressed.size(), longer, error));
		REQUIRE_FALSE(error.empty());

		error.clear();
		REQUIRE_FALSE(Decompress(decompressor, codec, compressed, compressed.size() / 2, output, error));
		REQUIRE_FALSE(error.empty());

		REQUIRE(Decompress(decompressor, codec, compressed, compressed.size(), output, error));
		REQUIRE(output == input);
	}
}

TEST_CASE("Block codecs decompress into a buffer that grows and ends with zeroed padding", "[formats]") {
	const auto input = BlockInput();
	for (auto codec : ALL_CODECS) {
		CAPTURE(static_cast<int>(codec));
		const auto compressed = Compress(codec, input);
		BlockDecompressor decompressor;
		AllocatedData out;
		idx_t size = 0;
		string error;
		if (codec == BlockCodec::LZ4) {
			REQUIRE_THROWS_AS(decompressor.TryDecompress(codec, const_data_ptr_cast(compressed.data()),
			                                             compressed.size(), Allocator::DefaultAllocator(), 8, out, size,
			                                             error),
			                  InternalException);
			continue;
		}
		for (idx_t padding : {idx_t(0), idx_t(8)}) {
			REQUIRE(decompressor.TryDecompress(codec, const_data_ptr_cast(compressed.data()), compressed.size(),
			                                   Allocator::DefaultAllocator(), padding, out, size, error));
			REQUIRE(string(const_char_ptr_cast(out.get()), size) == input);
			REQUIRE(out.GetSize() >= size + padding);
			for (idx_t i = 0; i < padding; i++) {
				REQUIRE(out.get()[size + i] == 0);
			}
		}
		REQUIRE_FALSE(decompressor.TryDecompress(codec, const_data_ptr_cast(compressed.data()), compressed.size() / 2,
		                                         Allocator::DefaultAllocator(), 0, out, size, error));
		REQUIRE_FALSE(error.empty());
	}
}

TEST_CASE("Block codecs compress inputs in pieces, and a deflate block can stay open", "[formats]") {
	const auto input = BlockInput();
	const std::span<const data_t> halves[] = {{const_data_ptr_cast(input.data()), 1000},
	                                          {const_data_ptr_cast(input.data()) + 1000, input.size() - 1000}};
	for (auto codec : {BlockCodec::DEFLATE, BlockCodec::GZIP, BlockCodec::ZSTD}) {
		CAPTURE(static_cast<int>(codec));
		BlockCompressor compressor(codec);
		const auto compressed = Compress(compressor, halves, true);
		BlockDecompressor decompressor;
		string error;
		string output(input.size(), '\0');
		REQUIRE(Decompress(decompressor, codec, compressed, compressed.size(), output, error));
		REQUIRE(output == input);
		const auto empty = Compress(compressor, {}, true);
		string nothing;
		REQUIRE(Decompress(decompressor, codec, empty, empty.size(), nothing, error));
	}

	BlockCompressor deflate(BlockCodec::DEFLATE);
	const auto open = Compress(deflate, halves, false);
	BlockDecompressor decompressor;
	string error;
	string output(input.size(), '\0');
	REQUIRE_FALSE(Decompress(decompressor, BlockCodec::DEFLATE, open, open.size(), output, error));
	REQUIRE(error == "the compressed data ends early");

	BlockCompressor snappy(BlockCodec::SNAPPY);
	string unused(snappy.MaxCompressedSize(input.size()), '\0');
	REQUIRE_THROWS_AS(snappy.Compress(halves, true, data_ptr_cast(&unused[0]), unused.size()), InternalException);
	REQUIRE_THROWS_AS(snappy.Compress(halves, false, data_ptr_cast(&unused[0]), unused.size()), InternalException);
}

TEST_CASE("Block codecs report input that is not their format", "[formats]") {
	string garbage(64, '\xFF');
	garbage[0] = '\x11';
	for (auto codec : ALL_CODECS) {
		CAPTURE(static_cast<int>(codec));
		BlockDecompressor decompressor;
		string error;
		string output(1000, '\0');
		REQUIRE_FALSE(Decompress(decompressor, codec, garbage, garbage.size(), output, error));
		REQUIRE_FALSE(error.empty());
		if (codec == BlockCodec::LZ4) {
			continue;
		}
		error.clear();
		AllocatedData out;
		REQUIRE_FALSE(DecompressGrowing(decompressor, codec, garbage, out, output, error));
		REQUIRE_FALSE(error.empty());
	}
}

TEST_CASE("Block codecs read concatenated frames and gzip members", "[formats]") {
	const auto input = BlockInput();
	const auto half = input.size() / 2;
	for (auto codec : {BlockCodec::GZIP, BlockCodec::ZSTD, BlockCodec::LZ4_FRAME}) {
		CAPTURE(static_cast<int>(codec));
		const auto compressed = Compress(codec, input.substr(0, half)) + Compress(codec, input.substr(half));
		BlockDecompressor decompressor;
		string error;
		string output(input.size(), '\0');
		REQUIRE(Decompress(decompressor, codec, compressed, compressed.size(), output, error));
		REQUIRE(output == input);
		AllocatedData out;
		for (idx_t round = 0; round < 2; round++) {
			output.clear();
			REQUIRE(DecompressGrowing(decompressor, codec, compressed, out, output, error));
			REQUIRE(output == input);
		}
	}
}

TEST_CASE("Hadoop LZ4 reads its frames, and one raw LZ4 block when the framing does not fit", "[formats]") {
	const auto input = BlockInput();
	const auto first = input.substr(0, input.size() / 2);
	const auto second = input.substr(first.size());
	const auto framed = HadoopLz4Frame(first.size(), first) + HadoopLz4Frame(second.size(), second);
	const auto raw = Compress(BlockCodec::LZ4, input);
	BlockDecompressor decompressor;
	string error;
	for (auto &compressed : {framed, raw}) {
		string output(input.size(), '\0');
		REQUIRE(Decompress(decompressor, BlockCodec::LZ4_HADOOP, compressed, compressed.size(), output, error));
		REQUIRE(output == input);

		string shorter(input.size() - 1, '\0');
		REQUIRE_FALSE(Decompress(decompressor, BlockCodec::LZ4_HADOOP, compressed, compressed.size(), shorter, error));
		REQUIRE_FALSE(error.empty());

		error.clear();
		REQUIRE_FALSE(
		    Decompress(decompressor, BlockCodec::LZ4_HADOOP, compressed, compressed.size() - 1, output, error));
		REQUIRE_FALSE(error.empty());
		error.clear();
	}

	const auto understated = HadoopLz4Frame(first.size() - 1, first) + HadoopLz4Frame(second.size(), second);
	string output(input.size(), '\0');
	REQUIRE_FALSE(Decompress(decompressor, BlockCodec::LZ4_HADOOP, understated, understated.size(), output, error));
	REQUIRE_FALSE(error.empty());

	string nothing;
	REQUIRE(Decompress(decompressor, BlockCodec::LZ4_HADOOP, string(), 0, nothing, error));

	AllocatedData out;
	REQUIRE_THROWS_AS(DecompressGrowing(decompressor, BlockCodec::LZ4_HADOOP, framed, out, output, error),
	                  InternalException);
	BlockCompressor compressor(BlockCodec::LZ4_HADOOP);
	REQUIRE_THROWS_AS(compressor.MaxCompressedSize(input.size()), InternalException);
}

TEST_CASE("Block codecs grow the output for data that compresses many times over", "[formats]") {
	string input(8 * 1024 * 1024, 'a');
	for (idx_t i = 0; i < input.size(); i += 4096) {
		input[i] = char('b' + i / 4096 % 20);
	}
	const string small(1000, 'c');
	for (auto codec : ALL_CODECS) {
		if (codec == BlockCodec::LZ4) {
			continue;
		}
		CAPTURE(static_cast<int>(codec));
		const auto compressed = Compress(codec, input);
		REQUIRE(compressed.size() * 16 < input.size());
		BlockDecompressor decompressor;
		AllocatedData out;
		string output;
		string error;
		REQUIRE(DecompressGrowing(decompressor, codec, compressed, out, output, error));
		REQUIRE(output == input);
		REQUIRE(DecompressGrowing(decompressor, codec, Compress(codec, small), out, output, error));
		REQUIRE(output == small);
		REQUIRE(out.GetSize() >= input.size());
	}
}
