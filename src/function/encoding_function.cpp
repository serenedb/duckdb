#include "duckdb/function/encoding_function.hpp"
#include "duckdb/main/config.hpp"

#include "duckdb/common/bswap.hpp"
#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"

#include "simdutf.h"

namespace duckdb {

namespace {

constexpr idx_t BLOCK_BYTES = 4096;

struct Latin1Decoder {
	using UNIT = char;
	static constexpr idx_t UTF8_PER_UNIT = 2;
	static constexpr bool SWAP = false;

	static bool StartsPair(const char *) {
		return false;
	}
	static idx_t Convert(const char *input, idx_t count, char *output) {
		return simdutf::convert_latin1_to_utf8(input, count, output);
	}
	static idx_t ValidUnits(const char *, idx_t count) {
		return count;
	}
};

template <std::endian ENDIAN>
struct Utf16Decoder {
	using UNIT = char16_t;
	static constexpr idx_t UTF8_PER_UNIT = 3;
	static constexpr bool SWAP = false;

	static bool StartsPair(const char *unit) {
		return (static_cast<uint8_t>(unit[ENDIAN == std::endian::big ? 0 : 1]) & 0xFC) == 0xD8;
	}
	static idx_t Convert(const char16_t *input, idx_t count, char *output) {
		if constexpr (ENDIAN == std::endian::big) {
			return simdutf::convert_utf16be_to_utf8(input, count, output);
		} else {
			return simdutf::convert_utf16le_to_utf8(input, count, output);
		}
	}
	static idx_t ValidUnits(const char16_t *input, idx_t count) {
		if constexpr (ENDIAN == std::endian::big) {
			return simdutf::validate_utf16be_with_errors(input, count).count;
		} else {
			return simdutf::validate_utf16le_with_errors(input, count).count;
		}
	}
};

template <std::endian ENDIAN>
struct Utf32Decoder {
	using UNIT = char32_t;
	static constexpr idx_t UTF8_PER_UNIT = 4;
	static constexpr bool SWAP = ENDIAN != std::endian::native;

	static bool StartsPair(const char *) {
		return false;
	}
	static idx_t Convert(const char32_t *input, idx_t count, char *output) {
		return simdutf::convert_utf32_to_utf8(input, count, output);
	}
	static idx_t ValidUnits(const char32_t *input, idx_t count) {
		return simdutf::validate_utf32_with_errors(input, count).count;
	}
};

template <typename OP>
auto VisitDecoder(TextEncoding encoding, OP &&op) {
	switch (encoding) {
	case TextEncoding::LATIN1:
		return op.template operator()<Latin1Decoder>();
	case TextEncoding::UTF16_LE:
		return op.template operator()<Utf16Decoder<std::endian::little>>();
	case TextEncoding::UTF16_BE:
		return op.template operator()<Utf16Decoder<std::endian::big>>();
	case TextEncoding::UTF32_LE:
		return op.template operator()<Utf32Decoder<std::endian::little>>();
	case TextEncoding::UTF32_BE:
		return op.template operator()<Utf32Decoder<std::endian::big>>();
	}
	throw InternalException("Unsupported text encoding");
}

template <typename DECODER>
const typename DECODER::UNIT *CopyUnits(const char *input, idx_t count, typename DECODER::UNIT *units) {
	memcpy(units, input, count * sizeof(typename DECODER::UNIT));
	if constexpr (DECODER::SWAP) {
		for (idx_t i = 0; i < count; i++) {
			units[i] = BSwap(units[i]);
		}
	}
	return units;
}

template <typename DECODER>
Utf8DecodeResult DecodeUnits(const char *input, idx_t size, char *output, idx_t capacity) {
	using UNIT = typename DECODER::UNIT;
	const idx_t units = size / sizeof(UNIT);
	const bool direct = !DECODER::SWAP && reinterpret_cast<uintptr_t>(input) % alignof(UNIT) == 0;
	UNIT block[BLOCK_BYTES / sizeof(UNIT)];
	idx_t read = 0;
	idx_t written = 0;
	while (true) {
		auto count = MinValue<idx_t>(units - read, (capacity - written) / DECODER::UTF8_PER_UNIT);
		if (!direct) {
			count = MinValue<idx_t>(count, BLOCK_BYTES / sizeof(UNIT));
		}
		if (count != 0 && DECODER::StartsPair(input + (read + count - 1) * sizeof(UNIT))) {
			count--;
		}
		if (count == 0) {
			break;
		}
		const auto batch = direct ? reinterpret_cast<const UNIT *>(input) + read
		                          : CopyUnits<DECODER>(input + read * sizeof(UNIT), count, block);
		const auto converted = DECODER::Convert(batch, count, output + written);
		if (converted == 0) {
			const auto valid = DECODER::ValidUnits(batch, count);
			written += DECODER::Convert(batch, valid, output + written);
			return {(read + valid) * sizeof(UNIT), written, Utf8DecodeStatus::INVALID};
		}
		read += count;
		written += converted;
	}
	while (read < units) {
		const auto sequence = input + read * sizeof(UNIT);
		const idx_t length = DECODER::StartsPair(sequence) ? 2 : 1;
		if (read + length > units) {
			break;
		}
		if (written == capacity) {
			return {read * sizeof(UNIT), written, Utf8DecodeStatus::FULL};
		}
		UNIT copy[2];
		char utf8[4];
		const auto converted = DECODER::Convert(CopyUnits<DECODER>(sequence, length, copy), length, utf8);
		if (converted == 0) {
			return {read * sizeof(UNIT), written, Utf8DecodeStatus::INVALID};
		}
		if (converted > capacity - written) {
			return {read * sizeof(UNIT), written, Utf8DecodeStatus::FULL};
		}
		memcpy(output + written, utf8, converted);
		read += length;
		written += converted;
	}
	const auto status = read * sizeof(UNIT) == size ? Utf8DecodeStatus::DONE : Utf8DecodeStatus::INCOMPLETE;
	return {read * sizeof(UNIT), written, status};
}

template <TextEncoding ENCODING>
void DecodeEncodedBuffer(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                         const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                         EncodingFunction *encoding_function) {
	const auto input = encoded_buffer.Ptr() + encoded_buffer.cur_pos;
	const auto size = encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos;
	const auto room = target_buffer_size - target_buffer_current_position;
	const auto result = DecodeToUtf8(ENCODING, input, size, target_buffer + target_buffer_current_position, room);
	encoded_buffer.cur_pos += result.read;
	target_buffer_current_position += result.written;
	switch (result.status) {
	case Utf8DecodeStatus::DONE:
		return;
	case Utf8DecodeStatus::INCOMPLETE:
		if (!encoded_buffer.last_buffer || target_buffer_current_position == target_buffer_size) {
			return;
		}
		break;
	case Utf8DecodeStatus::FULL: {
		const auto fit = room - result.written;
		if (fit == 0) {
			return;
		}
		char utf8[4];
		const auto rest = DecodeToUtf8(ENCODING, input + result.read, size - result.read, utf8,
		                               MinValue<idx_t>(sizeof(utf8), fit + encoding_function->GetBytesPerIteration()));
		D_ASSERT(rest.written > fit);
		memcpy(target_buffer + target_buffer_current_position, utf8, fit);
		memcpy(remaining_bytes_buffer, utf8 + fit, rest.written - fit);
		remaining_bytes_size = rest.written - fit;
		target_buffer_current_position = target_buffer_size;
		encoded_buffer.cur_pos += rest.read;
		return;
	}
	case Utf8DecodeStatus::INVALID:
		break;
	}
	constexpr bool UTF32 = ENCODING == TextEncoding::UTF32_LE || ENCODING == TextEncoding::UTF32_BE;
	const char *reason = result.status != Utf8DecodeStatus::INVALID ? "incomplete character at the end of the input"
	                     : UTF32                                    ? "invalid UTF-32 code point"
	                                                                : "invalid UTF-16 code unit";
	throw InvalidInputException(unordered_map<string, string> {{"encoding_error", reason}},
	                            UTF32 ? "File is not utf-32 encoded" : "File is not utf-16 encoded");
}

} // namespace

struct DefaultEncodeMethod {
	string name;
	encode_t encode_function;
	idx_t ratio;
	idx_t bytes_per_iteration;
};

Utf8DecodeResult DecodeToUtf8(TextEncoding encoding, const char *input, idx_t size, char *output, idx_t capacity) {
	return VisitDecoder(encoding,
	                    [&]<typename DECODER>() { return DecodeUnits<DECODER>(input, size, output, capacity); });
}

void DecodeUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                EncodingFunction *encoding_function) {
	throw InternalException("Decode UTF8 is not a valid function, and should be verified one level up.");
}

void EncodingFunctionSet::Initialize(DBConfig &config) {
	constexpr auto NATIVE_UTF16 =
	    std::endian::native == std::endian::big ? TextEncoding::UTF16_BE : TextEncoding::UTF16_LE;
	constexpr auto NATIVE_UTF32 =
	    std::endian::native == std::endian::big ? TextEncoding::UTF32_BE : TextEncoding::UTF32_LE;
	config.RegisterEncodeFunction({"utf-8", DecodeUTF8, 1, 1});
	config.RegisterEncodeFunction({"latin-1", DecodeEncodedBuffer<TextEncoding::LATIN1>, 2, 1});
	// bytes_per_iteration = 3: an astral char is 4 UTF-8 bytes, so up to 3 can straddle the target buffer.
	// lookup_bytes = 4 for both: the longest indivisible input sequence (a utf-16 surrogate pair / one
	// utf-32 unit), so CSVEncoder::Encode carries a split one into the next buffer instead of erroring.
	config.RegisterEncodeFunction({"utf-16", DecodeEncodedBuffer<NATIVE_UTF16>, 3, 4});
	config.RegisterEncodeFunction({"utf-16le", DecodeEncodedBuffer<TextEncoding::UTF16_LE>, 3, 4});
	config.RegisterEncodeFunction({"utf-16be", DecodeEncodedBuffer<TextEncoding::UTF16_BE>, 3, 4});
	config.RegisterEncodeFunction({"utf-32", DecodeEncodedBuffer<NATIVE_UTF32>, 3, 4});
	config.RegisterEncodeFunction({"utf-32le", DecodeEncodedBuffer<TextEncoding::UTF32_LE>, 3, 4});
	config.RegisterEncodeFunction({"utf-32be", DecodeEncodedBuffer<TextEncoding::UTF32_BE>, 3, 4});
}

void DBConfig::RegisterEncodeFunction(const EncodingFunction &function) const {
	lock_guard<mutex> l(encoding_functions->lock);
	const auto name = function.GetName();
	if (encoding_functions->functions.find(name) != encoding_functions->functions.end()) {
		throw InvalidInputException("Decoding function with name %s already registered", name);
	}
	encoding_functions->functions[name] = function;
}

optional_ptr<EncodingFunction> DBConfig::GetEncodeFunction(const string &name) const {
	lock_guard<mutex> l(encoding_functions->lock);
	// Check if the function is already loaded into the global compression functions.
	if (encoding_functions->functions.find(name) != encoding_functions->functions.end()) {
		return &encoding_functions->functions[name];
	}
	return nullptr;
}

vector<reference<EncodingFunction>> DBConfig::GetLoadedEncodedFunctions() const {
	lock_guard<mutex> l(encoding_functions->lock);
	vector<reference<EncodingFunction>> result;
	for (auto &function : encoding_functions->functions) {
		result.push_back(function.second);
	}
	return result;
}
} // namespace duckdb
