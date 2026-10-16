"""Independently validate retained texture reproduction and cmark stage data."""
import argparse
import hashlib
import json
import statistics
from pathlib import Path

from PIL import Image, ImageChops


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pixels(path):
    with Image.open(path) as image:
        return image.convert('RGB')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    before = read('build/texture-isolated.json')
    after = read('build/texture-native-fixed.json')
    browser = read('build/texture-browser/browser.json')
    cmark = read('build/cmark-stages-final/summary.json')
    assert before['layers'][0]['position_y_fraction'] == .5
    assert all(layer['position_y_fraction'] == layer['position_x_fraction'] == 0 for layer in after['layers'])
    assert len(before['cases']) == len(after['cases']) == 28
    # Heights differing by 40px produce a 20px phase shift at 50% Y. Compare
    # the actual old images, rather than inferring a pixel result from CSS.
    old_a = pixels('build/texture-isolated/1e+06-0-0.png')
    old_b = pixels('build/texture-isolated/1.00004e+06-0-0.png')
    assert old_a != old_b
    for y in range(20, 780):
        assert old_a.getpixel((5, y)) == old_b.getpixel((5, y + 20))
    assert all(case['same_as_reference'] for case in after['cases'])
    for offset in (0, 30):
        images = list(Path('build/texture-native-fixed').glob(f'*-{offset}-*.png'))
        assert len(images) == 14
        assert all(pixels(image) == pixels(images[0]) for image in images)
    assert len(after['editor_cycles']) == 8
    for row in after['editor_cycles']:
        assert row['same_as_first'] and row['scroll_y'] == 0
        assert row['first_block'] == after['editor_cycles'][0]['first_block']
        assert pixels(f"build/texture-native-fixed/editor-cycle-{row['cycle']}.png") == pixels('build/texture-native-fixed/editor-cycle-0.png')
    browser_rows = []
    for case in browser['cases']:
        assert case['position'] == '0% 0%, 0% 0%'
        image = pixels(case['screenshot']).crop((0, 0, 1086, 800))
        native = pixels(f"build/texture-native-fixed/{format(case['height'], '.6g')}-0-0.png").crop((0, 0, 1086, 800))
        difference = ImageChops.difference(image, native)
        maximum = max(max(pixel) for pixel in difference.getdata())
        # Independent Skia/Qt PNG compositing differs by one channel level;
        # this is not the exact-equality rule used for repeat/native comparisons.
        assert maximum <= 1
        browser_rows.append(dict(height=case['height'], max_channel_difference=maximum,
                                 screenshot=case['screenshot']))
    for path, expected in cmark['pinned_sha256'].items():
        assert digest(path) == expected, path
    # Verify the generated diagnostic source differs from vendor only at the
    # two explicitly declared phase calls. No application archive changes.
    vendor = Path('third_party/cmark-gfm/src/blocks.c').read_text(encoding='utf-8')
    generated = Path('build/cmark-profiled-blocks.inc').read_text(encoding='utf-8')
    for stage, call in enumerate(('process_inlines(parser, parser->refmap, parser->options);',
                                  'process_footnotes(parser);')):
        assert vendor.count(call) == 1
        generated = generated.replace(f'{{ muffin_probe_stage_begin({stage}); {call} muffin_probe_stage_end({stage}); }}', call)
    assert generated == vendor
    assert cmark['complete'] and len(cmark['records']) == 63
    clocks = 0
    for record in cmark['records']:
        raw = read(record['path'])
        assert raw == record['raw']
        assert abs(raw['total_ms'] - raw['setup_ms'] - raw['feed_ms'] - raw['finish_ms']) < .001
        if raw['profiling']:
            assert raw['inline_calls'] > 0 and raw['inline_ms'] <= raw['inline_walk_ms']
            assert raw['inline_walk_ms'] + raw['footnotes_ms'] + raw['consolidate_ms'] + raw['autolink_postprocess_ms'] <= raw['finish_ms']
            assert raw['postprocess_calls'] == int(raw['mode'] == 'full')
        control = next(r['raw'] for r in cmark['records'] if r['implementation'] == 'Archive' and
                       r['repeat'] == record['repeat'] and
                       all(r['raw'][key] == raw[key] for key in ('MiB', 'workload', 'mode')))
        assert raw['tree'] == control['tree'] and raw['html_sha256'] == control['html_sha256']
        clocks += 1
    for row in cmark['summaries']:
        for implementation, profiling in [('Archive', 'off'), ('Performance', 'stages'), ('Performance', 'off')]:
            key = implementation + '_' + profiling
            if key not in row:
                continue
            records = [r['raw'] for r in cmark['records'] if r['implementation'] == implementation and
                       r['profiling'] == profiling and r['raw']['MiB'] == row['mib'] and
                       r['raw']['workload'] == row['workload'] and r['raw']['mode'] == row['mode']]
            for field, median in row[key]['medians_ms'].items():
                assert statistics.median(raw[field] for raw in records) == median
    paths = [*Path('build/texture-isolated').glob('*.png'), *Path('build/texture-native-fixed').glob('*.png'),
             *Path('build/texture-browser').glob('*.png')]
    result = dict(texture_before=before, texture_after=after, browser=browser, cmark=cmark,
                  independent_audit=dict(old_height_change_reproduces_20px_phase=True,
                                         native_fixed_cases=28, editor_repeat_cases=8,
                                         browser_comparisons=browser_rows, cmark_records=clocks,
                                         generated_hooks_only=True, ordinary_archive_fingerprints_equal=True,
                                         image_sha256={str(path): digest(path) for path in paths}),
                  method='Isolated raster and native Windows QPA EditorView hidden from the desktop; no foreground GUI latency claim. '
                         'Exact native repeat pixel checks; independent browser comparison allows at most one compositing channel level. '
                         'cmark stages and ordinary archive controls measured in separate alternating fresh processes; not a product optimization result.')
    args.output.write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')
    print('Audited: old 20px phase reproduction; 28 stable rasters; 8 EditorView cycles; 3 browser comparisons; 63 cmark records.')


if __name__ == '__main__':
    main()
