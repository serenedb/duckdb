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

#include "core_functions/scalar/ascii_translate.hpp"

#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

static constexpr int16_t KEEP = -1;
static constexpr int16_t DELETE = -2;

bool AsciiTranslate::TryExecute(DataChunk &args, Vector &result) {
	auto &needle_vector = args.data[1];
	auto &thread_vector = args.data[2];
	if (needle_vector.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(needle_vector) ||
	    thread_vector.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(thread_vector)) {
		return false;
	}
	auto needle = ConstantVector::GetData<string_t>(needle_vector)[0];
	auto thread = ConstantVector::GetData<string_t>(thread_vector)[0];
	if (FirstNonAscii(needle.GetData(), needle.GetSize()) != needle.GetSize() ||
	    FirstNonAscii(thread.GetData(), thread.GetSize()) != thread.GetSize()) {
		return false;
	}
	int16_t table[128];
	for (auto &entry : table) {
		entry = KEEP;
	}
	bool has_delete = false;
	for (idx_t i = 0; i < needle.GetSize(); i++) {
		auto c = static_cast<unsigned char>(needle.GetData()[i]);
		if (table[c] != KEEP) {
			continue;
		}
		if (i < thread.GetSize()) {
			table[c] = static_cast<unsigned char>(thread.GetData()[i]);
		} else {
			table[c] = DELETE;
			has_delete = true;
		}
	}
	unsigned char map[256];
	for (idx_t c = 0; c < 256; c++) {
		map[c] = static_cast<unsigned char>(c);
		if (c < 128 && table[c] >= 0) {
			map[c] = static_cast<unsigned char>(table[c]);
		}
	}
	bool deleted[256] = {false};
	for (idx_t c = 0; c < 128; c++) {
		deleted[c] = table[c] == DELETE;
	}
	vector<char> buffer;
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t input) {
		auto data = reinterpret_cast<const unsigned char *>(input.GetData());
		auto size = input.GetSize();
		if (!has_delete) {
			auto target = StringVector::EmptyString(result, size);
			auto out = reinterpret_cast<unsigned char *>(target.GetDataWriteable());
			for (idx_t i = 0; i < size; i++) {
				out[i] = map[data[i]];
			}
			target.Finalize();
			return target;
		}
		buffer.resize(size);
		idx_t length = 0;
		for (idx_t i = 0; i < size; i++) {
			buffer[length] = static_cast<char>(map[data[i]]);
			length += !deleted[data[i]];
		}
		return StringVector::AddString(result, buffer.data(), length);
	});
	return true;
}

} // namespace duckdb
