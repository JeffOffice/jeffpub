# Bundled fonts

These open fonts ship with JeffPub 79 so publications look the same on Linux and Windows, and so `.pub` files that name common proprietary fonts render with matching widths. Each font remains under its own license. None is covered by JeffPub 79's GPL.

| Fonts | License |
|---|---|
| Families downloaded from Google Fonts (Arimo, Tinos, Cousine, Carlito, Caladea, Gelasio, and the rest of the `.ttf` files not listed below) | SIL Open Font License 1.1 (`OFL.txt`); copyright is held by each family's authors, as recorded in the font files |
| TeX Gyre Pagella, Bonum, Schola, Adventor (`TeXGyre*.otf`) | GUST Font License (`GUST-FONT-LICENSE.txt`) |
| DejaVu Sans (`DejaVuSans*.ttf`) | Bitstream Vera / DejaVu license (`DejaVu-LICENSE.txt`) |

Metric-compatible replacements: Arimo for Arial, Tinos for Times New Roman, Cousine for Courier New, Carlito for Calibri, Caladea for Cambria, Gelasio for Georgia, TeX Gyre Pagella for Palatino and Book Antiqua, TeX Gyre Bonum for Bookman, TeX Gyre Schola for Century Schoolbook. Open Sans stands in for Segoe UI (similar design, not metric-compatible). The full substitution table is in `src/core/fonts.cpp`.
