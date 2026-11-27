#include "duckdb/formats/block_codecs.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

#include "brotli/decode.h"
#include "brotli/encode.h"
#include "lz4.h"
#include "lz4frame.h"
#include "snappy.h"
#include "zstd.h"

#define ZLIB_CONST
#include <zlib.h>

namespace duckdb {

namespace {

constexpr idx_t MIN_GROWN_BYTES = 64 * 1024;
constexpr idx_t ZSTD_MAX_EXPANSION = 128 * 1024 / 4;
constexpr idx_t SNAPPY_COPY_BYTES = 3;
constexpr idx_t SNAPPY_MAX_COPY = 64;
constexpr idx_t DEFLATE_FLUSH_BYTES = 16;
constexpr int GZIP_WINDOW_BITS = MAX_WBITS + 16;
constexpr int DEFLATE_MEMORY_LEVEL = 8;

uInt StreamStep(idx_t size) {
	return UnsafeNumericCast<uInt>(MinValue<idx_t>(size, NumericLimits<uInt>::Maximum()));
}

int ZlibWindowBits(BlockCodec codec) {
	return codec == BlockCodec::GZIP ? GZIP_WINDOW_BITS : -MAX_WBITS;
}

LZ4F_preferences_t Lz4FramePreferences(int level) {
	LZ4F_preferences_t preferences;
	memset(&preferences, 0, sizeof(preferences));
	preferences.compressionLevel = level == BlockCompressor::DEFAULT_LEVEL ? 0 : level;
	return preferences;
}

struct BrotliDecoderDeleter {
	void operator()(BrotliDecoderState *state) const {
		duckdb_brotli::BrotliDecoderDestroyInstance(state);
	}
};

} // namespace

class BlockDecompressor::Output {
public:
	Output(data_ptr_t data_p, idx_t capacity_p) : data(data_p), capacity(capacity_p) {
	}
	Output(Allocator &allocator_p, AllocatedData &owned_p, idx_t padding_p)
	    : allocator(&allocator_p), owned(&owned_p), padding(padding_p) {
	}

	bool Fixed() const {
		return !allocator;
	}
	data_ptr_t Position() const {
		return data + used;
	}
	idx_t Room() const {
		return capacity - used;
	}
	idx_t Used() const {
		return used;
	}
	void Advance(idx_t count) {
		used += count;
	}
	void Expect(idx_t size) {
		if (allocator && Room() < size) {
			Reserve(size);
		}
	}
	bool Grow() {
		if (!allocator) {
			return false;
		}
		Reserve(MaxValue(capacity, MIN_GROWN_BYTES));
		return true;
	}
	void Finish() {
		if (allocator) {
			Reserve(0);
			memset(data + used, 0, padding);
		}
	}

private:
	void Reserve(idx_t more) {
		const auto want = used + more + padding;
		if (owned->GetSize() < want) {
			auto grown = allocator->Allocate(want);
			if (used) {
				memcpy(grown.get(), data, used);
			}
			*owned = std::move(grown);
		}
		data = owned->get();
		capacity = owned->GetSize() - padding;
	}

private:
	Allocator *allocator = nullptr;
	AllocatedData *owned = nullptr;
	idx_t padding = 0;
	data_ptr_t data = nullptr;
	idx_t capacity = 0;
	idx_t used = 0;
};

BlockDecompressor::BlockDecompressor() {
}

BlockDecompressor::~BlockDecompressor() {
	if (zlib) {
		inflateEnd(zlib.get());
	}
	if (zstd) {
		duckdb_zstd::ZSTD_freeDCtx(zstd);
	}
	if (lz4f) {
		duckdb_lz4::LZ4F_freeDecompressionContext(lz4f);
	}
}

bool BlockDecompressor::TryDecompress(BlockCodec codec, const_data_ptr_t src, idx_t src_size, data_ptr_t dst,
                                      idx_t dst_size, string &error) {
	Output out(dst, dst_size);
	if (!Decompress(codec, src, src_size, out, error)) {
		return false;
	}
	if (out.Used() != dst_size) {
		error = StringUtil::Format("the data decompresses to %llu bytes instead of %llu", out.Used(), dst_size);
		return false;
	}
	return true;
}

bool BlockDecompressor::TryDecompress(BlockCodec codec, const_data_ptr_t src, idx_t src_size, Allocator &allocator,
                                      idx_t padding, AllocatedData &out_data, idx_t &size, string &error) {
	Output out(allocator, out_data, padding);
	if (!Decompress(codec, src, src_size, out, error)) {
		return false;
	}
	out.Finish();
	size = out.Used();
	return true;
}

bool BlockDecompressor::Decompress(BlockCodec codec, const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	switch (codec) {
	case BlockCodec::DEFLATE:
	case BlockCodec::GZIP:
		return Inflate(ZlibWindowBits(codec), src, src_size, out, error);
	case BlockCodec::SNAPPY:
		return Snappy(src, src_size, out, error);
	case BlockCodec::ZSTD:
		return Zstd(src, src_size, out, error);
	case BlockCodec::LZ4:
		return Lz4(src, src_size, out, error);
	case BlockCodec::LZ4_FRAME:
		return Lz4Frame(src, src_size, out, error);
	case BlockCodec::BROTLI:
		return Brotli(src, src_size, out, error);
	}
	throw InternalException("Unknown block codec");
}

bool BlockDecompressor::Inflate(int window_bits, const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	if (!zlib) {
		zlib = make_uniq<z_stream>();
		memset(zlib.get(), 0, sizeof(z_stream));
		if (inflateInit2(zlib.get(), window_bits) != Z_OK) {
			zlib.reset();
			throw InternalException("Failed to initialize the zlib decoder");
		}
	} else if (inflateReset2(zlib.get(), window_bits) != Z_OK) {
		throw InternalException("Failed to reset the zlib decoder");
	}
	auto &stream = *zlib;
	const auto members = window_bits == GZIP_WINDOW_BITS;
	out.Expect(MaxValue(src_size * 4, MIN_GROWN_BYTES));
	while (true) {
		const auto in_step = StreamStep(src_size);
		const auto out_step = StreamStep(out.Room());
		stream.next_in = src;
		stream.avail_in = in_step;
		stream.next_out = out.Position();
		stream.avail_out = out_step;
		const auto ret = inflate(&stream, Z_NO_FLUSH);
		src += in_step - stream.avail_in;
		src_size -= in_step - stream.avail_in;
		out.Advance(out_step - stream.avail_out);
		if (ret == Z_STREAM_END) {
			if (!members || src_size == 0) {
				return true;
			}
			if (inflateReset(&stream) != Z_OK) {
				throw InternalException("Failed to reset the zlib decoder");
			}
			continue;
		}
		if (ret == Z_BUF_ERROR) {
			if (src_size == 0) {
				error = "the compressed data ends early";
				return false;
			}
			if (!out.Grow()) {
				error = StringUtil::Format("the data decompresses to more than %llu bytes", out.Used());
				return false;
			}
			continue;
		}
		if (ret != Z_OK) {
			error = stream.msg ? stream.msg : zError(ret);
			return false;
		}
	}
}

bool BlockDecompressor::Snappy(const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	size_t length;
	if (!duckdb_snappy::GetUncompressedLength(const_char_ptr_cast(src), src_size, &length) ||
	    length > (src_size / SNAPPY_COPY_BYTES + 1) * SNAPPY_MAX_COPY) {
		error = "invalid uncompressed length";
		return false;
	}
	out.Expect(length);
	if (out.Room() < length) {
		error = StringUtil::Format("the data decompresses to %llu bytes instead of %llu", length, out.Room());
		return false;
	}
	if (!duckdb_snappy::RawUncompress(const_char_ptr_cast(src), src_size, char_ptr_cast(out.Position()))) {
		error = "invalid compressed data";
		return false;
	}
	out.Advance(length);
	return true;
}

bool BlockDecompressor::Zstd(const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	if (!zstd) {
		zstd = duckdb_zstd::ZSTD_createDCtx();
		if (!zstd) {
			throw OutOfMemoryException("Failed to allocate the zstd decoder");
		}
	}
	if (out.Fixed()) {
		const auto ret = duckdb_zstd::ZSTD_decompressDCtx(zstd, out.Position(), out.Room(), src, src_size);
		if (duckdb_zstd::ZSTD_isError(ret)) {
			error = duckdb_zstd::ZSTD_getErrorName(ret);
			return false;
		}
		out.Advance(ret);
		return true;
	}
	duckdb_zstd::ZSTD_DCtx_reset(zstd, duckdb_zstd::ZSTD_reset_session_only);
	const auto known = duckdb_zstd::ZSTD_getFrameContentSize(src, src_size);
	out.Expect(known / ZSTD_MAX_EXPANSION <= src_size ? MaxValue<idx_t>(known, 1)
	                                                  : MaxValue(src_size * 4, MIN_GROWN_BYTES));
	duckdb_zstd::ZSTD_inBuffer input {src, src_size, 0};
	while (true) {
		duckdb_zstd::ZSTD_outBuffer output {out.Position(), out.Room(), 0};
		const auto ret = duckdb_zstd::ZSTD_decompressStream(zstd, &output, &input);
		if (duckdb_zstd::ZSTD_isError(ret)) {
			error = duckdb_zstd::ZSTD_getErrorName(ret);
			return false;
		}
		out.Advance(output.pos);
		if (input.pos == input.size) {
			if (ret == 0) {
				return true;
			}
			if (output.pos < output.size) {
				error = "the compressed data ends early";
				return false;
			}
		}
		if (out.Room() == 0) {
			out.Grow();
		}
	}
}

bool BlockDecompressor::Lz4(const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	if (!out.Fixed()) {
		throw InternalException("An LZ4 block decompresses only into a buffer of its known size");
	}
	if (src_size > NumericLimits<int32_t>::Maximum() || out.Room() > NumericLimits<int32_t>::Maximum()) {
		error = "the block is larger than LZ4 supports";
		return false;
	}
	const auto ret = duckdb_lz4::LZ4_decompress_safe(const_char_ptr_cast(src), char_ptr_cast(out.Position()),
	                                                 NumericCast<int>(src_size), NumericCast<int>(out.Room()));
	if (ret < 0) {
		error = "invalid compressed data";
		return false;
	}
	out.Advance(NumericCast<idx_t>(ret));
	return true;
}

bool BlockDecompressor::Lz4Frame(const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	if (!lz4f) {
		if (duckdb_lz4::LZ4F_isError(duckdb_lz4::LZ4F_createDecompressionContext(&lz4f, LZ4F_VERSION))) {
			lz4f = nullptr;
			throw OutOfMemoryException("Failed to allocate the LZ4 frame decoder");
		}
	} else {
		duckdb_lz4::LZ4F_resetDecompressionContext(lz4f);
	}
	out.Expect(MaxValue(src_size * 4, MIN_GROWN_BYTES));
	while (true) {
		size_t consumed = src_size;
		size_t produced = out.Room();
		const auto ret = duckdb_lz4::LZ4F_decompress(lz4f, out.Position(), &produced, src, &consumed, nullptr);
		if (duckdb_lz4::LZ4F_isError(ret)) {
			error = duckdb_lz4::LZ4F_getErrorName(ret);
			return false;
		}
		src += consumed;
		src_size -= consumed;
		out.Advance(produced);
		if (ret == 0) {
			if (src_size == 0) {
				return true;
			}
			continue;
		}
		if (consumed == 0 && produced == 0) {
			if (src_size == 0) {
				error = "the compressed data ends early";
				return false;
			}
			if (!out.Grow()) {
				error = StringUtil::Format("the data decompresses to more than %llu bytes", out.Used());
				return false;
			}
		}
	}
}

bool BlockDecompressor::Brotli(const_data_ptr_t src, idx_t src_size, Output &out, string &error) {
	unique_ptr<BrotliDecoderState, BrotliDecoderDeleter> state(
	    duckdb_brotli::BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
	if (!state) {
		throw OutOfMemoryException("Failed to allocate the Brotli decoder");
	}
	out.Expect(MaxValue(src_size * 4, MIN_GROWN_BYTES));
	size_t available_in = src_size;
	const uint8_t *next_in = src;
	while (true) {
		size_t available_out = out.Room();
		uint8_t *next_out = out.Position();
		const auto result = duckdb_brotli::BrotliDecoderDecompressStream(state.get(), &available_in, &next_in,
		                                                                 &available_out, &next_out, nullptr);
		out.Advance(NumericCast<idx_t>(next_out - out.Position()));
		switch (result) {
		case BROTLI_DECODER_RESULT_SUCCESS:
			return true;
		case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
			if (!out.Grow()) {
				error = StringUtil::Format("the data decompresses to more than %llu bytes", out.Used());
				return false;
			}
			break;
		case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
			error = "the compressed data ends early";
			return false;
		default:
			error = duckdb_brotli::BrotliDecoderErrorString(duckdb_brotli::BrotliDecoderGetErrorCode(state.get()));
			return false;
		}
	}
}

BlockCompressor::BlockCompressor(BlockCodec codec_p, int level_p) : codec(codec_p), level(level_p) {
	switch (codec) {
	case BlockCodec::ZSTD:
		zstd = duckdb_zstd::ZSTD_createCCtx();
		if (!zstd) {
			throw OutOfMemoryException("Failed to allocate the zstd encoder");
		}
		duckdb_zstd::ZSTD_CCtx_setParameter(zstd, duckdb_zstd::ZSTD_c_compressionLevel,
		                                    level == DEFAULT_LEVEL ? ZSTD_CLEVEL_DEFAULT : level);
		break;
	case BlockCodec::LZ4_FRAME:
		if (duckdb_lz4::LZ4F_isError(duckdb_lz4::LZ4F_createCompressionContext(&lz4f, LZ4F_VERSION))) {
			lz4f = nullptr;
			throw OutOfMemoryException("Failed to allocate the LZ4 frame encoder");
		}
		break;
	default:
		break;
	}
}

BlockCompressor::~BlockCompressor() {
	if (zlib) {
		deflateEnd(zlib.get());
	}
	if (zstd) {
		duckdb_zstd::ZSTD_freeCCtx(zstd);
	}
	if (lz4f) {
		duckdb_lz4::LZ4F_freeCompressionContext(lz4f);
	}
}

z_stream &BlockCompressor::Zlib() {
	if (!zlib) {
		zlib = make_uniq<z_stream>();
		memset(zlib.get(), 0, sizeof(z_stream));
		if (deflateInit2(zlib.get(), level == DEFAULT_LEVEL ? Z_DEFAULT_COMPRESSION : level, Z_DEFLATED,
		                 ZlibWindowBits(codec), DEFLATE_MEMORY_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
			zlib.reset();
			throw InvalidInputException("Failed to initialize the zlib encoder at level %d", level);
		}
	}
	return *zlib;
}

idx_t BlockCompressor::MaxCompressedSize(idx_t size) {
	switch (codec) {
	case BlockCodec::DEFLATE:
	case BlockCodec::GZIP:
		return deflateBound(&Zlib(), UnsafeNumericCast<uLong>(size)) + DEFLATE_FLUSH_BYTES;
	case BlockCodec::SNAPPY:
		return duckdb_snappy::MaxCompressedLength(size);
	case BlockCodec::ZSTD:
		return duckdb_zstd::ZSTD_compressBound(size);
	case BlockCodec::LZ4:
		if (size > LZ4_MAX_INPUT_SIZE) {
			throw InvalidInputException("A block of %llu bytes is larger than LZ4 compresses", size);
		}
		return NumericCast<idx_t>(duckdb_lz4::LZ4_compressBound(NumericCast<int>(size)));
	case BlockCodec::LZ4_FRAME: {
		const auto preferences = Lz4FramePreferences(level);
		return duckdb_lz4::LZ4F_compressFrameBound(size, &preferences);
	}
	case BlockCodec::BROTLI: {
		const auto bound = duckdb_brotli::BrotliEncoderMaxCompressedSize(size);
		if (bound == 0 && size > 0) {
			throw InvalidInputException("A block of %llu bytes is larger than Brotli compresses", size);
		}
		return bound;
	}
	}
	throw InternalException("Unknown block codec");
}

idx_t BlockCompressor::Compress(const_data_ptr_t src, idx_t size, data_ptr_t dst, idx_t capacity) {
	const std::span<const data_t> input(src, size);
	return Compress(std::span<const std::span<const data_t>>(&input, 1), true, dst, capacity);
}

idx_t BlockCompressor::Compress(std::span<const std::span<const data_t>> inputs, bool finish, data_ptr_t dst,
                                idx_t capacity) {
	if (codec == BlockCodec::DEFLATE || codec == BlockCodec::GZIP) {
		auto &stream = Zlib();
		if (deflateReset(&stream) != Z_OK) {
			throw InternalException("Failed to reset the zlib encoder");
		}
		const auto flush = finish ? Z_FINISH : Z_SYNC_FLUSH;
		idx_t written = 0;
		for (idx_t i = 0; i < inputs.size(); i++) {
			Deflate(inputs[i], i + 1 == inputs.size() ? flush : Z_NO_FLUSH, dst, capacity, written);
		}
		if (inputs.empty()) {
			Deflate({}, flush, dst, capacity, written);
		}
		return written;
	}
	if (!finish) {
		throw InternalException("Only a deflate block can end without finishing its stream");
	}
	if (inputs.size() <= 1) {
		return CompressOne(inputs.empty() ? std::span<const data_t>() : inputs[0], dst, capacity);
	}
	if (codec != BlockCodec::ZSTD) {
		throw InternalException("This block codec compresses one input at a time");
	}
	idx_t total = 0;
	for (auto &input : inputs) {
		total += input.size();
	}
	duckdb_zstd::ZSTD_CCtx_reset(zstd, duckdb_zstd::ZSTD_reset_session_only);
	duckdb_zstd::ZSTD_CCtx_setPledgedSrcSize(zstd, total);
	duckdb_zstd::ZSTD_outBuffer output {dst, capacity, 0};
	for (idx_t i = 0; i < inputs.size(); i++) {
		duckdb_zstd::ZSTD_inBuffer input {inputs[i].data(), inputs[i].size(), 0};
		const auto mode = i + 1 == inputs.size() ? duckdb_zstd::ZSTD_e_end : duckdb_zstd::ZSTD_e_continue;
		while (true) {
			const auto remaining = duckdb_zstd::ZSTD_compressStream2(zstd, &output, &input, mode);
			if (duckdb_zstd::ZSTD_isError(remaining)) {
				throw InternalException("Failed to compress a zstd block: %s",
				                        duckdb_zstd::ZSTD_getErrorName(remaining));
			}
			if (mode == duckdb_zstd::ZSTD_e_end ? remaining == 0 : input.pos == input.size) {
				break;
			}
			if (output.pos == output.size) {
				throw InternalException("A zstd block exceeds its compressed size bound");
			}
		}
	}
	return output.pos;
}

void BlockCompressor::Deflate(std::span<const data_t> input, int flush, data_ptr_t dst, idx_t capacity,
                              idx_t &written) {
	auto &stream = *zlib;
	auto data = input.data();
	auto remaining = input.size();
	while (true) {
		const auto step = MinValue<idx_t>(remaining, NumericLimits<uInt>::Maximum());
		const auto last = step == remaining;
		stream.next_in = data;
		stream.avail_in = UnsafeNumericCast<uInt>(step);
		do {
			const auto room = StreamStep(capacity - written);
			if (room == 0) {
				throw InternalException("A deflate block exceeds its compressed size bound");
			}
			stream.next_out = dst + written;
			stream.avail_out = room;
			const auto result = deflate(&stream, last ? flush : Z_NO_FLUSH);
			if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END) {
				throw InternalException("Failed to compress a deflate block");
			}
			written += room - stream.avail_out;
		} while (stream.avail_in > 0 || stream.avail_out == 0);
		data += step;
		remaining -= step;
		if (last) {
			return;
		}
	}
}

idx_t BlockCompressor::CompressOne(std::span<const data_t> input, data_ptr_t dst, idx_t capacity) {
	switch (codec) {
	case BlockCodec::SNAPPY: {
		size_t length;
		duckdb_snappy::RawCompress(const_char_ptr_cast(input.data()), input.size(), char_ptr_cast(dst), &length);
		D_ASSERT(length <= capacity);
		return length;
	}
	case BlockCodec::ZSTD: {
		const auto written = duckdb_zstd::ZSTD_compress2(zstd, dst, capacity, input.data(), input.size());
		if (duckdb_zstd::ZSTD_isError(written)) {
			throw InternalException("Failed to compress a zstd block: %s", duckdb_zstd::ZSTD_getErrorName(written));
		}
		return written;
	}
	case BlockCodec::LZ4: {
		const auto written = duckdb_lz4::LZ4_compress_default(
		    const_char_ptr_cast(input.data()), char_ptr_cast(dst), NumericCast<int>(input.size()),
		    NumericCast<int>(MinValue<idx_t>(capacity, NumericLimits<int32_t>::Maximum())));
		if (written <= 0) {
			throw InternalException("Failed to compress an LZ4 block");
		}
		return NumericCast<idx_t>(written);
	}
	case BlockCodec::LZ4_FRAME: {
		const auto preferences = Lz4FramePreferences(level);
		const auto written = duckdb_lz4::LZ4F_compressFrame_usingCDict(lz4f, dst, capacity, input.data(), input.size(),
		                                                               nullptr, &preferences);
		if (duckdb_lz4::LZ4F_isError(written)) {
			throw InternalException("Failed to compress an LZ4 frame: %s", duckdb_lz4::LZ4F_getErrorName(written));
		}
		return written;
	}
	case BlockCodec::BROTLI: {
		size_t length = capacity;
		if (!duckdb_brotli::BrotliEncoderCompress(level == DEFAULT_LEVEL ? BROTLI_DEFAULT_QUALITY : level,
		                                          BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE, input.size(),
		                                          input.data(), &length, dst)) {
			throw InternalException("Failed to compress a Brotli block");
		}
		return length;
	}
	default:
		throw InternalException("Unknown block codec");
	}
}

} // namespace duckdb
