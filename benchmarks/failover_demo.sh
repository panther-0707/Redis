#!/usr/bin/env bash
# Shows failover live: three backends, one is killed and later restarted.
#
# Usage: benchmarks/failover_demo.sh
# Needs a build in ./build (or BUILD_DIR). Redis is not required: the cache
# and rate limiter are switched off so that every request reaches a backend.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
LB_PORT=18080
BACKEND_BASE_PORT=18000

[[ -x "$BUILD_DIR/miniLB" && -x "$BUILD_DIR/demo_backend" ]] \
    || { echo "Build first: scripts/build.sh" >&2; exit 1; }

WORK="$(mktemp -d)"
declare -A BACKEND_PID
LB_PID=""

cleanup() {
    kill "${BACKEND_PID[@]}" $LB_PID 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

start_backend() {
    "$BUILD_DIR/demo_backend" --port "$((BACKEND_BASE_PORT + $1))" --name "backend-$1" >/dev/null &
    BACKEND_PID[$1]=$!
}

send_requests() {   # send_requests <count>: prints status and answering backend
    for _ in $(seq 1 "$1"); do
        curl -s -o /dev/null -w '  HTTP %{http_code}  served by %header{x-backend}\n' \
            "http://127.0.0.1:$LB_PORT/"
    done
}

show_health() {
    curl -s "http://127.0.0.1:$LB_PORT/stats" | grep '"id"' | sed 's/^ */  /'
}

cat > "$WORK/config.yaml" <<YAML
listen_port: $LB_PORT
log_level: warn
backends:
  - { host: 127.0.0.1, port: $((BACKEND_BASE_PORT + 1)) }
  - { host: 127.0.0.1, port: $((BACKEND_BASE_PORT + 2)) }
  - { host: 127.0.0.1, port: $((BACKEND_BASE_PORT + 3)) }
health_check: { path: /health, interval_ms: 1000, fail_threshold: 2, success_threshold: 2 }
rate_limit: { requests: 0, window_seconds: 60 }
cache: { enabled: false }
YAML

for i in 1 2 3; do start_backend "$i"; done
sleep 0.5
"$BUILD_DIR/miniLB" --config "$WORK/config.yaml" &
LB_PID=$!
sleep 1

echo "1) All three backends are up: round robin"
send_requests 6

echo
echo "2) Killing backend-2 (kill -9). Requests keep succeeding: one that reaches"
echo "   the dead backend is retried on another one."
kill -9 "${BACKEND_PID[2]}"
wait "${BACKEND_PID[2]}" 2>/dev/null || true   # reap it quietly
send_requests 6

echo
echo "3) backend-2 is out of rotation (2 failures in a row: failed requests and probes both count):"
sleep 3
show_health
send_requests 4

echo
echo "4) Restarting backend-2. After 2 successful health checks it is back:"
start_backend 2
sleep 3.5
show_health
send_requests 6
