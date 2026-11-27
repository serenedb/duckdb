#include "catch.hpp"
#include "duckdb/common/json_document.hpp"

#include <cstdlib>
#include <cstring>

using namespace duckdb;

namespace {

constexpr idx_t PAGE = 4096;

struct TwoPages {
	TwoPages() : data(static_cast<char *>(std::aligned_alloc(PAGE, 2 * PAGE))) {
		memset(data, '9', 2 * PAGE);
	}
	~TwoPages() {
		std::free(data);
	}
	char *data;
};

} // namespace

TEST_CASE("A JSON document parses alike wherever it lies in memory and whatever bytes follow it", "[json]") {
	const string documents[] = {"[12,34]", "{\"a\":\"x\\\"y\",\"b\":[true,null]}", "1234", "\"quoted\"", "true",
	                            "null"};
	const string after = "5678\"]}, \x01\xff";
	TwoPages pages;
	JSONTreeParser parser;
	for (auto &document : documents) {
		const idx_t size = document.size();
		for (const idx_t at : {idx_t(100), PAGE - size - 10, PAGE - size}) {
			CAPTURE(document, at);
			char *data = pages.data + at;
			memcpy(data, document.data(), size);
			memcpy(data + size, after.data(), after.size());

			JSONParseError error;
			auto root = parser.Parse(data, size, error);
			REQUIRE(root);
			JSONStringWriter parsed;
			parsed.Value(*root);
			REQUIRE(parsed.ToString() == document);

			JSONStringWriter written;
			REQUIRE(parser.Write(data, size, written, error));
			REQUIRE(written.ToString() == document);
			REQUIRE(parser.IsValid(data, size, error));
		}
	}
}
