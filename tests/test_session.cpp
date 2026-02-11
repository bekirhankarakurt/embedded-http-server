#include "test.hpp"

#include "ehs/session.hpp"

#include <memory>
#include <stdexcept>
#include <string>

using ehs::Request;
using ehs::Response;
using ehs::Session;
using ehs::Status;

namespace {

ehs::SessionOptions quiet_options() {
    ehs::SessionOptions options;
    options.send_date = false;
    options.server_name.clear();
    return options;
}

std::string drain(Session& session) {
    std::string out;
    for (int guard = 0; guard < 100000; ++guard) {
        auto view = session.output();
        if (view.empty()) {
            session.pump();
            view = session.output();
            if (view.empty()) {
                break;
            }
        }
        out += view;
        session.consume(view.size());
    }
    return out;
}

const ehs::Handler echo = [](Request& r) {
    if (r.path == "/throw") {
        throw std::runtime_error("boom");
    }
    return Response::text(Status::Ok, std::string(ehs::to_string(r.method)) + " " + r.path + " " + r.body);
};

}  // namespace

TEST(session, keep_alive_response) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(drain(s),
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 7\r\n"
             "Connection: keep-alive\r\n\r\nGET /a ");
    CHECK(!s.closing());
    CHECK(s.wants_input());
}

TEST(session, pipelining_preserves_order_and_close) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("GET /1 HTTP/1.1\r\nHost: x\r\n\r\nPOST /2 HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi"
              "GET /3 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\nGET /4 HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto out = drain(s);
    const auto p1 = out.find("GET /1");
    const auto p2 = out.find("POST /2 hi");
    const auto p3 = out.find("GET /3");
    CHECK(p1 < p2);
    CHECK(p2 < p3);
    CHECK(p3 != std::string::npos);
    CHECK(out.find("/4") == std::string::npos);
    CHECK(s.done());
    CHECK_EQ(s.requests_handled(), 3U);
}

TEST(session, head_has_length_but_no_body) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("HEAD /h HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto out = drain(s);
    CHECK(out.find("Content-Length: 8\r\n") != std::string::npos);
    CHECK(out.size() >= 4);
    CHECK_EQ(out.substr(out.size() - 4), "\r\n\r\n");
}

TEST(session, chunked_streaming_and_backpressure) {
    auto options = quiet_options();
    options.output_high_water = 64;
    const ehs::Handler handler = [](Request&) {
        auto n = std::make_shared<int>(0);
        return Response::stream(
            [n]() -> std::optional<std::string> {
                if (*n == 50) {
                    return std::nullopt;
                }
                ++*n;
                return std::string(10, 'a');
            },
            "text/plain");
    };
    Session s(handler, options);
    s.receive("GET /s HTTP/1.1\r\nHost: x\r\n\r\nGET /t HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(!s.wants_input());
    CHECK(s.output().size() < 200);
    const auto out = drain(s);
    CHECK(out.find("Transfer-Encoding: chunked") != std::string::npos);
    CHECK(out.find("a\r\naaaaaaaaaa\r\n") != std::string::npos);
    CHECK_EQ(s.requests_handled(), 2U);
    std::size_t terminators = 0;
    for (auto pos = out.find("\r\n0\r\n\r\n"); pos != std::string::npos; pos = out.find("\r\n0\r\n\r\n", pos + 1)) {
        ++terminators;
    }
    CHECK_EQ(terminators, 2U);
}

TEST(session, http10_stream_is_close_delimited) {
    const auto options = quiet_options();
    const ehs::Handler handler = [](Request&) {
        auto done = std::make_shared<bool>(false);
        return Response::stream(
            [done]() -> std::optional<std::string> {
                if (*done) {
                    return std::nullopt;
                }
                *done = true;
                return std::string("raw");
            },
            "text/plain");
    };
    Session s(handler, options);
    s.receive("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    const auto out = drain(s);
    CHECK(out.find("Transfer-Encoding") == std::string::npos);
    CHECK(out.find("Connection: close") != std::string::npos);
    CHECK_EQ(out.substr(out.size() - 3), "raw");
    CHECK(s.done());
}

TEST(session, handler_exception_is_500) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("GET /throw HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto out = drain(s);
    CHECK(out.find("HTTP/1.1 500 Internal Server Error") == 0);
    CHECK(out.find("boom") != std::string::npos);
    CHECK(!s.closing());
}

TEST(session, parse_error_closes) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("GARBAGE\r\n\r\n");
    const auto out = drain(s);
    CHECK(out.find("HTTP/1.1 400 Bad Request") == 0);
    CHECK(out.find("Connection: close") != std::string::npos);
    CHECK(s.done());
    s.receive("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(s.output().empty());
}

TEST(session, expect_continue) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("PUT /u HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n");
    CHECK_EQ(drain(s), "HTTP/1.1 100 Continue\r\n\r\n");
    s.receive("abc");
    CHECK(drain(s).find("PUT /u abc") != std::string::npos);
}

TEST(session, max_requests_per_connection) {
    auto options = quiet_options();
    options.max_requests_per_connection = 2;
    Session s(echo, options);
    s.receive("GET /1 HTTP/1.1\r\nHost: x\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto out = drain(s);
    CHECK(out.find("Connection: keep-alive") != std::string::npos);
    CHECK(out.find("Connection: close") != std::string::npos);
    CHECK(s.done());
}

TEST(session, graceful_finish_and_timeout) {
    const auto options = quiet_options();
    Session idle(echo, options);
    idle.finish_after_current();
    CHECK(idle.done());

    Session partial(echo, options);
    partial.receive("GET /slow HTTP/1.1\r\nHo");
    partial.finish_after_current();
    CHECK(!partial.done());
    partial.receive("st: x\r\n\r\n");
    CHECK(drain(partial).find("Connection: close") != std::string::npos);
    CHECK(partial.done());

    Session slow(echo, options);
    slow.receive("GET / HTTP/1.1\r\n");
    CHECK(slow.request_in_progress());
    slow.fail(Status::RequestTimeout);
    CHECK(drain(slow).find("HTTP/1.1 408 Request Timeout") == 0);
    CHECK(slow.done());
}

TEST(session, eof_after_request_flushes_then_closes) {
    const auto options = quiet_options();
    Session s(echo, options);
    s.receive("GET /e HTTP/1.1\r\nHost: x\r\n\r\n");
    s.receive_eof();
    CHECK(drain(s).find("GET /e") != std::string::npos);
    CHECK(s.done());
}

TEST(session, http_date_format) {
    CHECK_EQ(ehs::http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
}
