"""Validate and collect opt-in native GUI projection/queue diagnostic runs."""
import argparse
import json
import math
import platform
from datetime import datetime, timedelta, timezone
from pathlib import Path

from summarize_gui_performance import quantiles


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_distribution(record, values):
    require(record["n"] == len(values), "Incorrect distribution sample count")
    if values:
        for name, expected in quantiles(values).items():
            require(math.isclose(record[name], expected, abs_tol=1e-5), f"Incorrect {name}")
        require(math.isclose(record["mean_ms"], sum(values) / len(values), abs_tol=1e-5), "Incorrect mean")


def overlap(start, end, event):
    return max(0, min(end, event["end_ms"]) - max(start, event["start_ms"]))


def collect_phase(phase):
    samples = phase["samples"]
    require(len(samples) == phase["count"], "Missing input samples")
    require(phase["inputs_without_paint"] == 0, "Missing paint coverage")
    require(phase["hidden_tab_paints"] == 0, "A hidden tab painted")
    require(phase["timeline_dropped"] == 0, "Truncated diagnostic timeline")
    checks = {
        "sync": [s["sync_ms"] for s in samples],
        "scheduled_to_dispatch": [s["dispatch_ms"] for s in samples],
        "producer_jitter": [s["post_jitter_ms"] for s in samples],
        "scheduled_to_paint": [s["scheduled_to_paint_ms"] for s in samples],
        "dispatch_to_paint": [s["dispatch_to_paint_ms"] for s in samples],
    }
    for name, values in checks.items():
        check_distribution(phase[name], values)
    for sample in samples:
        for elapsed, start, end in (
            ("post_jitter_ms", "due_at_ms", "posted_at_ms"),
            ("dispatch_ms", "due_at_ms", "dispatch_at_ms"),
            ("sync_ms", "dispatch_at_ms", "end_at_ms"),
            ("scheduled_to_paint_ms", "due_at_ms", "paint_at_ms"),
            ("dispatch_to_paint_ms", "dispatch_at_ms", "paint_at_ms"),
        ):
            require(math.isclose(sample[elapsed], sample[end] - sample[start], abs_tol=1e-5),
                    f"Inconsistent input clock: {elapsed}")
        require(sample["end_at_ms"] >= sample["dispatch_at_ms"] >= sample["posted_at_ms"],
                "Invalid dispatch order")
    for entry in phase["timeline"]:
        require(entry["end_ms"] >= entry["start_ms"], "Invalid timeline interval")
        if "duration_ms" in entry:
            require(math.isclose(entry["duration_ms"], entry["end_ms"] - entry["start_ms"], abs_tol=1e-5),
                    "Inconsistent timeline duration")

    events = [e for e in phase["timeline"] if e["kind"] == "event"]
    drafts = [e for e in events if e["target"].startswith("QTimer/benchmarkDraft/")]
    blockers = []
    for index, sample in enumerate(samples):
        blocked = [e for e in drafts if overlap(sample["posted_at_ms"], sample["dispatch_at_ms"], e) > 0]
        if blocked:
            blockers.append({"input": index, "post_to_dispatch_ms": sample["dispatch_at_ms"] - sample["posted_at_ms"],
                             "draft_overlap_ms": sum(overlap(sample["posted_at_ms"], sample["dispatch_at_ms"], e)
                                                     for e in blocked),
                             "scheduled_to_paint_ms": sample["scheduled_to_paint_ms"]})
    slow_inputs = []
    for index in sorted(range(len(samples)), key=lambda i: samples[i]["scheduled_to_paint_ms"], reverse=True)[:5]:
        sample = samples[index]
        # Events/scopes can nest. Preserve intervals; never sum inclusive handlers.
        queued_events = sorted(
            (e for e in events if overlap(sample["posted_at_ms"], sample["dispatch_at_ms"], e) > 0),
            key=lambda e: e["duration_ms"], reverse=True,
        )[:5]
        slow_inputs.append({"input": index, **sample, "events_overlapping_queue": queued_events})
    return {
        "count": phase["count"], "cadence_ms": phase["cadence_ms"],
        **{name: phase[name] for name in checks},
        "post_to_dispatch": quantiles(s["dispatch_at_ms"] - s["posted_at_ms"] for s in samples),
        "paint_duration": phase["paint_duration"], "heartbeat_delay": phase["heartbeat_delay"],
        "scope_timings": phase["scope_timings"], "theme_stages": phase["theme_stages"],
        "coalesced_input_count": phase["coalesced_input_count"],
        "working_set_before": phase["working_set_before"], "working_set_after": phase["working_set_after"],
        "sampled_max_working_set": phase["sampled_max_working_set"],
        "draft_events": drafts, "inputs_blocked_by_draft": blockers, "slowest_inputs": slow_inputs,
        **({"draft_acknowledged_revision": phase["draft_acknowledged_revision"],
            "document_revision": phase["document_revision"]} if "draft_acknowledged_revision" in phase else {}),
        **({"editor_state_changes": {
            "changed": sum(s["editor_state_changed"] for s in samples),
            "unchanged": sum(not s["editor_state_changed"] for s in samples),
            "changed_sync": quantiles(s["sync_ms"] for s in samples if s["editor_state_changed"])
                if any(s["editor_state_changed"] for s in samples) else {"n": 0},
            "changed_scheduled_to_paint": quantiles(s["scheduled_to_paint_ms"] for s in samples if s["editor_state_changed"])
                if any(s["editor_state_changed"] for s in samples) else {"n": 0},
        }} if all("editor_state_changed" in s for s in samples) else {}),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    runs, summary = [], []
    for path in args.inputs:
        run = json.loads(path.read_text(encoding="utf-8"))
        require(run["qpa"] == "windows" and run["active_window"] and run["native_window_exposed"],
                f"Inactive/unexposed native window: {path}")
        require(run["detailed_trace"] and run["MiB_per_tab"] == 30 and run["tabs"] == 1,
                f"Expected traced 30 MiB single-tab diagnostic: {path}")
        phases = {phase["phase"]: collect_phase(phase) for phase in run["phases"]}
        require(set(phases) == {"typing", "cursor_horizontal", "cursor_vertical", "scroll_cold", "scroll_warm", "theme_switch"},
                "Incomplete phase matrix")
        for name, phase in phases.items():
            require(phase["count"] == (12 if name == "theme_switch" else 80), "Different operation count")
            require(phase["cadence_ms"] == (1500 if name == "theme_switch" else 100 if name.startswith("scroll_") else 50),
                    "Different operation cadence")
        enabled = run["draft_snapshot_enabled"]
        if run.get("draft_write_mode") == "background_serial":
            require(("draft.enqueueSnapshot" in phases["cursor_vertical"]["scope_timings"]) == enabled,
                    "Background draft control did not take effect")
            if enabled:
                last = run["phases"][-1]
                require(last["draft_acknowledged_revision"] == last["document_revision"],
                        "Normal background run did not acknowledge committed draft")
        else:
            require(bool(phases["cursor_vertical"]["draft_events"]) == enabled, "Draft control did not take effect")
        if not enabled:
            require(not any("draft.snapshotSession" in p["scope_timings"] or "draft.enqueueSnapshot" in p["scope_timings"]
                            for p in phases.values()),
                    "A control run executed a draft snapshot")
        summary.append({"run": path.parent.name, "draft_snapshot_enabled": enabled,
                        "draft_write_mode": run.get("draft_write_mode", "synchronous"),
                        "refined_scopes": "formatting.hashTheme" in phases["typing"]["scope_timings"],
                        "phases": phases})
        runs.append(run)
    for field in ("qpa", "qt_version", "css_sha256", "font", "window_size", "viewport_size", "device_pixel_ratio",
                  "screen_logical_dpi", "screen_refresh_hz", "pinned_window"):
        require(all(run[field] == runs[0][field] for run in runs), f"Different {field}")
    require(sum(r["draft_snapshot_enabled"] for r in runs) >= 2 and
            sum(not r["draft_snapshot_enabled"] for r in runs) >= 2, "Need repeated normal and control runs")
    require(len({r.get("draft_write_mode", "synchronous") for r in runs}) == 1, "Mixed draft implementations")
    result = {
        "date": datetime.fromisoformat(runs[0]["timestamp"].replace("Z", "+00:00"))
                        .astimezone(timezone(timedelta(hours=8))).date().isoformat(),
        "timezone": "Asia/Shanghai; individual timestamps UTC", "os": platform.platform(),
        "methodology": (
            "Sequential fresh 30 MiB single-tab Windows QPA processes; native window pinned and active/exposed "
            "at validation points. Synthetic Qt inputs and completed QWidget paints; not physical input or DWM "
            "presentation. Normal/control pairs retain identical cadence, CSS and fonts. Control only blocks the "
            "disposable tab's recovery-draft timer signals. No concurrent builds/tests. Optional subscopes "
            "identify projection, layout, theme-signature, edit-context, menu and GUI draft work; worker-thread "
            "logs are excluded from GUI timelines. Draft mode and scope availability are recorded per run. "
            "Tracing adds overhead; compare like-for-like scopes and do not pool configurations. "
            "Inclusive scopes and nested Qt events must not be summed. Scope start is reconstructed from the "
            "logged elapsed time, with end sampled in the log handler. Trace thresholds: scopes >=0.1ms, "
            "Timer/MetaCall/Paint/UpdateRequest handlers >=1ms, heartbeat extra delay >=16ms; 16384 entries/phase. "
            "All timelines are untruncated. Nearest-rank quantiles; full per-operation data and timelines retained. "
            "Working sets are process measurements, not heap sizes or guaranteed peaks."
        ),
        "summary": summary, "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Keep raw timelines compact: large diagnostic arrays are intended for scripted analysis.
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Validated {len(runs)} native GUI diagnostics: {args.output}")
    for row in summary:
        phase = row["phases"]["cursor_vertical"]
        print(f"{row['run']}: vertical sync p95={phase['sync']['p95_ms']:.1f}ms, "
              f"scheduled-paint p95/max={phase['scheduled_to_paint']['p95_ms']:.1f}/"
              f"{phase['scheduled_to_paint']['max_ms']:.1f}ms, "
              f"draft-blocked inputs={len(phase['inputs_blocked_by_draft'])}")


if __name__ == "__main__":
    main()
