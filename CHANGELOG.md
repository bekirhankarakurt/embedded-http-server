# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-11-04

### Added

- Single-threaded epoll HTTP/1.1 server with keep-alive, pipelining, `Expect: 100-continue`,
  per-connection request limits and request/idle/write timeouts.
- Incremental request parser with hard limits (request line, header bytes, header count, body)
  and request-smuggling defences (conflicting `Content-Length`, `Content-Length` +
  `Transfer-Encoding`, obsolete line folding).
- Chunked transfer encoding for request bodies and streamed responses, with back-pressure.
- Router with `:param` segments, `*` wildcards, middleware, automatic 404/405/`OPTIONS`/`HEAD`.
- Strict RFC 8259 JSON value type with parser, serializer and nesting limit.
- Streaming static file handler with path-traversal and symlink-escape protection and ETags.
- HTTP Basic authentication with constant-time credential comparison.
- Graceful shutdown that drains in-flight responses (async-signal-safe `Server::stop()`).
- `ehs-panel` device configuration panel (web UI + REST API) with simulated, procfs/sysfs and
  replay backends; `ehs-hello` minimal example.
- Unit, fuzz-style and socket integration tests; wrk benchmark suite.
- CI for GCC, Clang, ASan/UBSan, clang-tidy, cppcheck, coverage and CodeQL; release packages
  for linux-x86_64 and linux-aarch64.

[Unreleased]: https://github.com/bekirhankarakurt/embedded-http-server/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/bekirhankarakurt/embedded-http-server/releases/tag/v1.0.0
