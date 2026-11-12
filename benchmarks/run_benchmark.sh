#!/usr/bin/env bash
# Load-tests miniLB with wrk and prints a summary table.
#
# Usage: benchmarks/run_benchmark.sh
#
# Environment variables (defaults in brackets):
#   DURATION     wrk duration per scenario            [30s]
#   WRK_THREADS  wrk threads                          [4]
#   CONNECTIONS  concurrent connections               [200]
#   LB_THREADS   miniLB io_context threads            [4]
#   BUILD_DIR    where miniLB and demo_backend live   [build]
#
# Scenarios:
#   direct         wrk -> one demo backend, no load balancer (baseline)
#   lb-1-backend   wrk -> miniLB -> 1 backend,  GET /
#   lb-3-backends  wrk -> miniLB -> 3 backends, GET /
#   work-1-backend wrk -> miniLB -> 1 backend,  GET /work (backend-limited, ~1000 req/s each)
#   work-3-backends wrk -> miniLB -> 3 backends, GET /work
#   lb-cache       wrk -> miniLB (Redis cache on) -> 3 backends, GET /
#
# Rate limiting is switched off (otherwise nearly every request would be a
# 429) and logging is reduced to warnings. Everything runs on this machine
# on private ports, so wrk competes with miniLB for CPU: treat the numbers
# as relative, not absolute. Raw wrk output is kept in benchmarks/results/.
set -euo pipefail

cd "$(dirname "$0")/.."

DURATION="${DURATION:-30s}"
WRK_THREADS="${WRK_THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-200}"
LB_THREADS="${LB_THREADS:-4}"
BUILD_DIR="${BUILD_DIR:-build}"

LB_PORT=18080
BACKEND_BASE_PORT=18000   # backends listen on 18001, 18002, 18003
REDIS_PORT=16379

command -v wrk >/dev/null || { echo "wrk is not installed (apt-get install wrk)" >&2; exit 1; }
[[ -x "$BUILD_DIR/miniLB" && -x "$BUILD_DIR/demo_backend" ]] \
    || { echo "Build first: scripts/build.sh" >&2; exit 1; }

RESULTS="benchmarks/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULTS"
WORK="$(mktemp -d)"
PIDS=()

stop_all() {
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    PIDS=()
}
trap 'stop_all; rm -rf "$WORK"' EXIT

wait_for_port() {
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "port $1 did not open" >&2
    exit 1
}

start_backends() {   # start_backends <count>
    for i in $(seq 1 "$1"); do
        "$BUILD_DIR/demo_backend" --port "$((BACKEND_BASE_PORT + i))" --name "backend-$i" >/dev/null &
        PIDS+=($!)
        wait_for_port "$((BACKEND_BASE_PORT + i))"
    done
}

start_lb() {   # start_lb <backend-count> <cache: true|false>
    {
        echo "listen_port: $LB_PORT"
        echo "threads: $LB_THREADS"
        echo "log_level: warn"
        echo "backends:"
        for i in $(seq 1 "$1"); do
            echo "  - { host: 127.0.0.1, port: $((BACKEND_BASE_PORT + i)) }"
        done
        echo "rate_limit: { requests: 0, window_seconds: 60 }"
        echo "cache: { enabled: $2, ttl_seconds: 30 }"
        echo "redis: { host: 127.0.0.1, port: $REDIS_PORT }"
    } > "$WORK/config.yaml"
    "$BUILD_DIR/miniLB" --config "$WORK/config.yaml" > "$WORK/lb.log" 2>&1 &
    PIDS+=($!)
    wait_for_port "$LB_PORT"
}

SUMMARY=()
run_wrk() {   # run_wrk <label> <url>
    local out="$RESULTS/$1.txt"
    echo "==> $1: wrk -t$WRK_THREADS -c$CONNECTIONS -d$DURATION --latency $2"
    wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$2" > "$out" 2>&1 || true

    local rps p50 p99 bad errors
    rps=$(awk '/Requests\/sec:/ {print $2}' "$out")
    p50=$(awk '$1 == "50%" {print $2}' "$out")
    p99=$(awk '$1 == "99%" {print $2}' "$out")
    bad=$(awk '/Non-2xx or 3xx responses:/ {print $5}' "$out")
    errors=$(grep -c "Socket errors" "$out" || true)
    SUMMARY+=("$(printf '%-16s %12s %10s %10s %10s %14s' "$1" "${rps:-?}" "${p50:-?}" "${p99:-?}" \
        "${bad:-0}" "$([[ "$errors" == 0 ]] && echo none || echo "see file")")")
}

# ---- baseline: no load balancer -----------------------------------------------
start_backends 1
run_wrk direct "http://127.0.0.1:$((BACKEND_BASE_PORT + 1))/"
stop_all

# ---- through miniLB, cache off -------------------------------------------------
for count in 1 3; do
    start_backends "$count"
    start_lb "$count" false
    run_wrk "lb-$count-backend$([[ $count == 1 ]] || echo s)" "http://127.0.0.1:$LB_PORT/"
    run_wrk "work-$count-backend$([[ $count == 1 ]] || echo s)" "http://127.0.0.1:$LB_PORT/work"
    stop_all
done

# ---- through miniLB, Redis cache on ---------------------------------------------
if command -v redis-server >/dev/null; then
    redis-server --port "$REDIS_PORT" --save "" --appendonly no --bind 127.0.0.1 >/dev/null &
    PIDS+=($!)
    wait_for_port "$REDIS_PORT"
    start_backends 3
    start_lb 3 true
    run_wrk lb-cache "http://127.0.0.1:$LB_PORT/"
    stop_all
else
    echo "redis-server not found: skipping the cache scenario"
fi

echo
printf '%-16s %12s %10s %10s %10s %14s\n' scenario "requests/s" "p50" "p99" "non-2xx" "socket errors"
printf '%s\n' "${SUMMARY[@]}"
echo
echo "Raw wrk output: $RESULTS/"
