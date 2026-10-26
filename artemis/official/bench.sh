#!/usr/bin/env bash
# Runs Quill's upstream benchmarks and writes artemis_results.json at the repository root.
#   hot path: benchmarks/hot_path_latency/quill_hot_path_rdtsc_clock.cpp (1 and 4 producer threads;
#             each sample is the average per-message latency of one batch, in whole ns)
#   throughput: benchmarks/backend_throughput/quill_backend_throughput.cpp (4M messages, enqueue to
#             flushed file, million msgs/s)
# Every log line the benchmarks write is checked afterwards, so a version that drops, disables or
# corrupts logging fails instead of scoring.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
rm -f artemis_results.json
unset QUILL_LOG_LEVEL
hot=$root/build-official/benchmarks/hot_path_latency/BENCHMARK_quill_hot_path_rdtsc_clock
thr=$root/build-official/benchmarks/backend_throughput/BENCHMARK_quill_backend_throughput
work=$(mktemp -d "$root/build-official/run.XXXXXX")
trap 'rm -rf "$work"' EXIT

(cd "$work" && timeout 300 "$hot") > "$work/hot.txt" 2> "$work/hot.err"
(cd "$work" && timeout 120 "$thr") > "$work/thr.txt" 2> "$work/thr.err"
cat "$work/hot.txt" "$work/thr.txt"

if grep -qi "failed to set cpu affinity" "$work/hot.err" "$work/thr.err" "$work/hot.txt" "$work/thr.txt"; then
  echo "cpu affinity failed; refusing to score" >&2
  exit 1
fi

python3 - "$work" << 'EOF'
import json, math, os, re, sys

work = sys.argv[1]
hot = open(f"{work}/hot.txt").read()
thr = open(f"{work}/thr.txt").read()

# Output checks: every record present, with the arguments the benchmark logged.
hot_re = re.compile(r"Logging iteration: (\d+), message: (\d+), double: (\S+)$")
seen = 0
for line in open(f"{work}/quill_hot_path_rdtsc_clock.log"):
    m = hot_re.search(line.rstrip("\n"))
    if not m:
        if line.rstrip("\n").endswith("preallocate"):
            continue
        sys.exit(f"unexpected hot path log line: {line!r}")
    k = int(m.group(1))
    if float(m.group(3)) != float(k) + (0.1 * float(k)):
        sys.exit(f"hot path double mismatch: {line!r}")
    seen += 1
if seen != 400000:
    sys.exit(f"hot path log has {seen} records, expected 400000")

thr_re = re.compile(r"Iteration: (\d+) int: (\d+) double: (\S+)$")
iterations = bytearray(4000000)
seen = 0
for line in open(f"{work}/quill_backend_total_time.log"):
    m = thr_re.search(line.rstrip("\n"))
    if not m:
        if line.rstrip("\n").endswith("preallocate"):
            continue
        sys.exit(f"unexpected throughput log line: {line!r}")
    i = int(m.group(1))
    if i >= 4000000 or iterations[i] or int(m.group(2)) != 2 * i or float(m.group(3)) != i / 2:
        sys.exit(f"throughput record wrong or duplicated: {line!r}")
    iterations[i] = 1
    seen += 1
if seen != 4000000:
    sys.exit(f"throughput log has {seen} records, expected 4000000")

# Metrics.
results = {}
rows = re.findall(r"Thread Count (\d+) - Total messages (\d+) - [^\n]*\n[^\n]*\n\s*\|([^\n]*)", hot)
counts = [r[0] for r in rows]
if sorted(counts) != ["1", "4"]:
    sys.exit(f"expected one hot path row for 1 and 4 threads, got {counts}")
for threads, total, row in rows:
    fields = [f.strip() for f in row.strip().strip("|").split("|")]
    if int(total) != 200000 or len(fields) != 7 or not all(re.fullmatch(r"\d+", f) for f in fields):
        sys.exit(f"unexpected hot path row for {threads} thread(s): {row!r}")
    values = [int(f) for f in fields]
    if values != sorted(values) or values[0] <= 0:
        sys.exit(f"hot path percentiles not ordered/positive: {values}")
    p50, p75, p90, p95, p99, p999, worst = values
    results[f"hot_{threads}t_p50_ns"] = p50
    results[f"hot_{threads}t_p90_ns"] = p90
    results[f"hot_{threads}t_p99_ns"] = p99
    results[f"hot_{threads}t_p999_ns"] = p999
tm = re.findall(r"Throughput is ([0-9.]+) million msgs/sec", thr)
if len(tm) != 1 or not math.isfinite(float(tm[0])) or float(tm[0]) <= 0:
    sys.exit(f"expected one positive throughput result, got {tm}")
results["backend_mmsgs_per_sec"] = float(tm[0])

with open("artemis_results.json.tmp", "w") as f:
    json.dump(results, f)
os.replace("artemis_results.json.tmp", "artemis_results.json")
print(json.dumps(results))
EOF
