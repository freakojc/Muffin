# Lengyi theme port

`resources/themes/lengyi.css` (light) and `resources/themes/lengyi-dark.css`
(dark) bring the visual design of the **Lengyi Markdown Editor** into Muffin.

## Attribution and licence

| | |
| --- | --- |
| Original project | [woyin2024/lengyi-markdown-editor](https://github.com/woyin2024/lengyi-markdown-editor) |
| Original author | 冷逸 / 沃垠AI (Lengyi / Woyin AI) |
| Licence | MIT — full text in `resources/themes/theme-licenses/Lengyi-MIT.txt` |
| Provenance note | `resources/themes/theme-licenses/README.md` and the header comment of each stylesheet |

Both licences are MIT, so the port can ship inside Muffin's MIT-licensed tree
as long as the copyright notice and licence text travel with it, which they do
(the notice is embedded in the theme resource bundle via `src/themes.qrc`).

The port re-expresses the original's *design tokens* — palette, type scale,
spacing, corner radii, shadows and the dark code panel — for Muffin's document
and chrome hosts. It is not a copy of the browser application's markup,
script or UI code.

## How the original maps onto a Muffin theme

The Lengyi editor defines its whole look in two `:root` blocks (light) and
`[data-theme="dark"]` (dark). Those variables split across Muffin's two theme
surfaces:

| Lengyi source | Muffin target |
| --- | --- |
| `--bg` (app background) | `body` background → `ThemePage::viewportBackground`, `--muffin-chrome-background` |
| `--panel-bg` (panes, toolbars, modals) | `#write` background → `ThemeColors::background`, `--muffin-surface` |
| `--surface` / `--surface-hover` | inline-code chip, table header, `--muffin-canvas` / `--muffin-hover` |
| `--text` / `--text-secondary` / `--text-muted` | `ThemeColors::text` / `muted` / quote ink |
| `--accent` / `--accent-hover` / `--accent-soft` | links, caret/focus, `--muffin-accent`, selection tint |
| `--code-bg` / `--code-text` | the fenced-code panel and its ink |
| `--border` | table cell borders (document) and `--muffin-border` (chrome hairline, flattened to an opaque tone) |
| `.markdown-body` (`max-width: 800px`, `padding: 32px 40px`) | `#write` column geometry |
| `.preview-content` typography (16px / 1.8, heading scale, 20px rhythm) | `html` / `body` / `p` / heading rules |
| toolbars, panes, modals, status bar (DOM chrome) | `ThemeColors` chrome tokens consumed by `ChromeStyleSheet` |
| `[data-theme="dark"]` | a second built-in theme, `lengyi-dark` |

Chrome tokens use Muffin's documented `--muffin-*` extension variables, so the
menu bar, sidebar, tabs, dialogs, scrollbars and status bar all follow the same
indigo-on-neutral palette instead of only the document.

## Engine work the port required

Lengyi is a light theme with a **dark code panel**, which no built-in theme had
before. Rendering it faithfully needed two fixes in the theme/render layer
rather than theme-specific exceptions:

1. **Literal ink from CSS.** `BlockLayout::paintLiteralSource()` hard-coded the
   prose text colour for every literal block, so a theme's `pre { color }` was
   ignored — light code ink on a dark panel was invisible. `RenderTheme` now
   exposes `codeTextColor()` (the computed `pre` colour, falling back to the
   prose ink) and each literal call site passes the ink for the surface it
   paints: code fences/front matter/HTML-source use `codeTextColor()`, while
   the math- and invalid-HTML-source panels (which sit on the inline-code fill)
   keep `textColor()`.
2. **Syntax palette from the code surface.** `codeHighlightColor()` selected
   its light/dark palette from the *page* background, so a light theme with a
   dark fence got dark-purple keywords on a near-black panel. Painters now
   resolve the actual per-block fill (`LayoutBox` style, falling back to the
   code-block token) and call `codeHighlightColor(role, onDark)`. Variable,
   plain and operator/punctuation tokens ride the code ink instead of the page
   ink on a dark panel, and code line numbers and the fence scrollbar step down
   from the code ink so they stay legible.

Existing themes are unchanged in behaviour: every built-in declares either no
`pre` colour (so `codeTextColor() == textColor()`) or a code panel with the
same polarity as its page, which the added `RenderThemeTest` cases assert.

`resources/themes/lengyi.css` also resets the dark panel for rendered display
math (`div.md-math-block pre`), because a typeset formula is document content,
not source code — matching the original, where KaTeX display blocks sit on the
white page.

## Deliberate deviations

- **Selection tint**: the original uses `--accent-soft` (10% alpha), which is
  too faint to read as a selection. The port uses 16% (light) / 22% (dark).
- **Fonts are bundled** instead of relying on the system: the original's stacks
  stay (`Inter, SF Pro Text, -apple-system, PingFang SC, Microsoft YaHei` for
  prose, `JetBrains Mono, SF Mono, Fira Code, Consolas, Menlo` for code) with
  `Segoe UI`/`Roboto` inserted after `-apple-system`, but Muffin renders offline
  and never fetches webfonts, so the two families ship as WOFF2 next to the
  theme (see the next section). CJK falls back to the platform font, exactly as
  in the browser original.
- **The paper column tracks the canvas**: the original centres a fixed 800px
  `.markdown-body` inside a half-window preview pane. A desktop window is much
  wider, so the port uses `max-width: 92%` with `40px 56px 72px` gutters and a
  wider gutter past 1700px. Muffin re-resolves the percentage on every viewport
  change (`ThemePage::pageMaxWidth` from `CssLengthPercentage::used(viewport)`),
  and **Preferences → Appearance → Content Width** still overrides the theme.
- **Tables are denser than the original** (`0.92em`, `line-height: 1.5`,
  `8px 14px` padding, `white-space: nowrap` headers, `vertical-align: top`, a
  faint indigo zebra stripe, and `24px` spacing from the following block through
  `figure.table-figure`). Table columns are sized from each cell's unwrapped
  width and scaled down proportionally when the total exceeds the available
  width, so type size, cell padding and page width are the levers that stop rows
  from ballooning vertically.
- **Fence type size lives on `pre`**: the original sets `0.9em` on
  `pre code`, because the browser renders code text in the inner element.
  Muffin paints a fence's source from the `pre` style directly, so the port puts
  `font-size: 0.9em` (and `line-height: 1.8`) on `pre`. The rendered code text
  is the same 14.4px / 25.92px leading; only the element carrying it differs.
- **Browser-only UI** (draggable split, pane headers, table picker, export
  dialog) has no CSS equivalent in a native Qt app and is expressed through
  chrome tokens or not at all.
- Properties Muffin's CSS subset does not paint (for example `box-shadow` on
  `pre`/`img`, `overflow-x`, `vertical-align`) are kept in the stylesheet because
  the same file is embedded into HTML/PDF exports, where they do apply.

Everything else is carried over unchanged, including the quote's inner
paragraph rhythm and the `48px / 16px` heading spacing.

## Bundled fonts

| Face | Source | Licence |
| --- | --- | --- |
| Inter 400 / italic / 600 / 700 | [rsms/inter](https://github.com/rsms/inter) v4.1 static WOFF2, full unicode | SIL Open Font License 1.1 — `resources/themes/font-licenses/Inter-OFL.txt` |
| JetBrains Mono 400 / 700 | [JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono) v2.304 static WOFF2 | SIL Open Font License 1.1 — `JetBrains-Mono-OFL.txt`, authors in `JetBrains-Mono-AUTHORS.txt` |

Files live in `resources/themes/lengyi/` and are declared with relative
`@font-face` sources, the same pattern GitHub/Pixyll/Newsprint use: CMake
(`cmake/MuffinThemeFonts.cmake` + `scripts/prepare_theme_fonts.py`) converts
each WOFF2 into a native TTF in the build tree and generates the matching QRC
alias, so the authored CSS URLs keep working without committing generated
binaries. `pip install -r requirements-build.txt` (FontTools + Brotli) is
already required to configure the project.

## Verification

`tests/theme/CssThemeMapperTest.cpp` (`testLengyiBuiltInThemePortsThePalette`)
locks the ported tokens: viewport/paper/ink/accent colours, the dark code panel,
the 92% canvas-adaptive column with `40px 56px 72px` gutters, the compact table
metrics, the six embedded font faces, the 16px/1.8 type scale and the chrome
palette, for both variants. `tests/render/RenderThemeTest.cpp`
(`testCodeSurfacePaletteFollowsThePanel`) covers the renderer work above and
asserts that GitHub/Newsprint/Night/Pixyll/Whitey keep their previous ink and
syntax colours. Both themes also run through the shared layout smoke test.

The stylesheets themselves were additionally checked in Chromium by rendering
the same document twice — once with the original editor's `<style>` block on its
`.preview-content .markdown-body` host, once with `document-base.css` +
`lengyi.css` on Muffin's `html > body > content > #write` host — and comparing
computed styles element by element before the width/table improvements were
folded in. After the 92% column and denser tables, the probe re-verifies the
new geometry at 1024/1440/2560px viewports (92% column each time, bundled
`Inter`/`JetBrains Mono` loading from the relative `@font-face` URLs, table
cells at 0.92em/1.5/8-14 with zebra rows). Native Qt painting still needs a
built app to eyeball; run `MuffinRenderThemeTest` and `MuffinCssThemeMapperTest`
from a Conan/CMake build to confirm on your platform.

## Using the themes without a rebuild

A released Muffin build can run the Lengyi themes too. Either import
`resources/themes/lengyi.css` (or `lengyi-dark.css`) with
**Preferences → Appearance → Import Theme…** — Muffin's import copies the CSS
together with every local file it references, i.e. the whole `lengyi/` font
folder, so the embedded `@font-face` sources keep resolving — or copy the
stylesheet plus the `lengyi/` fonts folder directly into the user themes
directory (`QStandardPaths::AppDataLocation/themes`, i.e.
`%APPDATA%\Muffin\Muffin\themes` on Windows) and select it from the theme menu.
A **single-file** variant that embeds the fonts as base64 data URLs is kept in
the development workspace for convenience; it is not committed because the
repository pattern is to ship the original WOFF2 sources and convert at build
time. Built-in ids win over a same-named user file, so the shipped `lengyi` /
`lengyi-dark` stay canonical.
