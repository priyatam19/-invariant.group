"""Regression checks for trace validation and cross-version attribution."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def module(name):
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).resolve().parents[1] / "tools" / f"{name}.py"
    )
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


experiment = module("lpta_experiment")
comparison = module("lpta_compare")


def manifest():
    return {
        "schema_version": 1,
        "status": "passed",
        "llvm_version": "18.1.3",
        "pipeline": "default<O2>",
        "heuristic": "v1",
        "ir_sha256": "ir",
        "input": {"sha256": "source"},
        "source_hashes": {"src/LPTAInstrumentation.cpp": "plugin"},
        "summary": experiment.summarize(
            [
                {"unit_kind": "function", "pass": "P", "ir_changed": True},
                {"unit_kind": "function", "pass": "P", "ir_changed": False},
            ]
        ),
    }


class ExperimentTests(unittest.TestCase):
    def test_repeated_invocations_are_aggregated(self):
        before = manifest()
        after = copy.deepcopy(before)
        after["summary"] = experiment.summarize(
            [{"unit_kind": "function", "pass": "P", "ir_changed": True}]
        )
        diff = comparison.compare(before, after)
        self.assertEqual(
            diff["pass_changes"][0]["delta"],
            {"events": -1, "changed": 0, "candidates": 0},
        )

    def test_frontend_changes_are_labeled(self):
        before, after = manifest(), manifest()
        after["ir_sha256"] = "new-ir"
        self.assertEqual(
            comparison.compare(before, after)["comparison"],
            "same-source-includes-frontend-changes",
        )

    def test_refuse_confounded_comparison(self):
        for change in ("pipeline", "heuristic", "plugin", "input", "status"):
            with self.subTest(change=change):
                before, after = manifest(), manifest()
                if change == "plugin":
                    after["source_hashes"]["src/LPTAInstrumentation.cpp"] = "new"
                elif change == "input":
                    after["input"]["sha256"] = "new-source"
                    after["ir_sha256"] = "new-ir"
                else:
                    after[change] = "different"
                with self.assertRaises(ValueError):
                    comparison.compare(before, after)

    def test_reject_dropped_trace_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text(json.dumps({"seq": 1}) + "\n")
            with self.assertRaisesRegex(ValueError, "non-contiguous"):
                experiment.read_trace(path)

    def test_invalidated_units_need_no_after_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            row = {
                "seq": 0,
                "unit_kind": "loop",
                "pass": "LoopDeletionPass",
                "unit_name": "f",
                "invalidated": True,
                "incremental_update_candidate": False,
            }
            path.write_text(json.dumps(row) + "\n")
            self.assertEqual(experiment.read_trace(path), [row])


if __name__ == "__main__":
    unittest.main()
