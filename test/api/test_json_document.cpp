#include "catch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/json_document.hpp"

#include <limits>

using namespace duckdb;

TEST_CASE("A missing or mismatched JSON value reads as empty", "[api]") {
	const string text = R"({"s":"text","n":1,"t":true,"f":false,"nil":null,"a":[1],"big":18446744073709551616})";
	auto doc = JSONDocument::Parse(text.data(), text.size(), JSONReadFlags::BIGNUM_AS_RAW);
	auto root = doc->GetRoot();

	JSONValue missing;
	REQUIRE(!missing.IsValid());
	REQUIRE(missing.GetType() == JSONValueType::INVALID);
	REQUIRE(!missing.IsNull());
	REQUIRE(!missing.IsArray());
	REQUIRE(!missing.IsInteger());
	REQUIRE(!missing.GetBoolean());
	REQUIRE(missing.GetString().empty());
	REQUIRE(!missing.GetMember("s").IsValid());
	REQUIRE_THROWS_AS(missing.ToString(), SerializationException);

	REQUIRE(!root.GetMember("absent").IsValid());
	REQUIRE(root.GetMember("nil").IsNull());
	REQUIRE(!root.GetMember("s").IsNull());
	REQUIRE(!root.GetMember("s").IsArray());
	REQUIRE(!root.GetMember("s").IsObject());
	REQUIRE(root.GetMember("a").IsArray());
	REQUIRE(root.GetMember("s").GetString() == "text");
	REQUIRE(root.GetMember("big").GetString() == "18446744073709551616");
	REQUIRE(root.GetMember("n").GetString().empty());
	REQUIRE(!root.GetMember("n").GetBoolean());
	REQUIRE(root.GetMember("t").GetBoolean());
	REQUIRE(!root.GetMember("f").GetBoolean());

	idx_t visits = 0;
	root.IterateArray([&](JSONValue) { visits++; });
	root.GetMember("a").IterateObject([&](const string &, JSONValue) { visits++; });
	REQUIRE(visits == 0);
}

TEST_CASE("A JSON writer copies every kind of value of a parsed document", "[api]") {
	const string text = R"({"t":true,"f":false,"u":1,"s":-2,"d":0.5,"str":"x","nil":null,)"
	                    R"("raw":18446744073709551616,"a":[1,{"k":[]}],"o":{}})";
	auto doc = JSONDocument::Parse(text.data(), text.size(), JSONReadFlags::BIGNUM_AS_RAW);

	JSONWriter writer;
	auto root = writer.CreateObject();
	root.Add("copy", writer.CreateCopy(doc->GetRoot()));
	root.Add("missing", writer.CreateCopy(JSONValue()));
	writer.SetRoot(root);
	REQUIRE(writer.ToString() == R"({"copy":)" + text + R"(,"missing":null})");

	JSONWriter rootless;
	REQUIRE_THROWS_AS(rootless.ToString(), SerializationException);
}

TEST_CASE("A JSON string writer refuses NaN, infinity and invalid UTF-8 unless its flags allow them", "[api]") {
	const string invalid = "a\xFF";

	JSONStringWriter strict;
	REQUIRE_THROWS_AS(strict.Double(std::numeric_limits<double>::quiet_NaN()), SerializationException);
	REQUIRE_THROWS_AS(strict.Double(-std::numeric_limits<double>::infinity()), SerializationException);
	REQUIRE_THROWS_AS(strict.String(invalid), SerializationException);
	const string long_invalid = string(70, 'b') + invalid;
	REQUIRE_THROWS_AS(strict.String(long_invalid), SerializationException);
	strict.StartObject();
	REQUIRE_THROWS_AS(strict.Key(invalid), SerializationException);

	JSONStringWriter lenient(JSONWriteFlags::ALLOW_INF_AND_NAN | JSONWriteFlags::ALLOW_INVALID_UNICODE);
	lenient.StartObject();
	lenient.Key(invalid);
	lenient.StartArray();
	lenient.Double(std::numeric_limits<double>::quiet_NaN());
	lenient.Double(-std::numeric_limits<double>::infinity());
	lenient.String(invalid);
	lenient.String(long_invalid);
	lenient.EndArray();
	lenient.EndObject();
	REQUIRE(lenient.ToString() == "{\"a\xFF\":[NaN,-Infinity,\"a\xFF\",\"" + long_invalid + "\"]}");
}
