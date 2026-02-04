/// @file parser.hpp
/// @brief Incremental, allocation-bounded HTTP/1.1 request parser.
#pragma once

#include "ehs/http.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ehs {

/// Hard limits enforced while parsing. Exceeding one fails the request with a 4xx status.
struct Limits {
    std::size_t max_request_line{8192};   ///< 414 when exceeded.
    std::size_t max_header_bytes{16384};  ///< Sum of header and trailer lines; 431 when exceeded.
    std::size_t max_headers{64};          ///< Number of header fields; 431 when exceeded.
    std::size_t max_body{1U << 20U};      ///< Decoded body size; 413 when exceeded.
};

/// Push parser: feed arbitrary byte fragments, then take() the request once complete().
///
/// The parser never buffers more than one line plus the body, rejects ambiguous framing
/// (Content-Length together with Transfer-Encoding, conflicting Content-Length values,
/// obsolete line folding) and accepts bare LF line endings for robustness.
class RequestParser {
public:
    explicit RequestParser(Limits limits = {}) noexcept : limits_(limits) {}

    /// Consumes bytes from @p data and returns how many were used. Consumption stops at the
    /// end of a complete request (pipelined bytes are left to the caller) or on error.
    std::size_t feed(std::string_view data);

    [[nodiscard]] bool complete() const noexcept { return state_ == State::Complete; }
    [[nodiscard]] bool failed() const noexcept { return state_ == State::Error; }
    /// Status to answer with when failed().
    [[nodiscard]] Status error() const noexcept { return error_; }
    /// Short human-readable reason for the failure.
    [[nodiscard]] std::string_view error_detail() const noexcept { return detail_; }
    /// True once the header section has been parsed.
    [[nodiscard]] bool headers_complete() const noexcept;
    /// True when the client sent "Expect: 100-continue" and the body is still pending.
    [[nodiscard]] bool expects_continue() const noexcept;
    /// True when at least one byte of the current message has been consumed.
    [[nodiscard]] bool in_progress() const noexcept { return started_ && state_ != State::Complete; }

    /// Moves the completed request out and resets the parser for the next message.
    [[nodiscard]] Request take();
    /// Discards any partial state.
    void reset();

private:
    enum class State : std::uint8_t {
        RequestLine,
        Headers,
        Body,
        ChunkSize,
        ChunkData,
        ChunkDataEnd,
        Trailers,
        Complete,
        Error,
    };

    void fail(Status status, const char* detail) noexcept;
    void handle_line(std::string_view line);
    void parse_request_line(std::string_view line);
    void parse_header_line(std::string_view line);
    void finish_headers();
    void parse_chunk_size(std::string_view line);
    [[nodiscard]] std::size_t line_limit() const noexcept;

    Limits limits_;
    State state_{State::RequestLine};
    Status error_{Status::BadRequest};
    const char* detail_{""};
    Request request_;
    std::string line_;
    std::size_t header_bytes_{0};
    std::uint64_t remaining_{0};
    bool started_{false};
    bool expect_continue_{false};
};

}  // namespace ehs
