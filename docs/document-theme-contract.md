# Document theme contract

Document CSS is parsed once and retained in `ThemeDefinition::sourceSheet`.
`CssComputedStyleEngine` evaluates media conditions, custom properties,
inheritance, declaration order, specificity and `!important`. Box and border
shorthands enter the same cascade as their longhands. Computed element and root
font sizes provide the bases for `em` and `rem`; user text size and viewport zoom
are applied separately.

Lengths distinguish missing, invalid and valid values (including zero). Invalid
literal declarations are ignored before cascading; a winning declaration whose
`var()` substitution fails becomes `unset` at computed-value time. Box, border,
font and background shorthands reset their omitted longhands. Custom-property
names are case-sensitive; aliases are computed in the defining element before
inheritance, and dependency cycles invalidate their participating variables.

Computed box lengths keep a pixel component and a percentage component.
Markdown flow and HTML layout supply the actual containing content width for
margins, padding and mixed `calc()` expressions; vertical margin/padding
percentages also use this width. Font-relative units are frozen before
inheritance. Dimensional arithmetic rejects length-times-length and division by
zero. Border style initially means `none`; a visible style with no width uses
`medium`. HTML passes borders and `box-sizing` to Yoga.

Computed-style caches are scoped to the engine generation and opt-in immutable
element snapshots. Temporary elements are uncached. Editing/rebuilding clears
both computed and projected caches; live projections distinguish node objects
and requested style keys, including nodes with identical IDs.
Equivalent matched declarations and inherited inputs reuse immutable cascade,
computed-value, native projection and font results within that generation.
Cache hash buckets also compare their full inputs before reuse.
Equivalent inline runs share immutable used-style snapshots rather than keeping
a full box/paint recipe in every text span; positioned fragments own their geometry.

`:is()` and `:where()` accept forgiving selector lists; `:not()` accepts selector
lists including complex selectors. Their specificity follows the maximum
argument rule, with zero specificity for `:where()`. Media conditions include
width/height/resolution ranges, logical conditions, pointer/hover and reduced
motion preferences. This extends the native CSS subset; complete flex/grid,
cascade layers and browser formatting contexts remain outside this contract.

Nested `@supports` conditions retain source order alongside media conditions.
Declaration queries use the backend's capability list and shared cascade value
validation, including the supported filter functions. Unknown capabilities are
false; malformed conditions are rejected. Inactive branches cannot contribute
root variables, fonts or keyframes. `and`/`or`/`not` are supported; selector and
font technology queries are not advertised yet.

`CssThemeMapper::projectComputedStyle` projects this result into native text,
paint and box values. Theme prototypes, live Markdown nodes and HTML boxes use
this projection. Mermaid uses the same declaration cascade and retains CSS-wide
values until its SVG presentation and inheritance projection.

The mapper does not match declarations or run a second cascade. Prototypes and
live nodes share the `html > body > content > #write` host and semantic element ancestry,
including table sections and Markdown formatting around inline HTML. Anonymous
HTML fragment containers are transparent to selectors. Simple HTML tags retain
the Markdown editing projection, while their fonts come from the actual style
tree. Legacy JSON/manual theme fields are lowered into CSS by
`LegacyThemeAdapter` before entering this pipeline; they are not runtime geometry
fallbacks. Document defaults are compiled into the UI library, so all consumers
have the same defaults without relying on an application's resource initializer.

Markdown formatting exposes `span[md-inline]` wrappers around its native inline
tags (strong, em, code, a, del, mark, sub and sup); inline math uses `md-inline=math`.
Raw inline HTML keeps its authored tree. Task lists/items expose `task-list` and
`md-task-list-item`, checkbox attributes and checked state. Alerts expose
`md-alert-*` and separate `md-alert-text-container` / `md-alert-text-*` title spans;
fences expose `md-fences`, `md-fencescode`, `lang` and `data-language` metadata.
Markdown tables are contained in `figure.table-figure`; the wrapper owns its
padding, border and background, and sibling selectors navigate the wrapper.
These mappings belong to the shared style tree, not to individual themes.

The page uses the same background and border painter as element boxes. Image
layers are retained in CSS order and composited back to front over the solid
color. A `none` image retains its list slot; shorter longhand lists cycle
independently. Each layer has its own origin, clip, position, repeat and size.
Gradients and embedded base64 raster images support explicit sizes; bitmaps
also support intrinsic aspect ratios, `cover` and `contain`. The solid color
uses the bottom image layer's clip. Raster decoding is cached by data URL in a
bounded thread-local cache. External image URLs, SVG background resources,
non-normal background blend modes and `space`/`round` repeat remain outside
this extension. Background transitions sample each layer's position and size.

Generated content tokenizes adjacent CSS strings, escapes, `counter()` /
`counters()` and string `attr()` calls. Attribute values come from the originating
semantic element; a missing attribute uses the authored string fallback, while
an existing empty attribute stays empty. The live pseudo cache stores the
resolved token list and is invalidated with document styles. Markdown metadata
and authored HTML attributes use the same path. Numeric/unit-typed `attr()` is
not implemented. Alert titles and continuation quote prefixes use display
replacements with retained source ranges, so clicking and editing still address
the original Markdown. Entering an alert marker reveals its source syntax.

`RenderTheme::updateForViewport` evaluates the retained sheet using CSS viewport
dimensions (physical layout dimensions divided by zoom). Resizing the editor
rebuilds layout when this environment changes. Page width distinguishes the CSS
outer box, border, padding and text area; an explicit user width overrides the
theme width. `resources/themes/document-base.css` supplies document defaults
before authored CSS and remains independent of editor chrome.

`LayoutBox` stores the computed style, used lengths, font and border, padding,
content and overflow rectangles produced during layout. Paragraphs, headings,
code fences, quotes, table/row/cell boxes and embedded HTML use this snapshot. Painting, clicking,
caret placement and selection read its geometry rather than reconstructing it
from theme tokens. All four heading borders and padding sides enter the flow
box; fixed, minimum/maximum and fit-content sizes change actual occupied space.
Inline code and keyboard boxes reserve horizontal
padding and border width in the text layout. Their generated layout spacers map
to zero source and visible-text length, so they affect wrapping and cursor
coordinates without entering copy or saved Markdown. Format ranges address the
layout buffer, which can include these spacers and collapsed math/image atoms.

Generated `::before`/`::after` styles use the same stable originating-element
views for Markdown hosts and inline Markdown/HTML links. Attribute and structural
selectors navigate the origin's DOM position; inheritance includes its active
state. `GeneratedContentStyle` projects the live computed result once for flow
and positioned fragments. Flow text/icons participate in wrapping and source
mapping; positioned fragments retain relative used geometry and normal/hover/
focus endpoints. Painting only consumes these snapshots. Cached block translation
moves the containing origin, and structural style fingerprints invalidate stale
generated content. Link icons and list guides have no separate runtime recipe.
The compiled selector index conservatively filters pseudo subjects by their
rightmost tag/id before allocating snapshots; unrelated prose pays no per-state
pseudo cascade cost. Functional selectors remain candidates, and active media
changes replace the index with the engine's new environment snapshot.

Code blocks read `pre` typography and line height; inline code reads `code`.
Table header/body cells use actual `th`/`td` styles, including their individual
fonts, line heights, borders and padding. Inline HTML code/keyboard boxes preserve
local style attributes, and nested text spans reserve their shared box padding
once.
HTML block code/keyboard runs use the same box painter and reserve their edges
in their text buffer; metadata keeps content offsets separate from layout offsets.
Lazy promotion replaces both leading and trailing prototype margins with live
node margins.
Inline formula flow includes the actual inner math bounds when they exceed
KaTeX's nominal outer strut, and its paint/selection ranges use those same bounds.

Editing invalidates the style generation and sparse tree together. When a theme
uses structural selectors, already materialized blocks compare their saved style
fingerprints with the new tree and rebuild when affected, including inline and
table-cell styles. Unmaterialized blocks read fresh styles on promotion. Embedded
HTML fragments are conservatively rebuilt after structural edits because their
descendant tree is currently temporary. This preserves correctness while keeping
the Markdown tree sparse; it is not a full browser dependency graph.

Bundled webfonts are converted to native SFNT by CMake at build time using
`scripts/prepare_theme_fonts.py`. Only original WOFF/WOFF2 and license notices
are maintained in `resources/themes`; generated TTF live in the build tree's
`theme-fonts` directory. CMake generates `themes.qrc` in that build tree, retaining
authored CSS URL aliases, and orders resource compilation after font conversion.
Install the pinned conversion tools with
`python -m pip install -r requirements-build.txt` before configuring. Unchanged
fonts are not regenerated by incremental builds. Imported local WOFF/WOFF2 files are decoded offline using FreeType,
then registered through Qt; failed registrations do not enter the success cache.
Font aliases belong to the theme, and successful registrations are deduplicated
by content digest.

## Flex formatting stage

`CssLayoutStyle` is the common computed input for formatting. `CssFormattingContext`
owns Yoga configuration and Flex allocation; the HTML and Markdown adapters only
measure native content and materialize its final `LayoutBox`. It supports directions
and reversals, wrapping, order, alignment, baseline alignment, gaps, auto margins,
grow/shrink/basis, intrinsic width keywords and min/max constraints. Percentages
use the flex content box, while percentage padding uses its containing width.
Text reflows at the allocated width. Minimum violations freeze and redistribute
space after allocation, avoiding Yoga's early minimum-size floor of flex bases.

Markdown block containers and `#write` can establish a Flex context; paragraph
and heading items retain their native text/source mapping. Markdown inline runs
remain one inline formatting item rather than an arbitrary DOM of flex items. All items
inside a root Flex context are materialized together even under Lazy policy:
their geometry is interdependent. Editing recomputes container allocation and
reuses unchanged measurements and allocated children, while normal
vertical documents keep sparse promotion and suffix shifts. Visible-block queries
and clicking use actual rectangles because visual order may differ from source order.
`overflow` controls clipping and automatic minimums; embedded containers do not
yet provide their own interactive scrollbars. Orthogonal writing modes and full
WPT compatibility remain follow-up work.

`CssFlexLayoutTest` consumes `tests/fixtures/theme/flex-layout-browser.json`,
generated with `node scripts/probe_flex_css.mjs`. It compares Chrome box geometry
at 100% and 200% zoom and checks real edits, hit testing and eager/lazy convergence.
Geometry tolerance is 0.8px, with 1.25px for baseline-dependent coordinates because
the native Qt and Chrome font backends round ascent metrics differently.

## Grid formatting stage

`layoutFormattingItems` dispatches Flex and Grid from the same computed input.
`CssGridLayout` owns line/area placement, intrinsic track contributions, free-space
distribution and final item rectangles. Both document adapters use its returned
Grid area widths for percentage padding, content reflow and final boxes. Grid
tracks freeze font-relative lengths during computed-style resolution, including
explicit inheritance; percentages resolve when the Grid area is known.

The first stage supports length/percentage/`calc()` tracks, `auto`, `min-content`,
`max-content`, `fr`, `minmax()` and integer `repeat()`. Numeric positive/negative
lines, row/column spans, implicit track patterns, row/column auto-flow and dense
packing share the same placement pass. Gaps, item/content alignment, auto margins,
min/max item sizes and nested Grid/Flex use the common box snapshots. Root Grid
uses the same atomic materialization/invalidation and XY hit testing as Flex.
Track counts and authored line magnitudes are bounded at 1000 to keep malformed
or adversarial CSS from allocating an unbounded grid.

Automatic repetition (`auto-fill`/`auto-fit`) stays symbolic in computed styles.
The shared layout resolves its count against the definite content axis and gap.
For an indefinite preferred size, a definite maximum supplies the largest fitting
count; a minimum-only constraint supplies the smallest count satisfying it. The
border-box constraints first exclude padding and borders, and conflicting minimums
win over maximums. One repetition is used only when all these sizes are indefinite.
`auto-fit` collapses only unoccupied
repeated tracks and their gutters after placement, retaining the original explicit
line numbering. Fixed siblings, repeated track patterns and percentage/calc fixed
minimums participate in counting. Resizing re-expands the computed template rather
than caching an old count, including when Markdown root layout uses Lazy policy.

Named line lists retain case-sensitive names and merge adjacent repeat boundaries.
Positive/negative occurrences, named spans and implicit named lines resolve before
the common placement pass. `grid-template-areas` requires equal row lengths and
rectangular named regions; its implicit `name-start`/`name-end` lines use the same
resolver. Named shorthand omissions propagate custom identifiers according to
the CSS rules. Empty area cells do not reserve placement slots. Syntax-invalid
templates and areas are discarded by the common cascade.

`CssGridLayoutTest` reads `tests/fixtures/theme/grid-layout-browser.json`, generated
with `node scripts/probe_grid_css.mjs`. It checks Chrome geometry at 100% and 200%
zoom, native Markdown editing, caret/selection coordinates and full/lazy agreement.
Subgrid shares the parent's used tracks on one or both axes. `CssGridInheritance`
travels with each allocated item; it is not computed style and is never cached by
selector or NodeId. Shared tracks retain parent line names, with local names and
integer/auto-fill name repetition added at layout time. Padding, borders and
margins inset edge tracks; different subgrid gaps adjust both sides of each gutter.
Subgridded axes stretch and clamp placement to the inherited span, without implicit
tracks. Outside a grid item, `subgrid` behaves as `none` on that axis.

Descendant intrinsic contributions are translated onto parent tracks recursively,
then sized in global span order. A separate width query uses this same algorithm
without laying out blocks or measuring text height. Nested grids therefore measure
their actual tracks rather than summing widths as if they were Flex rows.
Indefinite `fr` tracks include max-content and spanning contributions;
`fit-content(length/percentage)` caps track growth without overriding its content
minimum. Explicit `min-content`/`max-content` minima may exceed a smaller maximum
in `minmax()`. Shared geometry is used by both adapters and native caret/selection;
a descendant edit invalidates its owning formatting context and parent tracks.

### Size dependencies and replaced content

Intrinsic measurement and final allocation use explicit definite/indefinite size
references. Percentage tracks in an indefinite axis behave as intrinsic tracks
while measuring; cyclic gap percentages contribute zero and `calc()` retains its
absolute part. Grid then freezes its intrinsic content height, applies min/max
constraints, and resolves percentage tracks and gaps once against that height.
Overflow from this pass does not increase the frozen height. In auto-height Flex
containers a percentage row gap stays zero, including when only a min/max height
is specified; its absolute `calc()` component still contributes.

`CssSizing` supplies common content constraints and replaced-element sizing.
Adapters carry natural image dimensions separately from computed CSS, project
lengths to layout pixels once, and pass definite allocations into the same ratio
calculation. HTML image dimension attributes also follow document zoom and remain
overridable by authored CSS. `aspect-ratio: auto <ratio>` retains an available natural image ratio;
an authored ratio follows `box-sizing`, while natural ratios use the content box.
Min/max constraints may transfer to the automatic opposite axis or break the ratio
when required. Flex automatic minimums and Grid track contributions use these
sizes. A directed dependency phase revisits columns after a definite row allocation
changes a stretched item's ratio contribution; column Flex likewise transfers its
allocated main size back to an automatic cross size.

Markdown and HTML image painting consumes the allocated content rectangle. Padding
and borders reserve space outside that rectangle, and image upscaling or an authored
ratio is not undone by a second painting-time natural-size cap. Images use the CSS
default `object-fit: fill`; additional `object-fit` modes are not yet implemented.

Browser fixtures run in standards mode with an explicit doctype. They cover cyclic
percentage/calc dependencies, min/max-only automatic repeats, natural and authored
image ratios, transferred constraints and both box-sizing modes at 100%/200% zoom.
Image cases additionally compare painted content rectangles. Native tests exercise
Markdown image hit testing and caret coordinates, resizing and full/lazy convergence.

Masonry, orthogonal writing modes and complete CSS Grid intrinsic sizing/WPT
coverage remain future work. The directed size-dependency phases do not establish
full browser compatibility for arbitrary nested cyclic constraints.
Escaped CSS custom identifiers and escaped area strings, and the full `grid` /
`grid-template` shorthands are not yet supported.

### Measurement phases and invalidation

`CssMeasureRequest` distinguishes intrinsic measurement, inline allocation,
block allocation, dependent allocation and final materialization. A negative
containing height is indefinite; a frozen cyclic intrinsic height does not become
a definite percentage reference. Fixed rows and inherited subgrid rows can
provide definite references during column contribution measurement. The Grid
scheduler processes row-to-column ratio transfers in a dependent phase rather
than recursively calling its own layout entry with a retry counter. Each nested
formatting context owns its phases and inherited references.

`measureCssItem` is the common memoized measurement entry. Its bounded request
key includes width, containing width/height, allocated height, phase and inherited
track geometry, names and definitions. Thus equal rectangles with different
percentage references or subgrid definitions cannot alias. Root edits retain
measurements only after validating a content/style/projection signature. The
signature covers descendants, resolved inline links, computed structural styles,
the stylesheet including pseudo-element rules, font/zoom, render settings and
active composition. Changed item identities and
changed signatures invalidate the item and its enclosing contribution query.

Formatting contexts retain the constraints and measurements consumed by their
previous track solution. A subsequent pass validates intrinsic inputs, styles,
inherited tracks and the exact measured contributions before reusing that
solution. A changed contribution triggers the ordinary shared Flex/Grid solver;
equal contributions stop propagation at the container boundary. This still
visits the affected context's input items; it does not promise sublinear track
solving for arbitrary cross-track dependencies.

Native block reuse applies recursively inside containers, including allocated
children. Measurement passes cannot consume retained layout objects. Final
materialization moves unchanged objects from the previous tree, translates their
geometry and updates absolute source positions together with reuse metadata.
Removed measurements/solutions are pruned after a full formatting pass. Explicit
full refreshes clear persistent measurements. Generated TOC content still uses a
conservative rebuild because of its document-wide outline dependency.
List labels and their shared gutter are measured once per list per pass and are
part of each item's reuse signature. A wider sibling marker invalidates the
affected indents; ordinary body edits retain siblings whose marker geometry is
unchanged.

### Viewport updates and lazy estimates

Viewport updates compare the active media-rule set before projecting a theme.
When that set is unchanged, immutable style-engine snapshots reuse compiled
selectors and candidate indices. Computed and cascade caches are discarded:
computed values carry their viewport context, and cascade matches contain
addresses into the snapshot's declaration storage. Theme copies retain their
own environment and caches.

Theme projections are reused only when the sheet has no viewport-unit values
and the page box has no percentage width or insets. Media-rule changes,
viewport-dependent projections, zoom and user text-size changes rebuild the
projection through the same computed-style engine. The current viewport still
reaches live nodes and inline declarations on the reuse path.

If projections and content width stay unchanged, native prose layouts can move
horizontally without reshaping text. Materialized HTML conservatively disables
this path because its inline styles may contain viewport units absent from the
theme sheet. Unbuilt HTML resolves the current environment when promoted.

Lazy paragraph and heading estimates cache used boxes, line heights and wrapping
capacity by prototype element and containing width within one layout pass.
Explicit height, min/max height and box sizing participate in the placeholder
height. Starting a new pass clears these entries, including after theme, font,
zoom or width changes. Visible promotion still computes the full live style.

### Resource versions

`LayoutResources` gives each image, font database and Mermaid cache entry separate
paint and geometry versions. A resource scope captures reads, including nested
reads, into the block or measurement dependency snapshot. Geometry version
changes invalidate measurements; paint-only changes preserve their validity.
Images with unchanged natural dimensions update retained pixels without text
shaping or track solving. Changed natural dimensions revalidate size
contributions, including CSS constraints that can make the allocated size stay
unchanged.

The shared image loader handles remote downloads, data URLs and cached local
files. Local file/directory watches detect replacement and modification. HTML
painting also consults this cache, so it cannot retain a private failed or stale
remote image. Font database notifications invalidate metric caches even when
the `QFont` key is unchanged. Mermaid keys include font geometry generations;
cache clears invalidate dependent layouts and superseded workers cannot publish
results into a later cache epoch.

Editor resource notifications are coalesced per event-loop turn. The layout
refreshes dependency owners among promoted blocks and pins the viewport across
geometry changes. Unpromoted content reads the current resource versions when
it is built. An in-progress asynchronous document open does not rebuild stale
document contents in response to a resource notification.
Leaving the loading state schedules a dependency refresh as well, so cancelling
an open cannot lose resource changes deferred while the old document was hidden.

### Inline formatting contexts and baselines

`inline-flex` and `inline-grid` reserve an atomic inline box in the surrounding
native text layout. Its shrink-to-fit width, margins and baseline come from the
shared Flex/Grid formatter. The same rectangle determines wrapping, painting and
pointer coordinates. Line boxes include the text strut as well as the component's
ascent/descent. Grid first/last baseline groups participate in track contribution
and final item alignment; container baselines propagate to nested inline boxes.
Container baseline export uses occupied rows, shared baseline groups and row-major
grid order; empty leading/trailing rows do not force a synthesized bottom baseline.

Markdown keeps each inactive HTML component's source and visible ranges while
collapsing its display buffer to one placeholder. Child text has source mappings
through nested tags and entities, so child links, caret hits and selections use
the retained HTML text layouts. Entering the group reveals editable source using
the existing projection policy. This does not add orthogonal writing modes,
arbitrary `vertical-align` values or browser-complete inline formatting.

Selected WPT sources are pinned with their revision, checksums and license under
`tests/fixtures/theme/wpt`. Adaptations document their scope; the ordinary native
tests consume committed Chromium geometry without a network or browser dependency.

## Verification

`DocumentStyleConformanceTest` covers inheritance and font-relative geometry,
shorthand/longhand ordering, conditional variables, responsive widths, native
font resources, imported webfont decoding, text/zoom scaling, heading flow,
HTML inheritance and separate code typography. Inline rendering tests check box
advances, unchanged visible text and caret/click round trips. The full suite
also checks editing, incremental/lazy layout and Mermaid.
Snapshot regressions compare eager and promoted lazy rectangles, test a heading
CSS edit across wrapping/borders/caret/selection, and compare structural edits
with a fresh layout. Chrome comparisons cover computed values and the used box
geometry of paragraphs, headings and preformatted blocks with both box-sizing
modes; native table behavior has separate regression coverage.
The committed reference is `tests/fixtures/theme/document-box-browser.json`.
Regenerate it with `node scripts/probe_document_css.mjs` after installing
Playwright. `CHROME_EXECUTABLE` selects a local browser; otherwise Playwright's
Chromium is used. The regular native test suite consumes this fixture without
requiring a browser installation.

`ResourceIncrementalLayoutTest` covers local file watching, asynchronous HTTP
images, same-size pixel updates, font registration, Mermaid completion and cache
clearing. Nested Flex/Grid/subgrid edits and image updates compare boxes, pixels,
caret and selection geometry with fresh eager and lazy layouts. Editor-level
checks cover coalesced notifications, viewport pinning and asynchronous open.
Newsprint, GitHub and Night are exercised at 100%/200% and narrow/wide viewports.
Work counters assert that paint-only changes skip layout and that an image
completion in a long document rebuilds its owning block rather than every block.

The same test includes a small timing probe without machine-dependent CI limits.
For a longer local run, build the Release target, then use PowerShell:

```powershell
$env:MUFFIN_LAYOUT_BENCH_BLOCKS = '1200'
$env:MUFFIN_LAYOUT_BENCH_ITERS = '12'
$env:MUFFIN_LAYOUT_BENCH_OUTPUT = "$PWD/build/theme-audit/resource-layout-performance.json"
ctest --preset conan-release -R '^MuffinResourceIncrementalLayoutTest$' --output-on-failure
```

The JSON reports median, p95 and maximum milliseconds for actual text edits,
image completion and viewport resizing. Typing includes the document update;
fresh references rebuild the same document at the same viewport width. Both
paths use lazy layout with the first eleven slots promoted. Image timing excludes
network transfer and measures decoded-resource publication through layout refresh.
Resize includes responsive theme recalculation and rebuilding the lazy estimates;
`resize_styles` and `resize_layout` split these costs. The resize probe always
rebuilds the lazy layout, even when an editor could use horizontal translation;
it deliberately has no unchanged-width reference. Remove these environment
variables to restore the smaller default test workload.

At 100% zoom and a 16px user text size, Chrome with the same host CSS and
Newsprint sheet gives the following reference values:

| Value | Expected |
| --- | --- |
| Text width below the 1400px breakpoint | 580px (640px outer box minus 60px padding) |
| Text width at or above the breakpoint | 854px (914px outer box minus 60px padding) |
| h1 font size / top margin | 30px / 60px |
| h1 bottom padding / border | 24.375px / 1px, `#c5c5c5` |
| h2 top margin | 48px |
| Code block line height | approximately 24px |

Generated heading content is inserted into the retained inline display stream,
with zero source extent. Its text is shaped by the same `QTextLayout`; atomic
shapes and masks own used `LayoutBox` fragments and participate in intrinsic
widths, line breaking, vertical alignment and visual overflow. Painting consumes
these fragments, and clicks on generated content snap to the adjoining source
caret. Source selections exclude the generated text. The heading-specific
advance reservation and draw-time inline position estimation have been deleted.
Absolutely positioned decorations remain outside normal inline flow.

`scripts/probe_inline_pseudos.mjs` records Chromium references with the bundled
font bytes. `RenderHeadingPseudoTest` compares generated-content geometry and
tests 100%/125%/200% zoom, narrow/multiline headings, local edits, source/visible
caret mapping, selection and fresh eager/lazy pixels. Pseudo styles use live
host topology for selectors, while inheriting from the originating element.
Heading counters retain the existing flat document-order scope. Complete CSS
counter scopes and margin collapsing for block pseudo-elements remain outside
this inline-layout contract.

`font_rendering::configureCssFont` is shared by Markdown and HTML. On Windows it
detects a missing bold face through OpenType metadata, verifies the uniform
DirectWrite simulation advance against the same unsimulated face, and removes
that extra advance while preserving bold ink. Spaces are compensated separately;
real bold faces and variable fonts retain their design metrics. Author spacing
is an explicit input, so applying the configuration twice does not accumulate a
correction. The original-family fallback stack remains intact.

The real-theme audit exports requested/resolved pixel sizes, requested/actual
face weights, native advances and layout spacing. The browser audit records the
same text's advance with the same imported font files. Qt currently rounds the
requested font size to integer logical pixels before font-engine resolution;
fractional CSS sizes can therefore retain a small advance difference. This
limitation is recorded explicitly instead of compensating with a family-specific
stretch or an undefined `QTextLayout::setRawFont` drawing path.

These values have been checked against Chrome computed styles and the Windows
native Qt font backend. This contract supports the native renderer's CSS subset;
it does not imply complete browser CSS support or pixel identity with Typora's
application-specific DOM and stylesheet. Unknown media grammar is skipped.

## Offline compatibility reports

Build the developer tool with `cmake --build --preset conan-release --target
muffin-theme-report`, then audit a CSS file without launching the GUI or
registering fonts:

```powershell
.\build\Release\muffin-theme-report.exe --dark --width 960 --height 900 `
  --output build\vlook-compatibility.json `
  tests\fixtures\theme\vlook-fancy-dark\vlook-fancy-dark.css
```

Without `--output`, JSON goes to stdout. Usage and I/O errors return 2;
capability findings return 0. The tool is outside the shipped app bundle.
It uses the native parser, selector compiler, media/supports evaluator and
declaration capability validator. Theme discovery does not invoke the audit.

Schema version 1 records the chosen environment, original/imported source
files, rule counts and grouped findings. Findings include stable codes,
severity, occurrence counts and at most three examples with the rule's original
file, line and column (UTF-16 units). Long values, including embedded images,
are truncated with a SHA-256 digest. Resources are never fetched. Local resource
existence is checked relative to the owning imported sheet.

`active-rule`, `inactive-condition`, `blocked-selector` and `excluded-context`
are distinct scopes. Active means a rule passes its conditions, not that it
matches a node or wins the cascade. Typora editor/export selectors stay separate
from missing document capabilities. `context-dependent-value` marks values
requiring element custom properties; `unverified-property` means no complete
capability validator exists, not that the renderer ignores the property.
Font existence is separate from font decoding/registration/fallback. The audit
does not check all CSS grammar, keyframe property support or final document
geometry, and a clean report does not imply browser or Typora pixel parity.

`MuffinCssCompatibilityReportTest` tests provenance through imports and nested
conditions, bounded output, conservative classifications and the real CLI.
It exports the fixed VLOOK audit beside the test executable as
`vlook-fancy-dark-compatibility.json`.

## Image resources, repeat modes and background blending

The common background painter supports local filesystem, `file:`, Qt resource
and embedded image URLs. Relative URLs, including those in custom properties,
resolve against the stylesheet that declares them, including imported sheets
and keyframes. CSS string escapes, quoted parentheses and percent-encoded paths
use the same tokenizer as dependency discovery. The renderer performs no remote
fetch; the compatibility report keeps remote images/fonts/imports explicit.

`CssImageResources` belongs to a parsed theme snapshot. Decoded sources use a
32 MiB cache with at most 64 entries; derived SVG rasters share a separate
16 MiB cache across that snapshot. Source files/data are limited to 32 MiB and
decoded intrinsic images to 16 megapixels. SVG rasters are cached by source
digest and target pixel size, using the paint target's DPR. Local sources are
not re-read during every layout/paint. ThemeManager's existing SHA-256 manifest
includes missing dependencies; changing an asset's bytes or creating a missing
file invalidates the theme even when size/timestamp are unchanged. Reloading
creates a new resource cache while already projected styles retain their images.
There is no persistent disk cache.

SVG backgrounds preserve intrinsic dimensions/ratio and root `viewBox` /
`preserveAspectRatio` (`none`, alignment, `meet`/`slice`); an SVG with only a
viewBox uses the background positioning area to determine its default size.
SVG animation is disabled. External SVG subresources, filters and CSS inside
SVG remain subject to QtSvg's own subset. Raster formats use Qt's available
image readers and only the first decoded frame. Oversized derived tiles are
rasterized with a maximum 4096 pixels per axis.

Each axis retains `repeat`, `no-repeat`, `space` or `round`. Space distributes
whole tiles between the positioning area's edges and falls back to one
positioned tile when fewer than two fit. Round chooses an integer tile count,
resizes that axis and rescales an opposite `auto` axis when needed. Percentage
anchors retain the pre-round size, matching the fixed Chromium references.
Origin, clip, list cycling, `none` slots and zoom/DPR use the same background
path for document pages, block boxes, inline fragments and pseudo backgrounds.
Repeated tiles use texture brushes to avoid one draw call per tiny stitch.

All sixteen CSS `background-blend-mode` values are implemented: normal,
multiply, screen, overlay, darken, lighten, color-dodge, color-burn, hard-light,
soft-light, difference, exclusion, hue, saturation, color and luminosity.
The stack is isolated from ancestors, borders and text, with the background
color below the last image. Normal backgrounds keep the direct painting path;
mixed stacks allocate only the visible intersection. Nonseparable color modes
use the W3C blend functions and source-over alpha. This does not implement
element/subtree `mix-blend-mode`, which remains a separate diagnostic.

Regenerate the browser oracle with `node scripts/probe_background_css.mjs`.
`tests/fixtures/theme/background-browser.json` records fixed physical pixel
samples for DPR 1, 1.25 and 2 without fonts/network. Samples within one pixel of
sharp edges are excluded to avoid Qt/Skia edge-antialiasing dependence; flat
interiors, smooth gradients, geometry and alpha/blend colors are compared with
a four-channel-unit tolerance. `MuffinCssBackgroundCompatibilityTest` also
checks imported-variable resources, SVG decoding/raster reuse, negative cache
invalidation, animation image selection and same-size/same-time asset updates
through the production ThemeManager manifest.

### Inline generated content and alert icons

Markdown inline semantic elements and their `md-inline` wrappers participate
in normal-flow `::before` / `::after` generation, as do alert title spans.
Insertion follows semantic tree order, including nested ancestors sharing the
same projection boundary. Generated text, shapes and icons retain zero source
extent: they reserve flow without entering editable text or Markdown. Link
descendants inherit their link target. Pointer hit testing on generated content
snaps to its originating editable boundary.

Each of the five alert kinds supplies a semantic `svg > path` child inside
`.md-alert-text.md-alert-text-{kind}`. Its baseline defaults are one em square,
a half-em right margin, middle alignment and `fill: currentColor`; author CSS
can override them. SVG geometry comes from computed styles and uses the shared
theme resource/raster cache. Solid path fill, title currentColor transitions,
display:none and ancestor opacity are covered by native raster checks.

Inline pseudo snapshots identify the source leaf and ancestor depth rather
than a tag alone; nested spans cannot alias each other's styles after document
invalidation. Generated text foreground/background and shape backgrounds use
the existing element state/transition machinery. Their own painted fragments
participate in the originating element's hover area.

`inline-semantics-browser.json` fixes ten Chromium scenarios and the authored
font manifest. Native tests compare character positions, line heights, SVG
geometry, nesting order and editing coordinates at 100%, 125% and 200% on both
text backends. The original VLOOK Fancy Dark fixture additionally covers all
five alert kinds in `alerts.md` and saves a review raster.

This contract covers normal flow in Markdown semantic inline elements. Empty
or complex embedded HTML descendants, inline absolutely positioned pseudos,
full flex layout and arbitrary SVG subtree styling remain separate compatibility
work. The shared effect compositor below covers generated text and shapes.

### Masks, filters, counters and finite positioning

Element and generated-content paint groups apply transforms, then the authored
filter list, mask composition and group opacity. Backgrounds, borders, glyphs
and descendants share one layer; normal-flow text pseudos and HTML block
subtrees also use this path. Page `#write::before` textures use the common
background/resource/mask projection rather than SVG-specific recolouring.

Masks reuse theme-snapshot resources, sizing, origin/clip, positioning and
repeat/space/round tiling. Alpha/luminance modes and add/subtract/intersect/exclude
composition are supported. `-webkit-mask*` aliases share the standard longhands;
legacy vendor composite operators are outside the current subset. Explicit
`none` entries retain list indices but are omitted when compositing, matching
the pinned Chromium oracle. Missing images instead produce an empty mask.
SVG mask fragment references, mask-border and the full SVG mask resource model
remain unsupported.

Filters preserve function order and repetitions: blur, brightness, contrast,
grayscale, sepia, hue-rotate, opacity, invert, saturate and drop-shadow. Colour
matrices operate in sRGB; blur lengths account for zoom and DPR. Large blurs
use a three-pass box approximation with a 256 physical-pixel radius cap.
Transitions pad compatible lists with identity functions; differently ordered
lists interpolate discretely. SVG url() filters are unsupported. Backdrop
sampling supports QImage-backed surfaces and includes a blur halo; nested
backdrop roots, combinations with intermediate opacity/filter/mask isolation
surfaces, rotated capture and QWidget/GPU backdrop sampling are separate work.
Backdrop-filter transitions are not yet interpolated.

Element filters back-propagate viewport demand through the ordered operations.
Source and translated shadow regions stay disjoint, with blur halos sampled
across 512 physical-pixel tiles. Huge drop-shadow offsets allocate neither the
transparent gap nor a downsampled bounding image; final pixels retain device
DPR. Masks run on the resulting tiles before group opacity. The current blur
cap bounds a single halo surface to 2051 x 2051 pixels (under 17 MiB); this is not a
total process-memory cap for multi-operation/nested effects. Background box
shadows also include the implicit device clip when allocating their raster.
Device clipping distinguishes logical QWidget dimensions from physical image/
pixmap dimensions; the separate 125% window regression covers right/bottom edges.
Ordinary wrapped inline masks use the run's union box; full CSS
box-decoration-break mask slicing is not claimed.

Markdown counters resolve `reset`, then `increment`, then `set` against the live
semantic tree. Before/children/after order, nested counter instances, parent vs
previous-sibling inheritance, display:none, case-sensitive names, counter() and
counters() are preserved. Resolved text and per-owner fingerprints participate
in formatting reuse and suffix invalidation after edits. Results cache by
document revision/style generation; cursor and unchanged-style viewport work
reuse the snapshot. Active supported selectors supply conservative counter and
dynamic-content subjects, including mandatory ancestor classes. Candidate
discovery uses the Markdown AST to reject unrelated inline trees; only candidate
elements and their ancestor paths need computed counter styles. Authored inline
HTML retains the full fallback for arbitrary classes, variables and counters.
Negative :has(tag/class) queries also avoid materializing plain Markdown inlines.
Reversed auto counters,
arbitrary custom @counter-style systems, generated marker scopes and independently
rendered HTML subtrees are not included in this evaluator. Variable content and
complex selectors remain conservative candidates; this is not a complete
custom-property dependency analysis.

Block-host absolute generated pseudos use the host padding box as their finite
containing block. Opposing insets stretch auto dimensions; min/max constraints,
content/border sizing, margins and percentage dimensions share the generated
content sizing helper. Paint and interactive state snapshots retain the same
constraints. This does not provide a general positioned descendant tree:
inline absolute pseudos, ancestor containing-block search for static hosts,
auto-margin centring, RTL overconstraints, fixed/sticky and layout during every
animation frame remain separate work.

`effects-browser.json` pins Chromium pixels at DPR 1/1.25/2, positioned rectangles
and counter text witnesses. The generator verifies counter results by comparing
screenshots with literal replacements, rather than treating computed content
expressions as resolved text. Native integration tests additionally exercise
incremental/lazy counter invalidation, normal-flow pseudo effects and compatible
vs discrete filter interpolation.

### Font identities and matching

Theme font registration uses private, content-derived SFNT family identities.
The source files and glyph/advance/shaping tables stay unchanged. CSS weight and
style descriptors determine matching independently of a file's internal family
or style name. Theme aliases belong to the theme snapshot, so another theme or
hot reload cannot replace the face retained by an existing document. Identical
content and descriptors reuse the process-local registration; no disk font cache
is written. Registered faces remain alive for retained layouts.

Ordered local/embedded src candidates share WOFF/WOFF2 decoding and native
registration. Missing or malformed candidates allow the next source to be tried.
Remote sources are not fetched. Font media conditions participate in projection
invalidation without registering again within an unchanged media bucket.

Family lists preserve quotes, comma-bearing names and the position of generic
families. Quoted generic names remain literal. Qt 6 numeric weights retain the
CSS 1..1000 scale; relative weights resolve against the parent's computed weight.
Italic faces, HTML and Markdown use the same projected font properties. The
DirectWrite synthetic-bold advance correction is shared as a core metric policy.

Bundled Noto fallback faces also have private identities, shared with Mermaid.
This prevents an installed same-name bold face from replacing the fixed regular
webfont used by browser references. Static-font STAT instance labels are excluded
from native matching; variable-font tables remain intact.

`font-mapping-browser.json` pins 17 Chromium scenarios and exact font hashes.
Both text backends check character positions, line count/height, heading and
inline font properties, real/synthetic styles and mixed Chinese/Latin fallback.
Integration checks cover source fallback, alias isolation, media changes,
embedded fonts and font-content invalidation with unchanged CSS.

Remote download, unicode-range subset dispatch, collection face indices,
variable weight/style range descriptors, arbitrary oblique angles and complete
font-synthesis control remain outside this acceptance set. local() currently
matches installed family names; PostScript names and per-face mixtures with file
sources need additional matching work. Fonts unavailable locally still use the
platform generics followed by fixed Noto fallback; this does not reproduce the
metrics of an absent original theme font.

### Interaction calculation and snapshot caches

Immutable semantic elements opt into caching with a unique lifetime identity.
Structural selector matches are retained per engine snapshot; only structurally
eligible dynamic selectors participate in subsequent state calculations. Nested
`:is()`, `:where()` and `:not()` retain their state dependencies. Dynamic negations
remain conservative candidates until the actual state is known.

Scoped hover/active state matches the subject and its ancestors, while focus
matches the subject. Every compound in a scoped selector sees that same subject,
including sibling combinators; unrelated elements never acquire its state.
Computed scoped styles are keyed by matched rules and inherited values, with
full equality checks after hashing. Temporary subject pointers are not retained
as cache identities. Equivalent ancestor results can be reused across subjects.

Paint recipes retain all eight runtime states but compile only the combinations
needed by structurally eligible rules and inherited dependencies. Unused states
share their projected values and animation tracks. Pseudos use their own stable
identities and inherit state-dependent values from their host. Structural edits,
theme changes and active media changes discard snapshot caches; temporary
elements and caller-supplied declarations bypass computed identity caching.

### Text-only style invalidation

A precise top-level replacement can preserve the style snapshot when complete
CSS-visible signatures agree: block metadata, semantic inline types, HTML
attributes/declarations, link targets, alert title classes and child topology.
Ordinary text and source offsets are excluded. This proof also applies to sheets
without counters; unknown or structural changes still use full invalidation.

An exclusively owned sparse adapter rebinds the affected block views using
captured top-level identities and child paths. Old AST addresses are used only
as map keys, never dereferenced. Inline views keep their CSS identities and
interaction recipes while their source ranges are refreshed by preorder ordinal.
Unchanged computed styles, fonts, ancestor predicates and layouts remain valid.
Retained theme snapshots force separate adapters rather than rebinding shared
views. Continuous edits must not accumulate old AST views or cache entries.

Local and full layouts use the same CSS page-margin/padding formula for the
document's trailing height, including updates to its last block.

A synchronous caret refresh can precede a queued structural range update.
Single-block rebuilds validate slot count and identity before invalidating styles
or resolving counters. Rejected stale requests leave the previous snapshot and
revision intact; the owning range update then applies structural dependencies.

### Incremental document counters

Counter discovery first filters Markdown blocks by compiled selector subjects,
semantic classes and possible inline types. Arbitrary authored HTML remains a
candidate because it can introduce classes, attributes and local declarations.
Only candidate subtrees and their ancestors enter the CSS cascade.

Compiled counter programs retain reset/increment/set operations, generated
content tokens and stable block-local semantic keys. They contain no AST or
adapter pointers. A precise top-level replacement range preserves unaffected
programs across style invalidations; ordinary text and shifted source offsets
do not change semantic context. HTML attributes, inline types, block metadata
and topology are compared as complete serialized inputs, not hash identities.

Each numbered block memoizes its complete incoming/outgoing counter state and
generated results. Changes propagate in document order until an unchanged
program receives the same input state; nested instances preserve parent aliases
and sibling scope behavior. Unknown changes, theme/media changes and structural
changes affecting contextual counter/variable/display rules rebuild programs.
Retained RenderTheme snapshots detach the counter cache before mutation.

Complete semantic signatures are interned by exact byte equality and reference
counted per top-level block; replacing the last user releases the old shape.
Incoming/outgoing counter snapshots are immutable and shared across equal
states. A hash only selects the candidate bucket: complete names, depths and
values must still agree. The state pool retains weak references and prunes dead
entries on evaluation, so replaced memo payloads are released immediately.
Diagnostics expose shared payload sizes and live state counts; these are not
total allocator/AST memory or process working-set estimates.

Contextual edits compare exact matching counter/content/display/custom
declarations and inherited dependencies before recompiling each candidate.
Equivalent structural alternatives share compiled results. General sibling
matching first rejects a missing required child tag using the semantic navigator;
when the tag exists, the full combinator matcher still determines the result.

### Retained raster and prototype lifetimes

Box-shadow rasters use a per-thread 16 MiB LRU budget with exact local geometry,
subpixel phase, QColor, radius and blur inputs. Invisible distant edges are
normalized outside a corner guard, so lazy page-height changes retain identical
visible masks. Integer translation can reuse a raster; changed visible clipping
or paint inputs cannot. Cold/warm, translated, clipped and tall-page paths retain
pixel parity, including the fractional-DPI effects acceptance suite.

Theme projection enables identity caches for its immutable prototypes even
when borrowing an engine. A scope guard releases only those prototype identities
before destroying the tree. Caller-owned document snapshots and cross-element
cascade/computed-value sharing remain valid. No disk cache is introduced.

### Viewport promotion and bounded memoization

The editor synchronously promotes the visible area with an 80 px paint guard.
After 120 ms without scrolling, a single-shot timer prefetches up to 1.5 screens
on each side, in slices of at most two blocks with a 4 ms budget checked between
blocks. A single indivisible block can exceed the time budget. Each slice reads
the current viewport and document/theme, so old requests cannot promote stale
ranges. Hidden views stop the timer; pending resize estimates take priority.

Lazy normal-flow layouts retain at most 64 detailed top-level blocks when idle,
except that the prefetch range, caret, selection endpoints and edited HTML block
are protected. Measured heights, stable position tokens and semantic AST data
remain resident. Eager/export and root formatting-context layouts keep their
existing behavior. Eviction releases detail and its inline/pseudo style memos;
structural block views and their ancestors remain valid. A shared semantic tree
is not pruned while another theme snapshot can own pointers into it.

CSS selector candidates use complete subject metadata (tag, ID, semantic classes,
attribute names and pseudo origin), conservatively including nested `:is()` and
`:where()`. Attribute values, ancestors, siblings, positions, negations and
states are resolved by the full matcher. Candidate groups preserve source order
and every selector's specificity. Equivalent matched candidate sets share Qt
storage. Final styles share by complete matched rules, inherited values and root
status; full equality is checked after hashing. Equal eight-state recipes share
immutable values while element animation timelines remain independent.

Shared final styles/cascade/value memos are limited to 2048 entries/buckets,
subject and matched-selector pools to 512 metadata keys/buckets, and shared paint
recipes to 512 entries. Variable-map fingerprint snapshots have 128 slots and
are validated by shared Qt map storage; public map mutations detach from retained
snapshots. These are count limits rather than byte-level process memory budgets.
Diagnostic cache counts and optional `muffin.perf.css` timing separate selector
matching, fingerprints and final lookups from actual computed-value work.

### Text paint overrides

Retained text layouts own their static character formats and shaping. Host
color sampling submits foreground overrides only when the sampled color differs
from the retained base color. Unchanged values reuse those formats; actual host
hover/focus/keyframe colors and independent inline color/stroke overrides still
paint without changing wrapping, caret positions or selection geometry.

`themeTextPaintStats()` records inputs to `TextLayout::draw`: draw calls, total
layout lines, static format ranges and dynamic selection ranges. These are
request counts, not counts of visible lines, glyphs or raster passes. They reset
with the other opt-in theme performance counters and are included as
`text_paint_work` in the manual GUI and theme probes.

### Projection and GUI queue diagnostics

The optional `muffin.perf.detail` category separates projection construction and
state matching, edit-context/settings queries, formatting/theme signatures,
inline shaping/boxes, block replacement, command predicates and recovery-draft
materialization/encoding/commit. It is disabled by default; these probes do not
change reuse, invalidation or persistence behavior. Projection constructor-body
timing excludes initializer work. All scope timings are inclusive.

The native GUI probe enables this category with `MUFFIN_GUI_TRACE=1` and records
input due/post/dispatch/end/paint clocks on a common phase-relative timeline.
Scope rows >=0.1 ms reconstruct start from logged elapsed time; Timer, MetaCall,
Paint and UpdateRequest handlers >=1 ms have directly sampled boundaries.
Heartbeat rows record extra delay >=16 ms. The cap is 16384 rows per phase;
`timeline_dropped` must be checked before interpreting intervals. Nested scopes
and Qt events must not be summed as exclusive work. Paint completion is QWidget
dispatch completion, not display presentation, and unchanged inputs can share a
later operation's paint. Input-state change is not currently recorded.

`MUFFIN_GUI_SKIP_DRAFT_SNAPSHOT=1` blocks only disposable benchmark tabs' draft
timer signals for causal controls; `draft_snapshot_enabled` identifies that
condition. Product draft recovery remains enabled. Do not mix traced/control
runs into ordinary six-configuration performance baselines. The focused
`summarize_projection_queue.py` validates repeated 30 MiB single-tab controls,
clock differences, quantiles and untruncated timelines, and preserves complete
raw data with draft-event/input-queue overlap intervals.

### Background recovery drafts and interaction snapshots

`DraftSnapshotWriter` owns one lazy serial I/O worker per window. The GUI captures
an independent `PieceTable` (shared Qt buffers/indices and copied piece/prefix
vectors), not a flattened document. Materialization, UTF-8 encoding, content and
metadata commits run on the worker. Each key has at most one running snapshot
and one latest pending snapshot; newer pending revisions replace older ones.
No worker accesses a session, tab or widget. Sharing the append buffer can make
a later append detach it; this does not turn the initial document into a full
copy, but long edit histories remain a possible copy-on-write cost.

Only successful queued completions advance a live tab's acknowledged revision.
Failures remain retryable. Cleaning invalidates pending snapshots and posted
completions using a distinct generation, and enqueues a cleanup barrier after
any running write. A new edit for that key executes after the barrier. Cleanup
returns before disk work completes; an in-flight commit can precede its queued
removal. Normal destruction drains the queue without requiring GUI event
delivery, preventing a late worker from recreating a draft after cleanup has
completed. Shutdown can wait for storage. Forced termination before a write or
cleanup completes can retain the previous recoverable state. The existing
content/metadata files remain independently atomic, not a two-file transaction.
MainWindow routes save/discard/reload cleanup through this same queue.

Formatting caches retain the source stylesheet owner and its mutation revision
across passes. Sheet mutation or replacement invalidates the cached theme hash;
global revisions distinguish copied sheets mutated independently and assignments
into the same address. Node signatures, constraints and font/image resource
generations keep their existing invalidation rules. No on-disk hash cache is
introduced. Menu refresh snapshots include the six inline-format flags, so their
checked predicates share one query per refresh. The snapshot is not retained
across selection, document or settings refreshes.

GUI probes report `draft_write_mode`, acknowledged/document revisions, and
`editor_state_changed` for synchronous selection, document revision, tab or
vertical-scroll changes. This flag does not represent all visual changes
(especially themes). Raw compatible latency distributions retain unchanged
inputs; separate changed-input distributions prevent interpreting a no-op's
later shared paint as an editing stall. Worker perf logs are excluded from GUI
timelines, whose collectors remain GUI-thread-only.

### Prepared theme projections and document semantics

`ThemeManager` retains at most eight process-local, document-free render
prototypes, keyed by theme id and user zoom/font settings. Source sheet ownership
and its mutation revision are checked after the existing content-digest validity
boundary. A custom definition reload clears prepared prototypes, including JSON
themes without a source sheet. Local imports and font/image resource edits keep
the same manifest invalidation rules, including same-size, same-timestamp edits.

Each prototype shares a four-entry viewport projection LRU. Its exact key includes
the sheet revision, CSS viewport dimensions, zoom, font setting and dark mode.
Entries contain projected values/fonts and compiled selectors, never a live AST,
counter program or document adapter. Each view forks its own mutable CSS engine;
invalidation in one view cannot clear another view's matches or retain its AST in
the prototype. New environments, media buckets and evicted entries are projected
through the normal mapper. Hidden tabs retain the latest pending theme and apply
it only on activation; their live document/layout stays untouched while hidden.

Theme changes may transfer the previous counter cache as a temporary **semantic
seed**. Reuse requires the same document, exact revision and top-level node id at
each position. Only exact, interned semantic byte arrays are reused. Counter CSS,
programs, scopes and output are compiled/evaluated for the new theme. The seed is
released after preparation; it is not a cache of previously visited themes.
Full invalidation can seed an unchanged document too; edits or document replacement
reject stale seeds. Existing precise local edit invalidation remains in effect.

During full preparation, top-level blocks share host ancestry. Exact repeated
shapes with no possible counter subject are skipped after one negative discovery
walk. Positive candidates are always compiled independently, preserving positional
selectors, inline HTML, generated content and nested reset/set/increment scopes.
`DocumentCounterStats` exposes semantic builds and candidate walks for regression
checks; these are not timing thresholds.

Opt-in `muffin.perf.detail` scopes separate theme snapshot creation, viewport
projection, counter compilation/evaluation, style invalidation and chrome updates.
Their nested wall times are inclusive; `ThemePerfStage` totals remain exclusive.
`scripts/summarize_theme_switch.py` validates paired native GUI traces and retains
per-action stages, first/repeated actions, first paint scopes, memory and raw
samples. The first action in a measured phase is not a cold application startup.

### Viewport update timeline and computed fingerprints

EditorView marks its viewport `WA_OpaquePaintEvent` only when the effective
viewport background has alpha 255. Every dirty rectangle is filled before any
document/loading branch. Theme/font/viewport projection changes synchronize the
palette and opacity flag, including responsive translucent backgrounds. This
allows Qt to skip painting ancestors beneath an opaque editor update; transparent
backgrounds retain normal parent composition.

The opt-in `muffin.perf.queue` category records the actual viewport dirty request
before Qt coalesces updates. It preserves asynchronous `QWidget::update`
semantics. The native GUI probe relates producer due/post, Qt operation dispatch,
dirty requests, update/paint events and completed viewport paints on one steady
clock. It retains coalesced paints and unchanged inputs. Nested event intervals
are unioned when estimating observed handler coverage, never summed.

Detailed Windows traces additionally include native messages, dispatcher
awake/about-to-block observations, thread CPU/cycles, and optional background
thread-state point samples. Dispatcher observations do not prove sleep duration;
point samples do not establish exact ready time or blocking stacks. Cycles are
not milliseconds at an assumed frequency. Synthetic Qt inputs bypass native
keyboard delivery, and QWidget paint completion is not compositor presentation.
New probe runs check activation changes and sample exposure during each phase;
lost-focus/exposure runs are retained but excluded from comparisons.

Computed-style fingerprints have a bounded 128-entry thread-local memo. A hit
requires exact scalar values and shared Qt storage identity for properties,
variables, computed lengths and computed grid tracks. Retained immutable value
snapshots make normal public-map writes detach; nested grid values remain value
types. Hash results and existing downstream equality validation are unchanged.
Memo entries retain no AST or image-resource owner and never persist to disk.
Map/scalar mutation, animation changes and eviction remain covered by tests.

### Command cursor queries and input-tail diagnostics

`EditorController::queryCursorCommandState` resolves one temporary editing
projection for the six inline-format flags and image source. MainWindow consumes
these values within one command refresh. The existing independent query APIs
retain their semantics; nothing is cached across selection, document or setting
changes. Cross-block/cross-text selections still have no uniform format flags;
images are queried at the focus. Standalone HTML image queries read the block
slice rather than flattening the complete piece table. Smart-punctuation settings
are read through one local QSettings per projection build, without a persistent
settings cache.

The manual input probe separates `off`, `minimal` and detailed recording. `off`
disables product perf scopes and stage timers, but still retains probe clocks,
paint hooks, heartbeat and memory sampling. Detailed records preserve complete
event timelines and all changed/unchanged input samples. Recording overhead and
Qt-posted event accumulation cannot be assumed to equal physical keyboard cost.

Native input mode uses Win32 SendInput virtual-key pairs and measures the
spontaneous Qt key handler. It checks the selected window's Win32 foreground
handle before each injection and aborts on mismatch; failed/incomplete inputs
are excluded. This is injected OS input, not physical keyboard/IME validation.
No foreground handle means no injection. The probe records Qt activation and
exposure separately from Win32 foreground state and read-only desktop metadata.
Missing foreground observations in old records mean unknown, not verified.

After measured phases, the probe requests the latest normal recovery draft and
waits asynchronously for all revisions to be acknowledged, with a 60-second
timeout. Input processing does not call the worker's blocking drain method.
`draft.deliverCompletion` separately measures GUI completion delivery, excluding
worker encoding/I/O. Drain schema 2 captures wait time and working set before
the final screenshot; initial unversioned records captured them after screenshot
encoding, while retaining the same acknowledged-version check. Both are process
working-set checkpoints, not heap accounting.

Matched input-delivery comparisons use typing-only fresh processes for both
delivery paths. Optional foreground readiness waits outside the measured phase
for 500ms of stable foreground, activation/exposure and editor focus, with a
120-second timeout. It does not steal focus while posting input. Every recording
mode serializes the same clock edges and counts of later workload inputs already
posted but not yet dispatched; these are not OS/Qt internal queue lengths.
The comparison validator requires explicit foreground observations, recomputes
clocks/backlog/quantiles, checks edit revisions and acknowledged drafts, and
retains failed runs alongside missing comparison cells. Synthetic fixtures in
its validation tests are never performance evidence.
