/// @file http.hpp
/// @brief Core HTTP/1.1 vocabulary types: methods, status codes, headers, requests and responses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ehs {

/// Request methods understood by the server. Anything else is answered with 501.
enum class Method : std::uint8_t { Get, Head, Post, Put, Delete, Patch, Options };

/// Parses an upper-case method token ("GET", "POST", ...).
[[nodiscard]] std::optional<Method> parse_method(std::string_view token) noexcept;

/// Returns the canonical token for @p method.
[[nodiscard]] std::string_view to_string(Method method) noexcept;

/// Status codes produced by the library and the example application.
enum class Status : std::uint16_t {
    Continue = 100,
    Ok = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    MovedPermanently = 301,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    PayloadTooLarge = 413,
    UriTooLong = 414,
    UnsupportedMediaType = 415,
    UnprocessableEntity = 422,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HttpVersionNotSupported = 505,
};

/// Numeric value of a status code.
[[nodiscard]] constexpr int code(Status status) noexcept { return static_cast<int>(status); }

/// Standard reason phrase for @p status ("OK", "Not Found", ...).
[[nodiscard]] std::string_view reason_phrase(Status status) noexcept;

/// ASCII case-insensitive comparison.
[[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept;

/// Returns true when the comma-separated header @p list contains @p token (case-insensitive).
[[nodiscard]] bool has_token(std::string_view list, std::string_view token) noexcept;

/// Decodes %XX escapes. Returns std::nullopt on malformed escapes or embedded NUL bytes.
[[nodiscard]] std::optional<std::string> url_decode(std::string_view input, bool plus_as_space);

/// Single header field.
struct Header {
    std::string name;   ///< Field name as received/sent.
    std::string value;  ///< Field value with surrounding whitespace removed.
};

/// Ordered, case-insensitive header collection.
class Headers {
public:
    using const_iterator = std::vector<Header>::const_iterator;

    /// Appends a field, keeping existing fields with the same name.
    void add(std::string name, std::string value);
    /// Replaces all fields named @p name with a single field.
    void set(std::string_view name, std::string value);
    /// Removes all fields named @p name; returns the number removed.
    std::size_t remove(std::string_view name) noexcept;
    /// Value of the first field named @p name.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept;
    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] const_iterator begin() const noexcept { return items_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return items_.end(); }

private:
    std::vector<Header> items_;
};

/// A fully parsed request.
struct Request {
    Method method{Method::Get};
    std::string target;      ///< Raw request-target, e.g. "/api/x%20y?a=1".
    std::string path;        ///< Percent-decoded path component.
    std::string query;       ///< Raw query string without '?'.
    int version_minor{1};    ///< 0 for HTTP/1.0, 1 for HTTP/1.1.
    Headers headers;
    std::string body;        ///< De-chunked body.
    std::unordered_map<std::string, std::string> params;  ///< Path parameters filled in by the router.

    /// Decoded value of query parameter @p name, if present.
    [[nodiscard]] std::optional<std::string> query_param(std::string_view name) const;
    /// Path parameter @p name, or an empty view when absent.
    [[nodiscard]] std::string_view param(const std::string& name) const;
    /// Whether the client asked to keep the connection open.
    [[nodiscard]] bool keep_alive() const noexcept;
};

/// Pull-style body generator. Returns the next piece of the body, or std::nullopt at the end.
using BodyProducer = std::function<std::optional<std::string>()>;

/// Response built by handlers. Either @ref body or @ref producer carries the payload.
struct Response {
    Status status{Status::Ok};
    Headers headers;
    std::string body;
    /// Streaming body. Sent chunked unless @ref content_length is set.
    BodyProducer producer;
    /// Declared size of a streamed body (identity encoding).
    std::optional<std::uint64_t> content_length;

    /// Plain response with the given content type.
    [[nodiscard]] static Response text(Status status, std::string body,
                                       std::string_view content_type = "text/plain; charset=utf-8");
    /// JSON error document: {"error":"<reason>","status":<code>} plus optional detail.
    [[nodiscard]] static Response error(Status status, std::string_view detail = {});
    /// Streaming response. Uses chunked transfer encoding.
    [[nodiscard]] static Response stream(BodyProducer producer, std::string_view content_type);
};

/// Request handler signature used by the session, router and server.
using Handler = std::function<Response(Request&)>;

}  // namespace ehs
