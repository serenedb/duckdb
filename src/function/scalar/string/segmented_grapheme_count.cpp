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

#include "duckdb/common/grapheme_segments.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "utf8proc_wrapper.hpp"

#include <cstring>

namespace duckdb {

static idx_t CountCrLf(const char *data, idx_t size) {
	idx_t count = 0;
	const char *pos = data;
	const char *end = data + size;
	while (pos + 1 < end) {
		auto hit = static_cast<const char *>(memchr(pos, '\r', static_cast<size_t>(end - pos - 1)));
		if (!hit) {
			break;
		}
		count += hit[1] == '\n';
		pos = hit + 1;
	}
	return count;
}

idx_t SegmentedGraphemeCount(const char *input, idx_t size) {
	idx_t count = 0;
	GraphemeSegments::ForEach(
	    input, size,
	    [&](idx_t begin, idx_t end) {
		    count += Utf8CharacterCount(input + begin, end - begin) - CountCrLf(input + begin, end - begin);
	    },
	    [&](idx_t begin, idx_t end) { count += Utf8Proc::GraphemeCount(input + begin, end - begin); });
	return count;
}

} // namespace duckdb
