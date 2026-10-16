"""Isolate Phycat paint costs with immutable local CSS overrides (offscreen)."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--paint-detail', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    source = Path(os.environ['APPDATA']) / 'Muffin/Muffin/themes/phycat-abyss.css'
    dependencies = [source, source.parent / 'phycat/phycat.dark.css',
                    source.parent / 'phycat/Cascadia-Code-Regular.ttf',
                    source.parent / 'phycat/LXGWWenKai-Regular.ttf', root / 'resources/themes/document-base.css']
    def digests():
        return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
    pinned = digests()
    variants = {
        'original': '',
        'no_strong_shadow': '#write strong {text-shadow:none!important}',
        'no_text_shadows': '#write,#write *,#write::before,#write::after,#write *::before,#write *::after'
                           '{text-shadow:none!important}',
        'no_link_masks': '#write p a:not(.md-toc-inner)::before{mask:none!important;-webkit-mask:none!important}',
    }
    # Import preserves relative font/image URLs inside the original files.
    # This edits only disposable wrappers; no installed theme is modified.
    wrappers = {}
    for name, override in variants.items():
        wrappers[name] = destination / (name + '.css')
        wrappers[name].write_text('@import url(' + json.dumps(source.as_posix(), ensure_ascii=False) + ');\n' + override + '\n',
                                  encoding='utf-8')
    preset = json.loads((root / 'build/generators/CMakePresets.json').read_text(encoding='utf-8'))
    env = dict(os.environ)
    env.update(QT_QPA_PLATFORM='offscreen', MUFFIN_FIRST_LAYOUT_ONLY='1',
               QT_PLUGIN_PATH=preset['testPresets'][0]['environment']['QT_PLUGIN_PATH'])
    env['PATH'] = preset['configurePresets'][0]['environment']['QT_HOST_PATH'] + '\\bin;' + env['PATH']
    for key in ['MUFFIN_THEME_LAYOUT_DETAIL', 'MUFFIN_THEME_CSS_DETAIL', 'MUFFIN_THEME_PAINT_DETAIL']:
        env.pop(key, None)
    env.pop('MUFFIN_THEME_PROBE_DOCUMENT', None)
    if args.paint_detail:
        env['MUFFIN_THEME_PAINT_DETAIL'] = '1'
    result = {'complete': False, 'dependency_sha256': pinned, 'overrides': variants,
              'wrapper_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in wrappers.values()},
              'method': 'Sequential fresh processes, 1 MiB, offscreen 960x900, rotated variant order each round. '
                        'CSS ablations intentionally change decoration pixels and are diagnostic, not optimizations. '
                        'Geometry, cursor hits, two return cycles and independent counter reference retained.', 'runs': []}
    def checkpoint():
        (destination / 'matrix.json').write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':')) + '\n',
                                                encoding='utf-8')
    checkpoint()
    names = list(variants)
    for repeat in range(args.repetitions):
        order = names[repeat % len(names):] + names[:repeat % len(names)]
        for name in order:
            assert pinned == digests(), 'Theme resources changed'
            output = destination / f'{name}-{repeat + 1}.json'
            print(output, flush=True)
            with output.with_suffix('.log').open('wb') as log:
                subprocess.run([str(root / 'build/Release/MuffinThemePerformanceProbe.exe'), str(output), '1',
                                str(wrappers[name])], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=120)
            raw = json.loads(output.read_text(encoding='utf-8'))
            assert raw['qpa'] == 'offscreen' and raw['full_counter_reference_matches'] and len(raw['visuals']) == 3
            result['runs'].append({'variant': name, 'repeat': repeat + 1, 'raw': raw, 'source': str(output)})
            checkpoint()
    assert pinned == digests(), 'Theme resources changed'
    reference = next(r['raw']['visuals'] for r in result['runs'] if r['variant'] == 'original')
    for run in result['runs']:
        for visual, expected in zip(run['raw']['visuals'], reference):
            assert visual['geometry'] == expected['geometry'], (run['variant'], 'Geometry changed')
        first = {k: v for k, v in run['raw']['visuals'][0].items() if k != 'stage'}
        assert all(first == {k: v for k, v in sample.items() if k != 'stage'} for sample in run['raw']['visuals'])
        if run['variant'] == 'original':
            assert run['raw']['visuals'] == reference
    result['geometry_matches'] = True
    result['complete'] = True
    checkpoint()
    print('Completed', len(result['runs']), 'processes', flush=True)


if __name__ == '__main__':
    main()
