#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/test/host/build"

cmake -S "$ROOT_DIR/test/host" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j
"$BUILD_DIR/host_unit_tests"
"$BUILD_DIR/host_json_fuzz"
