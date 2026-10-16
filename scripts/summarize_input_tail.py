"""Retain complete input-tail diagnostics without treating an unverified desktop as GUI acceptance."""
import argparse
import json
import math
from pathlib import Path


def interval_union(intervals):
    total, end = 0, -float("inf")
    for start, stop in sorted(intervals):
        total += max(0, stop - max(start, end))
        end = max(end, stop)
    return total


def queue_coverage(phase):
    samples, timeline = phase["samples"], phase.get("timeline", [])
    rows = []
    for i, sample in enumerate(samples):
        if "paint_start_at_ms" not in sample:
            continue
        start, stop = sample["end_at_ms"], sample["paint_start_at_ms"]
        row = {"index": i, "nested_or_immediate_paint": stop <= start,
               "queued_later_inputs_at_dispatch": sum(j > i and s["posted_at_ms"] <= sample["dispatch_at_ms"]
                                                      for j, s in enumerate(samples))}
        if stop > start:
            handlers = [(max(start, s["dispatch_at_ms"]), min(stop, s["end_at_ms"])) for s in samples
                        if s["dispatch_at_ms"] < stop and s["end_at_ms"] > start]
            events = [(max(start, e["start_ms"]), min(stop, e["end_ms"])) for e in timeline
                      if e["kind"] == "event" and e["start_ms"] < stop and e["end_ms"] > start]
            covered = interval_union(handlers + events)
            row.update({"handler_end_to_paint_start_ms": stop - start,
                        "subsequent_input_handler_count": len(handlers),
                        "input_handler_union_ms": interval_union(handlers),
                        "qt_event_union_ms": interval_union(events),
                        "combined_observed_union_ms": covered,
                        "uncovered_ms": stop - start - covered})
        rows.append(row)
    return rows


def collect(path):
    run = json.loads(path.read_text(encoding="utf-8"))
    run["source"] = str(path)
    native = run.get("input_delivery") == "windows_sendinput"
    complete = run.get("native_input_complete", True)
    quantiles = 0
    for phase in run["phases"]:
        if not complete:
            continue
        samples = phase["samples"]
        assert len(samples) == phase["count"], path
        assert not phase.get("timeline_dropped", 0) and not phase["hidden_tab_paints"], path
        for group, field in (("sync", "sync_ms"), ("scheduled_to_dispatch", "dispatch_ms"),
                             ("scheduled_to_paint", "scheduled_to_paint_ms"),
                             ("dispatch_to_paint", "dispatch_to_paint_ms")):
            values = sorted(s[field] for s in samples if field in s)
            assert len(values) == phase[group]["n"]
            for label, fraction in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99)):
                if values:
                    assert abs(values[math.ceil(fraction * len(values)) - 1] - phase[group][label]) < 1e-6
                    quantiles += 1
        for sample in samples:
            if "end_at_ms" in sample:
                assert abs(sample["end_at_ms"] - sample["dispatch_at_ms"] - sample["sync_ms"]) < 1e-5
        phase["changed_input_tail_indexes"] = [i for i, s in enumerate(samples)
            if s.get("editor_state_changed") and s.get("scheduled_to_paint_ms", 0) >= 100]
        phase["observed_queue_coverage"] = queue_coverage(phase)
    run["checked_quantiles"] = quantiles
    # Absence of Win32 foreground checks in older runs is explicitly unknown.
    foreground = run.get("windows_foreground", False) and all(
        p.get("windows_foreground_throughout", False) for p in run["phases"])
    qt_active = run["active_window"] and run["native_window_exposed"] and all(
        p.get("active_throughout", False) and p.get("exposed_throughout", False) for p in run["phases"])
    run["qt_window_checks_passed"] = bool(qt_active)
    run["valid_gui_acceptance"] = bool(complete and foreground and qt_active and
                                       run.get("draft_drain", {}).get("complete", False))
    run["native_delivery_checked"] = bool(native and complete)
    return run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--before", type=Path, nargs="+", required=True)
    parser.add_argument("--after", type=Path, nargs="+", required=True)
    parser.add_argument("--excluded", type=Path, nargs="*", default=[])
    parser.add_argument("--previous-reference", type=Path)
    parser.add_argument("--tool-smoke", type=Path, nargs="+", default=[])
    args = parser.parse_args()
    result = {group: [collect(p) for p in paths] for group, paths in
              (("before", args.before), ("after", args.after), ("excluded", args.excluded))}
    if args.previous_reference:
        previous = json.loads(args.previous_reference.read_text(encoding="utf-8"))
        phase = next(p for p in previous["phases"] if p["phase"] == "typing")
        result["previous_370ms_reference"] = {
            "source": str(args.previous_reference), "phase": "typing", "samples": phase["samples"],
            "timeline": phase["timeline"], "observed_queue_coverage": queue_coverage(phase)}
    if args.tool_smoke:
        result["tool_smoke"] = [collect(path) for path in args.tool_smoke]
    reference = result["before"][0]
    for run in result["before"] + result["after"]:
        for key in ("MiB_per_tab", "tabs", "font", "css_sha256", "qt_version", "window_size",
                    "viewport_size", "screen_refresh_hz", "screen_logical_dpi", "device_pixel_ratio"):
            assert run[key] == reference[key], key
        assert run["draft_drain"]["complete"]
        assert all(t["revision"] == t["acknowledged"] for t in run["draft_drain"]["tabs"])
    result["method"] = (
        "Complete raw records retained, including every latency tail and focus-invalid run. "
        "Qt-posted events are processing diagnostics, never physical keyboard evidence. "
        "Native mode sends Win32 virtual-key SendInput pairs only while the selected window is foreground; "
        "it is injected Windows input, not physical hardware. Foreground zero aborts before injection. "
        "Missing Win32 foreground observations are unknown, not acceptance. "
        "Off disables product perf scopes/stage timers, minimal records aggregate scopes, detailed adds event traces. "
        "All modes retain probe clocks, paint hooks, heartbeat and working-set sampling. "
        "Whole-run Qt focus/exposure checks are separate from Windows foreground checks. "
        "Paint completion is QWidget dispatch, not DWM presentation. All samples and timelines retained; "
        "Input-handler and nested Qt event coverage uses clipped interval unions, never sums. "
        "Handler wall-time coverage can itself include scheduling/waits; uncovered time has no assigned blocking cause. "
        "nearest-rank quantiles/clock deltas checked independently. End-of-run recovery drafts are requested "
        "and acknowledged asynchronously, with timeout, before reporting drained working-set bytes. "
        "Working set is not heap size; no population latency or leak conclusion from two repetitions."
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print({group: len(result[group]) for group in ("before", "after", "excluded")})
    print("checked quantiles", sum(r["checked_quantiles"] for group in ("before", "after", "excluded") for r in result[group]))


if __name__ == "__main__":
    main()
