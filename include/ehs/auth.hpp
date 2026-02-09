/// @file auth.hpp
/// @brief HTTP Basic authentication (RFC 7617) and Base64 helpers.
#pragma once

#include "ehs/http.hpp"
#include "ehs/router.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ehs {

/// Standard Base64 encoding with padding.
[[nodiscard]] std::string base64_encode(std::string_view input);
/// Strict Base64 decoding; rejects bad characters and padding.
[[nodiscard]] std::optional<std::string> base64_decode(std::string_view input);
/// Compares two strings in time that depends only on their lengths.
[[nodiscard]] bool constant_time_equals(std::string_view lhs, std::string_view rhs) noexcept;

/// User name and password extracted from an Authorization header.
struct Credentials {
    std::string user;
    std::string password;
};

/// Parses "Basic <base64(user:password)>".
[[nodiscard]] std::optional<Credentials> parse_basic_auth(std::string_view header_value);

/// Single-user Basic authentication guard.
class BasicAuth {
public:
    BasicAuth(std::string realm, std::string user, std::string password);

    /// True when @p request carries the configured credentials.
    [[nodiscard]] bool authorized(const Request& request) const;
    /// 401 response with a WWW-Authenticate challenge.
    [[nodiscard]] Response challenge() const;
    /// Middleware that protects every path except those starting with one of @p public_prefixes.
    [[nodiscard]] Middleware middleware(std::vector<std::string> public_prefixes = {}) const;

private:
    std::string realm_;
    std::string user_;
    std::string password_;
};

}  // namespace ehs
