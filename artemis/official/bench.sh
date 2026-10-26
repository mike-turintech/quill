#!/usr/bin/env bash
# Runs Quill's upstream benchmarks and writes artemis_results.json at the repository root.
#   hot path: benchmarks/hot_path_latency/quill_hot_path_rdtsc_clock.cpp (1 and 4 producer threads,
#             per-message latency in ns, from batches of 20)
#   throughput: benchmarks/backend_throughput/quill_backend_throughput.cpp (4M messages, million msgs/s)
set -euo pipefail
cd "$(dirname "$0")/../.."
rm -f artemis_results.json
hot=$(find build-official -name BENCHMARK_quill_hot_path_rdtsc_clock -type f -perm -u+x | head -1)
thr=$(find build-official -name BENCHMARK_quill_backend_throughput -type f -perm -u+x | head -1)
work=$(mktemp -d)
(cd "$work" && "$OLDPWD/$hot") > "$work/hot.txt"
(cd "$work" && "$OLDPWD/$thr") > "$work/thr.txt"
cat "$work/hot.txt" "$work/thr.txt"
python3 - "$work/hot.txt" "$work/thr.txt" << 'EOF'
import json, os, re, sys

hot = open(sys.argv[1]).read()
thr = open(sys.argv[2]).read()
results = {}
for threads, row in re.findall(r"Thread Count (\d+) - Total messages \d+ - [^\n]*\n[^\n]*\n\s*\|([^\n]*)", hot):
    values = [int(v) for v in re.findall(r"\d+", row)]
    if len(values) != 7:
        sys.exit(f"unexpected hot path row for {threads} thread(s): {row!r}")
    p50, p75, p90, p95, p99, p999, worst = values
    results[f"hot_{threads}t_p50_ns"] = p50
    results[f"hot_{threads}t_p90_ns"] = p90
    results[f"hot_{threads}t_p99_ns"] = p99
    results[f"hot_{threads}t_p999_ns"] = p999
m = re.search(r"Throughput is ([0-9.]+) million msgs/sec", thr)
if not m or "hot_1t_p50_ns" not in results or "hot_4t_p50_ns" not in results:
    sys.exit("benchmark output incomplete")
results["backend_mmsgs_per_sec"] = float(m.group(1))
with open("artemis_results.json.tmp", "w") as f:
    json.dump(results, f)
os.replace("artemis_results.json.tmp", "artemis_results.json")
print(json.dumps(results))
EOF
rm -rf "$work"
