#!/usr/bin/env bash
# Runs the wrk benchmark suite against a Release build.
# Usage: bench/run.sh [BUILD_DIR] [DURATION]
set -euo pipefail

BUILD_DIR=${1:-build}
DURATION=${2:-10s}
HELLO_PORT=18180
PANEL_PORT=18181
HERE=$(cd "$(dirname "$0")" && pwd)

command -v wrk >/dev/null || { echo "wrk not found (apt install wrk)" >&2; exit 1; }

"$BUILD_DIR/examples/ehs-hello" "$HELLO_PORT" 2>/dev/null &
HELLO_PID=$!
"$BUILD_DIR/examples/ehs-panel" --sim --quiet --port "$PANEL_PORT" 2>/dev/null &
PANEL_PID=$!
trap 'kill -INT $HELLO_PID $PANEL_PID 2>/dev/null; wait' EXIT
sleep 0.5

run() {
    local name=$1; shift
    printf '\n== %s ==\n' "$name"
    wrk --latency -d "$DURATION" "$@" | grep -E 'Requests/sec|Transfer/sec|  50%|  99%|Non-2xx|errors'
}

run "plain text, 64 connections"       -t2 -c64  "http://127.0.0.1:$HELLO_PORT/"
run "plain text, 256 connections"      -t2 -c256 "http://127.0.0.1:$HELLO_PORT/"
run "JSON status, 64 connections"      -t2 -c64  "http://127.0.0.1:$PANEL_PORT/api/status"
run "static index.html, 64 connections" -t2 -c64 "http://127.0.0.1:$PANEL_PORT/index.html"
run "PUT /api/config, 16 connections"  -t2 -c16 -s "$HERE/put_config.lua" "http://127.0.0.1:$PANEL_PORT/api/config"

printf '\nResident memory (kB):\n'
ps -o rss=,comm= -p "$HELLO_PID","$PANEL_PID"
