#include "duckdb/common/types/blob.hpp"
#include "duckdb/function/cast/default_casts.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/string_type.hpp"

#include "simdutf.h"

namespace duckdb {

constexpr const char *Blob::HEX_TABLE;
const int Blob::HEX_MAP[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

bool IsRegularCharacter(data_t c) {
	return c >= 32 && c <= 126 && c != '\\' && c != '\'' && c != '"';
}

idx_t Blob::GetStringSize(string_t blob) {
	auto data = const_data_ptr_cast(blob.GetData());
	auto len = blob.GetSize();
	idx_t str_len = 0;
	for (idx_t i = 0; i < len; i++) {
		if (IsRegularCharacter(data[i])) {
			// ascii characters are rendered as-is
			str_len++;
		} else {
			// non-ascii characters are rendered as hexadecimal (e.g. \x00)
			str_len += 4;
		}
	}
	return str_len;
}

void Blob::ToString(string_t blob, char *output) {
	auto data = const_data_ptr_cast(blob.GetData());
	auto len = blob.GetSize();
	idx_t str_idx = 0;
	for (idx_t i = 0; i < len; i++) {
		if (IsRegularCharacter(data[i])) {
			// ascii characters are rendered as-is
			output[str_idx++] = UnsafeNumericCast<char>(data[i]);
		} else {
			auto byte_a = data[i] >> 4;
			auto byte_b = data[i] & 0x0F;
			D_ASSERT(byte_a >= 0 && byte_a < 16);
			D_ASSERT(byte_b >= 0 && byte_b < 16);
			// non-ascii characters are rendered as hexadecimal (e.g. \x00)
			output[str_idx++] = '\\';
			output[str_idx++] = 'x';
			output[str_idx++] = Blob::HEX_TABLE[byte_a];
			output[str_idx++] = Blob::HEX_TABLE[byte_b];
		}
	}
	D_ASSERT(str_idx == GetStringSize(blob));
}

string Blob::ToString(string_t blob) {
	auto str_len = GetStringSize(blob);
	auto buffer = make_unsafe_uniq_array_uninitialized<char>(str_len);
	Blob::ToString(blob, buffer.get());
	return string(buffer.get(), str_len);
}

bool Blob::TryGetBlobSize(string_t str, idx_t &str_len, CastParameters &parameters) {
	auto data = const_data_ptr_cast(str.GetData());
	auto len = str.GetSize();
	str_len = 0;
	for (idx_t i = 0; i < len; i++) {
		if (data[i] == '\\') {
			if (i + 3 >= len) {
				string error = StringUtil::Format("Invalid hex escape code encountered in string -> blob conversion of "
				                                  "string \"%s\": unterminated escape code at end of blob",
				                                  str.GetString());
				HandleCastError::AssignError(error, parameters);
				return false;
			}
			if (data[i + 1] != 'x' || Blob::HEX_MAP[data[i + 2]] < 0 || Blob::HEX_MAP[data[i + 3]] < 0) {
				string error = StringUtil::Format(
				    "Invalid hex escape code encountered in string -> blob conversion of string \"%s\": %s",
				    str.GetString(), string(const_char_ptr_cast(data) + i, 4));
				HandleCastError::AssignError(error, parameters);
				return false;
			}
			str_len++;
			i += 3;
		} else if (data[i] <= 127) {
			str_len++;
		} else {
			string error = StringUtil::Format(
			    "Invalid byte encountered in STRING -> BLOB conversion of string \"%s\". All non-ascii characters "
			    "must be escaped with hex codes (e.g. \\xAA)",
			    str.GetString());
			HandleCastError::AssignError(error, parameters);
			return false;
		}
	}
	return true;
}

idx_t Blob::GetBlobSize(string_t str) {
	CastParameters parameters;
	return GetBlobSize(str, parameters);
}

idx_t Blob::GetBlobSize(string_t str, CastParameters &parameters) {
	idx_t str_len;
	auto result = Blob::TryGetBlobSize(str, str_len, parameters);
	if (!result) {
		throw InternalException("Blob::TryGetBlobSize failed but no exception was thrown!?");
	}
	return str_len;
}

void Blob::ToBlob(string_t str, data_ptr_t output) {
	auto data = const_data_ptr_cast(str.GetData());
	auto len = str.GetSize();
	idx_t blob_idx = 0;
	for (idx_t i = 0; i < len; i++) {
		if (data[i] == '\\') {
			int byte_a = Blob::HEX_MAP[data[i + 2]];
			int byte_b = Blob::HEX_MAP[data[i + 3]];
			D_ASSERT(i + 3 < len);
			D_ASSERT(byte_a >= 0 && byte_b >= 0);
			D_ASSERT(data[i + 1] == 'x');
			output[blob_idx++] = UnsafeNumericCast<data_t>((byte_a << 4) + byte_b);
			i += 3;
		} else if (data[i] <= 127) {
			output[blob_idx++] = data_t(data[i]);
		} else {
			throw ConversionException("Invalid byte encountered in STRING -> BLOB conversion. All non-ascii characters "
			                          "must be escaped with hex codes (e.g. \\xAA)");
		}
	}
	D_ASSERT(blob_idx == GetBlobSize(str));
}

string Blob::ToBlob(string_t str) {
	CastParameters parameters;
	return Blob::ToBlob(str, parameters);
}

string Blob::ToBlob(string_t str, CastParameters &parameters) {
	auto blob_len = GetBlobSize(str, parameters);
	auto buffer = make_unsafe_uniq_array_uninitialized<char>(blob_len);
	Blob::ToBlob(str, data_ptr_cast(buffer.get()));
	return string(buffer.get(), blob_len);
}

// base64 functions are adapted from https://gist.github.com/tomykaira/f0fd86b6c73063283afe550bc5d77594
string Blob::ToBase64(string_t blob) {
	auto base64_size = Blob::ToBase64Size(blob);
	auto data = make_uniq_array<char>(base64_size);
	Blob::ToBase64(blob, data.get());
	return string(data.get(), base64_size);
}

idx_t Blob::ToBase64Size(string_t blob) {
	// every 4 characters in base64 encode 3 bytes, plus (potential) padding at the end
	auto input_size = blob.GetSize();
	return ((input_size + 2) / 3) * 4;
}

void Blob::ToBase64(string_t blob, char *output) {
	simdutf::binary_to_base64(blob.GetData(), blob.GetSize(), output, simdutf::base64_default);
}

static bool TryFromBase64(string_t str, char *output, size_t &output_size) {
	auto result = simdutf::base64_to_binary_safe(str.GetData(), str.GetSize(), output, output_size,
	                                             simdutf::base64_default, simdutf::last_chunk_handling_options::strict);
	return result.error == simdutf::error_code::SUCCESS;
}

string Blob::FromBase64(string_t blob) {
	string result;
	bool success = false;
	result.resize_and_overwrite(FromBase64Size(blob), [&](char *data, size_t size) {
		success = TryFromBase64(blob, data, size);
		return success ? size : 0;
	});
	if (!success) {
		throw ConversionException("Could not decode string \"%s\" as base64", blob.GetString());
	}
	return result;
}

idx_t Blob::FromBase64Size(string_t str) {
	return simdutf::maximal_binary_length_from_base64(str.GetData(), str.GetSize());
}

idx_t Blob::FromBase64(string_t str, data_ptr_t output, idx_t output_size) {
	size_t decoded_size = output_size;
	if (!TryFromBase64(str, char_ptr_cast(output), decoded_size)) {
		throw ConversionException("Could not decode string \"%s\" as base64", str.GetString());
	}
	return decoded_size;
}

} // namespace duckdb
