// Device panel API tests against the simulated and replayed (fixture) backends.
#include "test.hpp"

#include "api.hpp"
#include "device.hpp"

#include "ehs/session.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Fixture {
    panel::SimulatedDevice device;
    panel::ConfigStore config;
    panel::EventLog log;
    panel::PanelContext context{device, config, log, nullptr};
    ehs::Router router;
    Fixture() { panel::register_routes(router, context, nullptr); }

    ehs::Response call(ehs::Method method, std::string target, std::string body = {},
                       std::string content_type = "application/json") {
        ehs::Request r;
        r.method = method;
        r.target = std::move(target);
        const auto question = r.target.find('?');
        r.path = r.target.substr(0, question);
        r.query = question == std::string::npos ? std::string() : r.target.substr(question + 1);
        r.body = std::move(body);
        r.headers.add("Content-Type", std::move(content_type));
        return router.handle(r);
    }
};

std::string drain(ehs::Response& res) {
    std::string out;
    while (auto block = res.producer()) {
        out += *block;
    }
    return out;
}

fs::path temp_path(const std::string& name) {
    return fs::temp_directory_path() / ("ehs-panel-" + std::to_string(::getpid()) + "-" + name);
}

}  // namespace

TEST(panel, config_validation) {
    std::string error;
    const panel::DeviceConfig base;
    auto ok = panel::apply_patch(base, *ehs::Json::parse(R"({"hostname":"radar-7","ip_mode":"static"})"), error);
    REQUIRE(ok.has_value());
    CHECK_EQ(ok->hostname, "radar-7");
    CHECK_EQ(ok->ip_mode, "static");

    const char* bad[] = {
        R"([])",
        R"({"hostname":"-bad"})",
        R"({"hostname":"has space"})",
        R"({"ip_mode":"bootp"})",
        R"({"ip_address":"300.1.1.1"})",
        R"({"netmask":"255.0.255.0"})",
        R"({"netmask":"0.0.0.0"})",
        R"({"gateway":"gw"})",
        R"({"ntp_server":"bad..host"})",
        R"({"telemetry_interval_ms":50})",
        R"({"telemetry_interval_ms":1.5})",
        R"({"telemetry_interval_ms":"1000"})",
        R"({"log_level":"trace"})",
        R"({"unknown":1})",
        R"({"hostname":5})",
    };
    for (const char* text : bad) {
        error.clear();
        CHECK(!panel::apply_patch(base, *ehs::Json::parse(text), error).has_value());
        CHECK(!error.empty());
    }
}

TEST(panel, rest_api_flow) {
    Fixture f;
    CHECK(f.call(ehs::Method::Get, "/api/health").body.find("\"ok\"") != std::string::npos);

    const auto status = ehs::Json::parse(f.call(ehs::Method::Get, "/api/status").body);
    REQUIRE(status.has_value());
    CHECK_EQ(status->find("device")->as_string(), "simulated");
    CHECK(status->find("telemetry")->find("cpu_temp_c")->is_number());

    auto updated = f.call(ehs::Method::Put, "/api/config", R"({"hostname":"node-1","log_level":"debug"})");
    CHECK_EQ(ehs::code(updated.status), 200);
    CHECK_EQ(f.config.get().hostname, "node-1");

    CHECK_EQ(ehs::code(f.call(ehs::Method::Patch, "/api/config", R"({"ip_mode":"x"})").status), 422);
    CHECK_EQ(ehs::code(f.call(ehs::Method::Put, "/api/config", "{oops").status), 400);
    CHECK_EQ(ehs::code(f.call(ehs::Method::Put, "/api/config", "{}", "text/plain").status), 415);

    const auto field = f.call(ehs::Method::Get, "/api/config/hostname");
    CHECK_EQ(field.body, R"({"field":"hostname","value":"node-1"})");
    CHECK_EQ(ehs::code(f.call(ehs::Method::Get, "/api/config/nope").status), 404);

    CHECK_EQ(ehs::code(f.call(ehs::Method::Post, "/api/reboot").status), 202);
    CHECK_EQ(f.device.reboot_requests(), 1U);

    auto logs = f.call(ehs::Method::Get, "/api/logs?limit=1");
    const auto text = drain(logs);
    CHECK(text.find("reboot requested") != std::string::npos);
    CHECK(text.find("configuration updated") == std::string::npos);
}

TEST(panel, config_store_persists_atomically) {
    const auto path = temp_path("config.json");
    fs::remove(path);
    {
        panel::ConfigStore store(path);
        auto cfg = store.get();
        cfg.hostname = "persisted";
        std::string error;
        CHECK(store.update(cfg, error));
    }
    const panel::ConfigStore reloaded(path);
    CHECK_EQ(reloaded.get().hostname, "persisted");
    CHECK(!fs::exists(path.string() + ".tmp"));

    std::ofstream(path) << "{\"hostname\":\"-\"}";
    bool threw = false;
    try {
        const panel::ConfigStore broken(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    fs::remove(path);
}

TEST(panel, simulated_device_is_deterministic) {
    const auto boot = std::chrono::steady_clock::time_point{};
    const panel::SimulatedDevice a(boot);
    const panel::SimulatedDevice b(boot);
    const auto when = boot + std::chrono::seconds(90);
    const auto ta = a.telemetry_at(when);
    const auto tb = b.telemetry_at(when);
    CHECK_EQ(ta.uptime_s, 90.0);
    CHECK_EQ(*ta.cpu_temp_c, *tb.cpu_temp_c);
    CHECK(*ta.cpu_temp_c > 35.0 && *ta.cpu_temp_c < 60.0);
    CHECK(ta.mem_available_kb < ta.mem_total_kb);
}

TEST(panel, linux_backend_reads_fixture_tree) {
    const auto root = temp_path("fs");
    fs::create_directories(root / "proc");
    fs::create_directories(root / "sys/class/thermal/thermal_zone0");
    std::ofstream(root / "proc/uptime") << "12345.67 4000.00\n";
    std::ofstream(root / "proc/loadavg") << "0.42 0.30 0.20 1/100 999\n";
    std::ofstream(root / "proc/meminfo") << "MemTotal:  512000 kB\nMemFree: 1000 kB\nMemAvailable: 256000 kB\n";
    std::ofstream(root / "sys/class/thermal/thermal_zone0/temp") << "51234\n";

    panel::LinuxDevice device(root / "proc", root / "sys");
    const auto t = device.read_telemetry();
    CHECK_EQ(t.uptime_s, 12345.67);
    CHECK_EQ(t.load_1m, 0.42);
    CHECK_EQ(t.mem_total_kb, 512000U);
    CHECK_EQ(t.mem_available_kb, 256000U);
    REQUIRE(t.cpu_temp_c.has_value());
    CHECK_EQ(*t.cpu_temp_c, 51.234);

    panel::LinuxDevice missing(root / "nothing", root / "nothing");
    CHECK(!missing.read_telemetry().cpu_temp_c.has_value());
    fs::remove_all(root);
}

TEST(panel, event_log_is_bounded) {
    panel::EventLog log(3);
    for (int i = 0; i < 10; ++i) {
        log.add("info", "event " + std::to_string(i));
    }
    CHECK_EQ(log.size(), 3U);
    const auto tail = log.tail(2);
    REQUIRE(tail.size() == 2);
    CHECK(tail[0].find("event 8") != std::string::npos);
    CHECK(tail[1].find("event 9") != std::string::npos);
}
