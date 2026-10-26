#!/usr/bin/env bash
# Correctness gate: Quill's full unit and integration test suite must pass, with no tests removed.
set -euo pipefail
cd "$(dirname "$0")/../.."
GEN="-G Ninja"
command -v ninja > /dev/null || GEN="-G Unix Makefiles"
rm -rf build-tests
cmake -S . -B build-tests "$GEN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
  -DQUILL_BUILD_TESTS=ON > /dev/null
cmake --build build-tests -j "${ARTEMIS_JOBS:-32}" > /dev/null
ctest --test-dir build-tests -j "${ARTEMIS_JOBS:-32}" --timeout 300 --output-on-failure 2>&1 \
  | tee build-tests/ctest.log | tail -3
total=$(grep -oE 'tests failed out of [0-9]+' build-tests/ctest.log | grep -oE '[0-9]+$')
if [ "${total:-0}" -lt 442 ]; then
  echo "expected at least 442 tests, found ${total:-0}" >&2
  exit 1
fi
