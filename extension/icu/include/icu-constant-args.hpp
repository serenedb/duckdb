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

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"

#include <string_view>

namespace duckdb {

struct ICUConstantArgs {
	static bool IsConstant(const Vector &arg) {
		return arg.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(arg);
	}

	template <class T>
	static bool TryGet(const Vector &arg, T &value) {
		if (!IsConstant(arg)) {
			return false;
		}
		value = *ConstantVector::GetData<T>(arg);
		return true;
	}

	static bool TryGetString(const Vector &arg, string &value) {
		std::string_view view;
		if (!TryGetView(arg, view)) {
			return false;
		}
		value = string(view);
		return true;
	}

	static bool TryGetView(const Vector &arg, std::string_view &value) {
		if (!IsConstant(arg)) {
			return false;
		}
		const auto &data = *ConstantVector::GetData<string_t>(arg);
		value = std::string_view(data.GetData(), data.GetSize());
		return true;
	}

	static bool TryGetPart(const Vector &arg, DatePartSpecifier &part) {
		string name;
		return TryGetString(arg, name) && TryGetDatePartSpecifier(name, part);
	}
};

} // namespace duckdb
