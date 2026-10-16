"""Interleaved headless parser phases; use GUI probes for open-to-paint latency."""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True, help="Directory containing baseline Core and probe")
    parser.add_argument("--after", type=Path, required=True, help="Directory containing candidate Core and probe")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--details", action="store_true", help="Opt-in per-block/pair clocks; separate from formal phases")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error("repeats must be 1..10")
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=False)
    directories = {"before": args.before.resolve(), "after": args.after.resolve()}
    pins = {str(directory / name): digest(directory / name) for directory in directories.values()
            for name in ("MuffinCore.dll", "MuffinDelimiterPerformanceProbe.exe")}
    assert pins[str(directories["before"] / "MuffinDelimiterPerformanceProbe.exe")] == pins[
        str(directories["after"] / "MuffinDelimiterPerformanceProbe.exe")]
    env = {k: v for k, v in os.environ.items() if not k.startswith("MUFFIN_")}
    env["PATH"] = str(root / "build/dist") + os.pathsep + env["PATH"]
    if args.details:
        env["MUFFIN_DELIMITER_PROFILE"] = "1"
    cases = [(30, 12), (1, 3072)] if args.details else [(1, 12), (10, 12), (30, 12), (1, 192), (1, 3072)]
    records = []
    for repeat in range(1, args.repeats + 1):
        for mib, paragraph in (cases if repeat % 2 else list(reversed(cases))):
            for label in (("before", "after") if repeat % 2 else ("after", "before")):
                executable = directories[label] / "MuffinDelimiterPerformanceProbe.exe"
                raw_path = args.output / f"{label}-{mib}MiB-{paragraph}runs-r{repeat}.json"
                result = subprocess.run([str(executable), str(mib), str(paragraph), "1"], cwd=root, env=env,
                                        capture_output=True, timeout=180, check=True)
                raw_path.write_bytes(result.stdout)
                raw_path.with_suffix(".log").write_bytes(result.stderr)
                raw = json.loads(result.stdout)
                row = raw["iterations"][0]
                records.append(dict(label=label, repeat=repeat, mib=mib, paragraph=paragraph, path=str(raw_path), raw=raw))
                phase = next(p for p in row["phases"] if p["name"] == "parse.splitStrikethrough")
                print(f"r{repeat} {label} {mib}MiB/{paragraph} runs: strike={phase['duration_ms']:.2f}ms "
                      f"parse={row['wall_ms']:.2f}ms", flush=True)
    assert {path: digest(Path(path)) for path in pins} == pins
    comparisons = []
    for mib, paragraph in cases:
        comparison = dict(mib=mib, paragraph=paragraph)
        for label in directories:
            rows = [r["raw"]["iterations"][0] for r in records if
                    (r["label"], r["mib"], r["paragraph"]) == (label, mib, paragraph)]
            comparison[label] = dict(wall_median_ms=statistics.median(r["wall_ms"] for r in rows),
                                    phases_ms={p["name"]: statistics.median(next(v["duration_ms"] for v in r["phases"]
                                               if v["name"] == p["name"]) for r in rows) for p in rows[0]["phases"]})
        comparisons.append(comparison)
    summary = dict(pinned_sha256=pins, details=args.details, records=records, comparisons=comparisons,
                   method="Same probe EXE, separate Core DLLs; fresh headless process, interleaved order reversed on even repeats. "
                          "Text creation and AST release excluded from parse timing. Coarse phase metrics enabled; "
                          "per-inline diagnostics disabled. Detail clocks are separate exploratory samples. Not GUI latency.")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
