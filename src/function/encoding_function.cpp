#include "duckdb/function/encoding_function.hpp"
#include "duckdb/main/config.hpp"

#include "duckdb/common/bswap.hpp"
#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"

#include "simdutf.h"

namespace duckdb {

namespace {

uint16_t ReadUTF16LE(const char *p) {
	return static_cast<uint16_t>(static_cast<unsigned char>(p[0]) | (static_cast<unsigned char>(p[1]) << 8));
}

uint16_t ReadUTF16BE(const char *p) {
	return static_cast<uint16_t>((static_cast<unsigned char>(p[0]) << 8) | static_cast<unsigned char>(p[1]));
}

char32_t ReadUTF32LE(const char *p) {
	return static_cast<char32_t>(static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
	                             (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
	                             (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
	                             (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24));
}

char32_t ReadUTF32BE(const char *p) {
	return static_cast<char32_t>((static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
	                             (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
	                             (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
	                             static_cast<uint32_t>(static_cast<unsigned char>(p[3])));
}

} // namespace

struct DefaultEncodeMethod {
	string name;
	encode_t encode_function;
	idx_t ratio;
	idx_t bytes_per_iteration;
};

template <std::endian Endian>
void DecodeUTF16ToUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                       const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                       EncodingFunction *encoding_function) {
	auto encoded_ptr = encoded_buffer.Ptr();
	const auto read_unit = [&](idx_t pos) {
		return Endian == std::endian::big ? ReadUTF16BE(encoded_ptr + pos) : ReadUTF16LE(encoded_ptr + pos);
	};
	const auto is_high_surrogate = [](uint16_t unit) {
		return unit >= 0xD800 && unit <= 0xDBFF;
	};
	// cur_pos stays 2-byte aligned (units are read/consumed in pairs), so the char16_t view is well-aligned.
	// simdutf's utf16le/be helpers interpret each 2-byte code unit in the file's endianness regardless of host.
	while (encoded_buffer.cur_pos + 1 < encoded_buffer.actual_encoded_buffer_size &&
	       target_buffer_current_position < target_buffer_size) {
		auto src = reinterpret_cast<const char16_t *>(encoded_ptr + encoded_buffer.cur_pos);
		idx_t avail_units = (encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos) / 2;
		// A high surrogate as the last unit may be the first half of a pair whose low half is in the next
		// file read: hold it back so CSVEncoder::Encode carries it into the next buffer (lookup_bytes = 4).
		// Only at the end of the file is it a real encoding error.
		if (is_high_surrogate(read_unit(encoded_buffer.cur_pos + (avail_units - 1) * 2))) {
			if (avail_units == 1) {
				if (encoded_buffer.last_buffer) {
					throw InvalidInputException("File is not utf-16 encoded");
				}
				return;
			}
			avail_units--;
		}
		idx_t out_needed = Endian == std::endian::big ? simdutf::utf8_length_from_utf16be(src, avail_units)
		                                              : simdutf::utf8_length_from_utf16le(src, avail_units);
		idx_t out_avail = target_buffer_size - target_buffer_current_position;
		if (out_needed <= out_avail) {
			// The remaining chunk fits in the target: transcode it all at once. simdutf decodes surrogate
			// PAIRS correctly and returns 0 on any unpaired/invalid surrogate.
			idx_t written =
			    Endian == std::endian::big
			        ? simdutf::convert_utf16be_to_utf8(src, avail_units, target_buffer + target_buffer_current_position)
			        : simdutf::convert_utf16le_to_utf8(src, avail_units,
			                                           target_buffer + target_buffer_current_position);
			if (written == 0) {
				throw InvalidInputException("File is not utf-16 encoded");
			}
			target_buffer_current_position += written;
			encoded_buffer.cur_pos += avail_units * 2;
			continue;
		}
		// Boundary: the target cannot hold the full conversion. Emit one codepoint (1 unit, or a 2-unit
		// surrogate pair) at a time, stashing the bytes that straddle the end of the target buffer. A pair
		// here is always complete: a trailing high surrogate was held back above.
		uint16_t unit = read_unit(encoded_buffer.cur_pos);
		idx_t cp_units = is_high_surrogate(unit) ? 2 : 1;
		char utf8[4];
		auto cp_src = reinterpret_cast<const char16_t *>(encoded_ptr + encoded_buffer.cur_pos);
		idx_t n = Endian == std::endian::big ? simdutf::convert_utf16be_to_utf8(cp_src, cp_units, utf8)
		                                     : simdutf::convert_utf16le_to_utf8(cp_src, cp_units, utf8);
		if (n == 0) {
			throw InvalidInputException("File is not utf-16 encoded");
		}
		encoded_buffer.cur_pos += cp_units * 2;
		idx_t k = 0;
		while (k < n && target_buffer_current_position < target_buffer_size) {
			target_buffer[target_buffer_current_position++] = utf8[k++];
		}
		if (k < n) {
			idx_t stash = 0;
			while (k < n) {
				remaining_bytes_buffer[stash++] = utf8[k++];
			}
			remaining_bytes_size = stash;
			return;
		}
	}
	// A trailing odd byte can complete in the next read; at the end of the file it cannot.
	if (encoded_buffer.last_buffer && target_buffer_current_position < target_buffer_size &&
	    encoded_buffer.cur_pos < encoded_buffer.actual_encoded_buffer_size) {
		throw InvalidInputException("File is not utf-16 encoded");
	}
}

template <std::endian Endian>
void DecodeUTF32ToUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                       const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                       EncodingFunction *encoding_function) {
	auto encoded_ptr = encoded_buffer.Ptr();
	// A UTF-32 code unit is 4 bytes and cur_pos stays 4-byte aligned (units are consumed whole). simdutf only
	// transcodes native-endian UTF-32, so the bulk fast-path runs only when the file endianness matches the host;
	// otherwise we decode one code unit at a time, assembling each from its 4 bytes (host-endianness independent).
	while (encoded_buffer.cur_pos + 3 < encoded_buffer.actual_encoded_buffer_size &&
	       target_buffer_current_position < target_buffer_size) {
		if constexpr (Endian == std::endian::native) {
			auto src = reinterpret_cast<const char32_t *>(encoded_ptr + encoded_buffer.cur_pos);
			idx_t avail_units = (encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos) / 4;
			idx_t out_needed = simdutf::utf8_length_from_utf32(src, avail_units);
			idx_t out_avail = target_buffer_size - target_buffer_current_position;
			if (out_needed <= out_avail) {
				// The remaining chunk fits in the target: transcode it all at once. simdutf returns 0 on any
				// code point above U+10FFFF or in the surrogate range.
				idx_t written =
				    simdutf::convert_utf32_to_utf8(src, avail_units, target_buffer + target_buffer_current_position);
				if (written == 0) {
					throw InvalidInputException("File is not utf-32 encoded");
				}
				target_buffer_current_position += written;
				encoded_buffer.cur_pos += avail_units * 4;
				continue;
			}
		}
		// Boundary (target buffer nearly full) or non-native endianness: emit one code unit (one codepoint) at a
		// time, stashing the UTF-8 bytes that straddle the end of the target buffer.
		char32_t cp = Endian == std::endian::big ? ReadUTF32BE(encoded_ptr + encoded_buffer.cur_pos)
		                                         : ReadUTF32LE(encoded_ptr + encoded_buffer.cur_pos);
		char utf8[4];
		idx_t n = simdutf::convert_utf32_to_utf8(&cp, 1, utf8);
		if (n == 0) {
			throw InvalidInputException("File is not utf-32 encoded");
		}
		encoded_buffer.cur_pos += 4;
		idx_t k = 0;
		while (k < n && target_buffer_current_position < target_buffer_size) {
			target_buffer[target_buffer_current_position++] = utf8[k++];
		}
		if (k < n) {
			idx_t stash = 0;
			while (k < n) {
				remaining_bytes_buffer[stash++] = utf8[k++];
			}
			remaining_bytes_size = stash;
			return;
		}
	}
	// A trailing partial code unit (1-3 bytes) can complete in the next read; at the end of the file it cannot.
	if (encoded_buffer.last_buffer && target_buffer_current_position < target_buffer_size &&
	    encoded_buffer.cur_pos < encoded_buffer.actual_encoded_buffer_size) {
		throw InvalidInputException("File is not utf-32 encoded");
	}
}

void DecodeLatin1ToUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                        const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                        EncodingFunction *encoding_function) {
	auto encoded_ptr = encoded_buffer.Ptr();
	// Latin-1 (ISO-8859-1): every byte 0x00-0xFF maps to U+0000-U+00FF, so any input is valid.
	while (encoded_buffer.cur_pos < encoded_buffer.actual_encoded_buffer_size &&
	       target_buffer_current_position < target_buffer_size) {
		auto src = encoded_ptr + encoded_buffer.cur_pos;
		idx_t avail = encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos;
		idx_t out_needed = simdutf::utf8_length_from_latin1(src, avail);
		idx_t out_avail = target_buffer_size - target_buffer_current_position;
		if (out_needed <= out_avail) {
			idx_t written = simdutf::convert_latin1_to_utf8(src, avail, target_buffer + target_buffer_current_position);
			target_buffer_current_position += written;
			encoded_buffer.cur_pos += avail;
			continue;
		}
		// Boundary: emit one byte (1-2 UTF-8 bytes) at a time, stashing the straddling byte.
		char utf8[2];
		idx_t n = simdutf::convert_latin1_to_utf8(src, 1, utf8);
		encoded_buffer.cur_pos++;
		idx_t k = 0;
		while (k < n && target_buffer_current_position < target_buffer_size) {
			target_buffer[target_buffer_current_position++] = utf8[k++];
		}
		if (k < n) {
			remaining_bytes_buffer[0] = utf8[k];
			remaining_bytes_size = 1;
			return;
		}
	}
}

void DecodeUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                EncodingFunction *encoding_function) {
	throw InternalException("Decode UTF8 is not a valid function, and should be verified one level up.");
}

void EncodingFunctionSet::Initialize(DBConfig &config) {
	config.RegisterEncodeFunction({"utf-8", DecodeUTF8, 1, 1});
	config.RegisterEncodeFunction({"latin-1", DecodeLatin1ToUTF8, 2, 1});
	// bytes_per_iteration = 3: an astral char is 4 UTF-8 bytes, so up to 3 can straddle the target buffer.
	// lookup_bytes = 4 for both: the longest indivisible input sequence (a utf-16 surrogate pair / one
	// utf-32 unit), so CSVEncoder::Encode carries a split one into the next buffer instead of erroring.
	config.RegisterEncodeFunction({"utf-16", DecodeUTF16ToUTF8<std::endian::native>, 3, 4});
	config.RegisterEncodeFunction({"utf-16le", DecodeUTF16ToUTF8<std::endian::little>, 3, 4});
	config.RegisterEncodeFunction({"utf-16be", DecodeUTF16ToUTF8<std::endian::big>, 3, 4});
	config.RegisterEncodeFunction({"utf-32", DecodeUTF32ToUTF8<std::endian::native>, 3, 4});
	config.RegisterEncodeFunction({"utf-32le", DecodeUTF32ToUTF8<std::endian::little>, 3, 4});
	config.RegisterEncodeFunction({"utf-32be", DecodeUTF32ToUTF8<std::endian::big>, 3, 4});
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
