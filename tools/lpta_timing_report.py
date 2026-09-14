#!/usr/bin/env python3
"""Summarize the Tier 2 experiment: how much analysis-recompute CPU time in
a real trace is attributable to a wasted recompute (invalidated, then later
actually recomputed) vs. computation that was needed regardless.

Reads the "analysis" record_type rows LPTAInstrumentation.cpp emits (one per
actual analysis (re)computation, with CPU timing) -- see
docs/trace-schema.json. Ignores "pass" records entirely.

Usage: lpta_timing_report.py trace.jsonl [trace2.jsonl ...]
"""

import argparse
import json
import sys
from collections import defaultdict


def load_analysis_records(path):
    records = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"warning: {path}:{lineno}: {e}", file=sys.stderr)
                continue
            if row.get("record_type") == "analysis":
                records.append(row)
    return records


def summarize(records):
    by_analysis = defaultdict(
        lambda: {"computes": 0, "total_us": 0, "wasted_computes": 0, "wasted_us": 0}
    )
    for r in records:
        d = by_analysis[r["analysis"]]
        d["computes"] += 1
        d["total_us"] += r["cpu_time_us"]
        if r.get("wasted_recompute"):
            d["wasted_computes"] += 1
            d["wasted_us"] += r["cpu_time_us"]
    return dict(by_analysis)


def print_report(summary, source):
    print(f"Tier 2 analysis-timing summary: {source}")
    print(
        f"{'analysis':<26} {'computes':>9} {'wasted':>7} {'total us':>10} {'wasted us':>10} {'% us wasted':>12}"
    )
    total_us = 0
    total_wasted_us = 0
    for name in sorted(summary):
        d = summary[name]
        pct = (d["wasted_us"] / d["total_us"] * 100) if d["total_us"] else 0.0
        print(
            f"{name:<26} {d['computes']:>9} {d['wasted_computes']:>7} "
            f"{d['total_us']:>10} {d['wasted_us']:>10} {pct:>11.1f}%"
        )
        total_us += d["total_us"]
        total_wasted_us += d["wasted_us"]
    overall_pct = (total_wasted_us / total_us * 100) if total_us else 0.0
    print(
        f"{'TOTAL':<26} {'':>9} {'':>7} {total_us:>10} {total_wasted_us:>10} {overall_pct:>11.1f}%"
    )
    print()
    print(
        f"{overall_pct:.1f}% of all measured analysis-recompute CPU time across "
        f"DominatorTree/LoopInfo/MemorySSA/ScalarEvolution was spent on a "
        f"recompute that followed an invalidation nothing needed until it was "
        f"needed again -- i.e. CPU time a correctly-preserving pass would not "
        f"have cost."
    )
    print(
        "Caveat: on a small/synthetic input, cpu_time_us values can be at or "
        "below OS timer-resolution noise (see docs/trace-schema.json); treat "
        "this number as indicative, not conclusive, until run against a real "
        "benchmark corpus."
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("traces", nargs="+", help="JSONL trace file(s) to summarize")
    args = ap.parse_args()

    all_records = []
    for trace in args.traces:
        all_records.extend(load_analysis_records(trace))

    if not all_records:
        print(
            "no 'analysis' record_type rows found in the given trace(s) -- "
            "was this trace produced before schema_version 2.1.0?",
            file=sys.stderr,
        )
        sys.exit(1)

    summary = summarize(all_records)
    print_report(summary, ", ".join(args.traces))


if __name__ == "__main__":
    main()
