#!/usr/bin/env python3
"""Summarize bench/dt_microbench.cpp's JSONL output into a scaling-curve
table: median incremental vs. full-recompute CPU time per (n_blocks,
position_pct), and the speedup ratio between them.

Median, not mean, per RESEARCH.md's stated measurement rigor (compile-time
measurements are right-skewed; a single scheduling hiccup shouldn't move
the summary number).

Usage: lpta_dt_microbench_report.py dt_microbench.jsonl
"""

import argparse
import json
import statistics
import sys
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace", help="JSONL output of bench/dt_microbench.cpp")
    args = ap.parse_args()

    samples = defaultdict(list)  # (n_blocks, position_pct, method) -> [us, ...]
    with open(args.trace) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"warning: {args.trace}:{lineno}: {e}", file=sys.stderr)
                continue
            key = (row["n_blocks"], row["position_pct"], row["method"])
            samples[key].append(row["cpu_time_us"])

    configs = sorted({(n, p) for (n, p, _m) in samples})

    print(
        f"{'n_blocks':>10} {'pos%':>5} {'incr median us':>15} {'full median us':>15} {'speedup':>9}"
    )
    for n, p in configs:
        incr = samples.get((n, p, "incremental"), [])
        full = samples.get((n, p, "full"), [])
        if not incr or not full:
            print(f"{n:>10} {p:>5}  (missing data)")
            continue
        incr_med = statistics.median(incr)
        full_med = statistics.median(full)
        speedup = full_med / incr_med if incr_med else float("inf")
        print(f"{n:>10} {p:>5} {incr_med:>15.1f} {full_med:>15.1f} {speedup:>8.1f}x")

    print()
    print(
        "Speedup = full-recompute median / incremental median. >1x means "
        "incremental update was cheaper at that (size, edit position)."
    )


if __name__ == "__main__":
    main()
