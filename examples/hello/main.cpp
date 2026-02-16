// ehs-hello: smallest useful server built on the library; also the benchmark target.

#include "ehs/json.hpp"
#include "ehs/router.hpp"
#include "ehs/server.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace {

std::atomic<ehs::Server*> g_server{nullptr};

extern "C" void on_signal(int /*signal*/) {
    if (auto* server = g_server.load()) {
        server->stop();
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 8080;
    if (argc > 1) {
        const std::string_view arg(argv[1]);
        const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), port);
        if (ec != std::errc{} || ptr != arg.data() + arg.size()) {
            static_cast<void>(std::fprintf(stderr, "usage: ehs-hello [PORT]\n"));
            return 2;
        }
    }

    ehs::Router router;
    router.get("/", [](ehs::Request&) { return ehs::Response::text(ehs::Status::Ok, "Hello, world!\n"); });
    router.get("/hello/:name", [](ehs::Request& request) {
        ehs::Json doc = ehs::Json::object();
        doc["greeting"] = "Hello, " + std::string(request.param("name")) + "!";
        return ehs::json_response(doc);
    });
    router.post("/echo", [](ehs::Request& request) {
        auto response = ehs::Response::text(ehs::Status::Ok, request.body);
        response.headers.set("Content-Type",
                             std::string(request.headers.get("Content-Type").value_or("application/octet-stream")));
        return response;
    });

    try {
        ehs::ServerOptions options;
        options.port = port;
        ehs::Server server([&router](ehs::Request& request) { return router.handle(request); }, options);
        server.listen();
        g_server.store(&server);
        static_cast<void>(std::signal(SIGINT, on_signal));
        static_cast<void>(std::signal(SIGTERM, on_signal));
        static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
        static_cast<void>(std::fprintf(stderr, "ehs-hello listening on http://0.0.0.0:%u\n", static_cast<unsigned>(server.port())));
        server.run();
        g_server.store(nullptr);
    } catch (const std::exception& e) {
        static_cast<void>(std::fprintf(stderr, "ehs-hello: %s\n", e.what()));
        return 1;
    }
    return 0;
}
