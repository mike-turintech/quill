#!/usr/bin/env bash
# Builds Quill's own (upstream) runtime benchmarks for Artemis. The benchmark sources are
# frozen: the Script checks their hashes before this runs.
set -euo pipefail
cd "$(dirname "$0")/../.."
GEN="-G Ninja"
command -v ninja > /dev/null || GEN="-G Unix Makefiles"
cmake -S . -B build-official "$GEN" -DCMAKE_BUILD_TYPE=Release -DQUILL_BUILD_BENCHMARKS=ON > /dev/null
cmake --build build-official -j "${ARTEMIS_JOBS:-32}" \
  --target BENCHMARK_quill_hot_path_rdtsc_clock BENCHMARK_quill_backend_throughput > /dev/null
