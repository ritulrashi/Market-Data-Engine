#!/usr/bin/env python3
"""Summarize raw benchmark logs from scripts/run_benchmarks.sh.

For each (mode, clients) configuration, prints the median, min and max over
runs of every metric as a Markdown table. Values come straight from the
logs; nothing is estimated.
"""
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "bench/results/codespace")


def kv(line):
    return {k: float(v) for k, v in re.findall(r"(\w+)=([-\d.e+]+)", line)}


rows = defaultdict(list)
for client_log in sorted(out_dir.glob("*_c*_r*_client.log")):
    m = re.match(r"(\w+)_c(\d+)_r(\d+)_client\.log", client_log.name)
    mode, clients, run = m.group(1), int(m.group(2)), int(m.group(3))
    c = kv(next(l for l in client_log.read_text().splitlines() if l.startswith("CLIENTS=")))
    server_lines = client_log.with_name(client_log.name.replace("_client", "_server")).read_text().splitlines()
    final = kv(next(l for l in server_lines if l.startswith("FINAL")))
    # Producer rate while all clients were connected: first/last STATS
    # sample with the full client count.
    stats = [kv(l) for l in server_lines if l.startswith("STATS")]
    full = [s for s in stats if s["clients"] == clients]
    prod_tps = (full[-1]["produced"] - full[0]["produced"]) / (full[-1]["t"] - full[0]["t"]) if len(full) > 1 else float("nan")
    delivered = final["sent"]
    rows[(mode, clients)].append({
        "run": run,
        "agg_tps": c["AGGREGATE_TPS"],
        "per_tps": c["PER_CONSUMER_TPS"],
        "p50": c["P50_US"],
        "p99": c["P99_US"],
        "p999": c["P999_US"],
        "max": c["MAX_US"],
        "prod_tps": prod_tps,
        "drop_pct": 100.0 * final["dropped"] / (delivered + final["dropped"]) if delivered + final["dropped"] else 0.0,
        "samples": c["SAMPLES"],
    })


def fmt(vals, digits):
    med = statistics.median(vals)
    return f"{med:,.{digits}f} ({min(vals):,.{digits}f} – {max(vals):,.{digits}f})"


cols = [
    ("agg_tps", "Aggregate ticks/s delivered", 0),
    ("per_tps", "Per-consumer ticks/s", 0),
    ("prod_tps", "Producer ticks/s", 0),
    ("p50", "P50 µs", 1),
    ("p99", "P99 µs", 1),
    ("p999", "P99.9 µs", 1),
    ("max", "Max µs", 1),
    ("drop_pct", "Dropped %", 2),
]

for mode in ("steady", "stress"):
    print(f"\n### {mode}\n")
    print("| Consumers | Runs | " + " | ".join(h for _, h, _ in cols) + " |")
    print("|---:|---:|" + "---:|" * len(cols))
    for clients in (1, 4, 12):
        rs = rows.get((mode, clients))
        if not rs:
            continue
        cells = [fmt([r[k] for r in rs], d) for k, _, d in cols]
        print(f"| {clients} | {len(rs)} | " + " | ".join(cells) + " |")

print("\n### Per-run raw values\n")
print("| Mode | Consumers | Run | Agg ticks/s | P50 µs | P99 µs | P99.9 µs | Max µs | Dropped % | Latency samples |")
print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for (mode, clients), rs in sorted(rows.items(), key=lambda kv_: (kv_[0][0], kv_[0][1])):
    for r in sorted(rs, key=lambda r: r["run"]):
        print(f"| {mode} | {clients} | {r['run']} | {r['agg_tps']:,.0f} | {r['p50']:,.1f} | {r['p99']:,.1f} | "
              f"{r['p999']:,.1f} | {r['max']:,.1f} | {r['drop_pct']:.2f} | {r['samples']:,.0f} |")
