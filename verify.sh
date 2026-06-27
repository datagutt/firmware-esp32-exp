#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== verify: starting ==="

# clang-format check (warn-only; repo has known pre-existing drift)
if command -v clang-format >/dev/null 2>&1; then
    echo "--- clang-format (warn-only) ---"
    fmt_out=$(find main -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
      ! -name 'embedded_tz_db.*' ! -name 'font5x7.h' \
      -print0 | xargs -0 clang-format --dry-run 2>&1 || true)
    if [ -n "$fmt_out" ]; then
        echo "WARN: clang-format drift detected (informational, not failing the gate)"
    else
        echo "clang-format: no drift"
    fi
else
    echo "WARN: clang-format not found, skipping"
fi

# cppcheck (skip if not installed)
if command -v cppcheck >/dev/null 2>&1; then
    echo "--- cppcheck ---"
    cppcheck --enable=warning,performance,portability \
        --force \
        --error-exitcode=1 \
        --inline-suppr \
        -I main \
        -I main/network \
        -I main/system \
        main \
        components/assets
    echo "cppcheck: PASS"
else
    echo "WARN: cppcheck not found, skipping"
fi

# Host tests (always run, hard gate)
echo "--- host tests ---"
bash "$ROOT_DIR/test/host/run_tests.sh"

echo "verify: OK"
