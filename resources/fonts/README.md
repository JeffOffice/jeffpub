# Bundled fonts

These open fonts ship with JeffPub so publications look the same on Linux and Windows, and so `.pub` files that name common proprietary fonts render with matching widths. Each font remains under its own license. None is covered by JeffPub's GPL.

| Fonts | License |
|---|---|
| Families downloaded from Google Fonts (Arimo, Tinos, Cousine, Carlito, Caladea, Gelasio, and the rest of the `.ttf` files not listed below) | SIL Open Font License 1.1 (`OFL.txt`); copyright is held by each family's authors, as recorded in the font files |
| TeX Gyre Pagella, Bonum, Schola, Adventor (`TeXGyre*.otf`) | GUST Font License (`GUST-FONT-LICENSE.txt`) |
| DejaVu Sans (`DejaVuSans*.ttf`) | Bitstream Vera / DejaVu license (`DejaVu-LICENSE.txt`) |
| Comic Relief (`ComicRelief-*.ttf`, v1.210, from [github.com/loudifier/Comic-Relief](https://github.com/loudifier/Comic-Relief)) | SIL Open Font License 1.1 (`OFL.txt`) |
| Selawik (`Selawik-*.ttf`, v1.01, from [github.com/microsoft/Selawik](https://github.com/microsoft/Selawik)) | SIL Open Font License 1.1 (`OFL.txt`) |
| Libre Franklin ExtraBold (`LibreFranklin-ExtraBold*.ttf`, v3.000, from [github.com/impallari/Libre-Franklin](https://github.com/impallari/Libre-Franklin); the other weights came from Google Fonts) | SIL Open Font License 1.1 (`OFL.txt`) |
| Liberation Sans Narrow (`LiberationSansNarrow-*.ttf`, v1.07.5, from [github.com/liberationfonts](https://github.com/liberationfonts/liberation-sans-narrow)) | GPL v2 with font exceptions (`LiberationSansNarrow-LICENSE.txt`) |

Metric-compatible replacements: Arimo for Arial, Tinos for Times New Roman, Cousine for Courier New, Carlito for Calibri, Caladea for Cambria, Gelasio for Georgia, TeX Gyre Pagella for Palatino and Book Antiqua, TeX Gyre Bonum for Bookman, TeX Gyre Schola for Century Schoolbook, Comic Relief for Comic Sans MS, Selawik for Segoe UI, Liberation Sans Narrow for Arial Narrow (each checked character by character against the original's widths). The full substitution table is in `src/core/fonts.cpp`.
