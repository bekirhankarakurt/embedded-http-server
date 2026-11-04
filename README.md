# embedded-http-server

**Zero-dependency HTTP/1.1 server in modern C++17 for embedded Linux devices: a device config panel and REST API in a ~400 KB binary that uses ~4 MB of RAM.**

[![CI](https://github.com/bekirhankarakurt/embedded-http-server/actions/workflows/ci.yml/badge.svg)](https://github.com/bekirhankarakurt/embedded-http-server/actions/workflows/ci.yml)
[![CodeQL](https://github.com/bekirhankarakurt/embedded-http-server/actions/workflows/codeql.yml/badge.svg)](https://github.com/bekirhankarakurt/embedded-http-server/actions/workflows/codeql.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://bekirhankarakurt.github.io/embedded-http-server/coverage.json)](https://bekirhankarakurt.github.io/embedded-http-server/coverage/)
[![Release](https://img.shields.io/github/v/release/bekirhankarakurt/embedded-http-server)](https://github.com/bekirhankarakurt/embedded-http-server/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64%20%7C%20aarch64-lightgrey.svg)](#requirements)

📖 **[Documentation](https://bekirhankarakurt.github.io/embedded-http-server/)** (API reference) · **[Coverage report](https://bekirhankarakurt.github.io/embedded-http-server/coverage/)**

![Demo](docs/demo.gif)

## Why / what

- **Built for devices, not data centres.** Single-threaded epoll loop, bounded buffers, hard limits and timeouts on every connection; no heap growth driven by clients.
- **Complete HTTP/1.1 where it matters.** Keep-alive, pipelining, chunked requests and responses, `Expect: 100-continue`, `HEAD`/`OPTIONS`, graceful shutdown that drains in-flight responses.
- **Batteries included, zero dependencies.** Router with `:params` and wildcards, strict JSON, streaming static files with ETags, HTTP Basic auth. Only the C++17 standard library and Linux syscalls.
- **Hardened input handling.** Rejects request-smuggling framing, path traversal and symlink escapes; parser is exercised by fuzz-style tests under ASan/UBSan.
- **Testable without hardware.** The protocol engine is transport-independent; the example panel runs on a simulated device, on real `/proc`+`/sys`, or replays recorded HTTP traffic from a file.

## Demo

`ehs-panel` is a device configuration panel (web UI + REST API) built on the library:

```bash
build/examples/ehs-panel --sim --auth admin:secret
# open http://localhost:8080  (user: admin, password: secret)
```

The GIF above is generated from [`docs/demo.tape`](docs/demo.tape) with [vhs](https://github.com/charmbracelet/vhs).

## 60-second quick start

```bash
git clone https://github.com/bekirhankarakurt/embedded-http-server.git
cd embedded-http-server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
build/examples/ehs-panel --sim
```

```bash
curl -s localhost:8080/api/status
```

Or download a prebuilt `linux-x86_64` / `linux-aarch64` package from [Releases](https://github.com/bekirhankarakurt/embedded-http-server/releases) and verify it with `sha256sum -c SHA256SUMS --ignore-missing`.

### Use it as a library

```cpp
#include "ehs/ehs.hpp"

int main() {
    ehs::Router router;
    router.get("/hello/:name", [](ehs::Request& req) {
        ehs::Json doc = ehs::Json::object();
        doc["greeting"] = "Hello, " + std::string(req.param("name"));
        return ehs::json_response(doc);
    });

    ehs::ServerOptions options;
    options.port = 8080;
    ehs::Server server([&](ehs::Request& req) { return router.handle(req); }, options);
    server.run();  // call server.stop() from a signal handler or another thread
}
```

Link against `ehs::ehs` (`add_subdirectory(embedded-http-server)` + `target_link_libraries(app PRIVATE ehs::ehs)`); see [`examples/hello/main.cpp`](examples/hello/main.cpp).

## Requirements

| Requirement | Version                                          |
|-------------|--------------------------------------------------|
| OS          | Linux (epoll, eventfd), kernel 2.6.27+           |
| Compiler    | GCC 9+ or Clang 10+ with C++17                   |
| CMake       | 3.16+                                            |
| Optional    | `wrk` (benchmarks), `gcovr` (coverage), `doxygen` (docs), `aarch64-linux-gnu-g++` (cross build) |

### Build options

| Option                    | Default | Description                                   |
|---------------------------|---------|-----------------------------------------------|
| `EHS_BUILD_TESTS`         | `ON`    | Unit, fuzz-style and socket integration tests |
| `EHS_BUILD_EXAMPLES`      | `ON`    | `ehs-panel` and `ehs-hello`                   |
| `EHS_WARNINGS_AS_ERRORS`  | `OFF`   | Add `-Werror` (enabled in CI)                 |
| `EHS_SANITIZE`            | `OFF`   | AddressSanitizer + UndefinedBehaviorSanitizer |
| `EHS_COVERAGE`            | `OFF`   | `--coverage` instrumentation for gcovr        |
| `CMAKE_TOOLCHAIN_FILE`    | –       | `cmake/aarch64-linux-gnu.cmake` for ARM64     |

## Usage

### `ehs-panel` CLI reference

| Option                     | Default        | Description                                              |
|----------------------------|----------------|----------------------------------------------------------|
| `-p, --port N`             | `8080`         | TCP port (`0` = ephemeral)                               |
| `-b, --bind ADDR`          | `0.0.0.0`      | IPv4 or IPv6 address                                     |
| `-w, --web-root DIR`       | bundled `web/` | Directory served for the web UI                          |
| `--no-web`                 | off            | Serve the REST API only                                  |
| `-c, --config FILE`        | in memory      | Persist configuration (atomic write + rename)            |
| `-a, --auth USER:PASS`     | off            | HTTP Basic auth on everything except `/api/health`       |
| `--sim`                    | off            | Simulated device instead of `/proc` and `/sys`           |
| `--proc-root DIR`          | `/proc`        | procfs root for the Linux backend                        |
| `--sys-root DIR`           | `/sys`         | sysfs root for the Linux backend                         |
| `--replay FILE`            | –              | Run recorded raw HTTP through the server, print replies  |
| `--max-connections N`      | `256`          | Further clients receive `503`                            |
| `--idle-timeout MS`        | `5000`         | Keep-alive idle timeout                                  |
| `--request-timeout MS`     | `10000`        | Time allowed to receive one request (`408` after)        |
| `-q, --quiet`              | off            | Disable the access log on stderr                         |
| `-v, --version` / `-h, --help` |            | Print version / usage                                    |

### REST API

| Method        | Path                  | Description                                 | Success |
|---------------|-----------------------|---------------------------------------------|---------|
| `GET`         | `/api/health`         | Liveness probe (never requires auth)        | 200     |
| `GET`         | `/api/status`         | Telemetry and server counters               | 200     |
| `GET`         | `/api/config`         | Current configuration                       | 200     |
| `GET`         | `/api/config/:field`  | One configuration field                     | 200     |
| `PUT`/`PATCH` | `/api/config`         | Validate, persist and apply (partial) config | 200    |
| `POST`        | `/api/reboot`         | Request a reboot                            | 202     |
| `GET`         | `/api/logs?limit=N`   | Event log, chunked NDJSON stream            | 200     |
| `GET`         | `/*`                  | Static web UI                               | 200/304 |

Errors are JSON: `{"error":"Unprocessable Entity","status":422,"detail":"netmask must be a contiguous IPv4 mask"}`.

### Recipes

**1. Run on a real board with persistent config and auth**

```bash
./ehs-panel --port 80 --config /var/lib/ehs/device.json --auth admin:"$PANEL_PASSWORD" --quiet
```

**2. Script a configuration change from a provisioning tool**

```bash
curl -fsS -u admin:secret -X PATCH -H 'Content-Type: application/json' \
  -d '{"ip_mode":"static","ip_address":"10.0.0.20","gateway":"10.0.0.1"}' \
  http://device.local:8080/api/config
```

**3. Reproduce a field bug offline from a captured request stream**

```bash
build/examples/ehs-panel --replay examples/device_panel/replay/session.http --no-web
```

**4. Test the Linux backend against captured `/proc` and `/sys` snapshots**

```bash
mkdir -p snap/proc snap/sys/class/thermal/thermal_zone0
echo "3600.5 100.0" > snap/proc/uptime
echo "0.25 0.20 0.10 1/80 1234" > snap/proc/loadavg
printf 'MemTotal: 512000 kB\nMemAvailable: 300000 kB\n' > snap/proc/meminfo
echo 54000 > snap/sys/class/thermal/thermal_zone0/temp
build/examples/ehs-panel --proc-root snap/proc --sys-root snap/sys --port 8081
```

**5. Cross-compile for an ARM64 target**

```bash
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DEHS_BUILD_TESTS=OFF
cmake --build build-arm -j
cmake --install build-arm --prefix out --strip
```

## Architecture

```
 clients ──TCP──▶ ┌─ ehs::Server (single thread) ──────────────────────────┐
                  │ epoll_wait → accept4 / recv / send → timeout sweep     │
                  │ eventfd stop() → stop accepting → drain → exit         │
                  └──────────┬───────────────────────────▲─────────────────┘
                     bytes in│                           │bytes out
                  ┌──────────▼───────────────────────────┴─────────────────┐
                  │ ehs::Session (per connection, no I/O)                  │
                  │ RequestParser → dispatch → Content-Length / chunked    │
                  │ keep-alive · pipelining · 100-continue · backpressure  │
                  │ (also driven by --replay and the unit tests)           │
                  └──────────┬─────────────────────────────────────────────┘
                             │ Request& → Response
                  ┌──────────▼─────────────────────────────────────────────┐
                  │ ehs::Router: middleware (BasicAuth) → route handlers   │
                  │   /api/* REST handlers          /* ehs::StaticFiles    │
                  └──────────┬─────────────────────────────────────────────┘
                             │
                  ┌──────────▼─────────────────────────────────────────────┐
                  │ panel::DeviceBackend                                   │
                  │   SimulatedDevice │ LinuxDevice(/proc, /sys fixtures)  │
                  └────────────────────────────────────────────────────────┘
```

### Design trade-offs

| Decision | Why | Cost |
|----------|-----|------|
| Single-threaded epoll, handlers run on the loop | Deterministic, no locks, tiny footprint; matches a single-core MCU-class SoC | A slow handler stalls all clients; offload long work |
| Level-triggered epoll, interest updated per state | Simple, robust back-pressure (stop reading while output is above the high-water mark) | One extra `epoll_ctl` on state changes |
| Session is transport-independent | Same code path for sockets, replay files and tests; easy to port to another event loop | One extra copy of inbound bytes into the session buffer |
| Pull-based `BodyProducer` for streaming | Large files and logs stream in 16 KiB blocks with bounded memory | Producers must be non-blocking |
| Strict parsing (no obs-fold, CL+TE rejected) | Closes request-smuggling vectors behind proxies | Some legacy clients are refused with 400 |
| No TLS in-process | Keeps the binary small and dependency-free | Put a TLS terminator in front on untrusted networks |
| Static files resolved per request, no cache | Always current, flat memory | ~21k req/s vs ~87k for in-memory responses |

### Limits and defaults

| Setting | Default | Response when exceeded |
|---------|---------|------------------------|
| Request line | 8 KiB | `414 URI Too Long` |
| Header section | 16 KiB / 64 fields | `431 Request Header Fields Too Large` |
| Body (decoded) | 1 MiB | `413 Payload Too Large` |
| Request timeout | 10 s | `408 Request Timeout`, then close |
| Idle keep-alive | 5 s | close |
| Write stall | 10 s | close |
| Connections | 256 | `503 Service Unavailable` |
| Requests per connection | 1000 | `Connection: close` on the last one |

## Testing

```bash
ctest --test-dir build --output-on-failure            # all suites
build/tests/ehs_tests parser session                    # selected suites
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEHS_SANITIZE=ON && cmake --build build-asan -j && ctest --test-dir build-asan
bench/run.sh build 10s                                  # wrk benchmarks, see bench/README.md
```

| Suite | What it covers |
|-------|----------------|
| `parser` | Framing, chunked bodies, limits, status mapping for 19 malformed inputs |
| `fuzz` | 20k mutated requests, random fragmentation, random garbage through a Session, JSON mutations, deep nesting |
| `json`, `router`, `auth`, `static_files` | Building blocks, traversal/symlink escape, ETag/304 |
| `session` | Keep-alive, pipelining order, HEAD, chunked streaming with back-pressure, HTTP/1.0, 100-continue, errors |
| `server` | Real sockets: keep-alive, pipelined chunked upload, 4 MiB slow reader, half-close, 408, idle timeout, 503 limit, graceful shutdown, IPv6 |
| `panel`, `panel_replay` | REST API, validation, atomic persistence, procfs/sysfs fixtures, replay file |

CI runs GCC and Clang (Release + Debug), ASan/UBSan, clang-tidy, cppcheck, coverage (gcovr, ≥ 80 % gate) and CodeQL on every push.

Benchmark reference (single core, loopback): **~87k req/s** plain text, **~58k req/s** JSON, p99 < 2 ms at 64 connections. Details in [bench/README.md](bench/README.md).

## Troubleshooting / FAQ

| Symptom | Cause / fix |
|---------|-------------|
| `bind: Permission denied` on port 80 | Ports < 1024 need root or `sudo setcap cap_net_bind_service=+ep ehs-panel` |
| `bind: Address already in use` | Another process owns the port: `ss -ltnp 'sport = :8080'` |
| `static root is not a directory` | Pass `--web-root` or use `--no-web`; installed packages look in `../share/ehs/web` |
| `cpu_temp_c` is `null` | The board has no `/sys/class/thermal/thermal_zone0`; use `--sys-root` or `--sim` |
| Browser keeps asking for a password | Credentials are case-sensitive; Basic auth is per-origin, so clear site data after changing them |
| Large uploads get `413` | Raise `ServerOptions::session.limits.max_body` in your application |
| Clients disconnected after 5 s idle | Keep-alive idle timeout; tune with `--idle-timeout` |

**Is it production-ready for the public internet?** It is designed for device management networks. Put a TLS-terminating reverse proxy in front if traffic crosses untrusted networks.

**Can handlers block?** They run on the event loop. Keep them short or hand work to another thread and return `202 Accepted`.

## Project layout

```
.
├── include/ehs/            Public API (one header per component, ehs.hpp umbrella)
│   ├── http.hpp            Method, Status, Headers, Request, Response
│   ├── parser.hpp          Incremental request parser + Limits
│   ├── session.hpp         Transport-independent connection state machine
│   ├── server.hpp          epoll server, UniqueFd, ServerStats
│   ├── router.hpp          Routes, path params, middleware
│   ├── json.hpp            JSON value, parser, serializer
│   ├── static_files.hpp    Streaming file handler
│   └── auth.hpp            Basic auth, Base64
├── src/                    Library implementation
├── examples/
│   ├── device_panel/       ehs-panel: REST API, device backends, web UI, replay sample
│   └── hello/              ehs-hello: minimal server, benchmark target
├── tests/                  Dependency-free test harness and suites (CTest)
├── bench/                  wrk scripts and reference results
├── cmake/                  aarch64 cross toolchain
├── docs/                   demo.tape / demo.gif
├── .github/                CI, CodeQL, Pages, release workflows; templates
├── Doxyfile                API documentation config
└── .clang-tidy             Static analysis profile
```

## Roadmap

- [x] HTTP/1.1 keep-alive, pipelining, chunked transfer
- [x] Router, JSON, static files, Basic auth
- [x] Limits, timeouts, graceful shutdown
- [x] Simulated / procfs / replay device backends
- [x] Fuzz-style tests, sanitizers, static analysis, coverage, CodeQL
- [x] Release packages for x86_64 and aarch64
- [ ] Optional TLS via a pluggable transport
- [ ] Server-Sent Events for live telemetry push
- [ ] Multi-loop mode (`SO_REUSEPORT`) for multi-core targets
- [ ] Digest / token authentication
- [ ] libFuzzer harness for continuous fuzzing

## License

MIT, see [LICENSE](LICENSE).

Bekirhan Karakurt - [github.com/bekirhankarakurt](https://github.com/bekirhankarakurt)
