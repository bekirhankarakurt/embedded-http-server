/// @file router.hpp
/// @brief Method + path routing with ":name" parameters, "*" wildcards and middleware.
#pragma once

#include "ehs/http.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ehs {

/// Runs before routing. Returning a response short-circuits the request (e.g. 401).
using Middleware = std::function<std::optional<Response>(Request&)>;

/// Request router.
///
/// Patterns are absolute paths made of literal segments, ":name" parameters and an optional
/// trailing "*" that captures the remaining path into the "*" parameter. Literal routes win
/// over parameter routes, which win over wildcards. Unknown paths yield 404, known paths with
/// a wrong method yield 405 with an "Allow" header. HEAD falls back to GET and OPTIONS is
/// answered automatically.
class Router {
public:
    /// Registers @p handler. Throws std::invalid_argument for malformed patterns.
    Router& add(Method method, std::string_view pattern, Handler handler);
    Router& get(std::string_view pattern, Handler handler) { return add(Method::Get, pattern, std::move(handler)); }
    Router& post(std::string_view pattern, Handler handler) { return add(Method::Post, pattern, std::move(handler)); }
    Router& put(std::string_view pattern, Handler handler) { return add(Method::Put, pattern, std::move(handler)); }
    Router& patch(std::string_view pattern, Handler handler) { return add(Method::Patch, pattern, std::move(handler)); }
    Router& del(std::string_view pattern, Handler handler) { return add(Method::Delete, pattern, std::move(handler)); }
    /// Appends a middleware; middlewares run in registration order.
    Router& use(Middleware middleware);

    /// Routes @p request and returns the handler's response.
    [[nodiscard]] Response handle(Request& request) const;

private:
    struct Segment {
        std::string text;
        bool parameter{false};
    };
    struct Route {
        Method method{Method::Get};
        std::vector<Segment> segments;
        bool wildcard{false};
        int specificity{0};
        Handler handler;
    };

    [[nodiscard]] static bool match(const Route& route, const std::vector<std::string>& segments,
                                    Request* request);

    std::vector<Route> routes_;
    std::vector<Middleware> middlewares_;
};

/// Splits the path part of a raw request-target into percent-decoded segments.
/// Returns std::nullopt when a segment has an invalid escape.
[[nodiscard]] std::optional<std::vector<std::string>> split_path(std::string_view target);

}  // namespace ehs
