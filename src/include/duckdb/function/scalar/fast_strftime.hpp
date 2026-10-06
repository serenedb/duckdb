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

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

class FastStrftime {
public:
	bool Compile(const string &format_string);
	void Execute(const Vector &input, Vector &result) const;

private:
	enum class Field : uint8_t { LITERAL, YEAR, MONTH, DAY, HOUR, MINUTE, SECOND };
	struct Op {
		Field field;
		uint32_t offset;
		uint32_t size;
	};

	vector<Op> ops;
	string literals;
	int64_t granularity = 0;
};

} // namespace duckdb
