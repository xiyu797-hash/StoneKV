#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSTONEKV_BUILD_TESTS=ON
cmake --build build-release --parallel "$(nproc)"
ctest --test-dir build-release --output-on-failure

if [[ "${1:-}" == "--asan" ]]; then
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSTONEKV_ENABLE_SANITIZERS=ON
  cmake --build build-asan --parallel "$(nproc)"
  ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
fi
