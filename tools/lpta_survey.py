#!/usr/bin/env python3
"""Reproduce the historical file-level LLVM survey on a pinned clean checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


PATTERNS = {
    "none": r"PreservedAnalyses::none\(\)",
    "incremental_dt": r"DomTreeUpdater|DT->applyUpdates\(|DT->insertEdge\(|DT->deleteEdge\(|DominatorTree::insertEdge|\.applyUpdates\(",
    "cfg_utility": r"SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(",
    "run_same_line": r"PreservedAnalyses [A-Za-z_:<>]*::run\(",
    "run_next_line": r"^PreservedAnalyses\n[^\n]*::run\(",
}


def survey(source):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()
    if git("status", "--porcelain"):
        raise ValueError("source checkout must be clean to identify reproducible evidence")
    root = source / "llvm/lib/Transforms"
    files = {str(path.relative_to(root)): path.read_text() for path in sorted(root.rglob("*.cpp"))}
    if not files:
        raise ValueError("llvm/lib/Transforms contains no .cpp files")
    hits = {name: {path for path, text in files.items() if re.search(pattern, text, re.M)}
            for name, pattern in PATTERNS.items()}
    candidates = (hits["none"] & hits["cfg_utility"]) - hits["incremental_dt"]
    return {
        "schema_version": 1, "llvm_commit": git("rev-parse", "HEAD"),
        "method": "historical-file-level-regex; comments and helpers can match; no dataflow proof",
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "patterns": PATTERNS,
        "counts": {"transform_cpp_files": len(files), "none_files": len(hits["none"]),
                   "none_occurrences": sum(len(re.findall(PATTERNS["none"], s)) for s in files.values()),
                   "incremental_dt_files": len(hits["incremental_dt"]),
                   "cfg_utility_files": len(hits["cfg_utility"]),
                   "raw_candidate_files": len(candidates),
                   "approximate_run_implementations": sum(len(re.findall(PATTERNS[key], s, re.M))
                       for s in files.values() for key in ("run_same_line", "run_next_line"))},
        "raw_candidates": {name: {"sha256": hashlib.sha256(files[name].encode()).hexdigest(),
                                  "none_lines": [i for i, line in enumerate(files[name].splitlines(), 1)
                                                 if re.search(PATTERNS["none"], line)],
                                  "cfg_utility_lines": [i for i, line in enumerate(files[name].splitlines(), 1)
                                                        if re.search(PATTERNS["cfg_utility"], line)]}
                           for name in sorted(candidates)},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = survey(args.source.resolve())
        with args.output.open("x") as output:
            output.write(json.dumps(result, indent=2) + "\n")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"survey failed: {error}\n")
    print(json.dumps(result["counts"], indent=2))


if __name__ == "__main__":
    main()
