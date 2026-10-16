"""Interleaved Core document preparation; independent from foreground GUI validation."""
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
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--details", action="store_true", help="Separate perturbed diagnostic samples")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error("repeats must be 1..10")
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=False)
    directories = {"before": args.before.resolve(), "after": args.after.resolve()}
    pins = {str(directory / name): digest(directory / name) for directory in directories.values()
            for name in ("MuffinCore.dll", "MuffinAstPerformanceProbe.exe")}
    assert pins[str(directories['before'] / 'MuffinAstPerformanceProbe.exe')] == pins[
        str(directories['after'] / 'MuffinAstPerformanceProbe.exe')]
    env = {k: v for k, v in os.environ.items() if not k.startswith("MUFFIN_")}
    env["PATH"] = str(root / "build/dist") + os.pathsep + env["PATH"]
    if args.details:
        env["MUFFIN_AST_PROFILE"] = "1"
    cases = [(30, "ascii"), (1, "escaped")] if args.details else [
        (1, "ascii"), (10, "ascii"), (30, "ascii"), (1, "unicode"), (1, "escaped")]
    records = []
    for repeat in range(1, args.repeats + 1):
        for mib, workload in (cases if repeat % 2 else list(reversed(cases))):
            for label in (("before", "after") if repeat % 2 else ("after", "before")):
                output = args.output / f"{label}-{mib}MiB-{workload}-r{repeat}.json"
                process = subprocess.run([str(directories[label] / "MuffinAstPerformanceProbe.exe"), str(mib), workload],
                                         env=env, cwd=root, capture_output=True, check=True, timeout=180)
                output.write_bytes(process.stdout)
                output.with_suffix(".log").write_bytes(process.stderr)
                raw = json.loads(process.stdout)
                records.append(dict(label=label, repeat=repeat, mib=mib, workload=workload, path=str(output), raw=raw))
                convert = next(p['duration_ms'] for p in raw['phases'] if p['name'] == 'parse.convertBlock')
                print(f"r{repeat} {label} {mib}MiB/{workload}: convert={convert:.2f}ms, "
                      f"parse={raw['parse_ms']:.2f}ms, session={raw['wall_ms']:.2f}ms", flush=True)
    comparisons = []
    for mib, workload in cases:
        row = dict(mib=mib, workload=workload)
        for label in directories:
            samples = [r['raw'] for r in records if (r['label'], r['mib'], r['workload']) == (label, mib, workload)]
            row[label] = dict(wall_median_ms=statistics.median(r['wall_ms'] for r in samples),
                              parse_median_ms=statistics.median(r['parse_ms'] for r in samples),
                              phases_ms={p['name']: statistics.median(next(v['duration_ms'] for v in r['phases']
                                         if v['name'] == p['name']) for r in samples) for p in samples[0]['phases']})
        comparisons.append(row)
    for sample in records:
        reference = next(r for r in records if r['label'] == 'before' and
                         (r['repeat'], r['mib'], r['workload']) == (sample['repeat'], sample['mib'], sample['workload']))
        for key in ('text_chars', 'blocks', 'inlines', 'shared_inlines', 'ast_fingerprint'):
            assert sample['raw'][key] == reference['raw'][key], key
    assert {p: digest(Path(p)) for p in pins} == pins
    result = dict(records=records, comparisons=comparisons, details=args.details, pinned_sha256=pins,
                  method="Same EXE, baseline/candidate Core DLLs, fresh processes in interleaved/reversed order. "
                         "Synchronous DocumentSession preparation, coarse parser metrics, no per-inline logging. "
                         "Construction/workload generation, node census and destruction excluded from wall_ms. "
                         "Detail samples separately add fine clocks and counts. Not GUI latency.")
    (args.output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
