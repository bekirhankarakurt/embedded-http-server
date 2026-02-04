#include "ehs/parser.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ehs {

namespace {

constexpr std::size_t kMaxChunkSizeLine = 1024;
constexpr std::size_t kMaxChunkDataEndLine = 2;

[[nodiscard]] bool is_token_char(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    constexpr std::string_view kSpecials = "!#$%&'*+-.^_`|~";
    return kSpecials.find(c) != std::string_view::npos;
}

[[nodiscard]] bool is_token(std::string_view s) noexcept {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_token_char);
}

[[nodiscard]] std::string_view trim_ows(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::optional<std::uint64_t> parse_decimal(std::string_view s) noexcept {
    if (s.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> parse_hex(std::string_view s) noexcept {
    if (s.empty() || s.size() > 16) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : s) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
        value = (value << 4U) | digit;
    }
    return value;
}

}  // namespace

bool RequestParser::headers_complete() const noexcept {
    switch (state_) {
        case State::Body:
        case State::ChunkSize:
        case State::ChunkData:
        case State::ChunkDataEnd:
        case State::Trailers:
        case State::Complete: return true;
        case State::RequestLine:
        case State::Headers:
        case State::Error: return false;
    }
    return false;
}

bool RequestParser::expects_continue() const noexcept {
    return expect_continue_ && headers_complete() && state_ != State::Complete;
}

void RequestParser::fail(Status status, const char* detail) noexcept {
    state_ = State::Error;
    error_ = status;
    detail_ = detail;
}

std::size_t RequestParser::line_limit() const noexcept {
    switch (state_) {
        case State::RequestLine: return limits_.max_request_line;
        case State::ChunkSize: return kMaxChunkSizeLine;
        case State::ChunkDataEnd: return kMaxChunkDataEndLine;
        case State::Headers:
        case State::Trailers: return limits_.max_header_bytes;
        case State::Body:
        case State::ChunkData:
        case State::Complete:
        case State::Error: break;
    }
    return 0;
}

std::size_t RequestParser::feed(std::string_view data) {
    std::size_t used = 0;
    while (used < data.size() && state_ != State::Complete && state_ != State::Error) {
        const auto rest = data.substr(used);
        if (state_ == State::Body || state_ == State::ChunkData) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining_, static_cast<std::uint64_t>(rest.size())));
            request_.body.append(rest.substr(0, take));
            remaining_ -= take;
            used += take;
            if (remaining_ == 0) {
                state_ = state_ == State::Body ? State::Complete : State::ChunkDataEnd;
            }
            continue;
        }

        const auto newline = rest.find('\n');
        const std::size_t take = newline == std::string_view::npos ? rest.size() : newline + 1;
        const std::size_t content = newline == std::string_view::npos ? take : newline;
        if (line_.size() + content > line_limit() + 1) {
            if (state_ == State::RequestLine) {
                fail(Status::UriTooLong, "request line too long");
            } else if (state_ == State::Headers || state_ == State::Trailers) {
                fail(Status::RequestHeaderFieldsTooLarge, "header section too large");
            } else {
                fail(Status::BadRequest, "malformed chunk framing");
            }
            return used;
        }
        if (state_ == State::Headers || state_ == State::Trailers) {
            header_bytes_ += take;
            if (header_bytes_ > limits_.max_header_bytes) {
                fail(Status::RequestHeaderFieldsTooLarge, "header section too large");
                return used;
            }
        }
        line_.append(rest.substr(0, take));
        used += take;
        if (newline == std::string_view::npos) {
            if (!line_.empty() && line_ != "\r") {
                started_ = true;
            }
            continue;
        }
        std::string_view line{line_};
        line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (state_ == State::RequestLine && line.empty()) {
            line_.clear();
            continue;
        }
        started_ = true;
        if (line.size() > line_limit()) {
            fail(state_ == State::RequestLine ? Status::UriTooLong : Status::BadRequest,
                 "line too long");
            return used;
        }
        handle_line(line);
        line_.clear();
    }
    return used;
}

void RequestParser::handle_line(std::string_view line) {
    switch (state_) {
        case State::RequestLine: parse_request_line(line); break;
        case State::Headers:
            if (line.empty()) {
                finish_headers();
            } else {
                parse_header_line(line);
            }
            break;
        case State::ChunkSize: parse_chunk_size(line); break;
        case State::ChunkDataEnd:
            if (line.empty()) {
                state_ = State::ChunkSize;
            } else {
                fail(Status::BadRequest, "missing CRLF after chunk data");
            }
            break;
        case State::Trailers:
            if (line.empty()) {
                state_ = State::Complete;
            } else if (line.find(':') == std::string_view::npos) {
                fail(Status::BadRequest, "malformed trailer");
            }
            break;
        case State::Body:
        case State::ChunkData:
        case State::Complete:
        case State::Error: break;
    }
}

void RequestParser::parse_request_line(std::string_view line) {
    const auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        fail(Status::BadRequest, "malformed request line");
        return;
    }
    const auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos) {
        fail(Status::BadRequest, "malformed request line");
        return;
    }
    const auto method_token = line.substr(0, sp1);
    const auto target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto version = line.substr(sp2 + 1);

    if (version == "HTTP/1.1") {
        request_.version_minor = 1;
    } else if (version == "HTTP/1.0") {
        request_.version_minor = 0;
    } else if (version.size() == 8 && version.substr(0, 5) == "HTTP/" && version[6] == '.' &&
               version[5] >= '0' && version[5] <= '9' && version[7] >= '0' && version[7] <= '9') {
        fail(Status::HttpVersionNotSupported, "unsupported HTTP version");
        return;
    } else {
        fail(Status::BadRequest, "malformed HTTP version");
        return;
    }

    const auto method = parse_method(method_token);
    if (!method) {
        fail(is_token(method_token) ? Status::NotImplemented : Status::BadRequest,
             "unsupported method");
        return;
    }
    request_.method = *method;

    const bool target_ok =
        !target.empty() && std::all_of(target.begin(), target.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u > 0x20U && u != 0x7FU;
        });
    if (!target_ok || (target.front() != '/' && !(target == "*" && *method == Method::Options))) {
        fail(Status::BadRequest, "invalid request target");
        return;
    }
    request_.target = std::string(target);
    const auto question = target.find('?');
    auto path = url_decode(target.substr(0, question), false);
    if (!path) {
        fail(Status::BadRequest, "invalid percent-encoding in path");
        return;
    }
    request_.path = std::move(*path);
    if (question != std::string_view::npos) {
        request_.query = std::string(target.substr(question + 1));
    }
    state_ = State::Headers;
}

void RequestParser::parse_header_line(std::string_view line) {
    if (line.front() == ' ' || line.front() == '\t') {
        fail(Status::BadRequest, "obsolete line folding");
        return;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || !is_token(line.substr(0, colon))) {
        fail(Status::BadRequest, "malformed header field");
        return;
    }
    const auto value = trim_ows(line.substr(colon + 1));
    const bool value_ok = std::all_of(value.begin(), value.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u == '\t' || (u >= 0x20U && u != 0x7FU);
    });
    if (!value_ok) {
        fail(Status::BadRequest, "invalid header value");
        return;
    }
    if (request_.headers.size() >= limits_.max_headers) {
        fail(Status::RequestHeaderFieldsTooLarge, "too many header fields");
        return;
    }
    request_.headers.add(std::string(line.substr(0, colon)), std::string(value));
}

void RequestParser::finish_headers() {
    if (request_.version_minor >= 1 && !request_.headers.contains("Host")) {
        fail(Status::BadRequest, "missing Host header");
        return;
    }

    std::optional<std::uint64_t> content_length;
    std::size_t transfer_encodings = 0;
    for (const auto& header : request_.headers) {
        if (iequals(header.name, "Content-Length")) {
            const auto value = parse_decimal(header.value);
            if (!value || (content_length && *content_length != *value)) {
                fail(Status::BadRequest, "invalid Content-Length");
                return;
            }
            content_length = value;
        } else if (iequals(header.name, "Transfer-Encoding")) {
            ++transfer_encodings;
        }
    }

    if (const auto expect = request_.headers.get("Expect")) {
        expect_continue_ = iequals(*expect, "100-continue");
    }

    if (transfer_encodings > 0) {
        if (content_length) {
            fail(Status::BadRequest, "both Content-Length and Transfer-Encoding");
            return;
        }
        const auto te = request_.headers.get("Transfer-Encoding");
        if (transfer_encodings != 1 || !te || !iequals(*te, "chunked")) {
            fail(Status::NotImplemented, "unsupported Transfer-Encoding");
            return;
        }
        state_ = State::ChunkSize;
        return;
    }
    if (content_length && *content_length > 0) {
        if (*content_length > limits_.max_body) {
            fail(Status::PayloadTooLarge, "body exceeds limit");
            return;
        }
        remaining_ = *content_length;
        request_.body.reserve(static_cast<std::size_t>(*content_length));
        state_ = State::Body;
        return;
    }
    state_ = State::Complete;
}

void RequestParser::parse_chunk_size(std::string_view line) {
    const auto semicolon = line.find(';');
    const auto size = parse_hex(trim_ows(line.substr(0, semicolon)));
    if (!size) {
        fail(Status::BadRequest, "invalid chunk size");
        return;
    }
    if (*size == 0) {
        state_ = State::Trailers;
        return;
    }
    if (*size > limits_.max_body - std::min(limits_.max_body, request_.body.size())) {
        fail(Status::PayloadTooLarge, "body exceeds limit");
        return;
    }
    remaining_ = *size;
    state_ = State::ChunkData;
}

Request RequestParser::take() {
    Request out = std::move(request_);
    reset();
    return out;
}

void RequestParser::reset() {
    state_ = State::RequestLine;
    error_ = Status::BadRequest;
    detail_ = "";
    request_ = Request{};
    line_.clear();
    header_bytes_ = 0;
    remaining_ = 0;
    started_ = false;
    expect_continue_ = false;
}

}  // namespace ehs
