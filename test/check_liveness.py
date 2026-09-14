#!/usr/bin/env python3
"""Regression test for ROADMAP.md Phase 2 (analysis-liveness-aware candidate
detection). Protects against silently reintroducing the Phase 1 over-firing
bug described in RESEARCH.md §5: without liveness tracking,
incremental_update_candidate fires on passes that never had DominatorTree
computed in the first place.
"""

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    version, clang, opt, plugin = (
        (args.build_dir / "lpta-tools.txt").read_text().splitlines()
    )
    env = os.environ.copy()
    env.pop("LPTA_FILTER_FUNC", None)

    with tempfile.TemporaryDirectory(
        prefix="lpta-liveness-", dir=args.build_dir
    ) as directory:
        out = Path(directory)
        ir = out / "input.ll"
        subprocess.run(
            [
                clang,
                "-S",
                "-emit-llvm",
                "-O0",
                "-Xclang",
                "-disable-O0-optnone",
                str(args.source / "test/sample.c"),
                "-o",
                str(ir),
            ],
            check=True,
        )
        env["LPTA_TRACE_OUT"] = str(out / "trace.jsonl")
        subprocess.run(
            [
                opt,
                f"-load-pass-plugin={plugin}",
                "-passes=default<O2>",
                "-disable-output",
                str(ir),
            ],
            env=env,
            check=True,
        )
        rows = [
            json.loads(line) for line in (out / "trace.jsonl").read_text().splitlines()
        ]

    if not rows:
        raise RuntimeError("empty trace")

    def major_version(record):
        v = record.get("schema_version")
        return int(v.split(".")[0]) if v else None

    if any(major_version(r) is not None and major_version(r) < 2 for r in rows):
        raise RuntimeError("schema_version regressed below major version 2")

    pass_records = [r for r in rows if r.get("record_type", "pass") == "pass"]
    candidates = [r for r in pass_records if r.get("incremental_update_candidate")]
    if not candidates:
        raise RuntimeError(
            "expected at least one incremental_update_candidate on the sample "
            "(if this legitimately changed, update the expectation deliberately)"
        )

    # The core Phase 2 invariant: every candidate must have had DominatorTree
    # actually live going in. A candidate with dt_live_before_pass=False would
    # mean the Phase 1 over-firing bug (RESEARCH.md §5) came back.
    bad = [r for r in candidates if not r.get("dt_live_before_pass")]
    if bad:
        raise RuntimeError(
            f"{len(bad)} candidate(s) have dt_live_before_pass=False -- "
            "the Phase 1 over-firing bug appears to have regressed: "
            f"first offender seq={bad[0].get('seq')} pass={bad[0].get('pass')!r}"
        )

    # At least one candidate should show real, measured wasted work, not just
    # a plausible-looking flag -- this is what makes Phase 2 more than a
    # relabeling of Phase 1's heuristic.
    if not any(r.get("dt_wasted_recompute_count", 0) >= 1 for r in candidates):
        raise RuntimeError(
            "no candidate shows dt_wasted_recompute_count >= 1 on the sample "
            "-- the wasted-recompute counter may not be wired up correctly"
        )

    print(
        f"LLVM {version}: {len(candidates)} liveness-verified "
        f"incremental_update_candidate event(s), all with DT live before the pass"
    )


if __name__ == "__main__":
    main()
