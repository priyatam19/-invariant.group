#!/usr/bin/env python3
"""Compare experiment manifests by pass/kind aggregate, without aligning events."""
import argparse
import json
from pathlib import Path


def compare(before, after):
    for manifest in (before, after):
        if manifest.get("schema_version") != 1 or manifest.get("status") != "passed":
            raise ValueError("comparison requires two successful schema-version-1 experiments")
    if before["pipeline"] != after["pipeline"] or before["heuristic"] != after["heuristic"]:
        raise ValueError("pipeline and candidate heuristic must match")
    if before["source_hashes"]["src/LPTAInstrumentation.cpp"] != after["source_hashes"]["src/LPTAInstrumentation.cpp"]:
        raise ValueError("plugin source differs; compiler-version effects cannot be isolated")
    same_ir = before["ir_sha256"] == after["ir_sha256"]
    same_source = before["input"]["sha256"] == after["input"]["sha256"]
    if not same_ir and not same_source:
        raise ValueError("experiments must share the source input or exact IR")
    groups = [{(r["unit_kind"], r["pass"]): r for r in m["summary"]["by_pass_and_kind"]}
              for m in (before, after)]
    changes = []
    for key in sorted(groups[0].keys() | groups[1].keys()):
        old, new = [group.get(key, {}) for group in groups]
        delta = {name: new.get(name, 0) - old.get(name, 0)
                 for name in ("events", "changed", "candidates")}
        if any(delta.values()):
            changes.append({"unit_kind": key[0], "pass": key[1], "delta": delta})
    return {"before_llvm": before["llvm_version"], "after_llvm": after["llvm_version"],
            "comparison": "same-ir" if same_ir else "same-source-includes-frontend-changes",
            "aggregation": "pass-and-unit-kind; repeated and enclosing invocations remain inclusive",
            "total_delta": {name: after["summary"][name] - before["summary"][name]
                            for name in ("events", "ir_changed_events", "incremental_update_candidates")},
            "pass_changes": changes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    try:
        result = compare(json.loads(args.before.read_text()), json.loads(args.after.read_text()))
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"cannot compare experiments: {error}\n")
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
