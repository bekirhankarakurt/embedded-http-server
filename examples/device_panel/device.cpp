#include "device.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace panel {

namespace {

[[nodiscard]] double round2(double value) noexcept { return std::round(value * 100.0) / 100.0; }

[[nodiscard]] bool valid_hostname(std::string_view name) noexcept {
    if (name.empty() || name.size() > 63 || name.front() == '-' || name.back() == '-') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

[[nodiscard]] bool valid_server_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 253) {
        return false;
    }
    while (!name.empty()) {
        const auto dot = name.find('.');
        if (!valid_hostname(name.substr(0, dot))) {
            return false;
        }
        if (dot == std::string_view::npos) {
            break;
        }
        name.remove_prefix(dot + 1);
    }
    return true;
}

[[nodiscard]] std::optional<std::uint32_t> parse_ipv4(const std::string& text) noexcept {
    in_addr addr{};
    if (::inet_pton(AF_INET, text.c_str(), &addr) != 1) {
        return std::nullopt;
    }
    return ntohl(addr.s_addr);
}

[[nodiscard]] bool valid_netmask(const std::string& text) noexcept {
    const auto mask = parse_ipv4(text);
    if (!mask || *mask == 0) {
        return false;
    }
    const std::uint32_t inverted = ~*mask;
    return (inverted & (inverted + 1U)) == 0;
}

[[nodiscard]] std::optional<std::string> read_first_line(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line)) {
        return std::nullopt;
    }
    return line;
}

[[nodiscard]] std::optional<double> read_leading_number(const std::filesystem::path& path) {
    const auto line = read_first_line(path);
    if (!line) {
        return std::nullopt;
    }
    std::istringstream stream(*line);
    double value = 0.0;
    if (!(stream >> value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::array<char, 32> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf.data(), n);
}

}  // namespace

ehs::Json to_json(const DeviceConfig& config) {
    ehs::Json doc = ehs::Json::object();
    doc["hostname"] = config.hostname;
    doc["ip_mode"] = config.ip_mode;
    doc["ip_address"] = config.ip_address;
    doc["netmask"] = config.netmask;
    doc["gateway"] = config.gateway;
    doc["ntp_server"] = config.ntp_server;
    doc["telemetry_interval_ms"] = config.telemetry_interval_ms;
    doc["log_level"] = config.log_level;
    return doc;
}

std::optional<DeviceConfig> apply_patch(const DeviceConfig& base, const ehs::Json& patch, std::string& error) {
    if (!patch.is_object()) {
        error = "configuration must be a JSON object";
        return std::nullopt;
    }
    DeviceConfig out = base;
    for (const auto& [key, value] : patch.as_object()) {
        if (key == "telemetry_interval_ms") {
            if (!value.is_number() || std::trunc(value.as_number()) != value.as_number() ||
                value.as_number() < 100.0 || value.as_number() > 60000.0) {
                error = "telemetry_interval_ms must be an integer in [100, 60000]";
                return std::nullopt;
            }
            out.telemetry_interval_ms = static_cast<int>(value.as_number());
            continue;
        }
        std::string* field = nullptr;
        if (key == "hostname") {
            field = &out.hostname;
        } else if (key == "ip_mode") {
            field = &out.ip_mode;
        } else if (key == "ip_address") {
            field = &out.ip_address;
        } else if (key == "netmask") {
            field = &out.netmask;
        } else if (key == "gateway") {
            field = &out.gateway;
        } else if (key == "ntp_server") {
            field = &out.ntp_server;
        } else if (key == "log_level") {
            field = &out.log_level;
        } else {
            error = "unknown field: " + key;
            return std::nullopt;
        }
        if (!value.is_string()) {
            error = key + " must be a string";
            return std::nullopt;
        }
        *field = value.as_string();
    }

    if (!valid_hostname(out.hostname)) {
        error = "hostname must be 1-63 characters of [A-Za-z0-9-] and not start or end with '-'";
    } else if (out.ip_mode != "dhcp" && out.ip_mode != "static") {
        error = "ip_mode must be \"dhcp\" or \"static\"";
    } else if (!parse_ipv4(out.ip_address)) {
        error = "ip_address must be a dotted IPv4 address";
    } else if (!valid_netmask(out.netmask)) {
        error = "netmask must be a contiguous IPv4 mask";
    } else if (!parse_ipv4(out.gateway)) {
        error = "gateway must be a dotted IPv4 address";
    } else if (!valid_server_name(out.ntp_server) && !parse_ipv4(out.ntp_server)) {
        error = "ntp_server must be a host name or IPv4 address";
    } else if (out.log_level != "debug" && out.log_level != "info" && out.log_level != "warn" &&
               out.log_level != "error") {
        error = "log_level must be one of debug, info, warn, error";
    } else {
        return out;
    }
    return std::nullopt;
}

ehs::Json to_json(const Telemetry& telemetry) {
    ehs::Json doc = ehs::Json::object();
    doc["uptime_s"] = round2(telemetry.uptime_s);
    doc["cpu_temp_c"] = telemetry.cpu_temp_c ? ehs::Json(round2(*telemetry.cpu_temp_c)) : ehs::Json(nullptr);
    doc["load_1m"] = round2(telemetry.load_1m);
    doc["mem_total_kb"] = telemetry.mem_total_kb;
    doc["mem_available_kb"] = telemetry.mem_available_kb;
    return doc;
}

Telemetry SimulatedDevice::telemetry_at(Clock::time_point now) const noexcept {
    const double t = std::chrono::duration<double>(now - boot_).count();
    Telemetry out;
    out.uptime_s = std::max(t, 0.0);
    out.cpu_temp_c = 47.5 + 6.0 * std::sin(t / 30.0) + 1.5 * std::sin(t / 3.7);
    out.load_1m = std::max(0.0, 0.35 + 0.25 * std::sin(t / 11.0));
    out.mem_total_kb = 262144;
    out.mem_available_kb = static_cast<std::uint64_t>(180000.0 + 20000.0 * std::sin(t / 45.0));
    return out;
}

void SimulatedDevice::request_reboot() {
    ++reboots_;
    boot_ = Clock::now();
}

LinuxDevice::LinuxDevice(std::filesystem::path proc_root, std::filesystem::path sys_root)
    : proc_root_(std::move(proc_root)), sys_root_(std::move(sys_root)) {}

Telemetry LinuxDevice::read_telemetry() {
    Telemetry out;
    out.uptime_s = read_leading_number(proc_root_ / "uptime").value_or(0.0);
    out.load_1m = read_leading_number(proc_root_ / "loadavg").value_or(0.0);
    if (const auto millideg = read_leading_number(sys_root_ / "class/thermal/thermal_zone0/temp")) {
        out.cpu_temp_c = *millideg / 1000.0;
    }
    std::ifstream meminfo(proc_root_ / "meminfo");
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemTotal:") {
            out.mem_total_kb = value;
        } else if (key == "MemAvailable:") {
            out.mem_available_kb = value;
        }
    }
    return out;
}

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {
    if (path_.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        return;
    }
    std::ifstream in(path_, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    const auto doc = ehs::Json::parse(text, &error);
    if (!doc) {
        throw std::runtime_error("invalid config file " + path_.string() + ": " + error);
    }
    auto loaded = apply_patch(DeviceConfig{}, *doc, error);
    if (!loaded) {
        throw std::runtime_error("invalid config file " + path_.string() + ": " + error);
    }
    config_ = std::move(*loaded);
}

bool ConfigStore::update(const DeviceConfig& config, std::string& error) {
    if (!path_.empty()) {
        auto temp = path_;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << to_json(config).dump() << '\n';
            out.flush();
            if (!out) {
                error = "cannot write " + temp.string();
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temp, path_, ec);
        if (ec) {
            error = "cannot replace " + path_.string() + ": " + ec.message();
            return false;
        }
    }
    config_ = config;
    return true;
}

void EventLog::add(std::string_view level, std::string_view message) {
    ehs::Json entry = ehs::Json::object();
    entry["ts"] = utc_timestamp();
    entry["level"] = std::string(level);
    entry["msg"] = std::string(message);
    entries_.push_back(entry.dump());
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
}

std::vector<std::string> EventLog::tail(std::size_t limit) const {
    const auto count = std::min(limit, entries_.size());
    return {entries_.end() - static_cast<std::ptrdiff_t>(count), entries_.end()};
}

}  // namespace panel
