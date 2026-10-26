#!/usr/bin/env bash
# Runs the benchmark from the repository root so artemis_results.json lands there.
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f artemis_results.json
./build-artemis/bench_hot_path
