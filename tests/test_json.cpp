#include "test.hpp"

#include "ehs/json.hpp"

#include <string>

using ehs::Json;

TEST(json, build_and_dump) {
    Json doc = Json::object();
    doc["name"] = "panel";
    doc["port"] = 8080;
    doc["ratio"] = 0.5;
    doc["on"] = true;
    doc["none"] = nullptr;
    doc["list"].push_back(1);
    doc["list"].push_back("two");
    CHECK_EQ(doc.dump(), R"({"name":"panel","port":8080,"ratio":0.5,"on":true,"none":null,"list":[1,"two"]})");
}

TEST(json, parse_roundtrip) {
    const std::string text = R"({"a":[1,2.25,-3e2],"b":{"c":null,"d":false},"e":"x\"y\\z\n"})";
    const auto doc = Json::parse(text);
    REQUIRE(doc.has_value());
    CHECK(doc->find("a")->is_array());
    CHECK_EQ(doc->find("a")->as_array().size(), 3U);
    CHECK_EQ(doc->find("a")->as_array()[2].as_number(), -300.0);
    CHECK(doc->find("b")->find("c")->is_null());
    CHECK_EQ(doc->find("e")->as_string(), "x\"y\\z\n");
    CHECK_EQ(Json::parse(doc->dump()).value(), *doc);
}

TEST(json, unicode_escapes) {
    const auto doc = Json::parse(R"("\u00e9\u20ac\ud83d\ude00\/")");
    REQUIRE(doc.has_value());
    CHECK_EQ(doc->as_string(), "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80/");
    CHECK_EQ(Json(std::string("\x01")).dump(), "\"\\u0001\"");
}

TEST(json, rejects_invalid) {
    const char* bad[] = {"",        "{",          "[1,]",     "{\"a\" 1}", "01",       "1.",   "-",
                         "tru",     "\"\\x\"",    "\"\x01\"", "[1] 2",     "1e999",    "{1:2}", "\"\\ud800\"",
                         "\"\\udc00\"", "\"abc", "nul"};
    for (const char* text : bad) {
        std::string error;
        CHECK(!Json::parse(text, &error).has_value());
        CHECK(!error.empty());
    }
}

TEST(json, numbers) {
    CHECK_EQ(Json(42).dump(), "42");
    CHECK_EQ(Json(-7).dump(), "-7");
    CHECK_EQ(Json(1.5).dump(), "1.5");
    CHECK_EQ(Json(1e300).dump(), "1e+300");
    CHECK_EQ(Json(0.0 / 1.0).dump(), "0");
    CHECK_EQ(Json::parse("1E2")->as_number(), 100.0);
}

TEST(json, type_errors) {
    const Json n(3);
    bool threw = false;
    try {
        static_cast<void>(n.as_string());
    } catch (const ehs::JsonTypeError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(n.find("x") == nullptr);
    Json s("str");
    threw = false;
    try {
        s["k"] = 1;
    } catch (const ehs::JsonTypeError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(json, response_helper) {
    const auto res = ehs::json_response(Json::object(), ehs::Status::Created);
    CHECK_EQ(ehs::code(res.status), 201);
    CHECK_EQ(res.body, "{}");
    CHECK_EQ(res.headers.get("Content-Type").value_or(""), "application/json");
}
