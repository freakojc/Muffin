"""Deterministic validation tests. All fixtures here are invented, never GUI evidence."""
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from summarize_input_comparison import DELIVERIES, ENVIRONMENT, MODES, collect, inspect_run
from run_input_comparison import run_comparison


def fixture(kind="qt", mode="off"):
    raw = []
    for i, (due, posted, dispatch, end, pending, pending_end, subsequent) in enumerate([
        (0, 1, 2, 100, 0, 1, 3), (50, 51, 105, 150, 1, 1, 2),
        (100, 101, 155, 200, 1, 1, 1), (150, 151, 205, 250, 0, 0, 0),
    ]):
        raw.append({
            "due_at_ms": due, "posted_at_ms": posted, "dispatch_at_ms": dispatch,
            "end_at_ms": end, "paint_start_at_ms": 260, "paint_at_ms": 275,
            "detail": "x/native" if kind == "native" else "x",
            "editor_state_changed": True, "sync_ms": end - dispatch,
            "post_jitter_ms": posted - due, "dispatch_ms": dispatch - due,
            "post_to_dispatch_ms": dispatch - posted, "scheduled_to_paint_ms": 275 - due,
            "dispatch_to_paint_ms": 275 - dispatch, "handler_end_to_paint_start_ms": 260 - end,
            "pending_inputs_at_dispatch": pending, "pending_inputs_after_handler": pending_end,
            "subsequent_inputs_before_paint": subsequent,
        })
    phase = {
        "phase": "typing", "active_throughout": True, "exposed_throughout": True,
        "windows_foreground_throughout": True, "cadence_ms": 50, "count": 4,
        "hidden_tab_paints": 0, "document_revision_before": 2, "document_revision": 6,
        "samples": raw,
    }
    # An independent, integer-rank reference for the deliberately small fixture.
    for key, field in (("sync", "sync_ms"), ("scheduled_to_dispatch", "dispatch_ms"),
                       ("scheduled_to_paint", "scheduled_to_paint_ms"), ("dispatch_to_paint", "dispatch_to_paint_ms")):
        values = sorted(s[field] for s in raw)
        phase[key] = {"n": 4, "p50_ms": values[1], "p95_ms": values[3], "p99_ms": values[3],
                      "max_ms": values[-1], "mean_ms": sum(values) / 4}
    run = {k: "fixed" for k in ENVIRONMENT}
    run.update({
        "qpa": "windows", "MiB_per_tab": 1, "tabs": 1, "active_window": True,
        "native_window_exposed": True, "windows_foreground": True, "foreground_wait_required": True,
        "foreground_wait_timed_out": False, "typing_only": True, "draft_snapshots_enabled": True,
        "deep_css_profile": False, "scheduler_sampling": False, "profiling_mode": mode,
        "detailed_trace": mode == "detailed", "input_delivery": DELIVERIES[kind],
        "native_input_complete": True, "native_posts": 4, "native_received": 4,
        "native_aborted": False, "native_contaminated": False,
        "draft_drain": {"complete": True, "schema_version": 2, "tabs": [{"revision": 6, "acknowledged": 6}]},
        "phases": [phase],
    })
    return run


class InputComparisonValidationTest(unittest.TestCase):
    def test_accepts_both_deliveries_and_counts_backlog(self):
        for kind in DELIVERIES:
            result = inspect_run(fixture(kind), 4)
            self.assertTrue(result["accepted"], result["reasons"])
            self.assertEqual(result["metrics"]["pending_inputs_at_dispatch"]["max"], 1)
            self.assertEqual(result["metrics"]["subsequent_inputs_before_paint"]["max"], 3)
            self.assertEqual(result["metrics"]["scheduled_to_paint_ms"]["p50"], 175)

    def test_rejects_invalid_measurements(self):
        mutations = [
            lambda r: r.pop("windows_foreground"),
            lambda r: r.update(windows_foreground=False),
            lambda r: r.update(foreground_wait_timed_out=True),
            lambda r: r.update(native_received=3),
            lambda r: r.update(native_contaminated=True),
            lambda r: r.update(typing_only=False),
            lambda r: r.update(detailed_trace=True),
            lambda r: r.update(deep_css_profile=True),
            lambda r: r.pop("font"),
            lambda r: r["draft_drain"]["tabs"][0].update(acknowledged=5),
            lambda r: r["phases"][0].update(windows_foreground_throughout=False),
            lambda r: r["phases"][0].update(document_revision=5),
            lambda r: r["phases"][0]["samples"].pop(),
            lambda r: r["phases"][0]["samples"][0].update(pending_inputs_at_dispatch=9),
            lambda r: r["phases"][0]["samples"][0].update(end_at_ms=200),
            lambda r: r["phases"][0]["samples"][0].pop("paint_start_at_ms"),
            lambda r: r["phases"][0]["samples"][0].update(editor_state_changed=False),
            lambda r: r["phases"][0]["samples"][1].update(due_at_ms=10),
            lambda r: r["phases"][0]["samples"][0].update(detail="other"),
            lambda r: r["phases"][0]["sync"].update(p95_ms=0),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                run = fixture("native")
                mutate(run)
                self.assertFalse(inspect_run(run, 4)["accepted"])

    def test_complete_collection_and_environment_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for repeat in (1, 2):
                for kind in DELIVERIES:
                    for mode in MODES:
                        (directory / f"input-{kind}-{mode}-{repeat}.json").write_text(
                            json.dumps(fixture(kind, mode)), encoding="utf-8")
            (directory / "input-native-off-preflight.json").write_text(json.dumps(fixture("native")), encoding="utf-8")
            (directory / "input-native-off-preflight.json.validation.json").write_text("{}", encoding="utf-8")
            (directory / "input-native-off-preflight-crash.json").write_bytes(b'{"truncated":\xff')
            result = collect(directory, 2, 4)
            self.assertTrue(result["complete"])
            self.assertEqual(result["accepted_matched_runs"], 12)
            self.assertEqual(len(result["records"]), 14)
            crash = next(r for r in result["records"] if r["file"].endswith("preflight-crash.json"))
            self.assertFalse(crash["accepted"])
            self.assertEqual(crash["raw_bytes_base64"], "eyJ0cnVuY2F0ZWQiOv8=")
            bad = fixture()
            bad["font"] = "different"
            (directory / "input-qt-off-2.json").write_text(json.dumps(bad), encoding="utf-8")
            result = collect(directory, 2, 4)
            self.assertFalse(result["complete"])
            self.assertEqual(result["accepted_matched_runs"], 11)
            self.assertEqual(result["missing_or_invalid_runs"], ["input-qt-off-2.json"])
            self.assertEqual(next(r["raw"] for r in result["records"] if r["file"] == "input-qt-off-2.json"), bad)


class InputComparisonRunnerTest(unittest.TestCase):
    def prepare(self, directory):
        workspace = Path(directory)
        (workspace / "build/Release").mkdir(parents=True)
        (workspace / "build/Release/MuffinGuiPerformanceProbe.exe").write_bytes(b"fake executable; never launched")
        (workspace / "build/generators").mkdir()
        (workspace / "build/generators/CMakePresets.json").write_text(json.dumps({
            "configurePresets": [{"environment": {"QT_HOST_PATH": "C:\\Qt"}}],
            "testPresets": [{"environment": {"QT_PLUGIN_PATH": "C:\\Qt\\plugins"}}],
        }), encoding="utf-8")
        return workspace

    def test_runs_fresh_children_in_reversed_order_without_environment_leak(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = self.prepare(temporary)
            observed = []

            def child(command, **kwargs):
                env = kwargs["env"]
                kind = "native" if env.get("MUFFIN_GUI_NATIVE_INPUT") else "qt"
                mode = env["MUFFIN_GUI_PROFILE"]
                self.assertEqual(env.get("MUFFIN_GUI_TRACE"), "1" if mode == "detailed" else None)
                self.assertEqual(env["MUFFIN_GUI_FOREGROUND_WAIT_MS"], "600000")
                self.assertNotIn("MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT", env)
                observed.append((kind, mode))
                Path(command[1]).write_text(json.dumps(fixture(kind, mode)), encoding="utf-8")
                return SimpleNamespace(returncode=0)

            with patch.dict(os.environ, {"MUFFIN_GUI_TRACE": "stale", "MUFFIN_GUI_NATIVE_INPUT": "stale",
                                          "MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT": "stale"}):
                original = dict(os.environ)
                with patch("run_input_comparison.subprocess.run", side_effect=child):
                    self.assertEqual(run_comparison(workspace, workspace / "result", 4, 2, 600), 0)
                self.assertEqual(dict(os.environ), original)
            self.assertEqual(len(observed), 13)
            self.assertEqual(observed[:3], [("native", "off"), ("native", "off"), ("qt", "off")])
            self.assertEqual(observed[-2:], [("qt", "off"), ("native", "off")])
            summary = json.loads((workspace / "result/comparison.json").read_text(encoding="utf-8"))
            self.assertTrue(summary["complete"])
            self.assertEqual(summary["accepted_matched_runs"], 12)

    def test_invalid_preflight_stops_and_retains_reason(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = self.prepare(temporary)

            def child(command, **kwargs):
                invalid = fixture("native")
                invalid["windows_foreground"] = False
                Path(command[1]).write_text(json.dumps(invalid), encoding="utf-8")
                return SimpleNamespace(returncode=0)

            with patch("run_input_comparison.subprocess.run", side_effect=child) as process:
                self.assertEqual(run_comparison(workspace, workspace / "result", 4, 2, 600), 1)
            self.assertEqual(process.call_count, 1)
            summary = json.loads((workspace / "result/comparison.json").read_text(encoding="utf-8"))
            self.assertFalse(summary["complete"])
            self.assertEqual(summary["accepted_matched_runs"], 0)
            self.assertIn("windows_foreground not confirmed", summary["records"][0]["reasons"])
            self.assertTrue(summary["runner_error"])

    def test_refuses_existing_destination_before_starting_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = self.prepare(temporary)
            result = workspace / "result"
            result.mkdir()
            (result / "old.json").write_text("old observation", encoding="utf-8")
            with patch("run_input_comparison.subprocess.run") as process:
                with self.assertRaises(FileExistsError):
                    run_comparison(workspace, result, 4, 2, 600)
            process.assert_not_called()
            self.assertEqual((result / "old.json").read_text(encoding="utf-8"), "old observation")


if __name__ == "__main__":
    unittest.main()
