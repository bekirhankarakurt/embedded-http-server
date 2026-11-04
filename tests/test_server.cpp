// Integration tests: real sockets against a Server running on an ephemeral port.
#include "test.hpp"

#include "ehs/router.hpp"
#include "ehs/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

class Client {
public:
    explicit Client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        connected_ = ::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    [[nodiscard]] bool connected() const { return connected_; }
    void send(std::string_view data) const {
        while (!data.empty()) {
            const auto n = ::send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                return;
            }
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    void shutdown_write() const { ::shutdown(fd_.get(), SHUT_WR); }
    // Reads until @p needle appears, the peer closes, or the timeout expires.
    std::string read_until(std::string_view needle, std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (buffer_.find(needle) == std::string::npos && !closed_) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left <= 0ms) {
                break;
            }
            pollfd pfd{fd_.get(), POLLIN, 0};
            if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) {
                break;
            }
            char buf[4096];
            const auto n = ::recv(fd_.get(), buf, sizeof(buf), 0);
            if (n <= 0) {
                closed_ = true;
                break;
            }
            buffer_.append(buf, static_cast<std::size_t>(n));
        }
        auto out = std::move(buffer_);
        buffer_.clear();
        return out;
    }
    [[nodiscard]] bool closed() const { return closed_; }
    bool wait_closed(std::chrono::milliseconds timeout = 2000ms) {
        read_until("\x01never\x02", timeout);
        return closed_;
    }

private:
    ehs::UniqueFd fd_;
    bool connected_{false};
    bool closed_{false};
    std::string buffer_;
};

struct RunningServer {
    ehs::Router router;
    std::unique_ptr<ehs::Server> server;
    std::thread thread;

    explicit RunningServer(ehs::ServerOptions options = {}) {
        router.get("/hello", [](ehs::Request&) { return ehs::Response::text(ehs::Status::Ok, "hi"); });
        router.post("/echo", [](ehs::Request& r) { return ehs::Response::text(ehs::Status::Ok, r.body); });
        router.get("/big", [](ehs::Request&) {
            return ehs::Response::text(ehs::Status::Ok, std::string(4U * 1024U * 1024U, 'x'));
        });
        options.bind_address = "127.0.0.1";
        options.port = 0;
        server = std::make_unique<ehs::Server>([this](ehs::Request& r) { return router.handle(r); }, options);
        server->listen();
        thread = std::thread([this] { server->run(); });
    }
    ~RunningServer() {
        server->stop();
        thread.join();
    }
    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;
    RunningServer(RunningServer&&) = delete;
    RunningServer& operator=(RunningServer&&) = delete;
    [[nodiscard]] std::uint16_t port() const { return server->port(); }
};

}  // namespace

TEST(server, keep_alive_multiple_requests) {
    const RunningServer rs;
    Client c(rs.port());
    REQUIRE(c.connected());
    for (int i = 0; i < 3; ++i) {
        c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        const auto res = c.read_until("hi");
        CHECK(res.find("HTTP/1.1 200 OK") == 0);
        CHECK(res.find("Connection: keep-alive") != std::string::npos);
    }
    CHECK(rs.server->stats().connections_accepted.load() == 1U);
}

TEST(server, pipelined_and_chunked_upload) {
    const RunningServer rs;
    Client c(rs.port());
    REQUIRE(c.connected());
    c.send("POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
           "GET /hello HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    const auto res = c.read_until("hi");
    CHECK(res.find("abc") != std::string::npos);
    CHECK(res.find("abc") < res.find("hi"));
    CHECK(c.wait_closed());
}

TEST(server, large_response_survives_slow_reader) {
    const RunningServer rs;
    Client c(rs.port());
    REQUIRE(c.connected());
    c.send("GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    std::this_thread::sleep_for(100ms);
    std::size_t total = 0;
    for (int i = 0; i < 100 && !c.closed(); ++i) {
        total += c.read_until("\x01never\x02", 100ms).size();
    }
    CHECK(total > 4U * 1024U * 1024U);
}

TEST(server, half_close_still_gets_response) {
    const RunningServer rs;
    Client c(rs.port());
    REQUIRE(c.connected());
    c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
    c.shutdown_write();
    const auto res = c.read_until("hi");
    CHECK(res.find("200 OK") != std::string::npos);
    CHECK(c.wait_closed());
}

TEST(server, request_timeout_sends_408) {
    ehs::ServerOptions options;
    options.request_timeout = 200ms;
    const RunningServer rs(options);
    Client c(rs.port());
    REQUIRE(c.connected());
    c.send("GET /hello HTTP/1.1\r\nHo");
    const auto res = c.read_until("408", 3000ms);
    CHECK(res.find("HTTP/1.1 408 Request Timeout") == 0);
    CHECK(c.wait_closed());
    CHECK(rs.server->stats().timeouts.load() >= 1U);
}

TEST(server, idle_keep_alive_timeout) {
    ehs::ServerOptions options;
    options.idle_timeout = 200ms;
    const RunningServer rs(options);
    Client c(rs.port());
    REQUIRE(c.connected());
    c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK(c.read_until("hi").find("200") != std::string::npos);
    CHECK(c.wait_closed(3000ms));
}

TEST(server, connection_limit_returns_503) {
    ehs::ServerOptions options;
    options.max_connections = 1;
    const RunningServer rs(options);
    Client first(rs.port());
    REQUIRE(first.connected());
    first.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK(first.read_until("hi").find("200") != std::string::npos);
    Client second(rs.port());
    REQUIRE(second.connected());
    CHECK(second.read_until("\r\n\r\n").find("503") != std::string::npos);
    CHECK(rs.server->stats().connections_rejected.load() == 1U);
}

TEST(server, graceful_shutdown_closes_idle_connections) {
    auto rs = std::make_unique<RunningServer>();
    Client idle(rs->port());
    REQUIRE(idle.connected());
    idle.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK(idle.read_until("hi").find("200") != std::string::npos);
    const auto start = std::chrono::steady_clock::now();
    rs.reset();
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    CHECK(idle.wait_closed(500ms));
}

TEST(server, invalid_bind_address_throws) {
    ehs::ServerOptions options;
    options.bind_address = "not-an-ip";
    ehs::Server server([](ehs::Request&) { return ehs::Response{}; }, options);
    bool threw = false;
    try {
        server.listen();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(server, ipv6_loopback) {
    ehs::ServerOptions options;
    options.bind_address = "::1";
    options.port = 0;
    ehs::Server server([](ehs::Request&) { return ehs::Response{}; }, options);
    try {
        server.listen();
        CHECK(server.port() != 0);
    } catch (const std::system_error&) {
        // IPv6 may be disabled in some containers; binding failure is acceptable there.
    }
}
