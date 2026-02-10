/// @file device.hpp
/// @brief Device model for the configuration panel: config, telemetry backends, event log.
#pragma once

#include "ehs/json.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace panel {

/// Persistent device configuration.
struct DeviceConfig {
    std::string hostname{"ehs-device"};
    std::string ip_mode{"dhcp"};  ///< "dhcp" or "static".
    std::string ip_address{"192.168.1.50"};
    std::string netmask{"255.255.255.0"};
    std::string gateway{"192.168.1.1"};
    std::string ntp_server{"pool.ntp.org"};
    int telemetry_interval_ms{1000};
    std::string log_level{"info"};  ///< "debug", "info", "warn" or "error".
};

[[nodiscard]] ehs::Json to_json(const DeviceConfig& config);

/// Applies the members of JSON object @p patch on top of @p base and validates the result.
/// Unknown keys, wrong types and invalid values are rejected with a message in @p error.
[[nodiscard]] std::optional<DeviceConfig> apply_patch(const DeviceConfig& base, const ehs::Json& patch,
                                                      std::string& error);

/// Snapshot of device health.
struct Telemetry {
    double uptime_s{0.0};
    std::optional<double> cpu_temp_c;
    double load_1m{0.0};
    std::uint64_t mem_total_kb{0};
    std::uint64_t mem_available_kb{0};
};

[[nodiscard]] ehs::Json to_json(const Telemetry& telemetry);

/// Hardware abstraction used by the REST API.
class DeviceBackend {
public:
    DeviceBackend() = default;
    virtual ~DeviceBackend() = default;
    DeviceBackend(const DeviceBackend&) = delete;
    DeviceBackend& operator=(const DeviceBackend&) = delete;
    DeviceBackend(DeviceBackend&&) = delete;
    DeviceBackend& operator=(DeviceBackend&&) = delete;

    /// "simulated" or "linux".
    [[nodiscard]] virtual std::string_view kind() const noexcept = 0;
    [[nodiscard]] virtual Telemetry read_telemetry() = 0;
    /// Requests a reboot. Implementations record the request; they never block.
    virtual void request_reboot() = 0;
    [[nodiscard]] virtual std::uint64_t reboot_requests() const noexcept = 0;
};

/// Deterministic synthetic device: smooth temperature/load curves, no hardware access.
class SimulatedDevice final : public DeviceBackend {
public:
    using Clock = std::chrono::steady_clock;

    explicit SimulatedDevice(Clock::time_point boot = Clock::now()) noexcept : boot_(boot) {}

    [[nodiscard]] std::string_view kind() const noexcept override { return "simulated"; }
    [[nodiscard]] Telemetry read_telemetry() override { return telemetry_at(Clock::now()); }
    /// Telemetry for an explicit point in time (used by tests).
    [[nodiscard]] Telemetry telemetry_at(Clock::time_point now) const noexcept;
    void request_reboot() override;
    [[nodiscard]] std::uint64_t reboot_requests() const noexcept override { return reboots_; }

private:
    Clock::time_point boot_;
    std::uint64_t reboots_{0};
};

/// Reads real telemetry from procfs/sysfs. Roots are configurable so tests can replay
/// captured files from a fixture directory.
class LinuxDevice final : public DeviceBackend {
public:
    explicit LinuxDevice(std::filesystem::path proc_root = "/proc", std::filesystem::path sys_root = "/sys");

    [[nodiscard]] std::string_view kind() const noexcept override { return "linux"; }
    [[nodiscard]] Telemetry read_telemetry() override;
    void request_reboot() override { ++reboots_; }
    [[nodiscard]] std::uint64_t reboot_requests() const noexcept override { return reboots_; }

private:
    std::filesystem::path proc_root_;
    std::filesystem::path sys_root_;
    std::uint64_t reboots_{0};
};

/// Holds the active configuration and persists it atomically (write + rename).
class ConfigStore {
public:
    /// Loads @p path if it exists. An empty path keeps the configuration in memory only.
    /// Throws std::runtime_error if the file exists but is invalid.
    explicit ConfigStore(std::filesystem::path path = {});

    [[nodiscard]] const DeviceConfig& get() const noexcept { return config_; }
    /// Replaces the configuration; returns false and sets @p error if persisting fails.
    [[nodiscard]] bool update(const DeviceConfig& config, std::string& error);

private:
    std::filesystem::path path_;
    DeviceConfig config_;
};

/// Bounded in-memory event log.
class EventLog {
public:
    explicit EventLog(std::size_t capacity = 256) : capacity_(capacity) {}

    void add(std::string_view level, std::string_view message);
    /// Most recent @p limit entries, oldest first, as JSON lines.
    [[nodiscard]] std::vector<std::string> tail(std::size_t limit) const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::size_t capacity_;
    std::deque<std::string> entries_;
};

}  // namespace panel
