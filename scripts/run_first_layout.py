"""Repeat fixed offscreen theme layout stages; no keyboard or foreground required."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--detail", action="store_true")
    parser.add_argument("--css-detail", action="store_true")
    parser.add_argument("--paint-detail", action="store_true")
    parser.add_argument("--lifecycle", action="store_true", help="Measure isolated ThemeManager cold and repeated selection/snapshots")
    parser.add_argument("--manager-document", action="store_true", help="Apply ThemeManager selection callbacks to an existing document and paint its first viewport")
    parser.add_argument("--real-switch", action="store_true", help="Use detached snapshots and semantic seed reuse, as EditorView::setTheme does")
    parser.add_argument("--sizes", type=int, nargs="+", default=[1, 10, 30])
    parser.add_argument("--themes", nargs="+", choices=['vlook', 'phycat', 'github'], default=['vlook', 'phycat', 'github'])
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--document", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    phycat = Path(os.environ['APPDATA']) / 'Muffin/Muffin/themes/phycat-abyss.css'
    themes = {
        'vlook': root / 'tests/fixtures/theme/vlook-fancy-dark/vlook-fancy-dark.css',
        'phycat': phycat,
        'github': root / 'resources/themes/github.css',
    }
    dependencies = [*themes.values(), phycat.parent / 'phycat/phycat.dark.css',
                    phycat.parent / 'phycat/Cascadia-Code-Regular.ttf', phycat.parent / 'phycat/LXGWWenKai-Regular.ttf',
                    root / 'resources/themes/document-base.css']
    if args.document:
        dependencies.append(args.document.resolve())
    def digests():
        return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
    pinned = digests()
    preset = json.loads((root / 'build/generators/CMakePresets.json').read_text(encoding='utf-8'))
    env = dict(os.environ)
    env.update(QT_QPA_PLATFORM='offscreen', MUFFIN_FIRST_LAYOUT_ONLY='1',
               QT_PLUGIN_PATH=preset['testPresets'][0]['environment']['QT_PLUGIN_PATH'])
    env['PATH'] = preset['configurePresets'][0]['environment']['QT_HOST_PATH'] + '\\bin;' + env['PATH']
    env.pop('MUFFIN_THEME_LAYOUT_DETAIL', None)
    env.pop('MUFFIN_THEME_CSS_DETAIL', None)
    env.pop('MUFFIN_THEME_PAINT_DETAIL', None)
    env.pop('MUFFIN_THEME_PROBE_DOCUMENT', None)
    env.pop('MUFFIN_THEME_REAL_SWITCH', None)
    env.pop('MUFFIN_THEME_LIFECYCLE_ONLY', None)
    env.pop('MUFFIN_THEME_MANAGER_DOCUMENT', None)
    if args.manager_document:
        if args.lifecycle:
            parser.error('--manager-document cannot be combined with --lifecycle')
        env['MUFFIN_THEME_MANAGER_DOCUMENT'] = '1'
    if args.lifecycle:
        env['MUFFIN_THEME_LIFECYCLE_ONLY'] = '1'
    if args.real_switch:
        env['MUFFIN_THEME_REAL_SWITCH'] = '1'
    if args.document:
        env['MUFFIN_THEME_PROBE_DOCUMENT'] = str(args.document.resolve())
    if args.detail:
        env['MUFFIN_THEME_LAYOUT_DETAIL'] = '1'
    if args.css_detail:
        env['MUFFIN_THEME_CSS_DETAIL'] = '1'
    if args.paint_detail:
        env['MUFFIN_THEME_PAINT_DETAIL'] = '1'
    runs = []
    result = {'method': 'Sequential fresh offscreen processes; same immutable local CSS/font dependencies. Two return-to-theme cycles per process. Stage timings are not GUI event/display latency; detailed scopes are inclusive and cannot be summed. Images and sampled geometry/cursor hits are retained for before/after comparison. Full counter reference checked separately after timed stages.',
              'lifecycle': args.lifecycle,
              'manager_document': args.manager_document,
              'switch_model': 'detached_with_semantic_seed' if args.real_switch or args.manager_document else 'legacy_selected_copy',
              'dependency_sha256': pinned, 'runs': runs, 'complete': False}
    def checkpoint():
        (destination / 'matrix.json').write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':'))+'\n', encoding='utf-8')
    checkpoint()
    binary = args.binary.resolve() if args.binary else root / 'build/Release/MuffinThemePerformanceProbe.exe'
    for repeat in range(1, args.repetitions + 1):
        for name, theme in themes.items():
            if name not in args.themes:
                continue
            for size in args.sizes:
                if digests() != pinned:
                    raise RuntimeError('Fixed theme/font dependencies changed during measurement')
                output = destination / f'{name}-{size}MiB-{repeat}.json'
                print(output, flush=True)
                with output.with_suffix('.log').open('wb') as log:
                    subprocess.run([str(binary), str(output), str(size), str(theme)],
                                   cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
                raw = json.loads(output.read_text(encoding='utf-8'))
                assert raw['qpa'] == 'offscreen'
                if args.lifecycle:
                    assert raw['lifecycle'] and raw['unchanged_definition_reused']
                else:
                    assert raw['full_counter_reference_matches'] and len(raw['visuals']) == 3
                    assert raw.get('manager_document', False) == args.manager_document
                    if args.manager_document:
                        assert len(raw['selection_to_paint']) == 3 and raw['selection_callbacks'] == 3
                runs.append({'theme_id': name, 'repeat': repeat, 'source': str(output), 'raw': raw})
                checkpoint()
    assert digests() == pinned
    result['complete'] = True
    checkpoint()
    print('Completed', len(runs), 'fresh processes', flush=True)


if __name__ == '__main__':
    main()
