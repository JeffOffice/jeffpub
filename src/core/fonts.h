#pragma once
// Bundled open fonts and substitutions for common proprietary fonts that
// are missing on Linux. A substitute is used only when the named font is absent.

#include <QString>
#include <QStringList>

namespace jp {

int loadBundledFonts();                      // returns number of font files loaded
void installFontSubstitutions();
QString substituteFor(const QString &family); // empty if none / font available
int substituteStretch(const QString &family);  // horizontal scale (percent) to imitate a condensed original, 100 if none
// Weight a missing font's name implies ("Franklin Gothic Medium" -> 500), so its
// substitute is set in that weight; 0 when the font is installed or no hint.
int substituteWeight(const QString &family);
QStringList bundledFamilies();

// Qt measures a QFont's point size at the platform's logical DPI (96 on most
// systems), but publication geometry is in points. Multiply a document point
// size by this before handing it to Qt so 1 layout unit = 1 point everywhere.
double fontPointFactor();

// One-time setup shared by the app, tools and tests: fonts, substitutions,
// and room for the very large pictures print publications use.
void initCore();

} // namespace jp
