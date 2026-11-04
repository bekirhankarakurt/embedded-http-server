#include "test.hpp"

#include "ehs/parser.hpp"

#include <string>
#include <string_view>

using ehs::Method;
using ehs::RequestParser;
using ehs::Status;

namespace {

struct Parsed {
    RequestParser parser;
    std::size_t used{0};
};

Parsed parse(std::string_view text, const ehs::Limits& limits = {}) {
    Parsed p{RequestParser(limits), 0};
    p.used = p.parser.feed(text);
    return p;
}

}  // namespace

TEST(parser, simple_get) {
    auto p = parse("GET /index.html?x=1&y=two HTTP/1.1\r\nHost: dev\r\nUser-Agent: t\r\n\r\n");
    REQUIRE(p.parser.complete());
    const auto r = p.parser.take();
    CHECK(r.method == Method::Get);
    CHECK_EQ(r.path, "/index.html");
    CHECK_EQ(r.query, "x=1&y=two");
    CHECK_EQ(r.version_minor, 1);
    CHECK_EQ(r.headers.get("host").value_or(""), "dev");
    CHECK_EQ(r.query_param("y").value_or(""), "two");
    CHECK(!r.query_param("z").has_value());
    CHECK(r.keep_alive());
}

TEST(parser, content_length_body) {
    auto p = parse("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhelloEXTRA");
    REQUIRE(p.parser.complete());
    CHECK_EQ(p.used, std::string_view("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello").size());
    CHECK_EQ(p.parser.take().body, "hello");
}

TEST(parser, chunked_body_with_extensions_and_trailers) {
    auto p = parse(
        "POST /c HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;ext=1\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nX-Trailer: v\r\n\r\n");
    REQUIRE(p.parser.complete());
    CHECK_EQ(p.parser.take().body, "Wikipedia in\r\n\r\nchunks.");
}

TEST(parser, byte_by_byte) {
    const std::string text = "PUT /a%20b HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc";
    RequestParser parser;
    for (const char c : text) {
        CHECK(!parser.complete());
        CHECK_EQ(parser.feed(std::string_view(&c, 1)), 1U);
    }
    REQUIRE(parser.complete());
    const auto r = parser.take();
    CHECK_EQ(r.path, "/a b");
    CHECK_EQ(r.body, "abc");
}

TEST(parser, bare_lf_and_leading_empty_lines) {
    auto p = parse("\r\n\nGET / HTTP/1.0\nHost: x\n\n");
    REQUIRE(p.parser.complete());
    const auto r = p.parser.take();
    CHECK_EQ(r.version_minor, 0);
    CHECK(!r.keep_alive());
}

TEST(parser, http10_keep_alive_token) {
    auto p = parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    REQUIRE(p.parser.complete());
    CHECK(p.parser.take().keep_alive());
}

TEST(parser, connection_close) {
    auto p = parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: foo, close\r\n\r\n");
    REQUIRE(p.parser.complete());
    CHECK(!p.parser.take().keep_alive());
}

TEST(parser, pipelined_requests_stop_at_boundary) {
    const std::string first = "GET /1 HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string both = first + "GET /2 HTTP/1.1\r\nHost: x\r\n\r\n";
    RequestParser parser;
    const auto used = parser.feed(both);
    CHECK_EQ(used, first.size());
    REQUIRE(parser.complete());
    CHECK_EQ(parser.take().path, "/1");
    parser.feed(std::string_view(both).substr(used));
    REQUIRE(parser.complete());
    CHECK_EQ(parser.take().path, "/2");
}

TEST(parser, expect_continue) {
    RequestParser parser;
    parser.feed("POST /u HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n");
    CHECK(parser.headers_complete());
    CHECK(parser.expects_continue());
    parser.feed("ok");
    CHECK(parser.complete());
    CHECK(!parser.expects_continue());
}

TEST(parser, errors_map_to_status) {
    struct Case {
        std::string_view text;
        Status status;
    };
    const Case cases[] = {
        {"GET / HTTP/1.1\r\n\r\n", Status::BadRequest},                                  // no Host
        {"GET /  HTTP/1.1\r\nHost: x\r\n\r\n", Status::BadRequest},                     // double space
        {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", Status::HttpVersionNotSupported},
        {"GET / HTTX/1.1\r\nHost: x\r\n\r\n", Status::BadRequest},
        {"BREW / HTTP/1.1\r\nHost: x\r\n\r\n", Status::NotImplemented},
        {"G(T / HTTP/1.1\r\nHost: x\r\n\r\n", Status::BadRequest},
        {"GET relative HTTP/1.1\r\nHost: x\r\n\r\n", Status::BadRequest},
        {"GET /%zz HTTP/1.1\r\nHost: x\r\n\r\n", Status::BadRequest},
        {"GET /%00 HTTP/1.1\r\nHost: x\r\n\r\n", Status::BadRequest},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", Status::BadRequest},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", Status::BadRequest},
        {"GET / HTTP/1.1\r\nNoColon\r\n\r\n", Status::BadRequest},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", Status::BadRequest},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", Status::BadRequest},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n", Status::BadRequest},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
         Status::BadRequest},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", Status::NotImplemented},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", Status::BadRequest},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\r\n", Status::BadRequest},
    };
    for (const auto& c : cases) {
        auto p = parse(c.text);
        CHECK(p.parser.failed());
        CHECK_EQ(ehs::code(p.parser.error()), ehs::code(c.status));
        CHECK(!p.parser.error_detail().empty());
    }
}

TEST(parser, limits) {
    ehs::Limits limits;
    limits.max_request_line = 32;
    limits.max_header_bytes = 64;
    limits.max_headers = 2;
    limits.max_body = 4;

    auto long_line = parse("GET /" + std::string(64, 'a') + " HTTP/1.1\r\n", limits);
    CHECK_EQ(ehs::code(long_line.parser.error()), 414);

    auto unterminated = parse("GET /" + std::string(64, 'a'), limits);
    CHECK_EQ(ehs::code(unterminated.parser.error()), 414);

    auto many = parse("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", limits);
    CHECK_EQ(ehs::code(many.parser.error()), 431);

    auto big_header = parse("GET / HTTP/1.1\r\nA: " + std::string(80, 'x') + "\r\n\r\n", limits);
    CHECK_EQ(ehs::code(big_header.parser.error()), 431);

    auto big_body = parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", limits);
    CHECK_EQ(ehs::code(big_body.parser.error()), 413);

    auto big_chunks = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\n", limits);
    CHECK_EQ(ehs::code(big_chunks.parser.error()), 413);
}

TEST(parser, options_asterisk) {
    auto ok = parse("OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(ok.parser.complete());
    auto bad = parse("GET * HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(bad.parser.failed());
}

TEST(parser, in_progress_tracking) {
    RequestParser parser;
    CHECK(!parser.in_progress());
    parser.feed("\r\n");
    CHECK(!parser.in_progress());
    parser.feed("GE");
    CHECK(parser.in_progress());
    parser.feed("T / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(!parser.in_progress());
    CHECK(parser.complete());
}
