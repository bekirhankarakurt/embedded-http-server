/// @file session.hpp
/// @brief Transport-independent HTTP/1.1 connection state machine.
///
/// A Session turns inbound bytes into outbound bytes. It owns parsing, keep-alive and
/// pipelining rules, response framing (Content-Length or chunked) and back-pressure, but
/// performs no I/O itself. The epoll server, the replay mode and the unit tests all drive
/// the same Session code.
#pragma once

#include "ehs/http.hpp"
#include "ehs/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ehs {

/// Per-connection behaviour.
struct SessionOptions {
    Limits limits{};
    std::size_t max_requests_per_connection{1000};  ///< Close after this many responses.
    std::size_t output_high_water{64U * 1024U};     ///< Stop producing/reading above this.
    std::string server_name{"ehs"};                 ///< "Server" header value; empty to omit.
    bool send_date{true};                           ///< Emit a "Date" header.
};

/// HTTP/1.1 protocol engine for one connection.
class Session {
public:
    /// @p handler must outlive the session.
    Session(const Handler& handler, const SessionOptions& options);

    /// Appends received bytes and processes as many complete requests as back-pressure allows.
    void receive(std::string_view data);
    /// Refills the output buffer from an active streaming body and resumes pipelined input.
    void pump();
    /// Signals that the peer will send no more data.
    void receive_eof();
    /// Requests "Connection: close" on the next response (graceful shutdown).
    void finish_after_current();

    /// Bytes ready to be written to the peer.
    [[nodiscard]] std::string_view output() const noexcept;
    /// Marks @p count bytes of output() as written.
    void consume(std::size_t count);

    /// Whether more input may be read now (false while streaming or above the high-water mark).
    [[nodiscard]] bool wants_input() const noexcept;
    /// Whether output is pending or a streaming body still has data.
    [[nodiscard]] bool has_pending_output() const noexcept;
    /// True once the connection must be closed (after pending output is flushed).
    [[nodiscard]] bool closing() const noexcept { return closing_; }
    /// True when everything is flushed and the connection should be closed now.
    [[nodiscard]] bool done() const noexcept { return closing_ && !has_pending_output(); }
    /// True when a request has been partially received.
    [[nodiscard]] bool request_in_progress() const noexcept;
    /// Number of responses produced.
    [[nodiscard]] std::uint64_t requests_handled() const noexcept { return handled_; }

    /// Queues a final error response and closes, e.g. on a read timeout.
    void fail(Status status);

private:
    void process();
    void dispatch(Request& request);
    void write_response(const Request& request, Response& response, bool keep_alive);
    void write_head(const Response& response, std::string_view framing, bool keep_alive);
    void fill_from_producer();
    void end_stream();
    [[nodiscard]] std::size_t buffered_output() const noexcept { return out_.size() - out_pos_; }

    const Handler* handler_;
    const SessionOptions* options_;
    RequestParser parser_;
    std::string in_;
    std::size_t in_pos_{0};
    std::string out_;
    std::size_t out_pos_{0};
    BodyProducer producer_;
    bool producer_chunked_{false};
    bool producer_keep_alive_{false};
    std::optional<std::uint64_t> producer_remaining_;
    bool continue_sent_{false};
    bool closing_{false};
    bool finish_requested_{false};
    bool eof_{false};
    std::uint64_t handled_{0};
};

/// Formats @p time_since_epoch_seconds as an IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT").
[[nodiscard]] std::string http_date(std::int64_t time_since_epoch_seconds);

}  // namespace ehs
