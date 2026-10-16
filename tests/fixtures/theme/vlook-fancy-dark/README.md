# VLOOK Fancy Dark regression fixture

Pinned, unmodified `vlook-fancy-dark.css` from the user's VLOOK **V2026.10.1** theme installation, captured on 2026-10-14.

- Upstream: https://github.com/MadMaxChow/VLOOK
- License: MIT; upstream notice retained in `LICENSE`.
- SHA-256: `dc9ce7c464f4ddf8644040fb77c7c7e408a70f1bee3ba2c9f9a8ac746901f3cc`

The CSS includes its embedded PNG grid and SVG assets. Tests never fetch the remote font imports. `sample.md` provides a small, stable Markdown document for host/selector, computed-style, layout and raster checks. Assertions use theme colors and geometry rather than platform-specific glyph screenshots.

`alerts.md` covers all five semantic alert kinds and their SVG title children.
The test saves `vlook-fancy-dark-alerts.png` beside its executable for visual
review; source Markdown and the original CSS remain separate fixtures.

Keep this snapshot fixed when changing the compatibility engine. Update the theme snapshot as a separate change, with version, hash and visual review.
