// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "beamng_fsr/json.hpp"

using namespace bfsr;

TEST(Json, ParsesScalars) {
    CHECK(Json::parse("true").asBool() == true);
    CHECK(Json::parse("false").asBool(true) == false);
    CHECK_NEAR(Json::parse("3.5").asNumber(), 3.5, 1e-9);
    CHECK_NEAR(Json::parse("-12").asNumber(), -12.0, 1e-9);
    CHECK_STR_EQ(Json::parse("\"hi\"").asString(), "hi");
    CHECK(Json::parse("null").isNull());
}

TEST(Json, ParsesNestedObject) {
    const std::string text = R"({"a":{"b":[1,2,{"c":"d"}]},"e":true})";
    std::string err;
    const Json j = Json::parse(text, &err);
    CHECK(err.empty());
    CHECK(j.isObject());
    CHECK_EQ(j["a"]["b"].size(), size_t{3});
    CHECK_NEAR(j["a"]["b"].at(1).asNumber(), 2.0, 1e-9);
    CHECK_STR_EQ(j["a"]["b"].at(2)["c"].asString(), "d");
    CHECK(j["e"].asBool());
}

TEST(Json, MissingKeysAreNullNotCrashes) {
    const Json j = Json::parse(R"({"a":1})");
    CHECK(j["nope"].isNull());
    CHECK(j["nope"]["deeper"].isNull());
    CHECK_EQ(j["nope"].asInt(42), 42);
    CHECK_STR_EQ(j["nope"].asString("fallback"), "fallback");
    // Indexing an object as an array must not be a crash either.
    CHECK(j.at(7).isNull());
}

TEST(Json, RejectsMalformedInput) {
    const char* bad[] = {
        "{",  "[1,2", "{\"a\"}", "{\"a\":}", "tru", "\"unterminated",
        "{\"a\":1,}",  // trailing comma is not accepted
        "[1] extra",
    };
    for (const char* t : bad) {
        std::string err;
        const Json j = Json::parse(t, &err);
        CHECK(j.isNull());
        CHECK(!err.empty());
    }
}

TEST(Json, RoundTripsThroughDump) {
    const std::string text = R"({"n":-1.5,"s":"a\"b\n","arr":[true,null,3],"o":{"k":1}})";
    const Json first = Json::parse(text);
    CHECK(!first.isNull());
    const Json second = Json::parse(first.dump(-1));
    CHECK(!second.isNull());
    CHECK_NEAR(second["n"].asNumber(), -1.5, 1e-9);
    CHECK_STR_EQ(second["s"].asString(), "a\"b\n");
    CHECK_EQ(second["arr"].size(), size_t{3});
    CHECK_NEAR(second["o"]["k"].asNumber(), 1.0, 1e-9);
}

TEST(Json, IntegersDoNotGrowDecimalNoise) {
    Json j = Json::object();
    j.set("count", Json(42));
    CHECK_CONTAINS(j.dump(-1), "\"count\":42");
}

TEST(Json, PreservesInsertionOrder) {
    Json j = Json::object();
    j.set("zebra", Json(1));
    j.set("apple", Json(2));
    j.set("mango", Json(3));
    const std::string dumped = j.dump(-1);
    CHECK(dumped.find("zebra") < dumped.find("apple"));
    CHECK(dumped.find("apple") < dumped.find("mango"));
}

TEST(Json, DecodesUnicodeEscapes) {
    const Json j = Json::parse(R"({"s":"éA"})");
    CHECK_STR_EQ(j["s"].asString(), "\xc3\xa9" "A");
}

TEST(Json, DecodesSurrogatePairs) {
    // U+1F600 encoded as a UTF-16 surrogate pair.
    const Json j = Json::parse(R"({"s":"😀"})");
    CHECK_STR_EQ(j["s"].asString(), "\xf0\x9f\x98\x80");
}

TEST(Json, RejectsDeeplyNestedInputWithoutStackOverflow) {
    std::string deep;
    for (int i = 0; i < 500; ++i) deep += "[";
    std::string err;
    const Json j = Json::parse(deep, &err);
    CHECK(j.isNull());
    CHECK_CONTAINS(err, "nesting too deep");
}

TEST(Json, EscapesControlCharactersOnOutput) {
    Json j = Json::object();
    j.set("k", Json(std::string("a\x01""b")));
    CHECK_CONTAINS(j.dump(-1), "\\u0001");
}
