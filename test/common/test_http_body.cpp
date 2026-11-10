#include "catch.hpp"

#include "duckdb/main/http/http_util.hpp"

#include <array>

namespace duckdb {

namespace {

const string HTTP_BODY_BYTES = "abcdefghij";

std::array<HTTPBodyPiece, 4> HTTPBodyPieces() {
	auto data = const_data_ptr_cast(HTTP_BODY_BYTES.data());
	return {{{data, 3}, {data + 3, 0}, {data + 3, 4}, {data + 7, 3}}};
}

string ReadHTTPBody(HTTPBodyReader &reader, idx_t chunk) {
	string result;
	data_t buffer[16];
	while (auto count = reader.Read(buffer, chunk)) {
		result.append(const_char_ptr_cast(buffer), count);
	}
	return result;
}

} // namespace

TEST_CASE("An HTTP body is read across its pieces in order", "[http]") {
	const auto pieces = HTTPBodyPieces();
	for (idx_t chunk = 1; chunk <= 11; chunk++) {
		HTTPBodyReader reader(pieces);
		REQUIRE(ReadHTTPBody(reader, chunk) == HTTP_BODY_BYTES);
	}
}

TEST_CASE("An HTTP body seeks to any position of its pieces", "[http]") {
	const auto pieces = HTTPBodyPieces();
	HTTPBodyReader reader(pieces);
	REQUIRE(reader.Seek(5));
	REQUIRE(ReadHTTPBody(reader, 16) == "fghij");
	REQUIRE(reader.Seek(3));
	REQUIRE(ReadHTTPBody(reader, 2) == "defghij");
	REQUIRE(reader.Seek(0));
	REQUIRE(ReadHTTPBody(reader, 16) == HTTP_BODY_BYTES);
	REQUIRE(reader.Seek(10));
	REQUIRE(ReadHTTPBody(reader, 16).empty());
	REQUIRE_FALSE(reader.Seek(11));
}

TEST_CASE("A PUT body of one piece is sent as a buffer, of several as pieces", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	const HTTPHeaders headers;
	const string url = "http://example.com/object";
	const string content_type = "application/octet-stream";
	const auto pieces = HTTPBodyPieces();

	PutRequestInfo empty(url, headers, params, {}, content_type);
	REQUIRE(empty.buffer_in == nullptr);
	REQUIRE(empty.buffer_in_len == 0);
	REQUIRE(empty.pieces.empty());

	PutRequestInfo whole(url, headers, params, std::span(pieces).subspan(2, 1), content_type);
	REQUIRE(whole.buffer_in == pieces[2].data);
	REQUIRE(whole.buffer_in_len == 4);
	REQUIRE(whole.pieces.empty());

	PutRequestInfo pieced(url, headers, params, pieces, content_type);
	REQUIRE(pieced.buffer_in == nullptr);
	REQUIRE(pieced.buffer_in_len == HTTP_BODY_BYTES.size());
	REQUIRE(pieced.pieces.size() == pieces.size());
}

} // namespace duckdb
