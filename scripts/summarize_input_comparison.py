"""Validate matched Windows/Qt typing probes; preserve failures and missing cells."""
import argparse
import base64
import json
import math
import re
from pathlib import Path


ENVIRONMENT = (
    "MiB_per_tab", "tabs", "font", "css_sha256", "qt_version", "window_size",
    "viewport_size", "screen_refresh_hz", "screen_logical_dpi", "device_pixel_ratio",
)
MODES = ("off", "minimal", "detailed")
DELIVERIES = {"native": "windows_sendinput", "qt": "qt_posted"}
NAME = re.compile(r"input-(native|qt)-(off|minimal|detailed)-(\d+)\.json$")
PREFLIGHT = re.compile(r"input-native-off-preflight(?:-[a-z0-9]+)?\.json$")


def distribution(values, unit="ms"):
    ordered = sorted(values)
    if not ordered:
        return {"n": 0, "unit": unit}
    return {
        "n": len(ordered), "unit": unit,
        **{label: ordered[math.ceil(fraction * len(ordered)) - 1]
           for label, fraction in (("p50", .5), ("p95", .95), ("p99", .99))},
        "max": ordered[-1], "mean": sum(ordered) / len(ordered),
    }


def inspect_run(run, samples=80):
    """Do not accept missing observations as evidence of a working foreground."""
    errors = []

    def require(condition, reason):
        if not condition:
            errors.append(reason)

    require(run.get("qpa") == "windows", "not Windows QPA")
    require(all(key in run and run[key] is not None for key in ENVIRONMENT), "missing environment observations")
    for key in ("active_window", "native_window_exposed", "windows_foreground",
                "foreground_wait_required", "typing_only", "draft_snapshots_enabled"):
        require(run.get(key) is True, f"{key} not confirmed")
    require(run.get("foreground_wait_timed_out") is False, "foreground wait failed or unobserved")
    for key in ("deep_css_profile", "scheduler_sampling"):
        require(run.get(key) is False, f"unexpected or unobserved {key}")
    require(run.get("MiB_per_tab") == 1 and run.get("tabs") == 1, "not 1 MiB / one tab")
    mode = run.get("profiling_mode")
    require(mode in MODES, "unknown profiling mode")
    require(run.get("detailed_trace") is (mode == "detailed"), "trace/profile mismatch")
    delivery = run.get("input_delivery")
    require(delivery in DELIVERIES.values(), "unknown input delivery")
    require(run.get("native_input_complete") is True, "incomplete input delivery")
    if delivery == DELIVERIES["native"]:
        require(run.get("native_posts") == samples and run.get("native_received") == samples,
                "native post/receive count mismatch")
        require(run.get("native_aborted") is False and run.get("native_contaminated") is False,
                "native input aborted or contaminated")
    drain = run.get("draft_drain", {})
    require(drain.get("complete") is True and drain.get("schema_version") == 2, "draft drain incomplete/old schema")
    tabs = drain.get("tabs", [])
    require(len(tabs) == 1 and all(t["revision"] == t["acknowledged"] for t in tabs),
            "latest recovery draft not acknowledged")
    phases = run.get("phases", [])
    require(len(phases) == 1 and phases[0].get("phase") == "typing", "not a matched typing-only run")
    if len(phases) != 1:
        return {"accepted": False, "reasons": errors, "metrics": {}}
    phase = phases[0]
    for key in ("active_throughout", "exposed_throughout", "windows_foreground_throughout"):
        require(phase.get(key) is True, f"{key} not confirmed")
    require(phase.get("cadence_ms") == 50 and phase.get("count") == samples, "wrong count/cadence")
    require(phase.get("hidden_tab_paints") == 0 and phase.get("timeline_dropped", 0) == 0,
            "hidden paint or truncated timeline")
    raw = phase.get("samples", [])
    require(len(raw) == samples, "missing raw input samples")
    require(phase.get("document_revision", 0) - phase.get("document_revision_before", 0) == samples,
            "typing did not produce the expected revision changes")
    required = ("due_at_ms", "posted_at_ms", "dispatch_at_ms", "end_at_ms", "paint_start_at_ms",
                "paint_at_ms", "editor_state_changed", "sync_ms", "post_jitter_ms", "dispatch_ms",
                "scheduled_to_paint_ms", "dispatch_to_paint_ms", "post_to_dispatch_ms",
                "handler_end_to_paint_start_ms", "pending_inputs_at_dispatch",
                "pending_inputs_after_handler", "subsequent_inputs_before_paint")
    require(all(all(key in s for key in required) for s in raw), "missing raw clock/backlog/paint observations")
    if any(any(key not in s for key in required) for s in raw):
        return {"accepted": False, "reasons": errors, "metrics": {}}
    for index, s in enumerate(raw):
        if index:
            require(abs(s["due_at_ms"] - raw[index - 1]["due_at_ms"] - 50) < 1e-5,
                    f"sample {index}: scheduled cadence inconsistent")
        require(s.get("detail") == ("x/native" if delivery == DELIVERIES["native"] else "x"),
                f"sample {index}: unexpected input text/delivery")
        later = [t for t in raw if t["dispatch_at_ms"] > s["dispatch_at_ms"]]
        clocks = {
            "sync_ms": s["end_at_ms"] - s["dispatch_at_ms"],
            "dispatch_ms": s["dispatch_at_ms"] - s["due_at_ms"],
            "post_jitter_ms": s["posted_at_ms"] - s["due_at_ms"],
            "post_to_dispatch_ms": s["dispatch_at_ms"] - s["posted_at_ms"],
            "scheduled_to_paint_ms": s["paint_at_ms"] - s["due_at_ms"],
            "dispatch_to_paint_ms": s["paint_at_ms"] - s["dispatch_at_ms"],
            "handler_end_to_paint_start_ms": max(0, s["paint_start_at_ms"] - s["end_at_ms"]),
        }
        for key, value in clocks.items():
            require(math.isfinite(value) and abs(value - s[key]) < 1e-5, f"sample {index}: {key} inconsistent")
        require(s["posted_at_ms"] <= s["dispatch_at_ms"] <= s["end_at_ms"] and
                s["dispatch_at_ms"] <= s["paint_start_at_ms"] <= s["paint_at_ms"],
                f"sample {index}: clock order inconsistent")
        require(s["editor_state_changed"] is True, f"sample {index}: no edit")
        counts = {
            "pending_inputs_at_dispatch": sum(t["posted_at_ms"] <= s["dispatch_at_ms"] for t in later),
            "pending_inputs_after_handler": sum(t["posted_at_ms"] <= s["end_at_ms"] <= t["dispatch_at_ms"] for t in later),
            "subsequent_inputs_before_paint": sum(t["dispatch_at_ms"] < s["paint_start_at_ms"] for t in later),
        }
        for key, value in counts.items():
            require(s[key] == value, f"sample {index}: {key} inconsistent")
    for key, field in (("sync", "sync_ms"), ("scheduled_to_dispatch", "dispatch_ms"),
                       ("scheduled_to_paint", "scheduled_to_paint_ms"),
                       ("dispatch_to_paint", "dispatch_to_paint_ms")):
        stat = distribution([s[field] for s in raw])
        saved = phase.get(key, {})
        require(saved.get("n") == stat["n"], f"{key}: count inconsistent")
        for q in ("p50", "p95", "p99", "max", "mean"):
            if stat["n"]:
                require(abs(saved.get(f"{q}_ms", float("inf")) - stat[q]) < 1e-5, f"{key}: {q} inconsistent")
    metrics = {key: distribution([s[key] for s in raw]) for key in (
        "sync_ms", "post_to_dispatch_ms", "scheduled_to_paint_ms",
        "dispatch_to_paint_ms", "handler_end_to_paint_start_ms")}
    metrics.update({key: distribution([s[key] for s in raw], "inputs") for key in (
        "pending_inputs_at_dispatch", "pending_inputs_after_handler", "subsequent_inputs_before_paint")})
    metrics["changed_tails_at_least_100ms"] = [i for i, s in enumerate(raw) if s["scheduled_to_paint_ms"] >= 100]
    metrics["drained_working_set_bytes"] = drain.get("working_set_after")
    metrics["draft_drain_wait_ms"] = drain.get("wait_ms")
    return {"accepted": not errors, "reasons": errors, "metrics": metrics}


def collect(directory, repetitions, samples):
    records, found = [], set()
    reference = None
    for path in sorted(directory.glob("*.json")):
        match = NAME.fullmatch(path.name)
        preflight = PREFLIGHT.fullmatch(path.name)
        if not match and not preflight:
            continue
        raw_bytes = path.read_bytes()
        try:
            run = json.loads(raw_bytes.decode("utf-8"))
        except ValueError as error:
            records.append({"file": path.name, "accepted": False, "reasons": [f"Invalid JSON: {error}"],
                            "raw_bytes_base64": base64.b64encode(raw_bytes).decode("ascii"), "metrics": {}})
            continue
        check = inspect_run(run, 4 if preflight else samples)
        if match:
            kind, mode, repeat = match.groups()
            key = (kind, mode, int(repeat))
            if run.get("input_delivery") != DELIVERIES[kind] or run.get("profiling_mode") != mode:
                check["reasons"].append("filename/delivery/profile mismatch")
                check["accepted"] = False
            if not 1 <= int(repeat) <= repetitions:
                check["reasons"].append("repetition outside requested range")
                check["accepted"] = False
            if check["accepted"]:
                environment = {k: run.get(k) for k in ENVIRONMENT}
                if reference is None:
                    reference = environment
                if environment != reference:
                    check["reasons"].append("environment differs between matched runs")
                    check["accepted"] = False
                else:
                    found.add(key)
        records.append({"file": path.name, "raw": run, **check})
    missing = [f"input-{kind}-{mode}-{repeat}.json" for repeat in range(1, repetitions + 1)
               for kind in DELIVERIES for mode in MODES if (kind, mode, repeat) not in found]
    return {
        "complete": not missing, "expected_runs": repetitions * len(MODES) * len(DELIVERIES),
        "accepted_matched_runs": len(found), "missing_or_invalid_runs": missing, "records": records,
        "method": (
            "Fresh 1 MiB/single-tab Windows processes; matched typing-only prefix, 50ms cadence. "
            "OS input is Win32 SendInput, not physical hardware or IME. Qt inputs bypass the Windows keyboard queue. "
            "Off still includes probe clocks, paint hooks, heartbeat and memory sampling. "
            "Every mode retains clock edges and derived counts of later posted inputs awaiting dispatch. "
            "These are observed injected-workload counts, not Windows/Qt internal queue length. "
            "Paint end is QWidget completion, not display presentation. Foreground/exposure checks are observations, "
            "not proof against sub-heartbeat focus changes. Incomplete/unknown foreground runs are excluded, "
            "with all raw failures and tails retained. Latest recovery drafts are acknowledged before memory sampling. "
            "Working set is not heap usage. Small-sample nearest-rank p99 can equal max. "
            "No causal performance improvement claim follows from an incomplete comparison."
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    single = commands.add_parser("check", help="Reject incomplete/invalid runs before continuing a live sequence")
    single.add_argument("input", type=Path)
    single.add_argument("--samples", type=int, default=80)
    group = commands.add_parser("collect", help="Retain all records and report missing comparison cells")
    group.add_argument("input", type=Path)
    group.add_argument("output", type=Path)
    group.add_argument("--samples", type=int, default=80)
    group.add_argument("--repetitions", type=int, default=2)
    group.add_argument("--allow-incomplete", action="store_true")
    args = parser.parse_args()
    if args.samples < 4 or (args.command == "collect" and args.repetitions < 1):
        parser.error("samples must be >=4 and repetitions >=1")
    if args.command == "check":
        result = inspect_run(json.loads(args.input.read_text(encoding="utf-8")), args.samples)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["accepted"] else 1
    result = collect(args.input, args.repetitions, args.samples)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Accepted {result['accepted_matched_runs']}/{result['expected_runs']} matched runs; retained {len(result['records'])} raw records")
    return 0 if result["complete"] or args.allow_incomplete else 1


if __name__ == "__main__":
    raise SystemExit(main())
