#!/usr/bin/env python3
"""Validate one or more JSONL trace files against docs/trace-schema.json.

This is the "schema-contract" check: it has no dependency on LLVM or the
compiled plugin, so it can run for any contributor (or CI job) working only
on tools/ or a UI, purely against the committed fixtures under
test/fixtures/. See PRODUCTIONIZATION.md §4.

Usage: validate_trace.py [--schema docs/trace-schema.json] trace.jsonl [...]
"""

import argparse
import json
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:
    print(
        "error: this script requires the 'jsonschema' package (pip install jsonschema)",
        file=sys.stderr,
    )
    sys.exit(2)


def validate_file(path, validator):
    errors = 0
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"{path}:{lineno}: invalid JSON: {e}", file=sys.stderr)
                errors += 1
                continue
            for err in validator.iter_errors(record):
                print(f"{path}:{lineno}: {err.message}", file=sys.stderr)
                errors += 1
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("traces", nargs="+", help="JSONL trace file(s) to validate")
    ap.add_argument(
        "--schema",
        default="docs/trace-schema.json",
        help="path to the trace JSON Schema",
    )
    args = ap.parse_args()

    schema = json.loads(Path(args.schema).read_text())
    validator = jsonschema.Draft202012Validator(schema)

    total_errors = 0
    total_records = 0
    for trace in args.traces:
        errors = validate_file(trace, validator)
        total_errors += errors
        total_records += sum(1 for _ in open(trace) if _.strip())
        status = "OK" if errors == 0 else f"{errors} error(s)"
        print(f"{trace}: {status}")

    if total_errors:
        print(
            f"\n{total_errors} schema violation(s) across {total_records} records",
            file=sys.stderr,
        )
        sys.exit(1)
    print(
        f"\nall {total_records} records across {len(args.traces)} "
        f"file(s) valid against {args.schema}"
    )


if __name__ == "__main__":
    main()
