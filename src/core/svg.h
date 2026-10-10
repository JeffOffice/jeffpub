#pragma once
// A small SVG reader for vector artwork: icons, and SVG pictures turned into
// shapes. It reads paths and the basic shapes, groups, <use>, transforms,
// style sheets and fill and stroke (a gradient fills with its first color).
// Text, embedded pictures and foreign content are left out and `skipped`
// says so; clip paths cut the shapes they cover, and masks, filters and
// patterns are ignored.

#include "core/items.h"

#include <QColor>
#include <QPainterPath>
#include <QRectF>
#include <QVector>

class QPainter;
class QSvgRenderer;

namespace jp::svg {

struct Element {
    QPainterPath path;              // in the drawing's coordinates, transforms applied
    QColor fill, stroke;            // invalid = none; alpha = opacity
    double strokeWidth = 1;         // in the drawing's coordinates
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    QVector<double> dashes;         // dash and gap lengths in the drawing's coordinates; empty = solid
    double dashOffset = 0;
};

struct Drawing {
    QRectF viewBox;                 // the coordinates the elements use
    QSizeF size;                    // the drawing's own size, in points
    QVector<Element> elements;
    bool skipped = false;           // had text or pictures this reader leaves out, or too many parts
    bool isValid() const { return !viewBox.isEmpty(); }
};

// `currentColor` is the color a drawing asks for with "currentColor".
Drawing read(const QByteArray &svg, const QColor &currentColor = Qt::black);
// SVG path data ("M2 8.5c0 2.3 1.5 4 3 5.5l7 7Z").
QPainterPath pathData(const QString &d);
// The drawing as editable artwork shapes (ShapeItem::isArt) filling
// `frame`: a shape for each element, or for each run of elements drawn alike
// when `merge` is set (an icon is then one shape). Several shapes come in a
// group. Colors equal to `current` become `currentRef`, so an icon drawn in
// a scheme color follows the color scheme. Null when nothing is drawn.
ItemPtr shapes(const Drawing &d, const QRectF &frame, bool merge, const QColor &current = QColor(), const ColorRef &currentRef = ColorRef());
// An SVG picture as artwork shapes in the picture's place, turned and
// flipped as it is (its crop, border and effects are left behind).
// `partial` tells whether text or pictures inside it were left out.
ItemPtr pictureShapes(const QByteArray &svg, const PictureItem &pic, bool *partial);
// Draws `svg`, which `renderer` has loaded, stretched over `bounds`. Qt's
// renderer draws masks, filters and patterns but leaves out clip-path, so a
// drawing with clipping is drawn in runs of parts under the same clip, each
// run cut to its clip as the painter's own clip (kept as a vector in PDFs
// and prints), in the order the parts were drawn.
void paint(QSvgRenderer &renderer, const QByteArray &svg, QPainter *p, const QRectF &bounds);
// An SVG elliptical arc from p0 to p1, as cubic Béziers.
void arcTo(QPainterPath &path, QPointF p0, double rx, double ry, double phiDeg, bool large, bool sweep, QPointF p1);

} // namespace jp::svg
