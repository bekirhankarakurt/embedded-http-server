/// @file server.hpp
/// @brief Single-threaded, non-blocking epoll HTTP/1.1 server (Linux).
#pragma once

#include "ehs/http.hpp"
#include "ehs/session.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace ehs {

/// Owning wrapper around a POSIX file descriptor.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd();
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int release() noexcept;
    void reset(int fd = -1) noexcept;

private:
    int fd_{-1};
};

/// Server configuration.
struct ServerOptions {
    std::string bind_address{"0.0.0.0"};  ///< IPv4 or IPv6 literal.
    std::uint16_t port{8080};             ///< 0 picks an ephemeral port (see Server::port()).
    int backlog{128};
    std::size_t max_connections{256};                ///< Further clients get 503 and are closed.
    std::chrono::milliseconds request_timeout{10000};  ///< Max time to receive a full request.
    std::chrono::milliseconds idle_timeout{5000};    ///< Keep-alive idle time between requests.
    std::chrono::milliseconds write_timeout{10000};  ///< Max time without write progress.
    std::chrono::milliseconds drain_timeout{3000};   ///< Grace period during shutdown.
    SessionOptions session{};
};

/// Runtime counters, readable from any thread.
struct ServerStats {
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_rejected{0};
    std::atomic<std::uint64_t> connections_active{0};
    std::atomic<std::uint64_t> requests_handled{0};
    std::atomic<std::uint64_t> timeouts{0};
};

/// Event-driven server. All handler calls happen on the thread that calls run().
class Server {
public:
    /// @p handler is invoked for every complete request.
    Server(Handler handler, ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /// Binds and listens. Throws std::system_error on failure.
    void listen();
    /// Bound port (useful when ServerOptions::port is 0). Valid after listen().
    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
    /// Runs the event loop until stop() is called and in-flight responses are drained.
    void run();
    /// Requests a graceful shutdown. Async-signal-safe and thread-safe.
    void stop() noexcept;
    /// Live counters.
    [[nodiscard]] const ServerStats& stats() const noexcept { return stats_; }

private:
    struct Connection;
    using Clock = std::chrono::steady_clock;

    void accept_connections();
    void handle_event(std::uint64_t id, std::uint32_t events);
    static void flush(Connection& connection);
    void update_interest(Connection& connection);
    void close_connection(std::uint64_t id);
    void set_listening(bool enabled);
    void account(Connection& connection);
    void begin_drain();
    void sweep(Clock::time_point now);

    Handler handler_;
    ServerOptions options_;
    UniqueFd epoll_fd_;
    UniqueFd listen_fd_;
    UniqueFd wake_fd_;
    std::uint16_t bound_port_{0};
    std::unordered_map<std::uint64_t, std::unique_ptr<Connection>> connections_;
    std::uint64_t next_id_{2};
    bool listen_paused_{false};
    bool draining_{false};
    Clock::time_point drain_deadline_;
    ServerStats stats_;
};

}  // namespace ehs
