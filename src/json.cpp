#include "ehs/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>
#include <tuple>
#include <utility>

namespace ehs {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] std::optional<Json> run(std::string* error) {
        auto value = parse_value(0);
        if (value) {
            skip_ws();
            if (pos_ != text_.size()) {
                value.reset();
                fail("trailing characters");
            }
        }
        if (!value && error != nullptr) {
            *error = error_ + " at offset " + std::to_string(pos_);
        }
        return value;
    }

private:
    void fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
        }
    }

    void skip_ws() noexcept {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    [[nodiscard]] bool consume_literal(std::string_view literal) noexcept {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<Json> parse_value(std::size_t depth) {
        if (depth > Json::kMaxDepth) {
            fail("nesting too deep");
            return std::nullopt;
        }
        skip_ws();
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return std::nullopt;
        }
        switch (text_[pos_]) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': {
                auto s = parse_string();
                if (!s) {
                    return std::nullopt;
                }
                return Json(std::move(*s));
            }
            case 't':
                if (consume_literal("true")) {
                    return Json(true);
                }
                break;
            case 'f':
                if (consume_literal("false")) {
                    return Json(false);
                }
                break;
            case 'n':
                if (consume_literal("null")) {
                    return Json(nullptr);
                }
                break;
            default: return parse_number();
        }
        fail("invalid literal");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Json> parse_object(std::size_t depth) {
        ++pos_;
        Json::Object members;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return Json(std::move(members));
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("expected object key");
                return std::nullopt;
            }
            auto key = parse_string();
            if (!key) {
                return std::nullopt;
            }
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                fail("expected ':'");
                return std::nullopt;
            }
            ++pos_;
            auto value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            members.emplace_back(std::move(*key), std::move(*value));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return Json(std::move(members));
            }
            fail("expected ',' or '}'");
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<Json> parse_array(std::size_t depth) {
        ++pos_;
        Json::Array items;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return Json(std::move(items));
        }
        while (true) {
            auto value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            items.push_back(std::move(*value));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return Json(std::move(items));
            }
            fail("expected ',' or ']'");
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            fail("truncated \\u escape");
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text_[pos_ + i];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid \\u escape");
                return std::nullopt;
            }
            value = value * 16U + digit;
        }
        pos_ += 4;
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    [[nodiscard]] std::optional<std::string> parse_string() {
        ++pos_;
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail("control character in string");
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                break;
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto cp = parse_hex4();
                    if (!cp) {
                        return std::nullopt;
                    }
                    if (*cp >= 0xD800U && *cp <= 0xDBFFU) {
                        if (!consume_literal("\\u")) {
                            fail("unpaired surrogate");
                            return std::nullopt;
                        }
                        const auto low = parse_hex4();
                        if (!low || *low < 0xDC00U || *low > 0xDFFFU) {
                            fail("invalid surrogate pair");
                            return std::nullopt;
                        }
                        *cp = 0x10000U + ((*cp - 0xD800U) << 10U) + (*low - 0xDC00U);
                    } else if (*cp >= 0xDC00U && *cp <= 0xDFFFU) {
                        fail("unpaired surrogate");
                        return std::nullopt;
                    }
                    append_utf8(out, *cp);
                    break;
                }
                default: fail("invalid escape"); return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] static bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

    [[nodiscard]] std::optional<Json> parse_number() {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
            fail("invalid value");
            return std::nullopt;
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                fail("invalid fraction");
                return std::nullopt;
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                fail("invalid exponent");
                return std::nullopt;
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        double value = 0.0;
        const auto* first = text_.data() + start;
        const auto* last = text_.data() + pos_;
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last || !std::isfinite(value)) {
            fail("number out of range");
            return std::nullopt;
        }
        return Json(value);
    }

    std::string_view text_;
    std::size_t pos_{0};
    std::string error_;
};

void append_number(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    constexpr double kMaxExactInteger = 9007199254740992.0;  // 2^53
    std::array<char, 32> buf{};
    if (std::trunc(value) == value && std::fabs(value) < kMaxExactInteger) {
        const auto [ptr, ec] =
            std::to_chars(buf.data(), buf.data() + buf.size(), static_cast<std::int64_t>(value));
        out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
        static_cast<void>(ec);
        return;
    }
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    static_cast<void>(ec);
    out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
}

}  // namespace

void append_json_string(std::string& out, std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                const auto u = static_cast<unsigned char>(c);
                if (u < 0x20U) {
                    out += "\\u00";
                    out.push_back(kHex[u >> 4U]);
                    out.push_back(kHex[u & 0x0FU]);
                } else {
                    out.push_back(c);
                }
            }
        }
    }
    out.push_back('"');
}

const Json* Json::find(std::string_view key) const noexcept {
    const auto* members = std::get_if<Object>(&value_);
    if (members == nullptr) {
        return nullptr;
    }
    for (const auto& [name, value] : *members) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

Json& Json::operator[](std::string_view key) {
    if (is_null()) {
        value_ = Object{};
    }
    auto* members = std::get_if<Object>(&value_);
    if (members == nullptr) {
        throw JsonTypeError("JSON value is not an object");
    }
    for (auto& [name, value] : *members) {
        if (name == key) {
            return value;
        }
    }
    members->emplace_back(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());
    return members->back().second;
}

void Json::push_back(Json value) {
    if (is_null()) {
        value_ = Array{};
    }
    auto* items = std::get_if<Array>(&value_);
    if (items == nullptr) {
        throw JsonTypeError("JSON value is not an array");
    }
    items->push_back(std::move(value));
}

void Json::dump_to(std::string& out) const {
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(value_) ? "true" : "false"; break;
        case Type::Number: append_number(out, std::get<double>(value_)); break;
        case Type::String: append_json_string(out, std::get<std::string>(value_)); break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : std::get<Array>(value_)) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                item.dump_to(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [name, item] : std::get<Object>(value_)) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                append_json_string(out, name);
                out.push_back(':');
                item.dump_to(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

std::optional<Json> Json::parse(std::string_view text, std::string* error) {
    return Parser(text).run(error);
}

Response json_response(const Json& doc, Status status) {
    return Response::text(status, doc.dump(), "application/json");
}

}  // namespace ehs
