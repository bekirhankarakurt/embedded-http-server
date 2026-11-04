/// @file json.hpp
/// @brief Small, strict JSON value type with parser and serializer (RFC 8259).
#pragma once

#include "ehs/http.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ehs {

/// Thrown when a Json value is accessed as the wrong type.
class JsonTypeError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// JSON document node. Objects keep insertion order.
class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    /// Maximum nesting accepted by parse().
    static constexpr std::size_t kMaxDepth = 64;

    Json() noexcept = default;
    Json(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
    Json(bool value) noexcept : value_(value) {}  // NOLINT(google-explicit-constructor)
    template <typename T,
              std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T value) noexcept : value_(static_cast<double>(value)) {}  // NOLINT(google-explicit-constructor)
    Json(const char* value) : value_(std::string(value)) {}  // NOLINT(google-explicit-constructor)
    Json(std::string value) noexcept : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Json(Array value) noexcept : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Json(Object value) noexcept : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] static Json array() { return Json(Array{}); }
    [[nodiscard]] static Json object() { return Json(Object{}); }

    [[nodiscard]] Type type() const noexcept { return static_cast<Type>(value_.index()); }
    [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type() == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type() == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }

    [[nodiscard]] bool as_bool() const { return get<bool>("bool"); }
    [[nodiscard]] double as_number() const { return get<double>("number"); }
    [[nodiscard]] const std::string& as_string() const { return get<std::string>("string"); }
    [[nodiscard]] const Array& as_array() const { return get<Array>("array"); }
    [[nodiscard]] const Object& as_object() const { return get<Object>("object"); }

    /// Member lookup; nullptr when this is not an object or the key is missing.
    [[nodiscard]] const Json* find(std::string_view key) const noexcept;
    /// Insert-or-access an object member. A null value becomes an empty object.
    Json& operator[](std::string_view key);
    /// Appends to an array. A null value becomes an empty array.
    void push_back(Json value);

    /// Compact serialization.
    [[nodiscard]] std::string dump() const;
    /// Parses @p text; on failure returns std::nullopt and writes a message to @p error.
    [[nodiscard]] static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);

    friend bool operator==(const Json& lhs, const Json& rhs) { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const Json& lhs, const Json& rhs) { return !(lhs == rhs); }

private:
    template <typename T>
    [[nodiscard]] const T& get(const char* expected) const {
        if (const auto* p = std::get_if<T>(&value_)) {
            return *p;
        }
        throw JsonTypeError(std::string("JSON value is not a ") + expected);
    }
    void dump_to(std::string& out) const;

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value_{nullptr};
};

/// Escapes @p text as a JSON string literal (including quotes) and appends it to @p out.
void append_json_string(std::string& out, std::string_view text);

/// Builds an "application/json" response from @p doc.
[[nodiscard]] Response json_response(const Json& doc, Status status = Status::Ok);

}  // namespace ehs
