"""Independently recompute footnote benchmark statistics and verify raw pins."""
import argparse
import hashlib
import json
import math
import statistics
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    data = json.loads(args.summary.read_text(encoding='utf-8'))
    assert data['complete'] and data['fingerprints_match']
    for path, expected in data['pinned_sha256'].items():
        assert digest(path) == expected, path
    records = data['records']
    repeats = len({r['repeat'] for r in records})
    assert repeats >= 3 and len(records) == 7 * 4 * repeats
    checked = 0
    for record in records:
        raw = json.loads(Path(record['path']).read_bytes())
        assert raw == record['raw'], record['path']
        assert math.isclose(raw['total_ms'], raw['setup_ms'] + raw['feed_ms'] + raw['finish_ms'], abs_tol=1e-8)
        for field, value in raw.items():
            if field.endswith('_ms'):
                assert math.isfinite(value) and value >= 0, (record['path'], field)
        assert raw['inline_ms'] <= raw['inline_walk_ms'] + .01
        assert sum(raw[key] for key in ('inline_walk_ms', 'footnotes_ms', 'consolidate_ms',
                                       'autolink_postprocess_ms')) <= raw['finish_ms'] + .01
        checked += 1
    comparisons = []
    for row in data['summaries']:
        samples = [r for r in records if r['raw']['MiB'] == row['mib'] and r['raw']['workload'] == row['workload']]
        first = samples[0]['raw']
        for record in samples:
            for key in ('tree', 'html_sha256', 'text_chars', 'utf8_bytes', 'extensions'):
                assert record['raw'][key] == first[key], (record['path'], key)
        for variant in ('baseline_archive', 'candidate_archive', 'baseline_stages', 'candidate_stages'):
            selected = [r['raw'] for r in samples if r['variant'] == variant]
            assert len(selected) == row[variant]['n'] == repeats
            for key, expected in row[variant]['medians_ms'].items():
                assert statistics.median(r[key] for r in selected) == expected
            for phase, fields in row[variant]['memory_medians_bytes'].items():
                for key, expected in fields.items():
                    assert statistics.median(r[phase][key] for r in selected) == expected
        before = row['baseline_archive']['medians_ms']['total_ms']
        after = row['candidate_archive']['medians_ms']['total_ms']
        comparisons.append(dict(mib=row['mib'], workload=row['workload'], before_ms=before, after_ms=after,
                                reduction_ms=before-after, reduction_percent=(before-after)/before*100))
    inputs_path = root / 'tests/fixtures/parser/cmark-footnote-inputs.json'
    oracle = json.loads((root / 'tests/fixtures/parser/cmark-footnote-oracle.json').read_text(encoding='utf-8'))
    candidate = json.loads((root / 'build/footnote-candidate-oracle-final.json').read_bytes())
    assert digest(inputs_path) == oracle['provenance']['inputs_sha256']
    assert len(candidate) == 636 and candidate == oracle['records']
    for repeat in range(3):
        original = json.loads((root / f'build/footnote-baseline/oracle-stable-{repeat}.json').read_bytes())
        assert original == candidate
    initial_path = root / 'build/footnote-baseline/oracle-initial.json'
    initial = json.loads(initial_path.read_text(encoding='utf-8'))['records']
    issue = json.loads((root / 'build/footnote-upstream-instability.json').read_text(encoding='utf-8'))
    mismatch_counts = []
    for repeat in range(2):
        original = json.loads((root / f'build/footnote-original-repeat-{repeat}.json').read_bytes())
        mismatch_counts.append(sum(a != b for a, b in zip(initial, original)))
    assert mismatch_counts == issue['original_repeat_mismatch_counts'] == [33, 30]
    vendor = (root / 'third_party/cmark-gfm/src/blocks.c').read_text(encoding='utf-8')
    generated = (root / 'build/cmark-profiled-blocks.inc').read_text(encoding='utf-8')
    for stage, call in enumerate(('process_inlines(parser, parser->refmap, parser->options);',
                                  'process_footnotes(parser);')):
        hook = f'{{ muffin_probe_stage_begin({stage}); {call} muffin_probe_stage_end({stage}); }}'
        assert generated.count(hook) == 1
        generated = generated.replace(hook, call)
    assert generated == vendor
    data['audit'] = dict(raw_records=checked, semantic_snapshots=636, stable_original_runs=3,
                         generated_hooks_only=True, pinned_sources_and_binaries_match=True,
                         comparisons=comparisons)
    data['semantic_oracle_provenance'] = oracle['provenance']
    data['known_original_instability'] = issue
    args.output.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(f'Audited {checked} raw records, 636 semantic snapshots and 3 original-library repetitions.')
    for row in comparisons:
        print(f"{row['mib']}MiB/{row['workload']}: {row['before_ms']:.2f} -> {row['after_ms']:.2f}ms "
              f"({row['reduction_percent']:+.1f}% reduction)")


if __name__ == '__main__':
    main()
