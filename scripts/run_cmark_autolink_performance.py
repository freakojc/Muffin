"""Interleave the frozen pre-autolink-change archives and the fused text pass."""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
from pathlib import Path


TIMES = ('setup_ms', 'feed_ms', 'finish_ms', 'total_ms', 'inline_ms', 'inline_walk_ms', 'footnotes_ms',
         'consolidate_ms', 'autolink_postprocess_ms', 'autolink_consolidate_ms', 'autolink_text_ms', 'release_ms')
COUNTS = ('autolink_visits', 'autolink_candidates', 'autolink_text_calls', 'autolink_text_bytes')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=Path('build/autolink-baseline'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if not 3 <= args.repeats <= 10:
        parser.error('repeats must be 3..10')
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=False)
    programs = {
        f'{version}_{variant}': directory / f'MuffinCmark{target}Probe.exe'
        for version, directory in [('baseline', args.baseline.resolve()), ('candidate', root / 'build/Release')]
        for variant, target in [('archive', 'Archive'), ('stages', 'Performance')]
    }
    baseline_pins = json.loads((args.baseline / 'pins.json').read_text(encoding='utf-8'))
    for variant, target in [('archive', 'Archive'), ('stages', 'Performance')]:
        assert digest(programs[f'baseline_{variant}']) == baseline_pins[f'build/Release/MuffinCmark{target}Probe.exe']
    sources = [root / 'scripts' / name for name in
               ('cmark_performance_probe.cpp', 'cmark_stage_profile.c', 'cmark_stage_profile.h', 'cmark_autolink_profile.c')]
    sources += [root / 'build' / name for name in ('cmark-profiled-blocks.inc', 'cmark-profiled-autolink.inc')]
    sources += list((root / 'third_party/cmark-gfm/src').glob('*.[ch]'))
    sources += list((root / 'third_party/cmark-gfm/extensions').glob('*.[ch]'))
    pins = {str(path): digest(path) for path in [*programs.values(), *sources]}
    preset = json.loads((root / 'build/generators/CMakePresets.json').read_text(encoding='utf-8'))
    env = {k: v for k, v in os.environ.items() if not k.startswith('MUFFIN_')}
    env['PATH'] = str(Path(preset['configurePresets'][0]['environment']['QT_HOST_PATH']) / 'bin') + os.pathsep + env.get('PATH', '')
    cases = [(size, workload) for size in (1, 10, 30) for workload in ('gui', 'gui-email')]
    cases += [(30, 'plain'), (1, 'extensions'), (1, 'gui-noise'), (1, 'gui-footnote'), (1, 'gui-undefined')]
    records = []
    for repeat in range(1, args.repeats + 1):
        for size, workload in (cases if repeat % 2 else list(reversed(cases))):
            variants = list(programs)
            if repeat % 2 == 0:
                variants.reverse()
            for variant in variants:
                profiling = 'stages' if variant.endswith('stages') else 'off'
                path = args.output / f'{size}MiB-{workload}-{variant}-r{repeat}.json'
                process = subprocess.run([str(programs[variant]), str(size), workload, 'full', profiling],
                                         cwd=root, env=env, capture_output=True, timeout=180)
                path.write_bytes(process.stdout)
                path.with_suffix('.log').write_bytes(process.stderr)
                process.check_returncode()
                raw = json.loads(process.stdout)
                assert (raw['MiB'], raw['workload'], raw['mode']) == (size, workload, 'full')
                records.append(dict(repeat=repeat, variant=variant, path=str(path.resolve()), raw=raw))
                print(f"r{repeat} {size}MiB/{workload} {variant}: total={raw['total_ms']:.2f}ms, "
                      f"merge={raw['consolidate_ms']:.2f}ms, post={raw['autolink_postprocess_ms']:.2f}ms", flush=True)
    summaries = []
    for size, workload in cases:
        selected = [r for r in records if (r['raw']['MiB'], r['raw']['workload']) == (size, workload)]
        baseline = selected[0]['raw']
        for record in selected:
            raw = record['raw']
            for key in ('tree', 'html_sha256', 'text_chars', 'utf8_bytes', 'extensions'):
                assert raw[key] == baseline[key], (record['path'], key)
            assert raw['inline_ms'] <= raw['inline_walk_ms'] + .01
            assert sum(raw[k] for k in ('inline_walk_ms', 'footnotes_ms', 'consolidate_ms',
                                        'autolink_postprocess_ms')) <= raw['finish_ms'] + .01
            # Text processing is nested in the fused consolidation stage for
            # candidates, and inside postprocess for the baseline.
            enclosing = 'consolidate_ms' if record['variant'].startswith('candidate') else 'autolink_postprocess_ms'
            assert raw['autolink_text_ms'] <= raw[enclosing] + .01
        row = dict(mib=size, workload=workload)
        for variant in programs:
            samples = [r['raw'] for r in selected if r['variant'] == variant]
            row[variant] = {
                'n': len(samples), 'medians_ms': {key: statistics.median(r[key] for r in samples) for key in TIMES},
                'counts': {key: [r[key] for r in samples] for key in COUNTS},
                'total_samples_ms': [r['total_ms'] for r in samples],
                'memory_medians_bytes': {
                    phase: {key: statistics.median(r[phase][key] for r in samples) for key in samples[0][phase]}
                    for phase in ('prepared_memory', 'parsed_memory', 'released_memory')}
            }
        summaries.append(row)
    assert {path: digest(path) for path in pins} == pins
    result = dict(complete=True, records=records, summaries=summaries, pinned_sha256=pins,
                  baseline_pins=baseline_pins, fingerprints_match=True,
                  method='Fresh processes, variants/cases reversed every second round, all production extensions enabled. '
                         'Ordinary archives compare parse time; diagnostic clocks perturb individual-text timings. '
                         'Candidate consolidate includes fused email processing; autolink_text is a subset, never add it again. '
                         'Baseline postprocess includes another consolidation, text processing and traversal. '
                         'Literal ownership preserved, no deferred get_literal allocation. '
                         'Generation/fingerprints/HTML/release excluded; release and OS memory separately sampled. Not GUI latency.')
    (args.output / 'summary.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
