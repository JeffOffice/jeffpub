#pragma once
// Theme-aware icons from the bundled Lucide SVG set (ISC license), plus a few
// drawn icons (color bars under "font color", shape previews).

#include <QColor>
#include <QIcon>
#include <QPainterPath>
#include <functional>

namespace jp {

QIcon icon(const QString &name);
// Icon with a colored bar underneath (font color, fill color, line color).
QIcon colorBarIcon(const QString &name, const QColor &bar);
// Icon drawn by a function into a size×size box.
QIcon drawnIcon(const std::function<void(QPainter *, const QRectF &)> &draw);
QIcon shapeIcon(const QString &shapeId);
// Whether the interface is dark. main() decides from the user's setting (or the
// system's color scheme) and records it here; every part of the UI asks this.
bool uiDark();
void setUiDark(bool dark);
QColor uiText();
QColor uiAccent();
// App-wide widget tweaks the style sheet can't express (tab bars that never scroll).
void installUiPolish();
// Application-wide style sheet: rounded fields and buttons, accent focus
// rings, an accent "primary" button (property primary=true) and slim scroll bars.
QString modernStyleSheet();

} // namespace jp
