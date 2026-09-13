#!/usr/bin/env python3
"""Check trace integrity and instrumentation transparency with matching tools."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    version, clang, opt, plugin = (args.build_dir / "lpta-tools.txt").read_text().splitlines()
    env = os.environ.copy()
    env.pop("LPTA_FILTER_FUNC", None)
    with tempfile.TemporaryDirectory(prefix="lpta-smoke-", dir=args.build_dir) as directory:
        out = Path(directory)
        ir = out / "input.ll"
        subprocess.run([clang, "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
                        str(args.source / "test/sample.c"), "-o", str(ir)], check=True)
        env["LPTA_TRACE_OUT"] = str(out / "trace.jsonl")
        for name, extra in (("plain", []), ("traced", [f"-load-pass-plugin={plugin}"])):
            subprocess.run([opt, *extra, "-passes=default<O2>", "-verify-each", "-S",
                            str(ir), "-o", str(out / f"{name}.ll")], env=env, check=True)
        if (out / "plain.ll").read_bytes() != (out / "traced.ll").read_bytes():
            raise RuntimeError("instrumentation changed optimized IR")
        rows = [json.loads(line) for line in (out / "trace.jsonl").read_text().splitlines()]
        if not rows or [r["seq"] for r in rows] != list(range(len(rows))):
            raise RuntimeError("empty or non-contiguous trace")
        if any(r["unit_kind"] not in {"function", "loop", "module", "scc"} for r in rows):
            raise RuntimeError("unrecognized IR unit")
        if not any(r.get("ir_changed") for r in rows):
            raise RuntimeError("no transformations observed on the sample")
        print(f"LLVM {version}: verifier, trace, and instrumentation transparency passed")


if __name__ == "__main__":
    main()
