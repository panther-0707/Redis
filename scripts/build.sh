#!/usr/bin/env bash
# Configure and build miniLB.
#
# Usage: scripts/build.sh [build-dir] [extra cmake args...]
#   scripts/build.sh                       # Release build in ./build
#   scripts/build.sh build-debug -DCMAKE_BUILD_TYPE=Debug
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
shift || true

cmake -S . -B "$BUILD_DIR" "$@"
cmake --build "$BUILD_DIR" -j "$(nproc)"

echo "Built into $BUILD_DIR/"
