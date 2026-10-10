#pragma once
// Bundled open fonts and substitutions for common proprietary fonts that
// are missing on Linux. A substitute is used only when the named font is absent.

#include <QString>
#include <QStringList>

namespace jp {

QStringList bundledFontDirs();                // where the fonts that ship with JeffPub are
int loadBundledFonts();                      // returns number of font files loaded
void installFontSubstitutions();
QString substituteFor(const QString &family); // empty if none / font available
int substituteStretch(const QString &family, bool bold = false, bool italic = false);
// The name to write in files other programs read: a bundled font that is
// metric-compatible with a standard one (Arimo, Carlito...) is written as
// that standard font, which every Windows computer has.
QString interchangeFontName(const QString &family);  // horizontal scale (percent) to imitate a condensed original, 100 if none
// Weight a missing font's name implies ("Franklin Gothic Medium" -> 500), so its
// substitute is set in that weight; 0 when the font is installed or no hint.
int substituteWeight(const QString &family);
// The width (ems) of a space in a missing font whose stand-in's spaces are
// narrower or wider; 0 when the font is installed or the stand-in matches.
double substituteSpaceEm(const QString &family, bool bold = false, bool italic = false);
// How much taller (over 1) or shorter to draw a missing font's stand-in so
// its letters stand as high as the real font's; 1 when installed or alike.
double substituteHeightScale(const QString &family, bool bold = false);
// A stock font's measured advance width for each character it has (em), from
// resources/fontwidths.txt: the face asked for, else the nearest the family
// has. Null when JeffPub has no measurements of the family.
const QHash<char16_t, double> *originalLetterWidths(const QString &family, bool bold, bool italic);
// The same fonts' pair kerning (em), keyed first << 16 | second, from
// resources/fontkerning.txt. Null when JeffPub has none for the family.
const QHash<quint32, double> *originalKerning(const QString &family, bool bold, bool italic);
QStringList bundledFamilies();

// Symbol fonts (Symbol, Wingdings) give their pictures their own character
// codes, either 0x20-0xFF or 0xF020-0xF0FF. When such a font is missing,
// this is the same picture as a Unicode character, or an empty string.
bool isSymbolFont(const QString &family);
QString symbolToUnicode(const QString &family, uint code);
// The other way: the symbol font and code (0xF020-0xF0FF) for a Unicode
// picture, or 0 when no symbol font has it.
uint unicodeToSymbol(QChar c, QString *family);

// Qt measures a QFont's point size at the platform's logical DPI (96 on most
// systems), but publication geometry is in points. Multiply a document point
// size by this before handing it to Qt so 1 layout unit = 1 point everywhere.
double fontPointFactor();

// One-time setup shared by the app, tools and tests: fonts, substitutions,
// and room for the very large pictures print publications use.
void initCore();

} // namespace jp
