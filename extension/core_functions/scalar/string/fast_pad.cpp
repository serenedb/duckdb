////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "core_functions/scalar/fast_pad.hpp"

#include "duckdb/function/scalar/string_common.hpp"

#include <cstring>

namespace duckdb {

static idx_t CharacterOffset(const char *data, idx_t size, idx_t chars) {
	auto ascii = FirstNonAscii(data, MinValue(size, chars));
	if (ascii == chars) {
		return chars;
	}
	idx_t seen = ascii;
	for (idx_t i = ascii; i < size; i++) {
		if (IsCharacter(data[i])) {
			if (seen == chars) {
				return i;
			}
			seen++;
		}
	}
	return size;
}

static void FillPad(char *target, idx_t bytes, const char *pad, idx_t pad_size) {
	if (pad_size == 1) {
		memset(target, pad[0], bytes);
		return;
	}
	idx_t written = MinValue(bytes, pad_size);
	memcpy(target, pad, written);
	while (written < bytes) {
		auto chunk = MinValue(bytes - written, written);
		memcpy(target + written, target, chunk);
		written += chunk;
	}
}

bool FastPad::TryPad(bool left, const string_t &str, idx_t len, const string_t &pad, Vector &result, string_t &out) {
	auto str_data = str.GetData();
	auto str_size = str.GetSize();
	idx_t keep_bytes;
	idx_t keep_chars;
	if (str_size <= len) {
		keep_bytes = str_size;
		keep_chars = Utf8CharacterCount(str_data, str_size);
	} else {
		keep_bytes = CharacterOffset(str_data, str_size, len);
		keep_chars = keep_bytes == str_size ? Utf8CharacterCount(str_data, str_size) : len;
	}
	auto fill_chars = len - keep_chars;
	auto pad_data = pad.GetData();
	auto pad_size = pad.GetSize();
	idx_t fill_bytes = 0;
	if (fill_chars > 0) {
		if (pad_size == 0) {
			return false;
		}
		auto pad_chars = Utf8CharacterCount(pad_data, pad_size);
		fill_bytes = (fill_chars / pad_chars) * pad_size + CharacterOffset(pad_data, pad_size, fill_chars % pad_chars);
	}
	out = StringVector::EmptyString(result, keep_bytes + fill_bytes);
	auto target = out.GetDataWriteable();
	if (left) {
		FillPad(target, fill_bytes, pad_data, pad_size);
		memcpy(target + fill_bytes, str_data, keep_bytes);
	} else {
		memcpy(target, str_data, keep_bytes);
		FillPad(target + keep_bytes, fill_bytes, pad_data, pad_size);
	}
	out.Finalize();
	return true;
}

} // namespace duckdb
