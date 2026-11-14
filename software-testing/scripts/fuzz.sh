#!/usr/bin/env bash
# Build the HTTP parser fuzzer and run it for a while.
#
# Usage: software-testing/scripts/fuzz.sh [seconds]      (default 60)
#
# Exits non-zero if the fuzzer finds a crash, a sanitizer report or a broken
# property; the input that caused it is left in build-fuzz/ as crash-<hash>
# and can be replayed with:  build-fuzz/http_parser_fuzz build-fuzz/crash-<hash>
#
# The fuzzer only needs the parser sources (no Boost, no Redis), so it is
# compiled directly instead of through CMake.
#
# Needs: clang and its runtime (sudo apt-get install clang libclang-rt-dev).
set -euo pipefail

cd "$(dirname "$0")/../.."

SECONDS_TO_RUN="${1:-60}"
BUILD_DIR="${BUILD_DIR:-build-fuzz}"
CXX="${FUZZ_CXX:-clang++}"

mkdir -p "$BUILD_DIR/corpus"
"$CXX" -std=c++17 -g -O1 -Iinclude -DMINILB_ASSERTIONS \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
    software-testing/fuzz/http_parser_fuzz.cpp \
    src/http/HttpParser.cpp src/http/Headers.cpp src/http/HttpRequest.cpp src/http/HttpResponse.cpp \
    -o "$BUILD_DIR/http_parser_fuzz"

# The first directory receives new inputs; the committed seeds are only read.
"$BUILD_DIR/http_parser_fuzz" "$BUILD_DIR/corpus" software-testing/fuzz/corpus \
    -dict=software-testing/fuzz/http.dict \
    -max_len=1024 -timeout=5 \
    -max_total_time="$SECONDS_TO_RUN" \
    -artifact_prefix="$BUILD_DIR/" \
    -print_final_stats=1
