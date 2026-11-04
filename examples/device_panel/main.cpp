// ehs-panel: device configuration panel built on the embedded HTTP server library.

#include "api.hpp"
#include "device.hpp"

#include "ehs/auth.hpp"
#include "ehs/router.hpp"
#include "ehs/server.hpp"
#include "ehs/session.hpp"
#include "ehs/static_files.hpp"
#include "ehs/version.hpp"

#include <unistd.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::atomic<ehs::Server*> g_server{nullptr};

extern "C" void on_signal(int /*signal*/) {
    if (auto* server = g_server.load()) {
        server->stop();
    }
}

struct Options {
    std::string bind{"0.0.0.0"};
    std::uint16_t port{8080};
    std::filesystem::path web_root;
    std::filesystem::path config_file;
    std::filesystem::path replay_file;
    std::filesystem::path proc_root{"/proc"};
    std::filesystem::path sys_root{"/sys"};
    std::optional<std::string> credentials;
    bool simulate{false};
    bool quiet{false};
    bool no_web{false};
    std::size_t max_connections{256};
    long idle_timeout_ms{5000};
    long request_timeout_ms{10000};
};

constexpr std::string_view kUsage = R"(Usage: ehs-panel [options]

Serve a device configuration panel (web UI + REST API) over HTTP/1.1.

Options:
  -p, --port N              TCP port (default 8080, 0 = ephemeral)
  -b, --bind ADDR           IPv4/IPv6 address to bind (default 0.0.0.0)
  -w, --web-root DIR        Directory with the web UI (default: bundled web/)
      --no-web              Serve the REST API only
  -c, --config FILE         Persist configuration to FILE (default: in memory)
  -a, --auth USER:PASS      Require HTTP Basic auth (except /api/health)
      --sim                 Use the simulated device instead of /proc and /sys
      --proc-root DIR       procfs root for the Linux backend (default /proc)
      --sys-root DIR        sysfs root for the Linux backend (default /sys)
      --replay FILE         Feed raw HTTP requests from FILE through the server
                            pipeline, print the responses to stdout and exit
      --max-connections N   Concurrent connection limit (default 256)
      --idle-timeout MS     Keep-alive idle timeout (default 5000)
      --request-timeout MS  Time allowed to receive one request (default 10000)
  -q, --quiet               Disable the access log
  -v, --version             Print version and exit
  -h, --help                Print this help and exit
)";

template <typename T>
[[nodiscard]] bool parse_number(std::string_view text, T& out) {
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

[[nodiscard]] std::filesystem::path default_web_root() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const auto installed = exe.parent_path().parent_path() / "share" / "ehs" / "web";
        if (std::filesystem::is_directory(installed, ec)) {
            return installed;
        }
    }
    return EHS_DEFAULT_WEB_ROOT;
}

// Returns an exit code when the program should stop (help, version, error).
[[nodiscard]] std::optional<int> parse_args(int argc, char** argv, Options& options) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        std::string_view v;
        const auto take = [&]() -> bool {
            if (i + 1 >= args.size()) {
                std::cerr << "ehs-panel: missing value for " << arg << '\n';
                return false;
            }
            v = args[++i];
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::cout << "ehs-panel " << ehs::kVersion << '\n';
            return 0;
        }
        if (arg == "--sim") {
            options.simulate = true;
        } else if (arg == "-q" || arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "--no-web") {
            options.no_web = true;
        } else if (arg == "-p" || arg == "--port") {
            if (!take() || !parse_number(v, options.port)) {
                std::cerr << "ehs-panel: invalid port\n";
                return 2;
            }
        } else if (arg == "-b" || arg == "--bind") {
            if (!take()) {
                return 2;
            }
            options.bind = std::string(v);
        } else if (arg == "-w" || arg == "--web-root") {
            if (!take()) {
                return 2;
            }
            options.web_root = std::string(v);
        } else if (arg == "-c" || arg == "--config") {
            if (!take()) {
                return 2;
            }
            options.config_file = std::string(v);
        } else if (arg == "-a" || arg == "--auth") {
            if (!take() || v.find(':') == std::string_view::npos) {
                std::cerr << "ehs-panel: --auth expects USER:PASS\n";
                return 2;
            }
            options.credentials = std::string(v);
        } else if (arg == "--proc-root") {
            if (!take()) {
                return 2;
            }
            options.proc_root = std::string(v);
        } else if (arg == "--sys-root") {
            if (!take()) {
                return 2;
            }
            options.sys_root = std::string(v);
        } else if (arg == "--replay") {
            if (!take()) {
                return 2;
            }
            options.replay_file = std::string(v);
        } else if (arg == "--max-connections") {
            if (!take() || !parse_number(v, options.max_connections) || options.max_connections == 0) {
                std::cerr << "ehs-panel: invalid --max-connections\n";
                return 2;
            }
        } else if (arg == "--idle-timeout") {
            if (!take() || !parse_number(v, options.idle_timeout_ms) || options.idle_timeout_ms <= 0) {
                std::cerr << "ehs-panel: invalid --idle-timeout\n";
                return 2;
            }
        } else if (arg == "--request-timeout") {
            if (!take() || !parse_number(v, options.request_timeout_ms) ||
                options.request_timeout_ms <= 0) {
                std::cerr << "ehs-panel: invalid --request-timeout\n";
                return 2;
            }
        } else {
            std::cerr << "ehs-panel: unknown option " << arg << "\n\n" << kUsage;
            return 2;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string utc_now() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::array<char, 32> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf.data(), n);
}

// Feeds a recorded request stream through a Session exactly like the socket server does.
int run_replay(const ehs::Handler& handler, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "ehs-panel: cannot open " << file << '\n';
        return 1;
    }
    const std::string input((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ehs::SessionOptions options;
    options.send_date = false;
    ehs::Session session(handler, options);
    session.receive(input);
    session.receive_eof();
    while (true) {
        auto out = session.output();
        if (out.empty()) {
            session.pump();
            out = session.output();
            if (out.empty()) {
                break;
            }
        }
        std::cout.write(out.data(), static_cast<std::streamsize>(out.size()));
        session.consume(out.size());
    }
    std::cout.flush();
    return 0;
}

int run(const Options& options) {
    std::unique_ptr<panel::DeviceBackend> device;
    if (options.simulate || !options.replay_file.empty()) {
        device = std::make_unique<panel::SimulatedDevice>();
    } else {
        device = std::make_unique<panel::LinuxDevice>(options.proc_root, options.sys_root);
    }
    panel::ConfigStore config(options.config_file);
    panel::EventLog log;
    log.add("info", std::string("panel started, device backend: ") + std::string(device->kind()));

    std::unique_ptr<ehs::StaticFiles> static_files;
    if (!options.no_web) {
        static_files = std::make_unique<ehs::StaticFiles>(options.web_root.empty() ? default_web_root()
                                                                                     : options.web_root);
    }

    std::unique_ptr<ehs::BasicAuth> auth;
    ehs::Router router;
    if (options.credentials) {
        const auto colon = options.credentials->find(':');
        auth = std::make_unique<ehs::BasicAuth>("ehs-panel", options.credentials->substr(0, colon),
                                                options.credentials->substr(colon + 1));
        router.use(auth->middleware({"/api/health"}));
    }

    ehs::ServerOptions server_options;
    server_options.bind_address = options.bind;
    server_options.port = options.port;
    server_options.max_connections = options.max_connections;
    server_options.idle_timeout = std::chrono::milliseconds(options.idle_timeout_ms);
    server_options.request_timeout = std::chrono::milliseconds(options.request_timeout_ms);
    server_options.session.server_name = "ehs/" + std::string(ehs::kVersion);

    const bool quiet = options.quiet || !options.replay_file.empty();
    const ehs::Handler handler = [&router, quiet](ehs::Request& request) {
        const auto start = std::chrono::steady_clock::now();
        auto response = router.handle(request);
        if (!quiet) {
            const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
            static_cast<void>(std::fprintf(stderr, "%s %s %s %d %.3fms\n", utc_now().c_str(),
                         std::string(ehs::to_string(request.method)).c_str(), request.target.c_str(),
                         ehs::code(response.status), elapsed.count()));
        }
        return response;
    };

    if (!options.replay_file.empty()) {
        panel::PanelContext context{*device, config, log, nullptr};
        panel::register_routes(router, context, static_files.get());
        return run_replay(handler, options.replay_file);
    }

    ehs::Server server(handler, server_options);
    panel::PanelContext context{*device, config, log, &server.stats()};
    panel::register_routes(router, context, static_files.get());
    server.listen();

    g_server.store(&server);
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    static_cast<void>(sigaction(SIGINT, &action, nullptr));
    static_cast<void>(sigaction(SIGTERM, &action, nullptr));
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));

    static_cast<void>(std::fprintf(stderr, "ehs-panel %s listening on http://%s:%u (device: %s%s)\n",
                 std::string(ehs::kVersion).c_str(), options.bind.c_str(), static_cast<unsigned>(server.port()),
                 std::string(device->kind()).c_str(), auth ? ", basic auth" : ""));
    server.run();
    g_server.store(nullptr);
    static_cast<void>(std::fprintf(stderr, "ehs-panel stopped after %llu requests\n",
                 static_cast<unsigned long long>(server.stats().requests_handled.load())));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (const auto exit_code = parse_args(argc, argv, options)) {
        return *exit_code;
    }
    try {
        return run(options);
    } catch (const std::exception& e) {
        std::cerr << "ehs-panel: " << e.what() << '\n';
        return 1;
    }
}
