"""Isolated cmark stages and ordinary archive controls; no GUI latency claims."""
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
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error('repeats must be 1..10')
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=False)
    programs = {name: root / f'build/Release/MuffinCmark{name}Probe.exe'
                for name in ('Performance', 'Archive')}
    sources = [root / 'build/cmark-profiled-blocks.inc', root / 'scripts/cmark_performance_probe.cpp', root / 'scripts/cmark_stage_profile.c',
               root / 'scripts/cmark_stage_profile.h']
    sources += list((root / 'third_party/cmark-gfm/src').glob('*.c'))
    sources += list((root / 'third_party/cmark-gfm/extensions').glob('*.c'))
    pins = {str(path): digest(path) for path in [*programs.values(), *sources]}
    env = {k: v for k, v in os.environ.items() if not k.startswith('MUFFIN_')}
    preset = json.loads((root / 'build/generators/CMakePresets.json').read_text(encoding='utf-8'))
    env['PATH'] = preset['configurePresets'][0]['environment']['QT_HOST_PATH'] + '/bin;' + env.get('PATH', '')
    cases = [(size, 'gui', mode) for size in (1, 10, 30) for mode in ('full', 'no-autolink')]
    cases += [(30, 'plain', 'full'), (1, 'extensions', 'full'),
              (1, 'extensions', 'no-autolink'), (1, 'extensions', 'none')]
    records = []
    for repeat in range(1, args.repeats + 1):
        for size, workload, mode in (cases if repeat % 2 else list(reversed(cases))):
            variants = [('Archive', 'off'), ('Performance', 'stages')]
            # Compare the diagnostic object's disabled hooks too, separately
            # from the ordinary archive, at each GUI size once.
            if repeat == 1 and workload == 'gui' and mode == 'full':
                variants.append(('Performance', 'off'))
            if repeat % 2 == 0:
                variants.reverse()
            for implementation, profiling in variants:
                path = args.output / f'{size}MiB-{workload}-{mode}-{implementation}-{profiling}-r{repeat}.json'
                process = subprocess.run([str(programs[implementation]), str(size), workload, mode, profiling],
                                         cwd=root, env=env, capture_output=True, timeout=180)
                path.write_bytes(process.stdout)
                path.with_suffix('.log').write_bytes(process.stderr)
                process.check_returncode()
                raw = json.loads(process.stdout)
                assert raw['MiB'] == size and raw['workload'] == workload and raw['mode'] == mode
                assert raw['profiling'] == (profiling == 'stages')
                assert all(raw[k] >= 0 for k in ('setup_ms', 'feed_ms', 'finish_ms', 'inline_ms',
                                                'consolidate_ms', 'autolink_postprocess_ms', 'release_ms'))
                assert raw['inline_ms'] <= raw['inline_walk_ms'] + .01
                assert raw['inline_walk_ms'] + raw['footnotes_ms'] + raw['consolidate_ms'] + raw['autolink_postprocess_ms'] <= raw['finish_ms'] + .01
                records.append(dict(repeat=repeat, implementation=implementation, profiling=profiling,
                                    path=str(path.resolve()), raw=raw))
                print(f"r{repeat} {size}MiB/{workload}/{mode} {implementation}/{profiling}: "
                      f"feed={raw['feed_ms']:.1f}, inline={raw['inline_ms']:.1f}, "
                      f"footnotes={raw['footnotes_ms']:.1f}, consolidate={raw['consolidate_ms']:.1f}, post={raw['autolink_postprocess_ms']:.1f}, "
                      f"total={raw['total_ms']:.1f}ms", flush=True)
    for record in records:
        raw = record['raw']
        baseline = next(r['raw'] for r in records if r['implementation'] == 'Archive' and
                        r['repeat'] == record['repeat'] and
                        all(r['raw'][k] == raw[k] for k in ('MiB', 'workload', 'mode')))
        for key in ('tree', 'html_sha256', 'text_chars', 'utf8_bytes', 'extensions'):
            assert raw[key] == baseline[key], (record['path'], key)
    summaries = []
    for size, workload, mode in cases:
        row = dict(mib=size, workload=workload, mode=mode)
        for implementation, profiling in [('Archive', 'off'), ('Performance', 'stages'), ('Performance', 'off')]:
            selected = [r['raw'] for r in records if r['implementation'] == implementation and
                        r['profiling'] == profiling and r['raw']['MiB'] == size and
                        r['raw']['workload'] == workload and r['raw']['mode'] == mode]
            if selected:
                row[f'{implementation}_{profiling}'] = {
                    'n': len(selected),
                    'medians_ms': {key: statistics.median(r[key] for r in selected) for key in
                                   ('setup_ms', 'feed_ms', 'finish_ms', 'total_ms', 'inline_ms', 'inline_walk_ms', 'footnotes_ms',
                                    'consolidate_ms', 'autolink_postprocess_ms', 'release_ms')},
                    'total_samples_ms': [r['total_ms'] for r in selected]}
        summaries.append(row)
    assert {path: digest(path) for path in pins} == pins
    result = dict(complete=True, records=records, summaries=summaries, pinned_sha256=pins,
                  fingerprints_match_ordinary_archive=True,
                  method='Fresh processes in alternating/reversed order. Ordinary unmodified archive controls and '
                         'diagnostic-only blocks.c object with two guarded generated phase hooks. Per-inline-container clocks '
                         'perturb diagnostic timing; inline sums exclude traversal, inline_walk includes it. Feed includes block extension hooks; '
                         'footnotes are timed directly; finish residual includes block finalization and cleanup. Autolink postprocess '
                         'is timed directly; removing extensions is a diagnostic ablation, not a product proposal. '
                         'Source generation, tree/HTML fingerprints and release excluded from parse timing. '
                         'Same-mode fingerprints checked; different extension modes can change semantics.')
    (args.output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
