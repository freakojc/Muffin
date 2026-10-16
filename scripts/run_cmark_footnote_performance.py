"""Interleave frozen pre-change cmark executables and the footnote fast path."""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=Path('build/footnote-baseline'))
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
    sources = [root / 'scripts/cmark_performance_probe.cpp', root / 'scripts/cmark_stage_profile.c',
               root / 'scripts/cmark_stage_profile.h', root / 'build/cmark-profiled-blocks.inc',
               root / 'third_party/cmark-gfm/src/parser.h']
    sources += list((root / 'third_party/cmark-gfm/src').glob('*.c'))
    sources += list((root / 'third_party/cmark-gfm/extensions').glob('*.c'))
    pins = {str(path): digest(path) for path in [*programs.values(), *sources]}
    preset = json.loads((root / 'build/generators/CMakePresets.json').read_text(encoding='utf-8'))
    env = {k: v for k, v in os.environ.items() if not k.startswith('MUFFIN_')}
    env['PATH'] = preset['configurePresets'][0]['environment']['QT_HOST_PATH'] + '/bin;' + env.get('PATH', '')
    cases = [(size, 'gui') for size in (1, 10, 30)]
    cases += [(30, 'plain'), (1, 'extensions'), (1, 'gui-footnote'), (1, 'gui-undefined')]
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
                assert raw['MiB'] == size and raw['workload'] == workload and raw['mode'] == 'full'
                assert raw['profiling'] == (profiling == 'stages')
                records.append(dict(repeat=repeat, variant=variant, path=str(path.resolve()), raw=raw))
                print(f"r{repeat} {size}MiB/{workload} {variant}: total={raw['total_ms']:.2f}ms, "
                      f"footnotes={raw['footnotes_ms']:.3f}ms", flush=True)
    summaries = []
    for size, workload in cases:
        selected = [r for r in records if r['raw']['MiB'] == size and r['raw']['workload'] == workload]
        baseline = selected[0]['raw']
        for record in selected:
            raw = record['raw']
            for key in ('tree', 'html_sha256', 'text_chars', 'utf8_bytes', 'extensions'):
                assert raw[key] == baseline[key], (record['path'], key)
            assert raw['inline_ms'] <= raw['inline_walk_ms'] + .01
            assert sum(raw[k] for k in ('inline_walk_ms', 'footnotes_ms', 'consolidate_ms',
                                        'autolink_postprocess_ms')) <= raw['finish_ms'] + .01
        row = dict(mib=size, workload=workload)
        for variant in programs:
            samples = [r['raw'] for r in selected if r['variant'] == variant]
            row[variant] = {
                'n': len(samples),
                'medians_ms': {key: statistics.median(r[key] for r in samples) for key in
                               ('setup_ms', 'feed_ms', 'finish_ms', 'total_ms', 'inline_ms', 'inline_walk_ms',
                                'footnotes_ms', 'consolidate_ms', 'autolink_postprocess_ms', 'release_ms')},
                'total_samples_ms': [r['total_ms'] for r in samples],
                'memory_medians_bytes': {
                    phase: {key: statistics.median(r[phase][key] for r in samples) for key in samples[0][phase]}
                    for phase in ('prepared_memory', 'parsed_memory', 'released_memory')}
            }
        summaries.append(row)
    assert {path: digest(path) for path in pins} == pins
    result = dict(complete=True, records=records, summaries=summaries, pinned_sha256=pins,
                  baseline_pins=baseline_pins, fingerprints_match=True,
                  method='Three or more fresh-process repeats; variants and cases reverse every second round. '
                         'All production extensions and footnotes remain enabled. Ordinary-archive timing is the '
                         'performance comparison; separately instrumented stages identify costs. Source preparation, '
                         'fingerprints/HTML and destruction are outside parse timing; destruction is reported separately. '
                         'Memory is OS sampling, not allocation attribution or a leak test. No GUI timing claims.')
    (args.output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
