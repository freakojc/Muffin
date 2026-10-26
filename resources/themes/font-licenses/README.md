# Theme font sources and licenses

Only original WOFF/WOFF2 fonts are maintained under `resources/themes/`.
CMake converts them into native TTF under the build directory's `theme-fonts/`
folder, using `scripts/prepare_theme_fonts.py`, and generates `themes.qrc` there.
Install the build dependencies with
`python -m pip install -r requirements-build.txt`. The application does not
depend on Python. Font family names, glyph data and face metadata are preserved;
only the webfont container is converted to SFNT.

- PT Serif (Newsprint): ParaType, SIL Open Font License; `PT-Serif-OFL.txt`.
- Open Sans (Github): Google / Steve Matteson, Apache License 2.0;
  `Open-Sans-Apache-2.0.txt`. The committed source is the v17 static family.
- Lato (Pixyll): Łukasz Dziedzic, SIL Open Font License;
  `Lato-OFL.txt`.
- Merriweather (Pixyll): Sorkin Type, SIL Open Font License;
  `Merriweather-OFL.txt`. The committed source is the v19 static family.
- Inter (Lengyi, Lengyi Dark): The Inter Project Authors / Rasmus Andersson,
  SIL Open Font License; `Inter-OFL.txt`. The committed source is the v4.1
  static family (regular, italic, semi-bold, bold) with full unicode coverage,
  so prose is not limited to a latin subset.
- JetBrains Mono (Lengyi, Lengyi Dark): JetBrains, SIL Open Font License;
  `JetBrains-Mono-OFL.txt`, authors in `JetBrains-Mono-AUTHORS.txt`. The
  committed source is v2.304 (regular, bold).

The font resources expose the original authored CSS URLs through QRC aliases,
so theme CSS stays independent of the native packaging format. The resource
bundle also embeds these notices and license texts.
