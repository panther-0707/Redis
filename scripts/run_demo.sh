#!/usr/bin/env bash
# Starts a complete local demo: Redis, three demo backends and miniLB.
# Press Ctrl-C to stop everything.
#
# Usage: scripts/run_demo.sh [config-file]      (default: config/config.yaml)
# Requires: a build in ./build (scripts/build.sh) and redis-server on PATH.
set -euo pipefail

cd "$(dirname "$0")/.."

CONFIG="${1:-config/config.yaml}"
BUILD_DIR="${BUILD_DIR:-build}"

if [[ ! -x "$BUILD_DIR/miniLB" || ! -x "$BUILD_DIR/demo_backend" ]]; then
    echo "Binaries not found in $BUILD_DIR/. Run scripts/build.sh first." >&2
    exit 1
fi

PIDS=()
cleanup() {
    echo
    echo "Stopping demo..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' INT TERM   # Ctrl-C / kill: leave through the EXIT trap above

# Redis: reuse one that is already running on 6379, otherwise start our own
# (in memory only: no snapshot files are written).
if redis-cli -p 6379 ping >/dev/null 2>&1; then
    echo "Using the Redis already running on port 6379"
elif command -v redis-server >/dev/null 2>&1; then
    redis-server --port 6379 --save "" --appendonly no >/dev/null &
    PIDS+=($!)
    echo "Started redis-server on port 6379"
else
    echo "redis-server not found: continuing without Redis (cache and rate limiting are skipped)"
fi

for i in 1 2 3; do
    "$BUILD_DIR/demo_backend" --port "800$i" --name "backend-$i" &
    PIDS+=($!)
done
sleep 0.5

cat <<'TIPS'

Try (in another terminal):
  curl -i http://localhost:8080/        # X-Backend rotates; repeat to see X-Cache: HIT
  curl -i http://localhost:8080/slow    # 2 s the first time, instant from the cache
  curl -s http://localhost:8080/stats   # live metrics as JSON

TIPS

"$BUILD_DIR/miniLB" --config "$CONFIG" &
PIDS+=($!)
wait "${PIDS[-1]}"
