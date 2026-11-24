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
#include "duckdb/common/char_traits_u8.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <string>
#include <string_view>

TEST_CASE("CharTraitsU8 orders bytes as unsigned and keeps every byte apart from eof", "[string]") {
	using Traits = duckdb::CharTraitsU8;
	const duckdb::data_t bytes[] = {0x00, 0x7f, 0x80, 0xff};
	REQUIRE(Traits::lt(0x7f, 0x80));
	REQUIRE_FALSE(Traits::lt(0x80, 0x7f));
	REQUIRE(Traits::eq(0xff, 0xff));
	REQUIRE_FALSE(Traits::eq(0x7f, 0x80));
	REQUIRE(Traits::find(bytes, 4, 0x80) == bytes + 2);
	REQUIRE(Traits::find(bytes, 2, 0x80) == nullptr);
	REQUIRE(Traits::find(nullptr, 0, 0x80) == nullptr);

	duckdb::data_t out[4] = {};
	REQUIRE(Traits::copy(out, bytes, 4) == out);
	REQUIRE(Traits::compare(out, bytes, 4) == 0);
	REQUIRE(Traits::move(out + 1, out, 3) == out + 1);
	REQUIRE(Traits::assign(out, 1, 0xff) == out);
	const duckdb::data_t moved[] = {0xff, 0x00, 0x7f, 0x80};
	REQUIRE(Traits::compare(out, moved, 4) == 0);
	duckdb::data_t one = 0;
	Traits::assign(one, 0x80);
	REQUIRE(one == 0x80);
	REQUIRE(Traits::copy(nullptr, nullptr, 0) == nullptr);
	REQUIRE(Traits::move(nullptr, nullptr, 0) == nullptr);
	REQUIRE(Traits::assign(nullptr, 0, 0xff) == nullptr);

	for (const auto byte : bytes) {
		REQUIRE(Traits::to_char_type(Traits::to_int_type(byte)) == byte);
		REQUIRE_FALSE(Traits::eq_int_type(Traits::to_int_type(byte), Traits::eof()));
		REQUIRE(Traits::eq_int_type(Traits::not_eof(Traits::to_int_type(byte)), Traits::to_int_type(byte)));
	}
	REQUIRE_FALSE(Traits::eq_int_type(Traits::not_eof(Traits::eof()), Traits::eof()));
}

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
