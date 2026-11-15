#!/usr/bin/env bash
# StoneKV server experiment runner. Never deletes user-provided paths.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
run_root="${1:-}"
if [[ -z "$run_root" ]]; then
  run_root="$(mktemp -d /tmp/stonekv-validation.XXXXXX)"
else
  if [[ -e "$run_root" ]]; then
    echo "ERROR: output directory already exists: $run_root" >&2
    exit 2
  fi
  mkdir -p "$run_root"
fi
run_root="$(realpath "$run_root")"
echo "Artifacts: $run_root"
{
  echo "timestamp=$(date -Is)"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo no-git)"
  uname -a
  lscpu | grep -E '^Model name:|^CPU\(s\):' || true
  lsblk -o NAME,SIZE,TYPE,ROTA,MODEL || true
  cmake --version | head -1
  g++ --version | head -1
  if command -v clang++ >/dev/null; then clang++ --version | head -1; fi
} >"$run_root/environment.txt" 2>&1
for compiler in gcc clang; do
  if [[ "$compiler" == clang ]] && ! command -v clang++ >/dev/null; then
    echo "clang missing" >"$run_root/clang.SKIPPED"
    continue
  fi
  if [[ "$compiler" == gcc ]]; then export CXX=g++; else export CXX=clang++; fi
  cmake -S . -B "$run_root/build-$compiler" -DCMAKE_BUILD_TYPE=Release >"$run_root/config-$compiler.log" 2>&1
  cmake --build "$run_root/build-$compiler" --parallel 4 >"$run_root/build-$compiler.log" 2>&1
  ctest --test-dir "$run_root/build-$compiler" --output-on-failure >"$run_root/test-$compiler.log" 2>&1
  echo "$compiler release PASS"
done
export CXX=g++
cmake -S . -B "$run_root/build-asan" -DCMAKE_BUILD_TYPE=Debug -DSTONEKV_ENABLE_SANITIZERS=ON >"$run_root/config-asan.log" 2>&1
cmake --build "$run_root/build-asan" --parallel 4 >"$run_root/build-asan.log" 2>&1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir "$run_root/build-asan" --output-on-failure >"$run_root/test-asan.log" 2>&1
echo "asan+ubsan PASS"
bench="$run_root/build-gcc/stonekv_bench"
for mode in 0 1; do
  if [[ "$mode" == 0 ]]; then count=20000; else count=2000; fi
  for round in 1 2 3; do
    "$bench" "$run_root/data-sync${mode}-${round}" "$count" 128 "$mode" 42 \
      >"$run_root/bench-sync${mode}-${round}.csv" 2>"$run_root/bench-sync${mode}-${round}.err"
    echo "benchmark sync=$mode round=$round completed"
  done
done
printf 'PASS. Please archive %s; do not claim production durability or long-term stability from these short runs.\n' "$run_root"
