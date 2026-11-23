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
#include "duckdb/common/enums/compression_type.hpp"

namespace duckdb {

TEST_CASE("IsSereneDBCompressionType holds exactly the columnstore codec ids", "[compression]") {
	const auto first = static_cast<uint8_t>(CompressionType::COMPRESSION_DICT_LZ4);
	const auto count = static_cast<uint8_t>(CompressionType::COMPRESSION_SERENEDB_COUNT);
	REQUIRE_FALSE(IsSereneDBCompressionType(static_cast<CompressionType>(first - 1)));
	REQUIRE(IsSereneDBCompressionType(CompressionType::COMPRESSION_DICT_LZ4));
	REQUIRE(IsSereneDBCompressionType(static_cast<CompressionType>(count - 1)));
	REQUIRE_FALSE(IsSereneDBCompressionType(CompressionType::COMPRESSION_SERENEDB_COUNT));
}

} // namespace duckdb
