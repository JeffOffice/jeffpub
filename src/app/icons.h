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
// The Lines gallery's connectors and freehand tools: "elbow", "curved"
// (with "Arrow" or "Double" after for arrowheads), "curve", "freeform",
// "scribble".
QIcon lineToolIcon(const QString &kind);
// Whether the interface is dark. main() decides from the user's setting (or the
// system's color scheme) and records it here; every part of the UI asks this.
bool uiDark();
void setUiDark(bool dark);
// Windows' high contrast: the system's own colors replace JeffPub's.
bool uiHighContrast();
void setUiHighContrast(bool on);
QColor uiText();
QColor uiAccent();
QColor focusRingColor();   // the ring around the control with the keyboard focus
QColor uiWindow();         // behind cards (File pages)
QColor uiPanel();          // side panels (File sidebar, page thumbnails)
QColor uiCard();           // cards and lists on them
QColor uiCardPressed();
QColor uiLine();           // borders of cards and panels
// App-wide widget tweaks the style sheet can't express (tab bars that never scroll).
void installUiPolish();
// Application-wide style sheet: rounded fields and buttons, accent focus
// rings, an accent "primary" button (property primary=true) and slim scroll bars.
QString modernStyleSheet();

} // namespace jp
