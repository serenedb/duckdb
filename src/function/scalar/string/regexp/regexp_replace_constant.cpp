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

#include "duckdb/function/scalar/regexp_replace_constant.hpp"
#include "duckdb/function/scalar/regexp_class_run.hpp"

#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

#include <cstring>

namespace duckdb {

static idx_t Utf8SequenceLength(const char *data, idx_t remaining) {
	auto lead = static_cast<unsigned char>(data[0]);
	idx_t length = lead < 0xC0 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
	return length <= remaining ? length : 1;
}

static bool ReplaceAll(const duckdb_re2::RE2 &re, const string_t &input, const duckdb_re2::StringPiece &rewrite,
                       duckdb_re2::StringPiece *vec, int nvec, std::string &buffer) {
	const duckdb_re2::StringPiece text(input.GetData(), input.GetSize());
	const char *begin = text.data();
	const char *p = begin;
	const char *end = p + text.size();
	const char *last_end = nullptr;
	bool replaced = false;
	while (p <= end) {
		if (!re.Match(text, static_cast<size_t>(p - begin), text.size(), duckdb_re2::RE2::UNANCHORED, vec, nvec)) {
			break;
		}
		if (!replaced) {
			buffer.clear();
		}
		if (p < vec[0].data()) {
			buffer.append(p, static_cast<size_t>(vec[0].data() - p));
		}
		if (vec[0].data() == last_end && vec[0].empty()) {
			if (p < end) {
				auto step = Utf8SequenceLength(p, static_cast<idx_t>(end - p));
				buffer.append(p, step);
				p += step;
			} else {
				p++;
			}
			replaced = true;
			continue;
		}
		re.Rewrite(&buffer, rewrite, vec, nvec);
		p = vec[0].data() + vec[0].size();
		last_end = p;
		replaced = true;
	}
	if (!replaced || last_end == nullptr) {
		return false;
	}
	if (p < end) {
		buffer.append(p, static_cast<size_t>(end - p));
	}
	return true;
}

static bool ReplaceRuns(const RegexpClassRun &class_run, const string_t &input, const duckdb_re2::StringPiece &rewrite,
                        std::string &buffer) {
	auto data = input.GetData();
	auto size = input.GetSize();
	idx_t pos = 0;
	idx_t begin;
	idx_t end;
	idx_t copied = 0;
	bool replaced = false;
	while (class_run.Next(data, size, pos, begin, end)) {
		if (!replaced) {
			buffer.clear();
			replaced = true;
		}
		buffer.append(data + copied, begin - copied);
		buffer.append(rewrite.data(), rewrite.size());
		copied = end;
	}
	if (!replaced) {
		return false;
	}
	buffer.append(data + copied, size - copied);
	return true;
}

void RegexpReplaceConstant::Execute(const Vector &strings, string_t replace, RegexLocalState &lstate, Vector &result,
                                    bool global) {
	const auto &re = lstate.constant_pattern;
	const duckdb_re2::StringPiece rewrite(replace.GetData(), replace.GetSize());
	bool rewrite_checked = false;
	const int nvec = 1 + duckdb_re2::RE2::MaxSubmatch(rewrite);
	StringVector::AddHeapReference(result, strings);
	std::string buffer;
	duckdb_re2::StringPiece vec[10];
	RegexpClassRun class_run;
	const bool use_class_run = global && !memchr(rewrite.data(), '\\', rewrite.size()) && class_run.Init(re);
	UnaryExecutor::Execute<string_t, string_t>(strings, result, [&](string_t input) {
		if (!rewrite_checked) {
			std::string rewrite_error;
			if (!re.CheckRewriteString(rewrite, &rewrite_error)) {
				throw InvalidInputException("Invalid replacement string for regexp_replace: %s", rewrite_error);
			}
			rewrite_checked = true;
		}
		if (use_class_run) {
			if (!ReplaceRuns(class_run, input, rewrite, buffer)) {
				return input;
			}
			return StringVector::AddString(result, buffer);
		}
		if (global) {
			if (!ReplaceAll(re, input, rewrite, vec, nvec, buffer)) {
				return input;
			}
			return StringVector::AddString(result, buffer);
		}
		const duckdb_re2::StringPiece piece(input.GetData(), input.GetSize());
		if (!re.Match(piece, 0, piece.size(), duckdb_re2::RE2::UNANCHORED, vec, nvec)) {
			return input;
		}
		const auto match_begin = static_cast<idx_t>(vec[0].data() - input.GetData());
		const auto match_end = match_begin + vec[0].size();
		buffer.assign(input.GetData(), match_begin);
		re.Rewrite(&buffer, rewrite, vec, nvec);
		buffer.append(input.GetData() + match_end, input.GetSize() - match_end);
		return StringVector::AddString(result, buffer);
	});
}

} // namespace duckdb
