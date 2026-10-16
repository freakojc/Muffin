"""Run fresh GUI probes with foreground permission passed by the preceding probe."""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from run_gui_theme_acceptance import MATRIX, digest, environment, inspect_run, require


def jobs_for(destination, cycles, repetitions):
    jobs = [{"kind": "bootstrap", "output": str(destination / "bootstrap.json"), "mib": 1, "tabs": 3,
             "cycles": 4, "profile": "minimal", "repeat": 0}]
    for repeat in range(1, repetitions + 1):
        for size, tabs in (MATRIX if repeat % 2 else tuple(reversed(MATRIX))):
            jobs.append({"kind": "formal", "output": str(destination / f"gui-theme-{size}MiB-{tabs}tabs-r{repeat}.json"),
                         "mib": size, "tabs": tabs, "cycles": cycles, "profile": "minimal", "repeat": repeat})
    for repeat in range(1, 3):
        jobs.append({"kind": "control", "output": str(destination / f"off-control-r{repeat}.json"),
                     "mib": 1, "tabs": 3, "cycles": cycles, "profile": "off", "repeat": repeat})
    return jobs


def run(args):
    # These are process-lifetime observations, not APIs for manipulating windows.
    import _winapi

    root = Path(__file__).resolve().parent.parent
    destination = args.output_directory.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    phycat = args.phycat.resolve()
    files = [root / f"build/Release/{n}" for n in
             ("MuffinGuiPerformanceProbe.exe", "Muffin.exe", "MuffinCore.dll", "MuffinUi.dll")]
    files += [root / "tests/fixtures/theme/vlook-fancy-dark/vlook-fancy-dark.css",
              root / "resources/themes/github.css", root / "resources/themes/night.css",
              root / "resources/themes/document-base.css", phycat, phycat.parent / "phycat/phycat.dark.css",
              phycat.parent / "phycat/Cascadia-Code-Regular.ttf", phycat.parent / "phycat/LXGWWenKai-Regular.ttf"]
    pinned = {str(p): digest(p) for p in files}
    jobs = jobs_for(destination, args.cycles, args.repetitions)
    for job in jobs:
        job["open_profile"] = "off" if job["kind"] == "control" and args.open_profile else args.open_profile or ""
    plan = destination / "chain.json"
    plan.write_text(json.dumps({"schema": 1, "executable": str(files[0]), "jobs": jobs}, indent=2) + "\n", encoding="utf-8")
    env = environment(root, phycat, "minimal", args.foreground_wait_seconds)
    if args.memory_census:
        env["MUFFIN_GUI_MEMORY_CENSUS"] = "1"
    if args.open_profile:
        env["MUFFIN_GUI_OPEN_PROFILE"] = args.open_profile
    if args.static_visual_audit:
        env["MUFFIN_GUI_STATIC_VISUAL_AUDIT"] = "1"
    env.update(MUFFIN_GUI_CHAIN_FILE=str(plan), MUFFIN_GUI_CHAIN_INDEX="0")
    handles = {}
    states = []
    failure = None
    process = None
    last = None
    last_readiness = None
    deadline = time.monotonic() + len(jobs) * 180 + args.foreground_wait_seconds
    try:
        first = jobs[0]
        with Path(first["output"] + ".log").open("wb") as log:
            process = subprocess.Popen([str(files[0]), first["output"], "1", "3", "4"], cwd=root, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        handles[process.pid] = _winapi.OpenProcess(0x00101000, False, process.pid)
        while True:
            require(time.monotonic() < deadline, "GUI chain timed out; raw observations retained")
            try:
                state = json.loads(Path(str(plan) + ".state.json").read_text(encoding="utf-8"))
            except (FileNotFoundError, json.JSONDecodeError):
                state = None
            if state != last and state is not None:
                states.append(state)
                index = state["index"]
                job = jobs[index]
                print(f"{index + 1}/{len(jobs)} {state['status']}: {job['kind']} {job['mib']} MiB/{job['tabs']} tabs "
                      f"r{job['repeat']} ({job['profile']})", flush=True)
                for pid in (state["pid"], state.get("successor_pid", 0)):
                    if pid and pid not in handles:
                        handles[pid] = _winapi.OpenProcess(0x00101000, False, pid)
                last = state
                require(not state["status"].startswith("failed"), f"GUI chain stopped: {state['status']}")
            for pid, handle in handles.items():
                if _winapi.WaitForSingleObject(handle, 0) == 0:
                    require(_winapi.GetExitCodeProcess(handle) == 0, f"GUI process {pid} exited with an error")
            if state is not None:
                try:
                    ready = json.loads(Path(state["output"] + ".ready.json").read_text(encoding="utf-8"))
                except (FileNotFoundError, json.JSONDecodeError):
                    ready = None
                if ready is not None:
                    identity = (state["index"], ready["status"], ready["win32"]["foreground_is_own_process"])
                    if identity != last_readiness:
                        if ready["status"] == "waiting_for_foreground" and not identity[2]:
                            print(f"Window {state['index'] + 1}: waiting for actual Windows foreground; click its editor.",
                                  flush=True)
                        last_readiness = identity
            if state is not None and state["status"] == "completed":
                # All timed operations have ended. Wait for actual exit before
                # validating data, hashing resources or decoding any screenshot.
                for handle in handles.values():
                    require(_winapi.WaitForSingleObject(handle, 30000) == 0, "GUI process teardown did not finish")
                    require(_winapi.GetExitCodeProcess(handle) == 0, "GUI process teardown failed")
                break
            time.sleep(.2)
        require({str(p): digest(p) for p in files} == pinned, "Binaries or fixed resources changed")
    except (OSError, ValueError, KeyboardInterrupt) as error:
        failure = str(error) or "Interrupted"
        # These handles were obtained only for this chain's own benchmark PIDs.
        # Cancel remaining measurements; never enumerate or kill user apps.
        for handle in handles.values():
            if _winapi.WaitForSingleObject(handle, 0) != 0:
                try:
                    # Existing read/synchronize handles cannot terminate; open
                    # a separate owned-process handle only on this failure path.
                    pid = next(pid for pid, candidate in handles.items() if candidate == handle)
                    cancel = _winapi.OpenProcess(0x0001, False, pid)
                    try:
                        _winapi.TerminateProcess(cancel, 1)
                    finally:
                        _winapi.CloseHandle(cancel)
                except OSError:
                    pass
    finally:
        for handle in handles.values():
            _winapi.CloseHandle(handle)
        if process is not None:
            process.wait(timeout=30)
    records = {"formal": [], "control": [], "bootstrap": []}
    attempts = []
    handoffs = []
    previous_pid = 0
    for index, job in enumerate(jobs):
        output = Path(job["output"])
        if not output.exists():
            continue
        attempt = {"path": str(output), "kind": job["kind"], "accepted": False}
        try:
            raw = json.loads(output.read_text(encoding="utf-8"))
            if args.open_profile:
                require(len(raw["open"]) == job["tabs"], "Missing file-open records")
                for opened in raw["open"]:
                    require(opened["foreground_at_first_paint"], "File-open first paint lacked actual foreground")
                    require(opened["open_profile"] == job["open_profile"], "Wrong file-open diagnostic mode")
                    require(0 < opened["install_finished_ms"] <= opened["first_full_paint_ms"],
                            "Invalid file-open timeline")
            if args.memory_census:
                require(len(raw["memory_census"]["tabs"]) == job["tabs"], "Missing memory census")
                require(raw["memory_close"]["all_original_tabs_destroyed"] and
                        len(raw["memory_close"]["closed"]) == job["tabs"] and
                        raw["memory_close"]["remaining_tabs"] == 1, "Incomplete standard-close audit")
            handoff = json.loads(Path(str(output) + ".chain.json").read_text(encoding="utf-8"))
            handoffs.append(handoff)
            require(raw["chain_index"] == index and handoff["index"] == index, "Wrong chain index")
            require(raw["predecessor_pid"] == previous_pid, "Predecessor process differs")
            previous_pid = raw["foreground_identity"]["own_process"]
            require(handoff["pid"] == previous_pid, "Process identity differs")
            require(handoff["status"] == ("completed" if index == len(jobs) - 1 else "handed_off"),
                    "Missing successful chain completion/handoff")
            if index < len(jobs) - 1:
                require(handoff["successor_pid"] > 0, "Missing successor process")
                require(handoff["handoff"]["origin"]["foreground_is_own_process"], "Launch origin was not foreground")
            record = inspect_run(raw, job["mib"], job["tabs"], job["cycles"], job["profile"])
            record.update(path=str(output), repeat=job["repeat"])
            records[job["kind"]].append(record)
            attempt["accepted"] = True
        except (OSError, ValueError, KeyError) as error:
            attempt["error"] = str(error)
            failure = failure or f"Invalid raw record: {output}: {error}"
        attempts.append(attempt)
    method = ("Fresh Windows QPA MainWindow processes. A foreground benchmark starts only the next benchmark and attempts "
              "AllowSetForegroundWindow for that exact PID, retaining failures. Normal child-process activation is also allowed; "
              "neither a grant nor process startup substitutes for actual foreground checks. The next process waits for its predecessor to exit before "
              "loading documents and starting timed work. Actual Win32 foreground, activation and exposure are still "
              "required. No input-queue attachment, fabricated status or system-setting changes. Startup night; six-way "
              "matrix, VLOOK/GitHub/Phycat cycles. Qt-posted menu actions, not native-keyboard latency. Complete viewport "
              "QWidget paint, not DWM presentation. Drafts retained/drained; untimed visual and full numbering checks. "
              "Coordinator only polls tiny state files/process exit during timing; all statistics and resource hashing "
              "are deferred until all GUI processes exit. Nearest-rank sample quantiles; all outliers retained.")
    for kind, directory, expected, profile in (("formal", destination, len(MATRIX) * args.repetitions, "minimal"),
                                                ("control", destination / "off-control", 2, "off")):
        directory.mkdir(parents=True, exist_ok=True)
        summary = {"complete": failure is None and len(records[kind]) == expected, "accepted_runs": len(records[kind]),
                   "expected_runs": expected, "cycles": args.cycles, "profile": profile, "pilot": kind == "control",
                   "runner_error": failure, "pinned_sha256": pinned, "records": records[kind],
                   "attempts": attempts, "chain_states": states, "handoffs": handoffs, "method": method}
        (directory / "acceptance.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (destination / "bootstrap-validation.json").write_text(json.dumps(records["bootstrap"], indent=2) + "\n", encoding="utf-8")
    print(f"Accepted {len(records['formal'])}/{len(MATRIX) * args.repetitions} formal, "
          f"{len(records['control'])}/2 off controls: {destination}", flush=True)
    if failure:
        print(failure, file=sys.stderr)
    return 0 if failure is None and len(records["formal"]) == len(MATRIX) * args.repetitions else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=6)
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--foreground-wait-seconds", type=int, default=120)
    parser.add_argument("--memory-census", action="store_true", help="Untimed payload census and standard tab-close checkpoints")
    parser.add_argument("--static-visual-audit", action="store_true", help="Normalize untimed visual audit hover state")
    parser.add_argument("--open-profile", choices=("off", "stages", "legacy"),
                        help="Opening diagnostics: disabled, coarse phases, or existing per-inline timers")
    parser.add_argument("--phycat", type=Path, default=Path(os.environ.get("APPDATA", ".")) /
                        "Muffin/Muffin/themes/phycat-abyss.css")
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
