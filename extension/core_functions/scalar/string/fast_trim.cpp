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

#include "core_functions/scalar/fast_trim.hpp"

#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "utf8proc.hpp"

namespace duckdb {

static bool IsSpaceSeparator(const char *data, idx_t size) {
	utf8proc_int32_t codepoint;
	auto bytes = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(data),
	                              UnsafeNumericCast<utf8proc_ssize_t>(size), &codepoint);
	D_ASSERT(bytes > 0);
	return utf8proc_category(codepoint) == UTF8PROC_CATEGORY_ZS;
}

static idx_t LeftTrimEnd(const char *data, idx_t size) {
	idx_t begin = 0;
	while (begin < size) {
		auto c = static_cast<unsigned char>(data[begin]);
		if (c < 0x80) {
			if (c != ' ') {
				break;
			}
			begin++;
			continue;
		}
		utf8proc_int32_t codepoint;
		auto bytes = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(data + begin),
		                              UnsafeNumericCast<utf8proc_ssize_t>(size - begin), &codepoint);
		D_ASSERT(bytes > 0);
		if (utf8proc_category(codepoint) != UTF8PROC_CATEGORY_ZS) {
			break;
		}
		begin += UnsafeNumericCast<idx_t>(bytes);
	}
	return begin;
}

static idx_t RightTrimBegin(const char *data, idx_t begin, idx_t size) {
	idx_t end = size;
	while (end > begin) {
		auto c = static_cast<unsigned char>(data[end - 1]);
		if (c < 0x80) {
			if (c != ' ') {
				break;
			}
			end--;
			continue;
		}
		idx_t start = end - 1;
		while (start > begin && (static_cast<unsigned char>(data[start]) & 0xC0) == 0x80) {
			start--;
		}
		if (!IsSpaceSeparator(data + start, end - start)) {
			break;
		}
		end = start;
	}
	return end;
}

void FastTrim::Execute(const Vector &input, Vector &result, bool ltrim, bool rtrim) {
	StringVector::AddHeapReference(result, input);
	UnaryExecutor::Execute<string_t, string_t>(input, result, [&](string_t str) {
		auto data = str.GetData();
		auto size = str.GetSize();
		idx_t begin = ltrim ? LeftTrimEnd(data, size) : 0;
		idx_t end = rtrim ? RightTrimBegin(data, begin, size) : size;
		if (begin == 0 && end == size) {
			return str;
		}
		return string_t(data + begin, UnsafeNumericCast<uint32_t>(end - begin));
	});
}

} // namespace duckdb
