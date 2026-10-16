"""Validate native GUI queue traces and retain raw operations and all long-tail windows."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def union_ms(intervals):
    total = 0
    end = -float("inf")
    for start, stop in sorted(intervals):
        total += max(0, stop - max(start, end))
        end = max(end, stop)
    return total


def collect(path, valid=True):
    run = json.loads(path.read_text(encoding="utf-8"))
    require(run["qpa"] == "windows" and run["queue_trace_version"] == 1 and run["detailed_trace"], str(path))
    if valid:
        require(run["active_window"] and run["native_window_exposed"], f"Inactive/unexposed run: {path}")
        require(run.get("windows_foreground", True), f"No Windows foreground: {path}")
    result = {key: value for key, value in run.items() if key not in ("phases", "screenshot")}
    result["source"] = str(path)
    result["valid_comparison"] = valid
    result["phases"] = []
    expected = ["typing", "cursor_horizontal", "cursor_vertical"]
    if not run.get("input_phases_only", False):
        expected += ["scroll_cold", "scroll_warm", "theme_switch"]
        if run["tabs"] > 1:
            expected += ["tab_switch_deferred_theme", "tab_switch_warm", "typing_multiple_tabs"]
    require([p["phase"] for p in run["phases"]] == expected, f"Incomplete phase sequence: {path}")
    for phase in run["phases"]:
        if valid:
            require(phase.get("windows_foreground_throughout", True), f"Lost Windows foreground: {path}/{phase['phase']}")
            require(phase.get("active_throughout", True) and phase.get("exposed_throughout", True),
                    f"Inactive/unexposed phase: {path}/{phase['phase']}")
        require(not phase["timeline_dropped"] and not phase["hidden_tab_paints"], f"Incomplete/hidden trace: {path}")
        samples = phase["samples"]
        require(len(samples) == phase["count"], f"Missing operation: {path}")
        for key, field in (("sync", "sync_ms"), ("scheduled_to_dispatch", "dispatch_ms"),
                           ("scheduled_to_paint", "scheduled_to_paint_ms"), ("dispatch_to_paint", "dispatch_to_paint_ms")):
            values = sorted(s[field] for s in samples if field in s)
            require(len(values) == phase[key]["n"], f"Different coverage: {key}")
            for label, quantile in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99)):
                if values:
                    require(abs(values[math.ceil(quantile * len(values)) - 1] - phase[key][label]) < 1e-6,
                            f"Incorrect {key}/{label}: {path}")
        record = {key: value for key, value in phase.items() if key not in ("timeline", "gui_thread_samples")}
        if phase["phase"] in ("typing", "cursor_horizontal", "cursor_vertical"):
            actions, windows = [], []
            worst = sorted(range(len(samples)), key=lambda i: samples[i].get("scheduled_to_paint_ms", 0), reverse=True)[:3]
            for index, sample in enumerate(samples):
                for end, start, field in (("end_at_ms", "dispatch_at_ms", "sync_ms"),
                                           ("dispatch_at_ms", "due_at_ms", "dispatch_ms"),
                                           ("paint_at_ms", "due_at_ms", "scheduled_to_paint_ms")):
                    if end in sample:
                        require(abs(sample[end] - sample[start] - sample[field]) < 1e-5, f"Clock mismatch: {path}")
                requests = [e for e in phase["timeline"] if e["kind"] == "update_request" and e["operation"] == index]
                action = {"index": index, "update_request_count": len(requests),
                          "post_to_dispatch_ms": sample["dispatch_ms"] - sample["post_jitter_ms"]}
                if requests:
                    action["first_update_at_ms"] = requests[0]["at_ms"]
                    action["last_update_at_ms"] = requests[-1]["at_ms"]
                if "paint_start_at_ms" in sample:
                    start, end = sample["end_at_ms"], sample["paint_start_at_ms"]
                    action["handler_end_to_paint_start_ms"] = max(0, end - start)
                    action["paint_duration_ms"] = sample["paint_at_ms"] - end
                    intervals = [(max(start, e["start_ms"]), min(end, e["end_ms"])) for e in phase["timeline"]
                                 if e["kind"] == "event" and e["start_ms"] < end and e["end_ms"] > start]
                    # Both nested notify() intervals and child paints can appear.
                    action["observed_qt_event_union_ms"] = union_ms(intervals)
                    cpu = [s for s in phase["gui_thread_samples"] if start <= s["at_ms"] <= end]
                    if len(cpu) >= 2:
                        action["sampled_interval_ms"] = cpu[-1]["at_ms"] - cpu[0]["at_ms"]
                        action["sampled_cpu_ms"] = cpu[-1]["cpu_ms"] - cpu[0]["cpu_ms"]
                        action["sampled_cycles"] = cpu[-1]["cycles"] - cpu[0]["cycles"]
                        action["thread_state_samples"] = dict(Counter(str(s.get("thread_state", "unavailable")) for s in cpu))
                action["long_tail"] = sample.get("scheduled_to_paint_ms", 0) >= 100 and sample["editor_state_changed"]
                if action["long_tail"] or index in worst:
                    windows.append((sample["due_at_ms"] - 20, sample.get("paint_at_ms", sample["end_at_ms"]) + 20))
                actions.append(action)
            record["actions"] = actions
            record["gui_thread_samples"] = phase["gui_thread_samples"]
            record["timeline_windows_ms"] = windows
            record["selected_timeline"] = [e for e in phase["timeline"] if any(
                e.get("start_ms", e.get("at_ms", 0)) <= end and e.get("end_ms", e.get("at_ms", 0)) >= start
                for start, end in windows)]
            record["all_qt_event_counts"] = dict(Counter(e["target"] + ":" + str(e["type"])
                                                          for e in phase["timeline"] if e["kind"] == "event"))
        result["phases"].append(record)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--before", nargs="+", type=Path, required=True)
    parser.add_argument("--after", nargs="+", type=Path, required=True)
    parser.add_argument("--intermediate", nargs="*", type=Path, default=[])
    parser.add_argument("--scheduler", nargs="*", type=Path, default=[])
    parser.add_argument("--invalid", nargs="*", type=Path, default=[])
    args = parser.parse_args()
    result = {label: [collect(path, label != "invalid") for path in paths]
              for label, paths in (("before", args.before), ("after", args.after),
                                   ("intermediate", args.intermediate), ("scheduler_diagnostics", args.scheduler),
                                   ("invalid", args.invalid))}
    reference = result["before"][0]
    for run in result["before"] + result["after"] + result["intermediate"] + result["scheduler_diagnostics"]:
        for key in ("css_sha256", "qt_version", "font", "viewport_size", "window_size", "device_pixel_ratio",
                    "screen_logical_dpi", "screen_refresh_hz", "draft_snapshot_enabled", "draft_write_mode"):
            require(run[key] == reference[key], f"Different environment: {key}")
    result["methodology"] = (
        "Sequential fresh native Windows GUI processes, no concurrent build/test; trace on, normal background drafts. "
        "Synthetic Qt inputs; completed QWidget paints, not compositor presentation. Active/exposed runs alone enter comparisons. "
        "All original samples, scopes and phase summaries retained; input phases retain CPU/thread samples and event counts. "
        "Timelines retain every changed-input >=100 ms tail and each phase's three slowest paints (20 ms margins). "
        "Nested Qt handler coverage is unioned, not summed; uncovered time includes other inputs, dispatcher/native work, logging "
        "and scheduling/waits. Coarse Windows CPU accounting and thread-state point samples are not exact scheduling durations. "
        "Cycle counts are not converted at an assumed CPU frequency. NtQuerySystemInformation is optional, absent in initial before runs. "
        "After comparisons use CPU/cycle sampling only, matching before. Scheduler diagnostics are a separate workload; "
        "their system-wide query cost is not attributed to editor processing. CPU fields in initial scheduler diagnostics "
        "were read before that query but timestamped after it; they cannot be treated as exact point-in-time CPU deltas. "
        "New probes record query duration and read CPU after the optional system query. "
        "WaitReason is meaningful only for state 5. State 1=ready, 2=running, 3=standby, 5=waiting. "
        "Zero-change inputs and shared/coalesced paints keep their original metrics. WPR CPU recording was blocked by local policy "
        "(0xc5585011), so there are no ETW ready-time/stack claims. Quantiles and clock differences checked from raw inputs. "
        "Working-set sampling is not heap accounting or a guaranteed peak."
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print({key: len(result[key]) for key in ("before", "after", "intermediate", "scheduler_diagnostics", "invalid")})


if __name__ == "__main__":
    main()
