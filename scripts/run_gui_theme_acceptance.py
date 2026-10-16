"""Measure actual Windows MainWindow theme switches; reject invalid foreground runs."""
import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path


TARGETS = ("gui-vlook", "github", "gui-phycat")
MATRIX = ((1, 1), (10, 3), (30, 1), (1, 3), (10, 1), (30, 3))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def distribution(values):
    values = sorted(values)
    if not values:
        return {"n": 0}
    return {"n": len(values), **{f"p{p}_ms": values[math.ceil(p / 100 * len(values)) - 1]
                                for p in (50, 95, 99)},
            "max_ms": values[-1], "mean_ms": sum(values) / len(values)}


def inspect_run(raw, size, tabs, cycles, profile):
    require(raw["qpa"] == "windows" and raw["theme_acceptance"], "Wrong GUI/mode")
    require(raw["MiB_per_tab"] == size and raw["tabs"] == tabs, "Wrong dimensions")
    require(raw["profiling_mode"] == profile and not raw["detailed_trace"], "Wrong profiling mode")
    require(not raw["deep_css_profile"] and not raw["scheduler_sampling"], "Unexpected detailed instrumentation")
    require(raw["input_delivery"] == "qt_posted" and not raw["typing_only"] and not raw["input_phases_only"],
            "Wrong command/input delivery")
    require(raw["startup_theme"] == "night", "Wrong startup theme")
    require(raw["foreground_wait_required"] and not raw["foreground_wait_timed_out"], "Foreground wait failed")
    require(raw["active_window"] and raw["windows_foreground"] and raw["native_window_exposed"],
            "Window not active/foreground/exposed")
    require(raw["draft_snapshots_enabled"] and raw["draft_drain"]["complete"], "Drafts disabled or incomplete")
    for tab in raw["draft_drain"]["tabs"]:
        # Unmodified hidden tabs need no draft. Modified tabs are explicitly acknowledged.
        require(tab["acknowledged"] in (0, tab["revision"]), "Draft acknowledgement is stale")
    expected = [("theme_switch", cycles * 3, 1500), ("typing_after_theme", 8, 100)]
    if tabs > 1:
        expected += [("tab_switch_deferred_theme", tabs - 1, 1500),
                     ("typing_multiple_tabs", 8, 100), ("tab_switch_warm", 8, 100)]
    require([(p["phase"], p["count"], p["cadence_ms"]) for p in raw["phases"]] == expected,
            "Incomplete or different phase sequence")
    edited = {row["tab"] for phase in raw["phases"] if phase["phase"].startswith("typing")
              for row in phase["samples"]}
    require(len(raw["draft_drain"]["tabs"]) == tabs, "Missing draft tab checkpoints")
    for index in edited:
        tab = raw["draft_drain"]["tabs"][index]
        require(tab["acknowledged"] == tab["revision"], "Edited tab has no current draft")
    for phase in raw["phases"]:
        require(phase["active_throughout"] and phase["exposed_throughout"] and phase["windows_foreground_throughout"],
                f"Lost activation/exposure/foreground: {phase['phase']}")
        require(phase["hidden_tab_paints"] == 0 and phase["inputs_without_paint"] == 0,
                "Hidden paint or missing first paint")
        rows = phase["samples"]
        require(len(rows) == phase["count"], "Missing operations")
        for row in rows:
            edges = (row["due_at_ms"], row["posted_at_ms"], row["dispatch_at_ms"],
                     row["end_at_ms"], row["paint_at_ms"])
            require(all(math.isfinite(x) for x in edges), "Invalid clock value")
            require(edges[1] <= edges[2] <= edges[3] <= edges[4] + .01, "Reversed clock edges")
            for key, calculated in (("sync_ms", edges[3] - edges[2]),
                                    ("dispatch_ms", edges[2] - edges[0]),
                                    ("post_jitter_ms", edges[1] - edges[0]),
                                    ("scheduled_to_paint_ms", edges[4] - edges[0]),
                                    ("dispatch_to_paint_ms", edges[4] - edges[2]),
                                    ("post_to_dispatch_ms", edges[2] - edges[1])):
                require(abs(row[key] - calculated) < .01, f"Wrong clock difference: {key}")
            require(all(value >= -.01 for value in row["stages_to_first_paint_ms"].values()), "Negative stage time")
        for key, column in (("sync", "sync_ms"), ("scheduled_to_dispatch", "dispatch_ms"),
                            ("scheduled_to_paint", "scheduled_to_paint_ms"),
                            ("dispatch_to_paint", "dispatch_to_paint_ms"), ("producer_jitter", "post_jitter_ms")):
            calculated = distribution([row[column] for row in rows])
            require(all(abs(phase[key][k] - v) < .01 for k, v in calculated.items()), f"Wrong distribution: {key}")
        if phase["phase"].startswith("typing"):
            require(all(row["editor_state_changed"] for row in rows), "Input did not edit the document")
            require(phase["document_revision"] > phase["document_revision_before"], "Input revision unchanged")
    theme = raw["phases"][0]
    require([row["detail"] for row in theme["samples"]] == list(TARGETS) * cycles, "Wrong theme sequence")
    require(theme["coalesced_input_count"] == 0, "Theme operations accumulated before painting")
    require(all(row["first_paint_full_viewport"] for row in theme["samples"]), "Partial paint used as theme completion")
    require(all(not row["pending_inputs_after_handler"] for row in theme["samples"]), "Theme queue accumulated")
    visuals = raw["visual_audit"]
    require(len(visuals) == 6 and [v["target"] for v in visuals] == list(TARGETS) * 2, "Incomplete visual audit")
    for index, visual in enumerate(visuals):
        require(visual["active"] and visual["exposed"] and visual["windows_foreground"], "Invalid visual window")
        require(visual["blocks"] and Path(visual["screenshot"]).is_file(), "Missing visual artifact")
        require(visual["caret_hit_offset"] == 5, "Visible caret does not round-trip")
        if index < 3:
            require(visual["full_counter_reference"], "Missing full numbering reference")
        else:
            for field in ("blocks", "caret", "font", "text_color"):
                require(visual[field] == visuals[index - 3][field], f"Stale style/geometry on repeat: {field}")
    return {"accepted": True, "size_mib": size, "tabs": tabs, "cycles": cycles, "profile": profile,
            "targets": {target: {"first_action": next(r for r in theme["samples"] if r["detail"] == target),
                                 "repeated_to_paint": distribution([r["dispatch_to_paint_ms"] for r in theme["samples"]
                                                                    if r["detail"] == target][1:])}
                        for target in TARGETS},
            "working_set_before_visual_audit": raw["working_set_before_visual_audit"],
            "draft_drain": raw["draft_drain"]}


def environment(root, phycat, profile, wait_seconds):
    preset = json.loads((root / "build/generators/CMakePresets.json").read_text(encoding="utf-8"))
    result = {k: v for k, v in os.environ.items() if not k.startswith("MUFFIN_GUI_") and k != "MUFFIN_CSS_DEEP_PROFILE"}
    result.update(QT_QPA_PLATFORM="windows", QT_PLUGIN_PATH=preset["testPresets"][0]["environment"]["QT_PLUGIN_PATH"],
                  MUFFIN_GUI_REQUIRE_FOREGROUND="1", MUFFIN_GUI_FOREGROUND_WAIT_MS=str(wait_seconds * 1000),
                  MUFFIN_GUI_THEME_ACCEPTANCE="1", MUFFIN_GUI_PHYCAT_SOURCE=str(phycat), MUFFIN_GUI_PROFILE=profile,
                  PYTHONDONTWRITEBYTECODE="1")
    result["PATH"] = preset["configurePresets"][0]["environment"]["QT_HOST_PATH"] + "/bin;" + result.get("PATH", "")
    return result


def run(args):
    root = Path(__file__).resolve().parent.parent
    destination = args.output_directory.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    phycat = args.phycat.resolve()
    files = [root / f"build/Release/{name}" for name in
             ("MuffinGuiPerformanceProbe.exe", "Muffin.exe", "MuffinCore.dll", "MuffinUi.dll")]
    files += [root / "tests/fixtures/theme/vlook-fancy-dark/vlook-fancy-dark.css",
              root / "resources/themes/github.css", root / "resources/themes/night.css",
              root / "resources/themes/document-base.css", phycat,
              phycat.parent / "phycat/phycat.dark.css", phycat.parent / "phycat/Cascadia-Code-Regular.ttf",
              phycat.parent / "phycat/LXGWWenKai-Regular.ttf"]
    pinned = {str(p): digest(p) for p in files}
    env = environment(root, phycat, args.profile, args.foreground_wait_seconds)
    records = []
    attempts = []
    failure = None
    matrix = MATRIX if not args.pilot else ((1, 3),)
    try:
        for repeat in range(1, args.repetitions + 1):
            for size, tabs in (matrix if repeat % 2 else tuple(reversed(matrix))):
                output = destination / f"gui-theme-{size}MiB-{tabs}tabs-r{repeat}.json"
                attempts.append({"path": str(output), "size_mib": size, "tabs": tabs, "repeat": repeat,
                                 "accepted": False})
                print(f"GUI {size} MiB/{tabs} tabs, repeat {repeat}, {args.cycles} cycles ({args.profile})", flush=True)
                with Path(str(output) + ".log").open("wb") as log:
                    process = subprocess.run([str(files[0]), str(output), str(size), str(tabs), str(args.cycles)],
                                             cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                             timeout=600, check=False)
                attempts[-1]["exit_code"] = process.returncode
                require(process.returncode == 0, f"Probe exit {process.returncode}; retained: {output}.log")
                record = inspect_run(json.loads(output.read_text(encoding="utf-8")), size, tabs, args.cycles, args.profile)
                record.update(path=str(output), repeat=repeat)
                records.append(record)
                attempts[-1]["accepted"] = True
                Path(str(output) + ".validation.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
                require({str(p): digest(p) for p in files} == pinned, "Binaries or fixed resources changed")
                print("Accepted: " + ", ".join(f"{t} repeat p95 {record['targets'][t]['repeated_to_paint']['p95_ms']:.1f}ms"
                                                for t in TARGETS), flush=True)
    except (OSError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        failure = str(error) or "Interrupted"
        if attempts:
            attempts[-1]["error"] = failure
    summary = {"timestamp": datetime.now().astimezone().isoformat(), "complete": failure is None,
               "expected_runs": len(matrix) * args.repetitions, "accepted_runs": len(records),
               "cycles": args.cycles, "profile": args.profile, "pilot": args.pilot,
               "runner_error": failure, "pinned_sha256": pinned, "records": records, "attempts": attempts,
               "method": "Sequential fresh Windows QPA MainWindow processes. Startup night; each cycle VLOOK, GitHub, Phycat. "
                         "Qt-posted standard menu actions and functional input checks, never native-keyboard latency. "
                         "Every phase requires Windows foreground, activation and exposure. Paint completion is QWidget "
                         "backing-store work, not DWM presentation. Nearest-rank empirical quantiles; all raw samples retained. "
                         "Recovery drafts enabled and drained; visual/reference work occurs after timed phases and memory checkpoint. "
                         "Per-operation stage times are exclusive; named scope intervals are inclusive and may overlap."}
    (destination / "acceptance.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Accepted {len(records)}/{summary['expected_runs']}: {destination / 'acceptance.json'}", flush=True)
    if failure:
        print(failure, file=sys.stderr)
    return 0 if failure is None else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--phycat", type=Path, default=Path(os.environ.get("APPDATA", ".")) /
                        "Muffin/Muffin/themes/phycat-abyss.css")
    parser.add_argument("--cycles", type=int, default=8)
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--profile", choices=("minimal", "off"), default="minimal")
    parser.add_argument("--foreground-wait-seconds", type=int, default=30)
    parser.add_argument("--pilot", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32" or not (4 <= args.cycles <= 60 and 1 <= args.repetitions <= 10 and
                                      1 <= args.foreground_wait_seconds <= 3600):
        parser.error("Use Windows; cycles=4..60, repetitions=1..10, foreground wait=1..3600 seconds")
    try:
        return run(args)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
