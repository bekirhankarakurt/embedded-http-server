#include "ehs/session.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <exception>
#include <utility>

namespace ehs {

namespace {

constexpr std::size_t kCompactThreshold = 4096;

[[nodiscard]] bool status_has_no_body(Status status) noexcept {
    const int c = code(status);
    return (c >= 100 && c < 200) || status == Status::NoContent || status == Status::NotModified;
}

[[nodiscard]] bool is_framing_header(std::string_view name) noexcept {
    return iequals(name, "Content-Length") || iequals(name, "Transfer-Encoding") ||
           iequals(name, "Connection") || iequals(name, "Date") || iequals(name, "Server");
}

void append_uint(std::string& out, std::uint64_t value, int base) {
    std::array<char, 24> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value, base);
    static_cast<void>(ec);
    out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
}

}  // namespace

std::string http_date(std::int64_t time_since_epoch_seconds) {
    const auto t = static_cast<std::time_t>(time_since_epoch_seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::array<char, 40> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buf.data(), n);
}

Session::Session(const Handler& handler, const SessionOptions& options)
    : handler_(&handler), options_(&options), parser_(options.limits) {}

void Session::receive(std::string_view data) {
    if (closing_ || eof_) {
        return;
    }
    in_.append(data);
    process();
}

void Session::receive_eof() {
    eof_ = true;
    process();
}

void Session::pump() {
    fill_from_producer();
    if (!producer_) {
        process();
    }
}

void Session::finish_after_current() {
    finish_requested_ = true;
    if (!request_in_progress() && !has_pending_output()) {
        closing_ = true;
    }
}

std::string_view Session::output() const noexcept {
    return std::string_view{out_}.substr(out_pos_);
}

void Session::consume(std::size_t count) {
    out_pos_ += std::min(count, buffered_output());
    if (out_pos_ == out_.size()) {
        out_.clear();
        out_pos_ = 0;
    } else if (out_pos_ > kCompactThreshold && out_pos_ * 2 > out_.size()) {
        out_.erase(0, out_pos_);
        out_pos_ = 0;
    }
}

bool Session::wants_input() const noexcept {
    return !closing_ && !eof_ && !producer_ && buffered_output() < options_->output_high_water;
}

bool Session::has_pending_output() const noexcept {
    return buffered_output() > 0 || static_cast<bool>(producer_);
}

bool Session::request_in_progress() const noexcept {
    return parser_.in_progress() || in_pos_ < in_.size();
}

void Session::fail(Status status) {
    if (closing_) {
        return;
    }
    in_.clear();
    in_pos_ = 0;
    if (producer_) {
        producer_ = nullptr;
        closing_ = true;
        return;
    }
    Request synthetic;
    auto response = Response::error(status);
    write_response(synthetic, response, false);
    closing_ = true;
}

void Session::process() {
    while (!closing_ && !producer_ && buffered_output() < options_->output_high_water &&
           in_pos_ < in_.size()) {
        const auto used = parser_.feed(std::string_view{in_}.substr(in_pos_));
        in_pos_ += used;
        if (parser_.failed()) {
            const auto detail = parser_.error_detail();
            in_.clear();
            in_pos_ = 0;
            Request synthetic;
            auto response = Response::error(parser_.error(), detail);
            write_response(synthetic, response, false);
            closing_ = true;
            break;
        }
        if (parser_.complete()) {
            auto request = parser_.take();
            continue_sent_ = false;
            dispatch(request);
            continue;
        }
        if (parser_.expects_continue() && !continue_sent_) {
            out_ += "HTTP/1.1 100 Continue\r\n\r\n";
            continue_sent_ = true;
        }
    }
    if (in_pos_ == in_.size()) {
        in_.clear();
        in_pos_ = 0;
    } else if (in_pos_ > kCompactThreshold) {
        in_.erase(0, in_pos_);
        in_pos_ = 0;
    }
    if (eof_ && !producer_ && in_.empty()) {
        closing_ = true;
    }
}

void Session::dispatch(Request& request) {
    ++handled_;
    const bool keep_alive = request.keep_alive() && !finish_requested_ && !eof_ &&
                            handled_ < options_->max_requests_per_connection;
    Response response;
    try {
        response = (*handler_)(request);
    } catch (const std::exception& e) {
        response = Response::error(Status::InternalServerError, e.what());
    } catch (...) {
        response = Response::error(Status::InternalServerError);
    }
    write_response(request, response, keep_alive);
}

void Session::write_head(const Response& response, std::string_view framing, bool keep_alive) {
    out_ += "HTTP/1.1 ";
    append_uint(out_, static_cast<std::uint64_t>(code(response.status)), 10);
    out_.push_back(' ');
    out_ += reason_phrase(response.status);
    out_ += "\r\n";
    if (!options_->server_name.empty()) {
        out_ += "Server: ";
        out_ += options_->server_name;
        out_ += "\r\n";
    }
    if (options_->send_date) {
        out_ += "Date: ";
        out_ += http_date(static_cast<std::int64_t>(std::time(nullptr)));
        out_ += "\r\n";
    }
    for (const auto& header : response.headers) {
        if (is_framing_header(header.name)) {
            continue;
        }
        out_ += header.name;
        out_ += ": ";
        out_ += header.value;
        out_ += "\r\n";
    }
    out_ += framing;
    out_ += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
}

void Session::write_response(const Request& request, Response& response, bool keep_alive) {
    const bool head = request.method == Method::Head;
    const bool no_body = status_has_no_body(response.status);
    std::string framing;

    if (response.producer && !no_body) {
        if (response.content_length) {
            framing = "Content-Length: ";
            append_uint(framing, *response.content_length, 10);
            framing += "\r\n";
        } else if (request.version_minor >= 1) {
            framing = head ? "" : "Transfer-Encoding: chunked\r\n";
        } else {
            keep_alive = false;
        }
        write_head(response, framing, keep_alive);
        if (head) {
            if (!keep_alive) {
                closing_ = true;
            }
            return;
        }
        producer_ = std::move(response.producer);
        producer_chunked_ = !response.content_length && request.version_minor >= 1;
        producer_remaining_ = response.content_length;
        producer_keep_alive_ = keep_alive;
        if (producer_remaining_ && *producer_remaining_ == 0) {
            end_stream();
            return;
        }
        fill_from_producer();
        return;
    }

    if (!no_body) {
        framing = "Content-Length: ";
        append_uint(framing, response.body.size(), 10);
        framing += "\r\n";
    }
    write_head(response, framing, keep_alive);
    if (!head && !no_body) {
        out_ += response.body;
    }
    if (!keep_alive) {
        closing_ = true;
    }
}

void Session::fill_from_producer() {
    while (producer_ && buffered_output() < options_->output_high_water) {
        std::optional<std::string> chunk;
        try {
            chunk = producer_();
        } catch (...) {
            producer_ = nullptr;
            closing_ = true;
            return;
        }
        if (!chunk) {
            end_stream();
            return;
        }
        if (chunk->empty()) {
            return;
        }
        if (producer_chunked_) {
            append_uint(out_, chunk->size(), 16);
            out_ += "\r\n";
            out_ += *chunk;
            out_ += "\r\n";
        } else if (producer_remaining_) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(*producer_remaining_, chunk->size()));
            out_.append(*chunk, 0, take);
            *producer_remaining_ -= take;
            if (*producer_remaining_ == 0) {
                end_stream();
                return;
            }
        } else {
            out_ += *chunk;
        }
    }
}

void Session::end_stream() {
    if (producer_chunked_) {
        out_ += "0\r\n\r\n";
    }
    const bool short_body = producer_remaining_ && *producer_remaining_ > 0;
    producer_ = nullptr;
    producer_remaining_.reset();
    if (!producer_keep_alive_ || short_body) {
        closing_ = true;
    }
}

}  // namespace ehs
