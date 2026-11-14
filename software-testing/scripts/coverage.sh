#!/usr/bin/env bash
# Build miniLB with coverage instrumentation, run every test and report
# line and branch coverage of src/ and include/ with gcovr.
#
# Usage: software-testing/scripts/coverage.sh
#   MIN_LINE=95 MIN_BRANCH=88 software-testing/scripts/coverage.sh   # thresholds (the defaults)
#   BUILD_DIR=build-coverage software-testing/scripts/coverage.sh
#
# Exits non-zero if a test fails or coverage is below a threshold, so CI can
# use it as a gate. The HTML report (one page per source file, uncovered
# lines and branches highlighted) ends up in $BUILD_DIR/coverage/index.html.
#
# Needs: gcovr (sudo apt-get install gcovr).
set -euo pipefail

cd "$(dirname "$0")/../.."

BUILD_DIR="${BUILD_DIR:-build-coverage}"
MIN_LINE="${MIN_LINE:-95}"
MIN_BRANCH="${MIN_BRANCH:-88}"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DMINILB_COVERAGE=ON
cmake --build "$BUILD_DIR" -j "$(nproc)"

# Counters accumulate across runs: start from zero.
find "$BUILD_DIR" -name '*.gcda' -delete

# The integration tests start the real miniLB binary; it writes its counters
# when it exits, which is why every test insists on a clean exit.
ctest --test-dir "$BUILD_DIR" --output-on-failure

mkdir -p "$BUILD_DIR/coverage"
# --exclude-throw-branches: GCC adds a hidden branch for every call that
# could throw; they cannot be covered by tests and would drown the real ones.
# --gcov-ignore-parse-errors: gcov can still print a negative count in rare
# cases (GCC bug 68080); warn about it instead of aborting the report.
gcovr --root . --object-directory "$BUILD_DIR" \
      --filter 'src/' --filter 'include/' \
      --exclude-throw-branches \
      --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
      --txt "$BUILD_DIR/coverage/lines.txt" \
      --html-details "$BUILD_DIR/coverage/index.html" \
      --xml "$BUILD_DIR/coverage/coverage.xml" \
      --print-summary \
      --fail-under-line "$MIN_LINE" --fail-under-branch "$MIN_BRANCH"

# Per-file branch table (the default text table above is by line).
gcovr --root . --object-directory "$BUILD_DIR" \
      --filter 'src/' --filter 'include/' \
      --exclude-throw-branches --txt-metric branch \
      --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
      --txt "$BUILD_DIR/coverage/branches.txt" > /dev/null

echo "Reports: $BUILD_DIR/coverage/{index.html,lines.txt,branches.txt,coverage.xml}"
