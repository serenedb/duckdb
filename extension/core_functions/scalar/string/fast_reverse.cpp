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

#include "core_functions/scalar/fast_reverse.hpp"

#include "duckdb/common/grapheme_segments.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "utf8proc_wrapper.hpp"

#include <cstring>

namespace duckdb {

static void ReverseBytes(const char *input, idx_t begin, idx_t end, idx_t size, char *output) {
	idx_t i = begin;
	for (; i + 8 <= end; i += 8) {
		uint64_t word;
		memcpy(&word, input + i, 8);
		word = __builtin_bswap64(word);
		memcpy(output + size - i - 8, &word, 8);
	}
	for (; i < end; i++) {
		output[size - i - 1] = input[i];
	}
}

static void ReverseSimple(const char *input, idx_t begin, idx_t end, idx_t size, char *output) {
	if (begin == end) {
		return;
	}
	if (FirstNonAscii(input + begin, end - begin) == end - begin && !memchr(input + begin, '\r', end - begin)) {
		ReverseBytes(input, begin, end, size, output);
		return;
	}
	for (idx_t i = begin; i < end;) {
		auto c = static_cast<unsigned char>(input[i]);
		if (c >= 0x80) {
			output[size - i - 2] = input[i];
			output[size - i - 1] = input[i + 1];
			i += 2;
		} else if (i + 1 < end && GraphemeSegments::IsCrLf(input, i)) {
			output[size - i - 2] = '\r';
			output[size - i - 1] = '\n';
			i += 2;
		} else {
			output[size - i - 1] = input[i];
			i++;
		}
	}
}

static void ReverseClusters(const char *input, idx_t begin, idx_t end, idx_t size, char *output) {
	for (auto cluster : Utf8Proc::GraphemeClusters(input + begin, end - begin)) {
		memcpy(output + size - begin - cluster.end, input + begin + cluster.start, cluster.end - cluster.start);
	}
}

void FastReverse::Reverse(const char *input, idx_t size, char *output) {
	if (FirstNonAscii(input, size) == size) {
		ReverseBytes(input, 0, size, size, output);
		return;
	}
	GraphemeSegments::ForEach(
	    input, size, [&](idx_t begin, idx_t end) { ReverseSimple(input, begin, end, size, output); },
	    [&](idx_t begin, idx_t end) { ReverseClusters(input, begin, end, size, output); });
}

} // namespace duckdb
