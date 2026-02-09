#include "ehs/router.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ehs {

namespace {

[[nodiscard]] std::vector<std::string_view> split_raw(std::string_view path) {
    std::vector<std::string_view> parts;
    if (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    while (true) {
        const auto slash = path.find('/');
        parts.push_back(path.substr(0, slash));
        if (slash == std::string_view::npos) {
            break;
        }
        path.remove_prefix(slash + 1);
    }
    return parts;
}

}  // namespace

std::optional<std::vector<std::string>> split_path(std::string_view target) {
    const auto path = target.substr(0, target.find('?'));
    std::vector<std::string> segments;
    for (const auto raw : split_raw(path)) {
        auto decoded = url_decode(raw, false);
        if (!decoded) {
            return std::nullopt;
        }
        segments.push_back(std::move(*decoded));
    }
    return segments;
}

Router& Router::add(Method method, std::string_view pattern, Handler handler) {
    if (pattern.empty() || pattern.front() != '/') {
        throw std::invalid_argument("route pattern must start with '/': " + std::string(pattern));
    }
    if (!handler) {
        throw std::invalid_argument("route handler is empty: " + std::string(pattern));
    }
    Route route;
    route.method = method;
    route.handler = std::move(handler);
    const auto parts = split_raw(pattern);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto part = parts[i];
        if (part == "*") {
            if (i + 1 != parts.size()) {
                throw std::invalid_argument("'*' must be the last segment: " + std::string(pattern));
            }
            route.wildcard = true;
            break;
        }
        Segment segment;
        if (!part.empty() && part.front() == ':') {
            if (part.size() == 1) {
                throw std::invalid_argument("empty parameter name: " + std::string(pattern));
            }
            segment.parameter = true;
            segment.text = std::string(part.substr(1));
            route.specificity += 1;
        } else {
            segment.text = std::string(part);
            route.specificity += 2;
        }
        route.segments.push_back(std::move(segment));
    }
    routes_.push_back(std::move(route));
    std::stable_sort(routes_.begin(), routes_.end(), [](const Route& a, const Route& b) {
        if (a.wildcard != b.wildcard) {
            return !a.wildcard;
        }
        return a.specificity > b.specificity;
    });
    return *this;
}

Router& Router::use(Middleware middleware) {
    middlewares_.push_back(std::move(middleware));
    return *this;
}

bool Router::match(const Route& route, const std::vector<std::string>& segments, Request* request) {
    const auto fixed = route.segments.size();
    if (route.wildcard ? segments.size() < fixed : segments.size() != fixed) {
        return false;
    }
    for (std::size_t i = 0; i < fixed; ++i) {
        const auto& segment = route.segments[i];
        if (!segment.parameter && segment.text != segments[i]) {
            return false;
        }
        if (segment.parameter && segments[i].empty()) {
            return false;
        }
    }
    if (request == nullptr) {
        return true;
    }
    for (std::size_t i = 0; i < fixed; ++i) {
        if (route.segments[i].parameter) {
            request->params[route.segments[i].text] = segments[i];
        }
    }
    if (route.wildcard) {
        std::string rest;
        for (std::size_t i = fixed; i < segments.size(); ++i) {
            if (i > fixed) {
                rest.push_back('/');
            }
            rest += segments[i];
        }
        request->params["*"] = std::move(rest);
    }
    return true;
}

Response Router::handle(Request& request) const {
    for (const auto& middleware : middlewares_) {
        if (auto early = middleware(request)) {
            return std::move(*early);
        }
    }
    const auto segments = split_path(request.target);
    if (!segments) {
        return Response::error(Status::BadRequest, "invalid path encoding");
    }

    const Method lookup = request.method == Method::Head ? Method::Get : request.method;
    std::vector<Method> allowed;
    for (const auto& route : routes_) {
        if (!match(route, *segments, nullptr)) {
            continue;
        }
        if (route.method == lookup) {
            static_cast<void>(match(route, *segments, &request));
            return route.handler(request);
        }
        if (std::find(allowed.begin(), allowed.end(), route.method) == allowed.end()) {
            allowed.push_back(route.method);
        }
    }
    if (allowed.empty()) {
        return Response::error(Status::NotFound);
    }
    if (std::find(allowed.begin(), allowed.end(), Method::Get) != allowed.end()) {
        allowed.push_back(Method::Head);
    }
    allowed.push_back(Method::Options);
    std::string allow;
    for (const auto method : allowed) {
        if (!allow.empty()) {
            allow += ", ";
        }
        allow += to_string(method);
    }
    Response response = request.method == Method::Options
                            ? Response::text(Status::NoContent, "")
                            : Response::error(Status::MethodNotAllowed);
    response.headers.set("Allow", std::move(allow));
    return response;
}

}  // namespace ehs
