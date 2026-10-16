"""Audit raw autolink timings, oracle results, fused hooks and Core preparation."""
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
    parser.add_argument('--ast-summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    data = json.loads(args.summary.read_text(encoding='utf-8'))
    ast = json.loads(args.ast_summary.read_text(encoding='utf-8'))
    assert data['complete'] and data['fingerprints_match']
    for pins in (data['pinned_sha256'], ast['pinned_sha256']):
        for path, expected in pins.items():
            assert digest(path) == expected, path
    for path, expected in data['baseline_pins'].items():
        if path != 'git_head':
            frozen = root / 'build/autolink-baseline' / Path(path).name
            assert digest(frozen) == expected, str(frozen)
    records = data['records']
    repeats = len({r['repeat'] for r in records})
    assert repeats >= 3 and len(records) == 11 * 4 * repeats
    for record in records:
        raw = json.loads(Path(record['path']).read_bytes())
        assert raw == record['raw']
        assert math.isclose(raw['total_ms'], sum(raw[k] for k in ('setup_ms', 'feed_ms', 'finish_ms')), abs_tol=1e-8)
        for key, value in raw.items():
            if key.endswith('_ms'):
                assert math.isfinite(value) and value >= 0
        assert raw['inline_ms'] <= raw['inline_walk_ms'] + .01
        assert sum(raw[k] for k in ('inline_walk_ms', 'footnotes_ms', 'consolidate_ms',
                                   'autolink_postprocess_ms')) <= raw['finish_ms'] + .01
        enclosing = 'consolidate_ms' if record['variant'].startswith('candidate') else 'autolink_postprocess_ms'
        assert raw['autolink_text_ms'] <= raw[enclosing] + .01
        if record['variant'] == 'candidate_stages':
            assert raw['postprocess_calls'] == 0 and raw['autolink_consolidate_ms'] == 0
            assert raw['autolink_text_calls'] == raw['autolink_candidates']
            if raw['workload'] in ('gui', 'plain'):
                assert raw['autolink_candidates'] == 0
            if raw['workload'] == 'gui-email':
                assert raw['autolink_candidates'] == 1
    comparisons = []
    for row in data['summaries']:
        samples = [r for r in records if (r['raw']['MiB'], r['raw']['workload']) == (row['mib'], row['workload'])]
        reference = samples[0]['raw']
        for record in samples:
            for key in ('tree', 'html_sha256', 'text_chars', 'utf8_bytes', 'extensions'):
                assert record['raw'][key] == reference[key]
        for variant in ('baseline_archive', 'baseline_stages', 'candidate_archive', 'candidate_stages'):
            selected = [r['raw'] for r in samples if r['variant'] == variant]
            assert len(selected) == row[variant]['n'] == repeats
            for key, expected in row[variant]['medians_ms'].items():
                assert statistics.median(r[key] for r in selected) == expected
            for key, expected in row[variant]['counts'].items():
                assert [r[key] for r in selected] == expected
            for phase, fields in row[variant]['memory_medians_bytes'].items():
                for key, expected in fields.items():
                    assert statistics.median(r[phase][key] for r in selected) == expected
        before = row['baseline_archive']['medians_ms']['total_ms']
        after = row['candidate_archive']['medians_ms']['total_ms']
        comparisons.append(dict(mib=row['mib'], workload=row['workload'], before_ms=before, after_ms=after,
                                reduction_ms=before-after, reduction_percent=(before-after)/before*100))
    for record in ast['records']:
        raw = json.loads(Path(record['path']).read_bytes())
        assert raw == record['raw']
        original = next(r['raw'] for r in ast['records'] if r['label'] == 'before' and
                        (r['repeat'], r['mib'], r['workload']) == (record['repeat'], record['mib'], record['workload']))
        for key in ('text_chars', 'blocks', 'inlines', 'shared_inlines', 'ast_fingerprint'):
            assert raw[key] == original[key]
    for row in ast['comparisons']:
        for label in ('before', 'after'):
            selected = [r['raw'] for r in ast['records'] if
                        (r['label'], r['mib'], r['workload']) == (label, row['mib'], row['workload'])]
            for field in ('wall', 'parse'):
                assert statistics.median(r[field+'_ms'] for r in selected) == row[label][field+'_median_ms']
            for name, expected in row[label]['phases_ms'].items():
                assert statistics.median(next(p['duration_ms'] for p in r['phases'] if p['name'] == name)
                                         for r in selected) == expected
    inputs = root / 'tests/fixtures/parser/cmark-autolink-inputs.json'
    oracle = json.loads((root / 'tests/fixtures/parser/cmark-autolink-oracle.json').read_text(encoding='utf-8'))
    assert oracle['provenance']['source_sha256'] == data['baseline_pins']
    assert digest(inputs) == oracle['provenance']['inputs_sha256']
    candidate = json.loads((root / 'build/autolink-candidate-oracle-final.json').read_bytes())
    assert len(candidate) == 1736 and candidate == oracle['records']
    for repeat in range(3):
        assert candidate == json.loads((root / f'build/autolink-baseline/oracle-{repeat}.json').read_bytes())
    blocks = (root / 'build/cmark-profiled-blocks.inc').read_text(encoding='utf-8')
    for stage, call in enumerate(('process_inlines(parser, parser->refmap, parser->options);',
                                  'process_footnotes(parser);')):
        hook = f'{{ muffin_probe_stage_begin({stage}); {call} muffin_probe_stage_end({stage}); }}'
        assert blocks.count(hook) == 1
        blocks = blocks.replace(hook, call)
    assert blocks == (root / 'third_party/cmark-gfm/src/blocks.c').read_text(encoding='utf-8')
    autolink = (root / 'build/cmark-profiled-autolink.inc').read_text(encoding='utf-8')
    assert autolink.count('muffin_autolink_text(parser, node);') == 2
    autolink = autolink.replace('muffin_autolink_text(parser, node);', 'postprocess_text(parser, node);')
    for statement in ('node = cmark_iter_get_node(iter);', 'cmark_chunk *literal = &node->as.literal;'):
        assert autolink.count(statement+' muffin_autolink_visit();') == 1
        autolink = autolink.replace(statement+' muffin_autolink_visit();', statement)
    assert autolink == (root / 'third_party/cmark-gfm/extensions/autolink.c').read_text(encoding='utf-8')
    data['ast_preparation'] = ast
    data['semantic_oracle_provenance'] = oracle['provenance']
    data['audit'] = dict(raw_cmark_records=len(records), raw_ast_records=len(ast['records']),
                         semantic_snapshots=1736, stable_original_runs=3, generated_hooks_only=True,
                         pinned_sources_and_binaries_match=True, comparisons=comparisons)
    args.output.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(f'Audited {len(records)} cmark records, {len(ast["records"])} Core records, 1736 snapshots and source/binary pins.')
    for row in comparisons:
        print(f"{row['mib']}MiB/{row['workload']}: {row['before_ms']:.2f} -> {row['after_ms']:.2f}ms "
              f"({row['reduction_percent']:+.1f}% reduction)")


if __name__ == '__main__':
    main()
