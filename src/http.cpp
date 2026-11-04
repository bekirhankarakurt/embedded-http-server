#include "ehs/http.hpp"

#include "ehs/json.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace ehs {

namespace {

constexpr std::array<std::pair<std::string_view, Method>, 7> kMethods{{
    {"GET", Method::Get},
    {"HEAD", Method::Head},
    {"POST", Method::Post},
    {"PUT", Method::Put},
    {"DELETE", Method::Delete},
    {"PATCH", Method::Patch},
    {"OPTIONS", Method::Options},
}};

[[nodiscard]] constexpr char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

std::optional<Method> parse_method(std::string_view token) noexcept {
    for (const auto& [name, method] : kMethods) {
        if (name == token) {
            return method;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Method method) noexcept {
    for (const auto& [name, value] : kMethods) {
        if (value == method) {
            return name;
        }
    }
    return "GET";
}

std::string_view reason_phrase(Status status) noexcept {
    switch (status) {
        case Status::Continue: return "Continue";
        case Status::Ok: return "OK";
        case Status::Created: return "Created";
        case Status::Accepted: return "Accepted";
        case Status::NoContent: return "No Content";
        case Status::MovedPermanently: return "Moved Permanently";
        case Status::NotModified: return "Not Modified";
        case Status::BadRequest: return "Bad Request";
        case Status::Unauthorized: return "Unauthorized";
        case Status::Forbidden: return "Forbidden";
        case Status::NotFound: return "Not Found";
        case Status::MethodNotAllowed: return "Method Not Allowed";
        case Status::RequestTimeout: return "Request Timeout";
        case Status::PayloadTooLarge: return "Payload Too Large";
        case Status::UriTooLong: return "URI Too Long";
        case Status::UnsupportedMediaType: return "Unsupported Media Type";
        case Status::UnprocessableEntity: return "Unprocessable Entity";
        case Status::RequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
        case Status::InternalServerError: return "Internal Server Error";
        case Status::NotImplemented: return "Not Implemented";
        case Status::ServiceUnavailable: return "Service Unavailable";
        case Status::HttpVersionNotSupported: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [](char a, char b) { return to_lower(a) == to_lower(b); });
}

bool has_token(std::string_view list, std::string_view token) noexcept {
    while (!list.empty()) {
        const auto comma = list.find(',');
        const auto item = trim(list.substr(0, comma));
        if (iequals(item, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return false;
}

std::optional<std::string> url_decode(std::string_view input, bool plus_as_space) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size()) {
                return std::nullopt;
            }
            const int hi = hex_value(input[i + 1]);
            const int lo = hex_value(input[i + 2]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            const auto decoded = static_cast<char>(hi * 16 + lo);
            if (decoded == '\0') {
                return std::nullopt;
            }
            out.push_back(decoded);
            i += 2;
        } else if (c == '+' && plus_as_space) {
            out.push_back(' ');
        } else if (c == '\0') {
            return std::nullopt;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void Headers::add(std::string name, std::string value) {
    items_.push_back(Header{std::move(name), std::move(value)});
}

void Headers::set(std::string_view name, std::string value) {
    remove(name);
    items_.push_back(Header{std::string(name), std::move(value)});
}

std::size_t Headers::remove(std::string_view name) noexcept {
    const auto old_size = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [name](const Header& h) { return iequals(h.name, name); }),
                 items_.end());
    return old_size - items_.size();
}

std::optional<std::string_view> Headers::get(std::string_view name) const noexcept {
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [name](const Header& h) { return iequals(h.name, name); });
    if (it == items_.end()) {
        return std::nullopt;
    }
    return std::string_view{it->value};
}

bool Headers::contains(std::string_view name) const noexcept { return get(name).has_value(); }

std::optional<std::string> Request::query_param(std::string_view name) const {
    std::string_view rest{query};
    while (!rest.empty()) {
        const auto amp = rest.find('&');
        const auto pair = rest.substr(0, amp);
        const auto eq = pair.find('=');
        const auto key = url_decode(pair.substr(0, eq), true);
        if (key && *key == name) {
            if (eq == std::string_view::npos) {
                return std::string{};
            }
            return url_decode(pair.substr(eq + 1), true);
        }
        if (amp == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

std::string_view Request::param(const std::string& name) const {
    const auto it = params.find(name);
    return it == params.end() ? std::string_view{} : std::string_view{it->second};
}

bool Request::keep_alive() const noexcept {
    const auto connection = headers.get("Connection");
    if (version_minor >= 1) {
        return !(connection && has_token(*connection, "close"));
    }
    return connection && has_token(*connection, "keep-alive");
}

Response Response::text(Status status, std::string body, std::string_view content_type) {
    Response res;
    res.status = status;
    res.body = std::move(body);
    res.headers.set("Content-Type", std::string(content_type));
    return res;
}

Response Response::error(Status status, std::string_view detail) {
    Json doc = Json::object();
    doc["error"] = Json(std::string(reason_phrase(status)));
    doc["status"] = Json(code(status));
    if (!detail.empty()) {
        doc["detail"] = Json(std::string(detail));
    }
    return Response::text(status, doc.dump(), "application/json");
}

Response Response::stream(BodyProducer producer, std::string_view content_type) {
    Response res;
    res.producer = std::move(producer);
    res.headers.set("Content-Type", std::string(content_type));
    return res;
}

}  // namespace ehs
