#include "test.hpp"

#include "ehs/auth.hpp"
#include "ehs/router.hpp"

#include <stdexcept>
#include <string>

using ehs::Method;
using ehs::Request;
using ehs::Response;
using ehs::Router;
using ehs::Status;

namespace {

Request make(Method method, std::string target) {
    Request r;
    r.method = method;
    r.target = std::move(target);
    r.path = r.target.substr(0, r.target.find('?'));
    return r;
}

Response ok(std::string body) { return Response::text(Status::Ok, std::move(body)); }

}  // namespace

TEST(router, literal_param_wildcard_precedence) {
    Router router;
    router.get("/*", [](Request& r) { return ok("static:" + std::string(r.param("*"))); });
    router.get("/users/:id", [](Request& r) { return ok("user:" + std::string(r.param("id"))); });
    router.get("/users/me", [](Request&) { return ok("me"); });
    router.get("/", [](Request&) { return ok("root"); });

    auto a = make(Method::Get, "/users/me");
    CHECK_EQ(router.handle(a).body, "me");
    auto b = make(Method::Get, "/users/42?x=1");
    CHECK_EQ(router.handle(b).body, "user:42");
    auto c = make(Method::Get, "/css/site.css");
    CHECK_EQ(router.handle(c).body, "static:css/site.css");
    auto d = make(Method::Get, "/");
    CHECK_EQ(router.handle(d).body, "root");
    auto e = make(Method::Get, "/users/");
    CHECK_EQ(router.handle(e).body, "static:users/");
}

TEST(router, params_are_decoded_per_segment) {
    Router router;
    router.get("/files/:name", [](Request& r) { return ok(std::string(r.param("name"))); });
    auto r = make(Method::Get, "/files/a%2Fb%20c");
    CHECK_EQ(router.handle(r).body, "a/b c");
    auto bad = make(Method::Get, "/files/%G0");
    CHECK_EQ(ehs::code(router.handle(bad).status), 400);
}

TEST(router, not_found_and_method_not_allowed) {
    Router router;
    router.get("/x", [](Request&) { return ok("x"); });
    router.post("/x", [](Request&) { return ok("posted"); });

    auto missing = make(Method::Get, "/y");
    CHECK_EQ(ehs::code(router.handle(missing).status), 404);

    auto wrong = make(Method::Delete, "/x");
    const auto res = router.handle(wrong);
    CHECK_EQ(ehs::code(res.status), 405);
    CHECK_EQ(res.headers.get("Allow").value_or(""), "GET, POST, HEAD, OPTIONS");

    auto options = make(Method::Options, "/x");
    CHECK_EQ(ehs::code(router.handle(options).status), 204);

    auto head = make(Method::Head, "/x");
    CHECK_EQ(router.handle(head).body, "x");
}

TEST(router, middleware_short_circuits) {
    Router router;
    int calls = 0;
    router.use([&calls](Request& r) -> std::optional<Response> {
        ++calls;
        if (r.path == "/blocked") {
            return Response::error(Status::Forbidden);
        }
        return std::nullopt;
    });
    router.get("/*", [](Request&) { return ok("pass"); });
    auto blocked = make(Method::Get, "/blocked");
    CHECK_EQ(ehs::code(router.handle(blocked).status), 403);
    auto open = make(Method::Get, "/open");
    CHECK_EQ(router.handle(open).body, "pass");
    CHECK_EQ(calls, 2);
}

TEST(router, invalid_patterns_throw) {
    Router router;
    const auto h = [](Request&) { return ok(""); };
    bool threw = false;
    try {
        router.get("nope", h);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        router.get("/a/*/b", h);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        router.get("/a/:", h);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(auth, base64_roundtrip) {
    CHECK_EQ(ehs::base64_encode(""), "");
    CHECK_EQ(ehs::base64_encode("f"), "Zg==");
    CHECK_EQ(ehs::base64_encode("fo"), "Zm8=");
    CHECK_EQ(ehs::base64_encode("foo"), "Zm9v");
    CHECK_EQ(ehs::base64_encode("foobar"), "Zm9vYmFy");
    CHECK_EQ(ehs::base64_decode("Zm9vYmE=").value_or("?"), "fooba");
    CHECK_EQ(ehs::base64_decode("Zg==").value_or("?"), "f");
    CHECK(!ehs::base64_decode("Zg=").has_value());
    CHECK(!ehs::base64_decode("Z===").has_value());
    CHECK(!ehs::base64_decode("Zg=a").has_value());
    CHECK(!ehs::base64_decode("Zm9v!A==").has_value());
}

TEST(auth, basic_header_parsing) {
    const auto c = ehs::parse_basic_auth("Basic " + ehs::base64_encode("admin:pa:ss"));
    REQUIRE(c.has_value());
    CHECK_EQ(c->user, "admin");
    CHECK_EQ(c->password, "pa:ss");
    CHECK(!ehs::parse_basic_auth("Bearer abc").has_value());
    CHECK(!ehs::parse_basic_auth("Basic " + ehs::base64_encode("nocolon")).has_value());
    CHECK(ehs::constant_time_equals("abc", "abc"));
    CHECK(!ehs::constant_time_equals("abc", "abd"));
    CHECK(!ehs::constant_time_equals("abc", "ab"));
}

TEST(auth, middleware_protects_routes) {
    const ehs::BasicAuth auth("dev", "admin", "secret");
    Router router;
    router.use(auth.middleware({"/public"}));
    router.get("/*", [](Request&) { return ok("in"); });

    auto anon = make(Method::Get, "/private");
    const auto denied = router.handle(anon);
    CHECK_EQ(ehs::code(denied.status), 401);
    CHECK(denied.headers.get("WWW-Authenticate").value_or("").find("realm=\"dev\"") != std::string::npos);

    auto wrong = make(Method::Get, "/private");
    wrong.headers.add("Authorization", "Basic " + ehs::base64_encode("admin:nope"));
    CHECK_EQ(ehs::code(router.handle(wrong).status), 401);

    auto good = make(Method::Get, "/private");
    good.headers.add("Authorization", "basic " + ehs::base64_encode("admin:secret"));
    CHECK_EQ(router.handle(good).body, "in");

    auto open = make(Method::Get, "/public/x");
    CHECK_EQ(router.handle(open).body, "in");
}
