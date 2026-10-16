"""Validate and collect the native-window GUI probe's six fresh-process runs."""
import argparse
import json
import math
import platform
from datetime import datetime, timedelta, timezone
from pathlib import Path


def quantiles(values):
    values = sorted(values)
    return {
        "n": len(values),
        **{
            label: values[max(0, math.ceil(p * len(values)) - 1)]
            for label, p in (("p50_ms", 0.5), ("p95_ms", 0.95), ("p99_ms", 0.99))
        },
        "max_ms": values[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Directory with gui-{1,10,30}MiB-{1,3}tabs.json")
    parser.add_argument("output", type=Path)
    parser.add_argument("--cpu", default=platform.processor() or "unknown")
    args = parser.parse_args()
    runs, summary = [], []
    for size in (1, 10, 30):
        for tabs in (1, 3):
            path = args.input / f"gui-{size}MiB-{tabs}tabs.json"
            run = json.loads(path.read_text(encoding="utf-8"))
            if run["qpa"] != "windows" or not run["native_window_exposed"] or not run["active_window"]:
                raise ValueError(f"Unexposed or inactive native window: {path}")
            if not run.get("windows_foreground", True):
                raise ValueError(f"No confirmed Windows foreground: {path}")
            if run.get("foreground_wait_timed_out", False):
                raise ValueError(f"Foreground wait timed out: {path}")
            if not run.get("native_input_complete", True) or not run.get("draft_drain", {}).get("complete", True):
                raise ValueError(f"Incomplete native input or recovery draft drain: {path}")
            if run["MiB_per_tab"] != size or run["tabs"] != tabs:
                raise ValueError(f"Incorrect workload: {path}")
            expected_phases = {
                "typing", "cursor_horizontal", "cursor_vertical", "scroll_cold", "scroll_warm", "theme_switch"
            }
            if tabs > 1:
                expected_phases.update({"tab_switch_deferred_theme", "tab_switch_warm", "typing_multiple_tabs"})
            if {phase["phase"] for phase in run["phases"]} != expected_phases:
                raise ValueError(f"Incomplete phase matrix: {path}")
            phases = {}
            for phase in run["phases"]:
                if not phase.get("active_throughout", True) or not phase.get("exposed_throughout", True):
                    raise ValueError(f"Inactive/unexposed phase: {path}, {phase['phase']}")
                if not phase.get("windows_foreground_throughout", True):
                    raise ValueError(f"Lost Windows foreground: {path}, {phase['phase']}")
                if phase["hidden_tab_paints"]:
                    raise ValueError(f"Hidden editor paint: {path}, {phase['phase']}")
                if len(phase["samples"]) != phase["count"]:
                    raise ValueError(f"Missing raw samples: {path}, {phase['phase']}")
                painted = [sample["scheduled_to_paint_ms"] for sample in phase["samples"]
                           if "scheduled_to_paint_ms" in sample]
                if (len(painted) != phase["scheduled_to_paint"]["n"]
                        or phase["count"] - len(painted) != phase["inputs_without_paint"]):
                    raise ValueError(f"Inconsistent paint coverage: {path}, {phase['phase']}")
                phase["post_to_dispatch"] = quantiles(
                    max(0, sample["dispatch_ms"] - sample["post_jitter_ms"]) for sample in phase["samples"]
                )
                phases[phase["phase"]] = {
                    key: phase[key]
                    for key in (
                        "count", "cadence_ms", "sync", "scheduled_to_dispatch", "post_to_dispatch",
                        "scheduled_to_paint", "dispatch_to_paint", "paint_duration", "heartbeat_delay",
                        "working_set_before", "working_set_after", "sampled_max_working_set",
                        "coalesced_input_count", "inputs_without_paint", "hidden_tab_paints",
                    )
                }
                for optional in ("active_cache_stats", "text_paint_work"):
                    if optional in phase:
                        phases[phase["phase"]][optional] = phase[optional]
                if all("editor_state_changed" in sample for sample in phase["samples"]):
                    changed = [sample for sample in phase["samples"] if sample["editor_state_changed"]]
                    phases[phase["phase"]]["editor_state_changes"] = {
                        "changed": len(changed), "unchanged": len(phase["samples"]) - len(changed),
                        "changed_sync": quantiles(sample["sync_ms"] for sample in changed) if changed else {"n": 0},
                        "changed_scheduled_to_paint": quantiles(sample["scheduled_to_paint_ms"] for sample in changed
                                                               if "scheduled_to_paint_ms" in sample)
                        if any("scheduled_to_paint_ms" in sample for sample in changed) else {"n": 0},
                    }
                if phase["phase"] == "theme_switch":
                    phases[phase["phase"]]["by_target_theme"] = {
                        target: {
                            "sync": quantiles(s["sync_ms"] for s in phase["samples"] if s["detail"] == target),
                            "scheduled_to_paint": quantiles(
                                s["scheduled_to_paint_ms"] for s in phase["samples"]
                                if s["detail"] == target and "scheduled_to_paint_ms" in s
                            ),
                        }
                        for target in ("github", "gui-vlook")
                    }
            summary.append({"MiB_per_tab": size, "tabs": tabs, "phases": phases})
            runs.append(run)
    hashes = {run["css_sha256"] for run in runs}
    if len(hashes) != 1:
        raise ValueError("Runs use different CSS files")
    result = {
        "date": (
            datetime.fromisoformat(runs[0]["timestamp"].replace("Z", "+00:00"))
            .astimezone(timezone(timedelta(hours=8))).date().isoformat()
        ),
        "timezone": "Asia/Shanghai (individual run timestamps are UTC)",
        "cpu": args.cpu,
        "os": platform.platform(),
        "css_sha256": hashes.pop(),
        "methodology": (
            "Six independent fresh Windows QPA processes, sequentially without a concurrent build/test. "
            "Each runs the actual MainWindow/DocumentTab/EditorView and application bindings, without dictionary "
            "resources or main() platform integration. Native window is active/exposed at validation points. "
            "An independent producer posts synthetic Qt input at fixed cadence, bypassing Windows input dispatch. "
            "Paint completion is a completed QWidget paint event, not compositor/display presentation. "
            "Normal draft timers and theme animations run; user settings/files are isolated; autosave and update checks "
            "are disabled. Timers: 8ms GUI heartbeat, 20ms working-set sampling. Perf scopes and event logging are enabled. "
            "Nearest-rank quantiles, 80 operations per continuous phase, 12 alternating theme actions (6 per theme), "
            "2 first activations with deferred themes. p99 equals max for these small samples; not a population latency "
            "guarantee. Coalesced operations share the first completed viewport paint after dispatch returns; "
            "an operation that paints synchronously records its last nested viewport paint instead. "
            "A tab switched away before its first paint is reported as inputs_without_paint; paint quantiles exclude it. "
            "Working sets and sampled maxima are not heap sizes or guaranteed peaks."
        ),
        "summary": summary,
        "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {len(runs)} native GUI runs: {args.output}")
    for row in summary:
        print(f"{row['MiB_per_tab']} MiB x {row['tabs']} tabs")
        for name, phase in row["phases"].items():
            print(
                f"  {name:30} sync p95={phase['sync']['p95_ms']:7.1f} ms, "
                f"scheduled paint p95={phase['scheduled_to_paint']['p95_ms']:8.1f} ms, "
                f"memory={phase['working_set_after'] / 2**20:7.1f} MiB"
            )


if __name__ == "__main__":
    main()
