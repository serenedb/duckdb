#include "duckdb/formats/block_codecs.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

#define ZLIB_CONST
#include <zlib.h>

namespace duckdb {

namespace {

constexpr idx_t MIN_GROWN_BYTES = 64 * 1024;
constexpr int GZIP_WINDOW_BITS = MAX_WBITS + 16;

uInt StreamStep(idx_t size) {
	return UnsafeNumericCast<uInt>(MinValue<idx_t>(size, NumericLimits<uInt>::Maximum()));
}

} // namespace

class BlockDecompressor::Output {
public:
	Output(data_ptr_t data_p, idx_t capacity_p) : data(data_p), capacity(capacity_p) {
	}
	Output(Allocator &allocator_p, AllocatedData &owned_p, idx_t padding_p)
	    : allocator(&allocator_p), owned(&owned_p), padding(padding_p) {
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
	case BlockCodec::GZIP:
		return Inflate(GZIP_WINDOW_BITS, src, src_size, out, error);
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

} // namespace duckdb
