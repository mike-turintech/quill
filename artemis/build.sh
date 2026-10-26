#!/usr/bin/env bash
# Builds the Artemis benchmark against the in-tree (header-only) Quill.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-artemis
g++ -std=c++17 -O3 -DNDEBUG -march=native -Iinclude artemis/bench_hot_path.cpp -o build-artemis/bench_hot_path -pthread
