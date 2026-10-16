"""Validate repeated native GUI theme-switch traces and preserve per-action stages."""
import argparse
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def distribution(values):
    values = sorted(values)
    return {"n": len(values), "max_ms": values[-1], **{
        name: values[math.ceil(percentile * len(values)) - 1]
        for name, percentile in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99))
    }}


def collect(path):
    run = json.loads(path.read_text(encoding="utf-8"))
    require(run["qpa"] == "windows" and run["active_window"] and run["native_window_exposed"],
            f"Inactive or unexposed native window: {path}")
    require(run["detailed_trace"] and run["draft_snapshot_enabled"] and run["draft_write_mode"] == "background_serial",
            f"Different diagnostics/draft mode: {path}")
    result = {key: value for key, value in run.items() if key not in ("phases", "screenshot")}
    result["source"] = str(path)
    result["phases"] = []
    for phase in run["phases"]:
        require(not phase["hidden_tab_paints"] and not phase["timeline_dropped"], f"Hidden paint or truncated trace: {path}")
        require(not any(int(key.split("/")[-1]) == 1 for key in phase["hidden_editor_events_tab_slash_qevent_type"]),
                f"Hidden editor timer: {path}")
        samples = phase["samples"]
        require(len(samples) == phase["count"], f"Missing operation: {path}")
        for metric, sample_key in (("sync", "sync_ms"), ("scheduled_to_dispatch", "dispatch_ms"),
                                   ("scheduled_to_paint", "scheduled_to_paint_ms"),
                                   ("dispatch_to_paint", "dispatch_to_paint_ms")):
            values = [sample[sample_key] for sample in samples if sample_key in sample]
            require(len(values) == phase[metric]["n"], f"Inconsistent {metric} coverage: {path}")
            if values:
                for key, expected in distribution(values).items():
                    require(abs(phase[metric][key] - expected) < 1e-6, f"Incorrect {metric}/{key}: {path}")
        for sample in samples:
            for end, start, duration in (("end_at_ms", "dispatch_at_ms", "sync_ms"),
                                          ("dispatch_at_ms", "due_at_ms", "dispatch_ms"),
                                          ("paint_at_ms", "due_at_ms", "scheduled_to_paint_ms")):
                if end in sample:
                    require(abs(sample[end] - sample[start] - sample[duration]) < 1e-5,
                            f"Inconsistent operation clock: {path}")
        record = {key: value for key, value in phase.items() if key != "timeline"}
        if phase["phase"] == "theme_switch":
            require(len(samples) == 12 and phase["cadence_ms"] == 1500, f"Different theme cadence: {path}")
            record["timeline"] = phase["timeline"]
            seen = set()
            actions = []
            for index, sample in enumerate(samples):
                scopes = {}
                for entry in phase["timeline"]:
                    if entry["kind"] == "scope" and entry["operation"] == index:
                        scopes.setdefault(entry["name"], []).append(entry["duration_ms"])
                action = {"index": index, "target": sample["detail"], "first_action_for_target": sample["detail"] not in seen,
                          "scope_inclusive_ms": {key: sum(values) for key, values in scopes.items()}}
                seen.add(sample["detail"])
                paints = [entry for entry in phase["timeline"] if entry["kind"] == "scope" and
                          entry["name"] == "view.paint" and "paint_at_ms" in sample and
                          abs(entry["end_ms"] - sample["paint_at_ms"]) < 3]
                action["first_paint_scope_ms"] = min(paints, key=lambda p: abs(p["end_ms"] - sample["paint_at_ms"]))[
                    "duration_ms"] if paints else None
                actions.append(action)
            record["actions"] = actions
        result["phases"].append(record)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--before", type=Path, nargs="+", required=True)
    parser.add_argument("--after", type=Path, nargs="+", required=True)
    args = parser.parse_args()
    runs = {label: [collect(path) for path in paths] for label, paths in (("before", args.before), ("after", args.after))}
    reference = runs["before"][0]
    for run in runs["before"] + runs["after"]:
        for key in ("MiB_per_tab", "tabs", "css_sha256", "qt_version", "font", "viewport_size", "window_size",
                    "device_pixel_ratio", "screen_logical_dpi", "screen_refresh_hz", "pinned_window"):
            require(run[key] == reference[key], f"Different benchmark environment: {key}")
    result = {"methodology": "Repeated fresh Windows GUI processes; sequential without builds/tests. Active/exposed validation points; "
              "synthetic Qt input and QWidget paint completion, not physical display presentation. Normal background drafts. "
              "Scope intervals are inclusive and must not be summed across nesting. Exclusive phase stages are separate. "
              "First action for a target is the first action in the measured phase; startup already used VLOOK. "
              "Each first_paint_scope_ms matches the recorded paint completion within 3 ms, or is null. "
              "All samples, theme timelines and working-set measurements retained; nearest-rank quantiles checked from raw samples "
              "and clock differences. Working sets are not heap sizes or guaranteed peaks. Small samples do not establish a population tail.",
              **runs}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Validated {len(runs['before'])} before and {len(runs['after'])} after runs: {args.output}")


if __name__ == "__main__":
    main()
