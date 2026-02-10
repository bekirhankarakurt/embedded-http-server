/// @file api.hpp
/// @brief REST API of the device configuration panel.
#pragma once

#include "device.hpp"

#include "ehs/router.hpp"
#include "ehs/server.hpp"
#include "ehs/static_files.hpp"

namespace panel {

/// Objects the API operates on. All references must outlive the router.
struct PanelContext {
    DeviceBackend& device;
    ConfigStore& config;
    EventLog& log;
    const ehs::ServerStats* stats{nullptr};  ///< Optional; reported by /api/status.
};

/// Registers the REST endpoints and, when @p static_files is non-null, the web UI.
///
/// | Method      | Path               | Description                          |
/// |-------------|--------------------|--------------------------------------|
/// | GET         | /api/health        | Liveness probe                       |
/// | GET         | /api/status        | Telemetry and server counters        |
/// | GET         | /api/config        | Current configuration                |
/// | GET         | /api/config/:field | One configuration field              |
/// | PUT / PATCH | /api/config        | Validate, persist and apply changes  |
/// | POST        | /api/reboot        | Request a device reboot (202)        |
/// | GET         | /api/logs?limit=N  | Event log as chunked NDJSON          |
void register_routes(ehs::Router& router, PanelContext& context, const ehs::StaticFiles* static_files);

}  // namespace panel
