"""Run guarded native/Qt typing comparisons on an interactive Windows desktop."""
import argparse
import json
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from summarize_input_comparison import collect, inspect_run


def probe_environment(presets, original, wait_seconds):
    environment = dict(original)
    for name in ("MUFFIN_GUI_NATIVE_INPUT", "MUFFIN_GUI_TRACE", "MUFFIN_GUI_SCHEDULER_SAMPLES",
                 "MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT", "MUFFIN_CSS_DEEP_PROFILE"):
        environment.pop(name, None)
    qt = presets["configurePresets"][0]["environment"]
    environment.update({
        "PATH": qt["QT_HOST_PATH"] + "\\bin;" + original.get("PATH", ""),
        "QT_PLUGIN_PATH": presets["testPresets"][0]["environment"]["QT_PLUGIN_PATH"],
        "QT_QPA_PLATFORM": "windows", "MUFFIN_GUI_INPUT_ONLY": "1", "MUFFIN_GUI_TYPING_ONLY": "1",
        "MUFFIN_GUI_PIN_WINDOW": "1", "MUFFIN_GUI_REQUIRE_FOREGROUND": "1",
        "MUFFIN_GUI_FOREGROUND_WAIT_MS": str(wait_seconds * 1000),
    })
    return environment


def run_comparison(workspace, destination, samples, repetitions, wait_seconds):
    probe = workspace / "build/Release/MuffinGuiPerformanceProbe.exe"
    if not probe.is_file():
        raise ValueError("Build MuffinGuiPerformanceProbe with the conan-release preset first")
    presets = json.loads((workspace / "build/generators/CMakePresets.json").read_text(encoding="utf-8"))
    environment = probe_environment(presets, os.environ, wait_seconds)
    # Never overwrite earlier raw observations, even when repeating a failed run.
    destination.mkdir(parents=True, exist_ok=False)
    failure = None
    try:
        sequence = [("native", "off", "preflight", 4)]
        for repeat in range(1, repetitions + 1):
            modes = ("off", "minimal", "detailed") if repeat % 2 else ("detailed", "minimal", "off")
            kinds = ("native", "qt") if repeat % 2 else ("qt", "native")
            sequence.extend((kind, mode, str(repeat), samples) for mode in modes for kind in kinds)
        for kind, mode, suffix, count in sequence:
            child_env = dict(environment, MUFFIN_GUI_PROFILE=mode)
            if kind == "native":
                child_env["MUFFIN_GUI_NATIVE_INPUT"] = "1"
            if mode == "detailed":
                child_env["MUFFIN_GUI_TRACE"] = "1"
            output = destination / f"input-{kind}-{mode}-{suffix}.json"
            print(f"Next: {kind}/{mode}/{suffix}; keep the temporary Muffin editor in the foreground", flush=True)
            with Path(str(output) + ".log").open("wb") as log:
                process = subprocess.run([str(probe), str(output), "1", "1", str(count)],
                                         cwd=workspace, env=child_env, stdout=log, stderr=subprocess.STDOUT,
                                         check=False)
            if process.returncode:
                raise ValueError(f"Probe exit {process.returncode}; retained log: {output}.log")
            result = inspect_run(json.loads(output.read_text(encoding="utf-8")), count)
            Path(str(output) + ".validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            if not result["accepted"]:
                raise ValueError(f"Invalid input run; raw data/reasons retained: {output}.validation.json")
    except (OSError, ValueError, KeyboardInterrupt) as error:
        failure = str(error) or "Interrupted"
    summary = collect(destination, repetitions, samples)
    if failure:
        summary["runner_error"] = failure
        summary["complete"] = False
    (destination / "comparison.json").write_text(
        json.dumps(summary, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Accepted {summary['accepted_matched_runs']}/{summary['expected_runs']}; {destination / 'comparison.json'}", flush=True)
    if failure:
        print(failure, file=sys.stderr, flush=True)
    return 0 if summary["complete"] and not failure else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=80)
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--foreground-wait-seconds", type=int, default=120)
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()
    if not (4 <= args.samples <= 500 and 1 <= args.repetitions <= 10 and 1 <= args.foreground_wait_seconds <= 3600):
        parser.error("samples=4..500, repetitions=1..10, foreground wait=1..3600 seconds")
    if sys.platform != "win32":
        parser.error("Use an interactive Windows desktop")
    workspace = Path(__file__).resolve().parent.parent
    destination = args.output_directory or Path("build") / f"input-comparison-{datetime.now():%Y%m%d-%H%M%S-%f}"
    if not destination.is_absolute():
        destination = workspace / destination
    try:
        return run_comparison(workspace, destination, args.samples, args.repetitions, args.foreground_wait_seconds)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
