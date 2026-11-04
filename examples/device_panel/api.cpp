#include "api.hpp"

#include "ehs/json.hpp"
#include "ehs/version.hpp"

#include <charconv>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace panel {

namespace {

constexpr std::size_t kDefaultLogLimit = 100;

[[nodiscard]] ehs::Response update_config(PanelContext& context, const ehs::Request& request) {
    const auto content_type = request.headers.get("Content-Type");
    if (!content_type || content_type->substr(0, content_type->find(';')) != "application/json") {
        return ehs::Response::error(ehs::Status::UnsupportedMediaType, "expected application/json");
    }
    std::string error;
    const auto doc = ehs::Json::parse(request.body, &error);
    if (!doc) {
        return ehs::Response::error(ehs::Status::BadRequest, error);
    }
    const auto updated = apply_patch(context.config.get(), *doc, error);
    if (!updated) {
        return ehs::Response::error(ehs::Status::UnprocessableEntity, error);
    }
    if (!context.config.update(*updated, error)) {
        context.log.add("error", error);
        return ehs::Response::error(ehs::Status::InternalServerError, error);
    }
    context.log.add("info", "configuration updated: " + doc->dump());
    return ehs::json_response(to_json(context.config.get()));
}

[[nodiscard]] std::size_t parse_limit(const ehs::Request& request) {
    const auto text = request.query_param("limit");
    if (!text) {
        return kDefaultLogLimit;
    }
    std::size_t value = 0;
    const auto* end = text->data() + text->size();
    const auto [ptr, ec] = std::from_chars(text->data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return kDefaultLogLimit;
    }
    return value;
}

}  // namespace

void register_routes(ehs::Router& router, PanelContext& context, const ehs::StaticFiles* static_files) {
    router.get("/api/health", [](ehs::Request&) {
        ehs::Json doc = ehs::Json::object();
        doc["status"] = "ok";
        doc["version"] = std::string(ehs::kVersion);
        return ehs::json_response(doc);
    });

    router.get("/api/status", [&context](ehs::Request&) {
        ehs::Json doc = ehs::Json::object();
        doc["device"] = std::string(context.device.kind());
        doc["hostname"] = context.config.get().hostname;
        doc["telemetry"] = to_json(context.device.read_telemetry());
        doc["reboot_requests"] = context.device.reboot_requests();
        if (context.stats != nullptr) {
            ehs::Json server = ehs::Json::object();
            server["connections_active"] = context.stats->connections_active.load();
            server["connections_accepted"] = context.stats->connections_accepted.load();
            server["connections_rejected"] = context.stats->connections_rejected.load();
            server["requests_handled"] = context.stats->requests_handled.load();
            server["timeouts"] = context.stats->timeouts.load();
            doc["server"] = std::move(server);
        }
        return ehs::json_response(doc);
    });

    router.get("/api/config", [&context](ehs::Request&) { return ehs::json_response(to_json(context.config.get())); });

    router.get("/api/config/:field", [&context](ehs::Request& request) {
        const auto doc = to_json(context.config.get());
        const auto field = std::string(request.param("field"));
        const auto* value = doc.find(field);
        if (value == nullptr) {
            return ehs::Response::error(ehs::Status::NotFound, "unknown field: " + field);
        }
        ehs::Json out = ehs::Json::object();
        out["field"] = field;
        out["value"] = *value;
        return ehs::json_response(out);
    });

    router.put("/api/config", [&context](ehs::Request& request) { return update_config(context, request); });
    router.patch("/api/config", [&context](ehs::Request& request) { return update_config(context, request); });

    router.post("/api/reboot", [&context](ehs::Request&) {
        context.device.request_reboot();
        context.log.add("warn", "reboot requested via API");
        ehs::Json doc = ehs::Json::object();
        doc["status"] = "rebooting";
        return ehs::json_response(doc, ehs::Status::Accepted);
    });

    router.get("/api/logs", [&context](ehs::Request& request) {
        auto lines = std::make_shared<std::vector<std::string>>(context.log.tail(parse_limit(request)));
        auto index = std::make_shared<std::size_t>(0);
        return ehs::Response::stream(
            [lines, index]() -> std::optional<std::string> {
                if (*index >= lines->size()) {
                    return std::nullopt;
                }
                return (*lines)[(*index)++] + "\n";
            },
            "application/x-ndjson");
    });

    if (static_files != nullptr) {
        router.get("/*", [static_files](ehs::Request& request) {
            return static_files->serve(request, request.param("*"));
        });
    }
}

}  // namespace panel
