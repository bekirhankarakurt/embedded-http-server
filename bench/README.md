# Benchmarks

Load is generated with [wrk](https://github.com/wg/wrk) (keep-alive HTTP/1.1, `apt install wrk`).
The server is single-threaded by design, so numbers scale with one core.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
bench/run.sh build 10s
```

`run.sh` starts `ehs-hello` (port 18180) and `ehs-panel --sim` (port 18181), runs every scenario
and stops both servers. Each scenario is a plain `wrk` command and can be run on its own, e.g.:

```bash
wrk -t2 -c64 -d10s --latency http://127.0.0.1:18180/
wrk -t2 -c16 -d10s -s bench/put_config.lua http://127.0.0.1:18181/api/config
```

## Reference results

Release build, GCC 13, Intel Xeon @ 2.80 GHz (4 vCPU cloud VM), client and server on the same
host over loopback, 5 s per scenario:

| Scenario                         | Conns | Req/s  | p50     | p99     |
|----------------------------------|------:|-------:|--------:|--------:|
| `GET /` plain text (ehs-hello)   |    64 | 87,300 | 0.66 ms | 1.86 ms |
| `GET /` plain text (ehs-hello)   |   256 | 95,600 | 2.01 ms | 6.86 ms |
| `GET /api/status` JSON telemetry |    64 | 58,500 | 0.97 ms | 2.52 ms |
| `GET /index.html` static file    |    64 | 21,600 | 2.72 ms | 11.6 ms |
| `PUT /api/config` validate+store |    16 | 40,500 | 0.37 ms | 1.38 ms |

Resident memory under load: ~4 MB (`ehs-hello`), ~4.7 MB (`ehs-panel`).

Static files are opened, `stat`ed and canonicalised on every request (no cache), which keeps
memory flat and always serves the current file at the cost of throughput.

## Tips for embedded targets

- Build with `-DCMAKE_BUILD_TYPE=MinSizeRel` and `strip` the binary for the smallest footprint.
- Lower `SessionOptions::output_high_water` to cap per-connection buffering on small-RAM devices.
- `ServerOptions::max_connections` bounds total memory: each idle connection costs a socket plus
  a few hundred bytes of session state.
