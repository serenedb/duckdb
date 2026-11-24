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

#include "catch.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <string>
#include <string_view>

TEST_CASE("A string_t views its bytes as characters and as bytes", "[string]") {
	for (const std::string bytes : {"", "inlined", "longer than the twelve inlined bytes"}) {
		const duckdb::string_t value(bytes.data(), static_cast<uint32_t>(bytes.size()));
		REQUIRE(value.AsView() == bytes);
		REQUIRE(value.AsView().data() == value.GetData());
		const std::basic_string_view<duckdb::data_t, duckdb::CharTraitsU8> expected(
		    duckdb::const_data_ptr_cast(bytes.data()), bytes.size());
		REQUIRE(value.AsBytesView() == expected);
		REQUIRE(value.AsBytesView().data() == duckdb::const_data_ptr_cast(value.GetData()));
	}
}
