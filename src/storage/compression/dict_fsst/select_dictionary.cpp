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

#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/storage/compression/dict_fsst/decompression.hpp"

namespace duckdb {
namespace dict_fsst {

void CompressedStringScanState::SelectDictionary(Vector &result, idx_t start, idx_t span, const SelectionVector &sel,
                                                 idx_t sel_count) {
	D_ASSERT(dictionary);
	auto &codes = GetSelVec(start, span);
	if (!dict_sel || dict_sel_size < sel_count) {
		dict_sel_size = MaxValue<idx_t>(sel_count, STANDARD_VECTOR_SIZE);
		dict_sel = make_buffer<SelectionVector>(dict_sel_size);
	}
	auto *out = dict_sel->data();
	for (idx_t i = 0; i < sel_count; i++) {
		out[i] = codes.get_index(sel.get_index(i));
	}
	result.Dictionary(dictionary, *dict_sel, sel_count);
}

} // namespace dict_fsst
} // namespace duckdb
