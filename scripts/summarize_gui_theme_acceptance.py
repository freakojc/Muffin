"""Revalidate GUI theme-switch raw clocks and preserve every sample, including outliers."""
import argparse
import json
from pathlib import Path

from run_gui_theme_acceptance import TARGETS, distribution, inspect_run, require


def collect(directory):
    manifest = json.loads((directory / "acceptance.json").read_text(encoding="utf-8"))
    require(manifest["complete"] and manifest["accepted_runs"] == manifest["expected_runs"], "Incomplete matrix")
    records = []
    for record in manifest["records"]:
        raw = json.loads(Path(record["path"]).read_text(encoding="utf-8"))
        inspect_run(raw, record["size_mib"], record["tabs"], manifest["cycles"], manifest["profile"])
        raw["repeat"] = record["repeat"]
        raw["raw_path"] = record["path"]
        records.append(raw)
    return manifest, records


def summarize(records):
    matrix = []
    for size, tabs in sorted({(r["MiB_per_tab"], r["tabs"]) for r in records}):
        runs = [r for r in records if (r["MiB_per_tab"], r["tabs"]) == (size, tabs)]
        targets = {}
        for target in TARGETS:
            first, repeated = [], []
            for run in runs:
                rows = [r for r in run["phases"][0]["samples"] if r["detail"] == target]
                first.append(rows[0])
                repeated.extend(rows[1:])
            targets[target] = {kind: {
                "to_first_paint": distribution([r["dispatch_to_paint_ms"] for r in rows]),
                "handler": distribution([r["sync_ms"] for r in rows]),
                "paint_wait": distribution([r["handler_end_to_paint_start_ms"] for r in rows]),
                "queue_wait": distribution([r["post_to_dispatch_ms"] for r in rows]),
                "stages_exclusive": {stage: distribution([r["stages_to_first_paint_ms"][stage] for r in rows])
                                     for stage in rows[0]["stages_to_first_paint_ms"]}}
                for kind, rows in (("first", first), ("repeated", repeated))}
        memory = [{"repeat": r["repeat"], "post_open_mib": r["open"][-1]["working_set_bytes"] / 1048576,
                   "theme_before_mib": r["phases"][0]["working_set_before"] / 1048576,
                   "theme_after_mib": r["phases"][0]["working_set_after"] / 1048576,
                   "theme_sampled_max_mib": r["phases"][0]["sampled_max_working_set"] / 1048576,
                   "draft_drained_mib": r["draft_drain"]["working_set_after"] / 1048576,
                   "draft_drain_wait_ms": r["draft_drain"]["wait_ms"]}
                  for r in runs]
        functional = {}
        for name in ("typing_after_theme", "tab_switch_deferred_theme", "typing_multiple_tabs", "tab_switch_warm"):
            rows = [s for r in runs for p in r["phases"] if p["phase"] == name for s in p["samples"]]
            if rows:
                functional[name] = distribution([r["dispatch_to_paint_ms"] for r in rows])
        matrix.append({"size_mib": size, "tabs": tabs, "targets": targets, "memory": memory,
                       "functional_checks_to_paint": functional})
    return matrix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--off-control", type=Path)
    args = parser.parse_args()
    manifest, records = collect(args.directory)
    require(not manifest["pilot"] and {(r["MiB_per_tab"], r["tabs"]) for r in records} ==
            {(s, t) for s in (1, 10, 30) for t in (1, 3)}, "Missing six-way GUI matrix")
    matrix = summarize(records)
    ranked = []
    for run in records:
        for index, sample in enumerate(run["phases"][0]["samples"]):
            ranked.append({"size_mib": run["MiB_per_tab"], "tabs": run["tabs"], "repeat": run["repeat"],
                           "index": index, "first_for_target": index < 3, "sample": sample,
                           "raw_path": run["raw_path"]})
    ranked.sort(key=lambda r: -r["sample"]["dispatch_to_paint_ms"])
    result = {"method": manifest["method"], "pinned_sha256": manifest["pinned_sha256"],
              "accepted_runs": len(records), "matrix": matrix, "slowest_20": ranked[:20], "runs": records,
              "chain_handoffs": manifest.get("handoffs", []), "chain_states": manifest.get("chain_states", [])}
    bootstrap = args.directory / "bootstrap.json"
    if manifest.get("handoffs") and bootstrap.is_file():
        bootstrap_raw = json.loads(bootstrap.read_text(encoding="utf-8"))
        inspect_run(bootstrap_raw, 1, 3, 4, "minimal")
        result["bootstrap_excluded_from_formal_statistics"] = bootstrap_raw
    if args.off_control:
        off_manifest, off_records = collect(args.off_control)
        require(off_manifest["profile"] == "off", "Control has profiling enabled")
        require(off_manifest["pinned_sha256"] == manifest["pinned_sha256"], "Control uses different binaries/resources")
        result["off_control"] = {"matrix": summarize(off_records), "runs": off_records}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    for row in matrix:
        print(f"{row['size_mib']} MiB/{row['tabs']} tabs: " + ", ".join(
            f"{t} repeat p50/p95/max " + "/".join(f"{row['targets'][t]['repeated']['to_first_paint'][k]:.1f}"
                                                  for k in ('p50_ms', 'p95_ms', 'max_ms')) for t in TARGETS))
    print(f"Revalidated {len(records)} real GUI processes: {args.output}")


if __name__ == "__main__":
    main()
