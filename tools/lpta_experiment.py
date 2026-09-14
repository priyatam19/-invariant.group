#!/usr/bin/env python3
"""Build LPTA with one LLVM installation and record a verified experiment."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_trace(path):
    rows = [
        json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()
    ]
    for seq, row in enumerate(rows):
        if row.get("seq") != seq:
            raise ValueError(f"{path}: non-contiguous sequence at event {seq}")
        if row.get("unit_kind") not in {"function", "loop", "module", "scc"}:
            raise ValueError(f"{path}: unknown IR unit at event {seq}")
        for key in ("pass", "unit_name", "invalidated", "incremental_update_candidate"):
            if key not in row:
                raise ValueError(f"{path}: missing {key} at event {seq}")
        if not row["invalidated"]:
            for key in (
                "ir_changed",
                "cfg_changed",
                "before_instr_count",
                "after_instr_count",
            ):
                if key not in row:
                    raise ValueError(f"{path}: missing {key} at event {seq}")
    return rows


def summarize(rows):
    groups = {}
    for row in rows:
        # Aggregate repeated invocations deliberately. A pass/unit-name key is
        # not a unique invocation identity and cannot support event alignment.
        key = (row["unit_kind"], row["pass"])
        group = groups.setdefault(
            key,
            {
                "unit_kind": key[0],
                "pass": key[1],
                "events": 0,
                "changed": 0,
                "candidates": 0,
            },
        )
        group["events"] += 1
        group["changed"] += bool(row.get("ir_changed"))
        group["candidates"] += bool(row.get("incremental_update_candidate"))
    return {
        "events": len(rows),
        "ir_changed_events": sum(bool(r.get("ir_changed")) for r in rows),
        "incremental_update_candidates": sum(
            bool(r.get("incremental_update_candidate")) for r in rows
        ),
        "unit_kinds": dict(Counter(r["unit_kind"] for r in rows)),
        "by_pass_and_kind": [groups[key] for key in sorted(groups)],
    }


def without_seq(rows):
    return [{key: value for key, value in row.items() if key != "seq"} for row in rows]


def run_experiment(args):
    source = args.source.resolve()
    prefix = args.llvm_prefix.resolve()
    output = args.output.resolve()
    # A run never overwrites evidence from an earlier experiment.
    output.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 1,
        "status": "running",
        "commands": [],
        "checks": {},
        "heuristic": "cfg-counts-and-direct-dt-preservation-v1",
    }
    env = os.environ.copy()
    env.pop("LPTA_FILTER_FUNC", None)
    env.pop("LPTA_TRACE_OUT", None)

    def run(command, overrides=None):
        command = [str(part) for part in command]
        manifest["commands"].append({"argv": command, "env": overrides or {}})
        result = subprocess.run(
            command,
            cwd=source,
            env={**env, **(overrides or {})},
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        with (output / "commands.log").open("a") as log:
            log.write(
                json.dumps(manifest["commands"][-1])
                + "\n"
                + result.stdout
                + result.stderr
            )
        if result.returncode:
            raise RuntimeError(
                f"command exited {result.returncode}: {command!r}\n{result.stderr}"
            )
        return result.stdout.strip()

    try:
        tools = {
            name: prefix / "bin" / name for name in ("llvm-config", "clang", "opt")
        }
        for name, path in tools.items():
            if not path.is_file():
                raise ValueError(f"missing matching tool: {path}")
        version = run([tools["llvm-config"], "--version"])
        versions = {name: run([path, "--version"]) for name, path in tools.items()}
        for name in ("clang", "opt"):
            match = re.search(r"version\s+(\d+\.\d+\.\d+)", versions[name])
            if not match or match.group(1) != version:
                raise ValueError(f"{name} does not match llvm-config {version}")
        manifest.update(
            llvm_version=version,
            tool_versions=versions,
            llvm_prefix=str(prefix),
            pipeline=args.pipeline,
        )
        inputs = args.input.resolve() if args.input else source / "test/sample.c"
        manifest["input"] = {"path": str(inputs), "sha256": sha256(inputs)}
        manifest["source_hashes"] = {
            name: sha256(source / name)
            for name in (
                "CMakeLists.txt",
                "src/LPTAInstrumentation.cpp",
                "tools/lpta_report.py",
            )
        }
        manifest["runner_sha256"] = sha256(__file__)
        if (source / ".git").exists():
            manifest["source_revision"] = run(["git", "rev-parse", "HEAD"])
            manifest["source_status"] = run(["git", "status", "--porcelain"])
        build = args.build_dir.resolve() if args.build_dir else output / "build"
        cmakedir = run([tools["llvm-config"], "--cmakedir"])
        run(
            [
                "cmake",
                "-S",
                source,
                "-B",
                build,
                f"-DLLVM_DIR={cmakedir}",
                "-DCMAKE_BUILD_TYPE=Release",
            ]
        )
        run(["cmake", "--build", build, "-j", str(args.jobs)])
        plugin = build / "LPTAInstrumentation.so"
        if not plugin.is_file():
            raise ValueError(f"plugin not found: {plugin}")
        ir = output / "input.ll"
        if inputs.suffix == ".ll":
            ir.write_bytes(inputs.read_bytes())
            manifest["input_mode"] = "existing-ir"
        elif inputs.suffix == ".c":
            run(
                [
                    tools["clang"],
                    "-S",
                    "-emit-llvm",
                    "-O0",
                    "-Xclang",
                    "-disable-O0-optnone",
                    inputs,
                    "-o",
                    ir,
                ]
            )
            manifest["input_mode"] = "matching-clang"
        else:
            raise ValueError("input must be a .c or .ll file")
        manifest["ir_sha256"] = sha256(ir)
        opt = [tools["opt"], f"-passes={args.pipeline}"]
        load = f"-load-pass-plugin={plugin}"
        trace = output / "trace.jsonl"
        run([*opt, load, "-disable-output", ir], {"LPTA_TRACE_OUT": str(trace)})
        rows = read_trace(trace)
        if not rows:
            raise ValueError("experiment produced no trace events")
        manifest["summary"] = summarize(rows)
        manifest["checks"]["trace_schema_and_sequence"] = True
        for name, flags in (("plain", []), ("instrumented", [load])):
            run(
                [*opt, *flags, "-verify-each", "-S", ir, "-o", output / f"{name}.ll"],
                {"LPTA_TRACE_OUT": str(output / "verified-trace.jsonl")},
            )
        if (output / "plain.ll").read_bytes() != (
            output / "instrumented.ll"
        ).read_bytes():
            raise ValueError("instrumentation changed the optimized IR")
        manifest["checks"]["verify_each_and_identical_optimized_ir"] = True
        functions = sorted(
            {r["unit_name"] for r in rows if r["unit_kind"] == "function"}
        )
        missing = "__lpta_missing_function__"
        while missing in functions:
            missing += "_"
        filters = [missing]
        if functions:
            filters.insert(0, functions[0])
        if len(functions) > 1:
            filters.insert(1, ",".join(functions[:2]))
        manifest["filter_event_counts"] = {}
        for index, selected in enumerate(filters):
            filtered_path = output / f"filter-{index}.jsonl"
            run(
                [*opt, load, "-disable-output", ir],
                {"LPTA_TRACE_OUT": str(filtered_path), "LPTA_FILTER_FUNC": selected},
            )
            filtered = read_trace(filtered_path)
            expected = [
                r
                for r in rows
                if r["unit_kind"] in {"function", "loop"}
                and r["unit_name"] in selected.split(",")
            ]
            if without_seq(filtered) != without_seq(expected):
                raise ValueError(f"filter changed the trace contents: {selected}")
            manifest["filter_event_counts"][selected] = len(filtered)
        manifest["checks"]["function_filters"] = True
        run(
            [
                sys.executable,
                source / "tools/lpta_report.py",
                trace,
                "-o",
                output / "report.html",
            ]
        )
        manifest["checks"]["html_generation"] = True
        manifest["artifacts"] = {
            name: sha256(output / name)
            for name in (
                "input.ll",
                "trace.jsonl",
                "plain.ll",
                "instrumented.ll",
                "report.html",
            )
        }
        manifest["status"] = "passed"
    except Exception as error:
        manifest.update(status="failed", error=str(error))
        raise
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(
        json.dumps(
            {
                "manifest": str(output / "manifest.json"),
                "llvm": version,
                "events": len(rows),
                "checks": manifest["checks"],
            },
            indent=2,
        )
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-prefix", type=Path, required=True)
    parser.add_argument(
        "--output", type=Path, required=True, help="new directory for this experiment"
    )
    parser.add_argument(
        "--source", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument(
        "--input", type=Path, help="C or existing LLVM IR; defaults to test/sample.c"
    )
    parser.add_argument(
        "--build-dir", type=Path, help="optional reusable build directory"
    )
    parser.add_argument("--pipeline", default="default<O2>")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        run_experiment(args)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"experiment failed: {error}\n")


if __name__ == "__main__":
    main()
