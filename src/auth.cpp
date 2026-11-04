#include "ehs/auth.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace ehs {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int decode_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string base64_encode(std::string_view input) {
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= input.size(); i += 3) {
        const auto n = (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16U) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8U) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]));
        out.push_back(kAlphabet[(n >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(n >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(n >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[n & 0x3FU]);
    }
    const auto rest = input.size() - i;
    if (rest > 0) {
        auto n = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16U;
        if (rest == 2) {
            n |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8U;
        }
        out.push_back(kAlphabet[(n >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(n >> 12U) & 0x3FU]);
        out.push_back(rest == 2 ? kAlphabet[(n >> 6U) & 0x3FU] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view input) {
    if (input.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const bool last = i + 4 == input.size();
        std::array<int, 4> v{};
        std::size_t padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = input[i + j];
            if (c == '=' && last && j >= 2) {
                ++padding;
                v.at(j) = 0;
                continue;
            }
            if (padding > 0) {
                return std::nullopt;
            }
            v.at(j) = decode_char(c);
            if (v.at(j) < 0) {
                return std::nullopt;
            }
        }
        const auto n = (static_cast<std::uint32_t>(v[0]) << 18U) | (static_cast<std::uint32_t>(v[1]) << 12U) |
                       (static_cast<std::uint32_t>(v[2]) << 6U) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<char>((n >> 16U) & 0xFFU));
        if (padding < 2) {
            out.push_back(static_cast<char>((n >> 8U) & 0xFFU));
        }
        if (padding < 1) {
            out.push_back(static_cast<char>(n & 0xFFU));
        }
    }
    return out;
}

bool constant_time_equals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned diff = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]));
    }
    return diff == 0;
}

std::optional<Credentials> parse_basic_auth(std::string_view header_value) {
    constexpr std::string_view kScheme = "Basic";
    if (header_value.size() <= kScheme.size() || !iequals(header_value.substr(0, kScheme.size()), kScheme) ||
        header_value[kScheme.size()] != ' ') {
        return std::nullopt;
    }
    auto encoded = header_value.substr(kScheme.size() + 1);
    while (!encoded.empty() && encoded.front() == ' ') {
        encoded.remove_prefix(1);
    }
    const auto decoded = base64_decode(encoded);
    if (!decoded) {
        return std::nullopt;
    }
    const auto colon = decoded->find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    return Credentials{decoded->substr(0, colon), decoded->substr(colon + 1)};
}

BasicAuth::BasicAuth(std::string realm, std::string user, std::string password)
    : realm_(std::move(realm)), user_(std::move(user)), password_(std::move(password)) {}

bool BasicAuth::authorized(const Request& request) const {
    const auto header = request.headers.get("Authorization");
    if (!header) {
        return false;
    }
    const auto credentials = parse_basic_auth(*header);
    if (!credentials) {
        return false;
    }
    const bool user_ok = constant_time_equals(credentials->user, user_);
    const bool password_ok = constant_time_equals(credentials->password, password_);
    return user_ok && password_ok;
}

Response BasicAuth::challenge() const {
    auto response = Response::error(Status::Unauthorized);
    response.headers.set("WWW-Authenticate", "Basic realm=\"" + realm_ + "\", charset=\"UTF-8\"");
    return response;
}

Middleware BasicAuth::middleware(std::vector<std::string> public_prefixes) const {
    return [this, prefixes = std::move(public_prefixes)](Request& request) -> std::optional<Response> {
        for (const auto& prefix : prefixes) {
            if (request.path.compare(0, prefix.size(), prefix) == 0) {
                return std::nullopt;
            }
        }
        if (authorized(request)) {
            return std::nullopt;
        }
        return challenge();
    };
}

}  // namespace ehs
