#include "render/shapes.h"

#include <QtMath>
#include <cmath>

namespace jp {

namespace {

using V = const QVector<double> &;

QPainterPath poly(std::initializer_list<QPointF> pts)
{
    QPainterPath p;
    bool first = true;
    for (const QPointF &pt : pts) {
        if (first) p.moveTo(pt); else p.lineTo(pt);
        first = false;
    }
    p.closeSubpath();
    return p;
}

QPainterPath polyV(const QVector<QPointF> &pts)
{
    QPainterPath p;
    for (int i = 0; i < pts.size(); ++i) {
        if (i == 0) p.moveTo(pts[i]); else p.lineTo(pts[i]);
    }
    p.closeSubpath();
    return p;
}

QPainterPath starPath(double w, double h, int n, double inner)
{
    QVector<QPointF> pts;
    for (int i = 0; i < n * 2; ++i) {
        const double ang = M_PI * i / n - M_PI / 2, r = (i % 2) ? inner : 1.0;
        pts << QPointF(w / 2 + std::cos(ang) * r * w / 2, h / 2 + std::sin(ang) * r * h / 2);
    }
    return polyV(pts);
}

QPainterPath ngon(double w, double h, int n)
{
    QVector<QPointF> pts;
    for (int i = 0; i < n; ++i) {
        const double ang = -M_PI / 2 + 2 * M_PI * i / n;
        pts << QPointF(w / 2 + std::cos(ang) * w / 2, h / 2 + std::sin(ang) * h / 2);
    }
    return polyV(pts);
}

double A(V a, int i, double d) { return i < a.size() ? a[i] : d; }
double mn(double w, double h) { return std::min(w, h); }

QPainterPath roundRect(double w, double h, double r)
{
    QPainterPath p;
    r = std::min({r, w / 2, h / 2});
    p.addRoundedRect(QRectF(0, 0, w, h), r, r);
    return p;
}

QRectF insetRect(double w, double h, double fx, double fy) { return QRectF(w * fx, h * fy, w * (1 - 2 * fx), h * (1 - 2 * fy)); }

// Arrow pointing right with shaft thickness t (fraction of h) and head length hl (fraction of min).
QPainterPath arrowRight(double w, double h, double t, double hl)
{
    const double head = std::min(w, hl * mn(w, h));
    const double y0 = h * (0.5 - t / 2), y1 = h * (0.5 + t / 2);
    return poly({{0, y0}, {w - head, y0}, {w - head, 0}, {w, h / 2}, {w - head, h}, {w - head, y1}, {0, y1}});
}

QPainterPath rotated(const QPainterPath &p, double w, double h, double deg, double pw, double ph)
{
    // Rotate a path built in a pw×ph box into the w×h box.
    QTransform t;
    t.translate(w / 2, h / 2);
    t.rotate(deg);
    t.translate(-pw / 2, -ph / 2);
    return t.map(p);
}

QPainterPath ellipse(double w, double h)
{
    QPainterPath p;
    p.addEllipse(QRectF(0, 0, w, h));
    return p;
}

QPainterPath wavePath(double w, double h, double amp, int waves)
{
    QPainterPath p;
    const double a = amp * h;
    p.moveTo(0, a);
    for (int i = 0; i < waves; ++i) {
        const double x0 = w * i / waves, x1 = w * (i + 1) / waves;
        p.cubicTo(x0 + (x1 - x0) / 3, -a * 0.5, x0 + 2 * (x1 - x0) / 3, a * 2.5, x1, a);
    }
    p.lineTo(w, h - a);
    for (int i = waves - 1; i >= 0; --i) {
        const double x1 = w * i / waves, x0 = w * (i + 1) / waves;
        p.cubicTo(x0 - (x0 - x1) / 3, h - a * 2.5 + a * 0 + a * 0, x0 - 2 * (x0 - x1) / 3, h + a * 0.5, x1, h - a);
    }
    p.closeSubpath();
    return p;
}

QPainterPath calloutTail(const QPainterPath &body, double w, double h, double tx, double ty, double baseX, double baseY, double spread)
{
    QPainterPath tail;
    tail.moveTo(baseX - spread, baseY);
    tail.lineTo(tx * w, ty * h);
    tail.lineTo(baseX + spread, baseY);
    tail.closeSubpath();
    return body.united(tail).simplified();
}

} // namespace

static QVector<ShapeDef> build()
{
    QVector<ShapeDef> L;
    auto add = [&](const char *id, const char *name, const char *cat, PathFn fn, QVector<double> defs = {}, QVector<AdjHandle> hs = {},
                   double inset = 0.1, RectFn tr = nullptr, bool open = false) {
        ShapeDef d;
        d.id = QString::fromLatin1(id);
        d.name = QString::fromLatin1(name);
        d.category = QString::fromLatin1(cat);
        d.path = fn;
        d.defaults = defs;
        d.handles = hs;
        d.inset = inset;
        d.textRect = tr;
        d.open = open;
        L << d;
    };

    // ---------------- Rectangles ----------------
    add("rect", "Rectangle", "Rectangles", [](double w, double h, V) { QPainterPath p; p.addRect(0, 0, w, h); return p; }, {}, {}, 0);
    add("roundRect", "Rounded Rectangle", "Rectangles", [](double w, double h, V a) { return roundRect(w, h, A(a, 0, 0.1667) * mn(w, h)); },
        {0.1667}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.05);
    add("snip1", "Snip Single Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double s = A(a, 0, 0.1667) * mn(w, h);
        return poly({{0, 0}, {w - s, 0}, {w, s}, {w, h}, {0, h}}); }, {0.1667}, {{0, 0, 2, true, 0, 0, 0.5}}, 0.05);
    add("snip2same", "Snip Same Side Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double s = A(a, 0, 0.1667) * mn(w, h);
        return poly({{s, 0}, {w - s, 0}, {w, s}, {w, h}, {0, h}, {0, s}}); }, {0.1667}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.05);
    add("snip2diag", "Snip Diagonal Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double s = A(a, 0, 0.1667) * mn(w, h);
        return poly({{0, 0}, {w - s, 0}, {w, s}, {w, h}, {s, h}, {0, h - s}}); }, {0.1667}, {{0, 0, 2, true, 0, 0, 0.5}}, 0.05);
    add("snipRound", "Snip and Round Single Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double s = A(a, 0, 0.1667) * mn(w, h);
        QPainterPath p; p.moveTo(s, 0); p.lineTo(w - s, 0); p.lineTo(w, s); p.lineTo(w, h); p.lineTo(0, h); p.lineTo(0, s);
        p.quadTo(0, 0, s, 0); p.closeSubpath(); return p; }, {0.1667}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.05);
    add("round1", "Round Single Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double r = A(a, 0, 0.1667) * mn(w, h);
        QPainterPath p; p.moveTo(0, 0); p.lineTo(w - r, 0); p.quadTo(w, 0, w, r); p.lineTo(w, h); p.lineTo(0, h); p.closeSubpath(); return p; },
        {0.1667}, {{0, 0, 2, true, 0, 0, 0.5}}, 0.05);
    add("round2same", "Round Same Side Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double r = A(a, 0, 0.1667) * mn(w, h);
        QPainterPath p; p.moveTo(r, 0); p.lineTo(w - r, 0); p.quadTo(w, 0, w, r); p.lineTo(w, h); p.lineTo(0, h); p.lineTo(0, r);
        p.quadTo(0, 0, r, 0); p.closeSubpath(); return p; }, {0.1667}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.05);
    add("round2diag", "Round Diagonal Corner Rectangle", "Rectangles", [](double w, double h, V a) {
        const double r = A(a, 0, 0.1667) * mn(w, h);
        QPainterPath p; p.moveTo(r, 0); p.lineTo(w, 0); p.lineTo(w, h - r); p.quadTo(w, h, w - r, h); p.lineTo(0, h); p.lineTo(0, r);
        p.quadTo(0, 0, r, 0); p.closeSubpath(); return p; }, {0.1667}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.05);

    // ---------------- Basic Shapes ----------------
    add("ellipse", "Oval", "Basic Shapes", [](double w, double h, V) { return ellipse(w, h); }, {}, {}, 0.15);
    add("triangle", "Isosceles Triangle", "Basic Shapes", [](double w, double h, V a) {
        return poly({{A(a, 0, 0.5) * w, 0}, {w, h}, {0, h}}); }, {0.5}, {{0, 0, 0, false, 0, 0, 1}},
        0, [](double w, double h, V) { return QRectF(w * 0.25, h * 0.5, w * 0.5, h * 0.48); });
    add("rtTriangle", "Right Triangle", "Basic Shapes", [](double w, double h, V) { return poly({{0, 0}, {w, h}, {0, h}}); }, {}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.06, h * 0.5, w * 0.5, h * 0.45); });
    add("parallelogram", "Parallelogram", "Basic Shapes", [](double w, double h, V a) {
        const double s = A(a, 0, 0.25) * w; return poly({{s, 0}, {w, 0}, {w - s, h}, {0, h}}); }, {0.25}, {{0, 0, 0, false, 0, 0, 1}}, 0.15);
    add("trapezoid", "Trapezoid", "Basic Shapes", [](double w, double h, V a) {
        const double s = A(a, 0, 0.25) * w; return poly({{s, 0}, {w - s, 0}, {w, h}, {0, h}}); }, {0.25}, {{0, 0, 0, false, 0, 0, 0.5}}, 0.15);
    add("diamond", "Diamond", "Basic Shapes", [](double w, double h, V) { return poly({{w / 2, 0}, {w, h / 2}, {w / 2, h}, {0, h / 2}}); }, {}, {}, 0.25);
    add("pentagon", "Regular Pentagon", "Basic Shapes", [](double w, double h, V) { return ngon(w, h, 5); }, {}, {}, 0.2);
    add("hexagon", "Hexagon", "Basic Shapes", [](double w, double h, V a) {
        const double s = A(a, 0, 0.25) * w; return poly({{s, 0}, {w - s, 0}, {w, h / 2}, {w - s, h}, {s, h}, {0, h / 2}}); },
        {0.25}, {{0, 0, 0, false, 0, 0, 0.5}}, 0.15);
    add("heptagon", "Heptagon", "Basic Shapes", [](double w, double h, V) { return ngon(w, h, 7); }, {}, {}, 0.18);
    add("octagon", "Octagon", "Basic Shapes", [](double w, double h, V a) {
        const double s = A(a, 0, 0.29) * mn(w, h);
        return poly({{s, 0}, {w - s, 0}, {w, s}, {w, h - s}, {w - s, h}, {s, h}, {0, h - s}, {0, s}}); },
        {0.29}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.12);
    add("decagon", "Decagon", "Basic Shapes", [](double w, double h, V) { return ngon(w, h, 10); }, {}, {}, 0.12);
    add("dodecagon", "Dodecagon", "Basic Shapes", [](double w, double h, V) { return ngon(w, h, 12); }, {}, {}, 0.12);
    add("pie", "Partial Circle", "Basic Shapes", [](double w, double h, V a) {
        QPainterPath p; p.moveTo(w / 2, h / 2); p.arcTo(QRectF(0, 0, w, h), 0, A(a, 0, 0.75) * 360); p.closeSubpath(); return p; },
        {0.75}, {}, 0.2);
    add("chord", "Chord", "Basic Shapes", [](double w, double h, V a) {
        QPainterPath p; p.arcMoveTo(QRectF(0, 0, w, h), 45); p.arcTo(QRectF(0, 0, w, h), 45, A(a, 0, 0.75) * 360); p.closeSubpath(); return p; },
        {0.75}, {}, 0.2);
    add("teardrop", "Teardrop", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w / 2, 0); p.lineTo(w, 0); p.lineTo(w, h / 2);
        p.arcTo(QRectF(0, 0, w, h), 0, -270); p.closeSubpath(); return p; }, {}, {}, 0.15);
    add("frame", "Frame", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.125) * mn(w, h); QPainterPath p; p.addRect(0, 0, w, h); p.addRect(t, t, w - 2 * t, h - 2 * t); return p; },
        {0.125}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.12);
    add("halfFrame", "Half Frame", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.333) * mn(w, h);
        return poly({{0, 0}, {w, 0}, {w - t * w / h * 0.0 - t, t}, {t, t}, {t, h - t}, {0, h}}); }, {0.333}, {}, 0.1);
    add("corner", "L-Shape", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5) * mn(w, h); return poly({{0, 0}, {t, 0}, {t, h - t}, {w, h - t}, {w, h}, {0, h}}); },
        {0.5}, {{0, 0, 2, false, 0.5, 0, 1}}, 0.1);
    add("diagStripe", "Diagonal Stripe", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5); return poly({{0, h * t}, {w * t, 0}, {w, 0}, {0, h}}); }, {0.5}, {}, 0.15);
    add("plus", "Cross", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.25) * mn(w, h);
        return poly({{t, 0}, {w - t, 0}, {w - t, t}, {w, t}, {w, h - t}, {w - t, h - t}, {w - t, h}, {t, h}, {t, h - t}, {0, h - t}, {0, t}, {t, t}}); },
        {0.25}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.25);
    add("plaque", "Plaque", "Basic Shapes", [](double w, double h, V a) {
        const double r = A(a, 0, 0.1667) * mn(w, h);
        QPainterPath p; p.moveTo(r, 0); p.lineTo(w - r, 0); p.arcTo(QRectF(w - r, -r, 2 * r, 2 * r), 180, 90);
        p.lineTo(w, h - r); p.arcTo(QRectF(w - r, h - r, 2 * r, 2 * r), 90, 90); p.lineTo(r, h);
        p.arcTo(QRectF(-r, h - r, 2 * r, 2 * r), 0, 90); p.lineTo(0, r); p.arcTo(QRectF(-r, -r, 2 * r, 2 * r), 270, 90); p.closeSubpath(); return p; },
        {0.1667}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.12);
    add("can", "Can", "Basic Shapes", [](double w, double h, V a) {
        const double e = A(a, 0, 0.25) * mn(w, h); QPainterPath p;
        p.moveTo(0, e / 2); p.arcTo(QRectF(0, 0, w, e), 180, -180); p.lineTo(w, h - e / 2); p.arcTo(QRectF(0, h - e, w, e), 0, -180); p.closeSubpath();
        p.moveTo(0, e / 2); p.arcTo(QRectF(0, 0, w, e), 180, 180); return p; },
        {0.25}, {{0, 1, 2, false, 0.5, 0, 0.5}}, 0, [](double w, double h, V a) { const double e = A(a, 0, 0.25) * mn(w, h); return QRectF(0, e, w, h - 1.5 * e); });
    add("cube", "Cube", "Basic Shapes", [](double w, double h, V a) {
        const double d = A(a, 0, 0.25) * mn(w, h); QPainterPath p;
        p.addPolygon(QPolygonF({QPointF(0, d), QPointF(d, 0), QPointF(w, 0), QPointF(w, h - d), QPointF(w - d, h), QPointF(0, h), QPointF(0, d)}));
        p.moveTo(0, d); p.lineTo(w - d, d); p.lineTo(w, 0); p.moveTo(w - d, d); p.lineTo(w - d, h); return p; },
        {0.25}, {{0, 1, 2, false, 0, 0, 1}}, 0, [](double w, double h, V a) { const double d = A(a, 0, 0.25) * mn(w, h); return QRectF(0, d, w - d, h - d); });
    add("bevel", "Bevel", "Basic Shapes", [](double w, double h, V a) {
        const double d = A(a, 0, 0.125) * mn(w, h); QPainterPath p; p.addRect(0, 0, w, h); p.addRect(d, d, w - 2 * d, h - 2 * d);
        p.moveTo(0, 0); p.lineTo(d, d); p.moveTo(w, 0); p.lineTo(w - d, d); p.moveTo(w, h); p.lineTo(w - d, h - d); p.moveTo(0, h); p.lineTo(d, h - d);
        return p; }, {0.125}, {{0, 0, 2, false, 0, 0, 0.5}}, 0.13);
    add("donut", "Donut", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.25) * mn(w, h); QPainterPath p; p.addEllipse(QRectF(0, 0, w, h)); p.addEllipse(QRectF(t, t, w - 2 * t, h - 2 * t)); return p; },
        {0.25}, {{0, 0, 2, false, 0.5, 0, 0.5}}, 0.3);
    add("noSmoking", "\"No\" Symbol", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.18) * mn(w, h); QPainterPath ring; ring.addEllipse(QRectF(0, 0, w, h));
        QPainterPath inner; inner.addEllipse(QRectF(t, t, w - 2 * t, h - 2 * t));
        QPainterPath bar; bar.addRect(QRectF(-w, -t / 2, 3 * w, t));
        QTransform tr; tr.translate(w / 2, h / 2); tr.rotate(45); tr.translate(-w / 2, 0);
        QPainterPath slash = tr.map(bar).intersected(inner);
        return ring.subtracted(inner).united(slash); }, {0.18}, {}, 0.3);
    add("blockArc", "Block Arc", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.25) * mn(w, h); QPainterPath p; p.arcMoveTo(QRectF(0, 0, w, h), 180); p.arcTo(QRectF(0, 0, w, h), 180, -180);
        p.lineTo(w - t, h / 2); p.arcTo(QRectF(t, t, w - 2 * t, h - 2 * t), 0, 180); p.closeSubpath(); return p; }, {0.25}, {}, 0.2);
    add("foldedCorner", "Folded Corner", "Basic Shapes", [](double w, double h, V a) {
        const double f = A(a, 0, 0.1667) * mn(w, h); QPainterPath p = poly({{0, 0}, {w, 0}, {w, h - f}, {w - f, h}, {0, h}});
        p.moveTo(w - f, h); p.lineTo(w - f * 0.8, h - f * 0.8); p.lineTo(w, h - f); return p; }, {0.1667}, {{0, 0, 2, true, 1, 0, 0.5}}, 0.08);
    add("smiley", "Smiley Face", "Basic Shapes", [](double w, double h, V a) {
        QPainterPath p; p.addEllipse(QRectF(0, 0, w, h));
        p.addEllipse(QRectF(w * 0.3, h * 0.3, w * 0.1, h * 0.12)); p.addEllipse(QRectF(w * 0.6, h * 0.3, w * 0.1, h * 0.12));
        const double s = A(a, 0, 0.05);
        p.moveTo(w * 0.28, h * 0.65); p.quadTo(w * 0.5, h * (0.65 + s * 3), w * 0.72, h * 0.65); return p; }, {0.05}, {}, 0.2);
    add("heart", "Heart", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w / 2, h * 0.25);
        p.cubicTo(w / 2, h * 0.05, w * 0.08, -h * 0.02, w * 0.02, h * 0.3);
        p.cubicTo(-w * 0.04, h * 0.58, w * 0.3, h * 0.75, w / 2, h);
        p.cubicTo(w * 0.7, h * 0.75, w * 1.04, h * 0.58, w * 0.98, h * 0.3);
        p.cubicTo(w * 0.92, -h * 0.02, w / 2, h * 0.05, w / 2, h * 0.25); p.closeSubpath(); return p; }, {}, {}, 0.25);
    add("lightning", "Lightning Bolt", "Basic Shapes", [](double w, double h, V) {
        return poly({{w * 0.39, 0}, {w * 0.61, h * 0.29}, {w * 0.52, h * 0.34}, {w * 0.79, h * 0.6}, {w * 0.7, h * 0.65}, {w, h},
                     {w * 0.48, h * 0.71}, {w * 0.58, h * 0.67}, {w * 0.18, h * 0.45}, {w * 0.3, h * 0.4}, {0, h * 0.17}}); }, {}, {}, 0.3);
    add("sun", "Sun", "Basic Shapes", [](double w, double h, V a) {
        const double r = A(a, 0, 0.25); QPainterPath p; p.addEllipse(QRectF(w * r, h * r, w * (1 - 2 * r), h * (1 - 2 * r)));
        for (int i = 0; i < 8; ++i) {
            const double ang = i * M_PI / 4, a1 = ang - 0.13, a2 = ang + 0.13;
            const double ri = 0.5 - r + 0.06, ro = 0.5;
            p.addPolygon(QPolygonF({QPointF(w / 2 + std::cos(a1) * w * (0.5 - ri), h / 2 + std::sin(a1) * h * (0.5 - ri)),
                                    QPointF(w / 2 + std::cos(ang) * w * ro, h / 2 + std::sin(ang) * h * ro),
                                    QPointF(w / 2 + std::cos(a2) * w * (0.5 - ri), h / 2 + std::sin(a2) * h * (0.5 - ri))}));
            p.closeSubpath();
        }
        return p; }, {0.25}, {}, 0.3);
    add("moon", "Moon", "Basic Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5); QPainterPath p; p.moveTo(w, 0); p.arcTo(QRectF(0, 0, 2 * w, h), 90, 180);
        p.arcTo(QRectF(w * t, 0, 2 * w * (1 - t), h), 270, -180); p.closeSubpath(); return p; }, {0.5}, {{0, 0, 0, false, 0.5, 0.05, 0.95}}, 0.25);
    add("cloud", "Cloud", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p;
        const QRectF c[] = {{0.05, 0.3, 0.35, 0.4}, {0.2, 0.08, 0.35, 0.4}, {0.45, 0.05, 0.35, 0.42}, {0.62, 0.25, 0.36, 0.42},
                            {0.45, 0.5, 0.38, 0.42}, {0.15, 0.52, 0.38, 0.42}, {0.0, 0.42, 0.3, 0.32}};
        for (const QRectF &r : c) { QPainterPath e; e.addEllipse(QRectF(r.x() * w, r.y() * h, r.width() * w, r.height() * h)); p = p.united(e); }
        return p; }, {}, {}, 0.22);
    add("arc", "Arc", "Basic Shapes", [](double w, double h, V a) {
        QPainterPath p; p.arcMoveTo(QRectF(0, 0, w, h), 90); p.arcTo(QRectF(0, 0, w, h), 90, -A(a, 0, 0.25) * 360); return p; },
        {0.25}, {}, 0.2, nullptr, true);
    add("bracketPair", "Double Bracket", "Basic Shapes", [](double w, double h, V a) {
        const double r = A(a, 0, 0.1667) * mn(w, h); QPainterPath p;
        p.moveTo(r, 0); p.quadTo(0, 0, 0, r); p.lineTo(0, h - r); p.quadTo(0, h, r, h);
        p.moveTo(w - r, 0); p.quadTo(w, 0, w, r); p.lineTo(w, h - r); p.quadTo(w, h, w - r, h); return p; }, {0.1667}, {}, 0.1, nullptr, true);
    add("bracePair", "Double Brace", "Basic Shapes", [](double w, double h, V a) {
        const double r = A(a, 0, 0.0833) * mn(w, h) * 2; QPainterPath p;
        p.moveTo(2 * r, 0); p.quadTo(r, 0, r, r); p.lineTo(r, h / 2 - r); p.quadTo(r, h / 2, 0, h / 2); p.quadTo(r, h / 2, r, h / 2 + r);
        p.lineTo(r, h - r); p.quadTo(r, h, 2 * r, h);
        p.moveTo(w - 2 * r, 0); p.quadTo(w - r, 0, w - r, r); p.lineTo(w - r, h / 2 - r); p.quadTo(w - r, h / 2, w, h / 2);
        p.quadTo(w - r, h / 2, w - r, h / 2 + r); p.lineTo(w - r, h - r); p.quadTo(w - r, h, w - 2 * r, h); return p; }, {0.0833}, {}, 0.12, nullptr, true);
    add("leftBracket", "Left Bracket", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w, 0); p.quadTo(0, 0, 0, w); p.lineTo(0, h - w); p.quadTo(0, h, w, h); return p; }, {}, {}, 0.1, nullptr, true);
    add("rightBracket", "Right Bracket", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, 0); p.quadTo(w, 0, w, w); p.lineTo(w, h - w); p.quadTo(w, h, 0, h); return p; }, {}, {}, 0.1, nullptr, true);
    add("leftBrace", "Left Brace", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w, 0); p.quadTo(w / 2, 0, w / 2, w / 2); p.lineTo(w / 2, h / 2 - w / 2); p.quadTo(w / 2, h / 2, 0, h / 2);
        p.quadTo(w / 2, h / 2, w / 2, h / 2 + w / 2); p.lineTo(w / 2, h - w / 2); p.quadTo(w / 2, h, w, h); return p; }, {}, {}, 0.1, nullptr, true);
    add("rightBrace", "Right Brace", "Basic Shapes", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, 0); p.quadTo(w / 2, 0, w / 2, w / 2); p.lineTo(w / 2, h / 2 - w / 2); p.quadTo(w / 2, h / 2, w, h / 2);
        p.quadTo(w / 2, h / 2, w / 2, h / 2 + w / 2); p.lineTo(w / 2, h - w / 2); p.quadTo(w / 2, h, 0, h); return p; }, {}, {}, 0.1, nullptr, true);

    // ---------------- Block Arrows ----------------
    add("rightArrow", "Right Arrow", "Block Arrows", [](double w, double h, V a) { return arrowRight(w, h, A(a, 0, 0.5), A(a, 1, 0.5)); },
        {0.5, 0.5}, {{0, 1, 1, false, 0, 0.05, 1}}, 0.15);
    add("leftArrow", "Left Arrow", "Block Arrows", [](double w, double h, V a) { return rotated(arrowRight(w, h, A(a, 0, 0.5), A(a, 1, 0.5)), w, h, 180, w, h); },
        {0.5, 0.5}, {}, 0.15);
    add("upArrow", "Up Arrow", "Block Arrows", [](double w, double h, V a) { return rotated(arrowRight(h, w, A(a, 0, 0.5), A(a, 1, 0.5)), w, h, -90, h, w); },
        {0.5, 0.5}, {}, 0.15);
    add("downArrow", "Down Arrow", "Block Arrows", [](double w, double h, V a) { return rotated(arrowRight(h, w, A(a, 0, 0.5), A(a, 1, 0.5)), w, h, 90, h, w); },
        {0.5, 0.5}, {}, 0.15);
    add("leftRightArrow", "Left-Right Arrow", "Block Arrows", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5), hd = std::min(w / 2, A(a, 1, 0.5) * mn(w, h));
        const double y0 = h * (0.5 - t / 2), y1 = h * (0.5 + t / 2);
        return poly({{0, h / 2}, {hd, 0}, {hd, y0}, {w - hd, y0}, {w - hd, 0}, {w, h / 2}, {w - hd, h}, {w - hd, y1}, {hd, y1}, {hd, h}}); },
        {0.5, 0.5}, {}, 0.2);
    add("upDownArrow", "Up-Down Arrow", "Block Arrows", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5), hd = std::min(h / 2, A(a, 1, 0.5) * mn(w, h));
        const double x0 = w * (0.5 - t / 2), x1 = w * (0.5 + t / 2);
        return poly({{w / 2, 0}, {w, hd}, {x1, hd}, {x1, h - hd}, {w, h - hd}, {w / 2, h}, {0, h - hd}, {x0, h - hd}, {x0, hd}, {0, hd}}); },
        {0.5, 0.5}, {}, 0.2);
    add("quadArrow", "Quad Arrow", "Block Arrows", [](double w, double h, V a) {
        const double m = mn(w, h), t = A(a, 0, 0.12) * m, hw = A(a, 1, 0.22) * m, hl = A(a, 2, 0.22) * m;
        const double cx = w / 2, cy = h / 2;
        return poly({{cx, 0}, {cx + hw, hl}, {cx + t, hl}, {cx + t, cy - t}, {w - hl, cy - t}, {w - hl, cy - hw}, {w, cy}, {w - hl, cy + hw},
                     {w - hl, cy + t}, {cx + t, cy + t}, {cx + t, h - hl}, {cx + hw, h - hl}, {cx, h}, {cx - hw, h - hl}, {cx - t, h - hl},
                     {cx - t, cy + t}, {hl, cy + t}, {hl, cy + hw}, {0, cy}, {hl, cy - hw}, {hl, cy - t}, {cx - t, cy - t}, {cx - t, hl}, {cx - hw, hl}}); },
        {0.12, 0.22, 0.22}, {}, 0.3);
    add("bentArrow", "Bent Arrow", "Block Arrows", [](double w, double h, V a) {
        const double m = mn(w, h), t = A(a, 0, 0.25) * m, hd = A(a, 1, 0.25) * m * 2, hl = A(a, 2, 0.25) * m;
        QPainterPath p; p.moveTo(0, h); p.lineTo(0, hd / 2 + t); p.quadTo(0, hd / 2 - t / 2, t * 1.5, hd / 2 - t / 2);
        p.lineTo(w - hl, hd / 2 - t / 2); p.lineTo(w - hl, 0); p.lineTo(w, hd / 2); p.lineTo(w - hl, hd); p.lineTo(w - hl, hd / 2 + t / 2);
        p.lineTo(t * 1.5, hd / 2 + t / 2); p.quadTo(t, hd / 2 + t / 2, t, hd / 2 + t); p.lineTo(t, h); p.closeSubpath(); return p; },
        {0.25, 0.25, 0.25}, {}, 0.2);
    add("uturnArrow", "U-Turn Arrow", "Block Arrows", [](double w, double h, V a) {
        const double m = mn(w, h), t = A(a, 0, 0.25) * m, hl = A(a, 1, 0.25) * m;
        const double r = std::max(t, (w - hl * 1.5) / 2);
        QPainterPath p; p.moveTo(0, h); p.lineTo(0, r); p.arcTo(QRectF(0, 0, 2 * r, 2 * r), 180, -180);
        p.lineTo(2 * r, h - hl); p.lineTo(2 * r + hl * 0.75, h - hl); p.lineTo(2 * r - t / 2, h); p.lineTo(2 * r - t - hl * 0.75, h - hl);
        p.lineTo(2 * r - t, h - hl); p.lineTo(2 * r - t, r); p.arcTo(QRectF(t, t, 2 * r - 2 * t, 2 * r - 2 * t), 0, 180); p.lineTo(t, h);
        p.closeSubpath(); return p; }, {0.25, 0.25}, {}, 0.2);
    add("leftUpArrow", "Left-Up Arrow", "Block Arrows", [](double w, double h, V a) {
        const double m = mn(w, h), t = A(a, 0, 0.18) * m, hw = A(a, 1, 0.22) * m;
        return poly({{0, h - hw}, {hw, h - 2 * hw}, {hw, h - hw - t / 2}, {w - hw - t / 2, h - hw - t / 2}, {w - hw - t / 2, hw}, {w - 2 * hw, hw},
                     {w - hw, 0}, {w, hw}, {w - hw + t / 2, hw}, {w - hw + t / 2, h - hw + t / 2}, {hw, h - hw + t / 2}, {hw, h}}); },
        {0.18, 0.22}, {}, 0.25);
    add("bentUpArrow", "Bent-Up Arrow", "Block Arrows", [](double w, double h, V a) {
        const double m = mn(w, h), t = A(a, 0, 0.25) * m, hw = A(a, 1, 0.25) * m;
        return poly({{0, h - t}, {w - hw - t / 2, h - t}, {w - hw - t / 2, hw}, {w - 2 * hw, hw}, {w - hw, 0}, {w, hw}, {w - hw + t / 2, hw},
                     {w - hw + t / 2, h}, {0, h}}); }, {0.25, 0.25}, {}, 0.25);
    add("curvedRightArrow", "Curved Right Arrow", "Block Arrows", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w, h * 0.72); p.lineTo(w * 0.6, h * 0.45); p.lineTo(w * 0.62, h * 0.62);
        p.cubicTo(w * 0.2, h * 0.55, 0, h * 0.35, 0, 0); p.lineTo(0, h * 0.28); p.cubicTo(0, h * 0.62, w * 0.25, h * 0.88, w * 0.62, h * 0.9);
        p.lineTo(w * 0.6, h); p.closeSubpath(); return p; }, {}, {}, 0.3);
    add("curvedLeftArrow", "Curved Left Arrow", "Block Arrows", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, h * 0.72); p.lineTo(w * 0.4, h * 0.45); p.lineTo(w * 0.38, h * 0.62);
        p.cubicTo(w * 0.8, h * 0.55, w, h * 0.35, w, 0); p.lineTo(w, h * 0.28); p.cubicTo(w, h * 0.62, w * 0.75, h * 0.88, w * 0.38, h * 0.9);
        p.lineTo(w * 0.4, h); p.closeSubpath(); return p; }, {}, {}, 0.3);
    add("curvedUpArrow", "Curved Up Arrow", "Block Arrows", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w * 0.72, 0); p.lineTo(w * 0.45, h * 0.4); p.lineTo(w * 0.62, h * 0.38);
        p.cubicTo(w * 0.55, h * 0.8, w * 0.35, h, 0, h); p.lineTo(w * 0.28, h); p.cubicTo(w * 0.62, h, w * 0.88, h * 0.75, w * 0.9, h * 0.38);
        p.lineTo(w, h * 0.4); p.closeSubpath(); return p; }, {}, {}, 0.3);
    add("curvedDownArrow", "Curved Down Arrow", "Block Arrows", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w * 0.72, h); p.lineTo(w * 0.45, h * 0.6); p.lineTo(w * 0.62, h * 0.62);
        p.cubicTo(w * 0.55, h * 0.2, w * 0.35, 0, 0, 0); p.lineTo(w * 0.28, 0); p.cubicTo(w * 0.62, 0, w * 0.88, h * 0.25, w * 0.9, h * 0.62);
        p.lineTo(w, h * 0.6); p.closeSubpath(); return p; }, {}, {}, 0.3);
    add("stripedRightArrow", "Striped Right Arrow", "Block Arrows", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5), y0 = h * (0.5 - t / 2), y1 = h * (0.5 + t / 2), s = w * 0.04;
        QPainterPath p = arrowRight(w, h, t, A(a, 1, 0.5)).subtracted([&] { QPainterPath c; c.addRect(0, 0, s * 4.5, h); return c; }());
        p.addRect(0, y0, s, y1 - y0); p.addRect(s * 2, y0, s, y1 - y0); return p; }, {0.5, 0.5}, {}, 0.2);
    add("notchedRightArrow", "Notched Right Arrow", "Block Arrows", [](double w, double h, V a) {
        const double t = A(a, 0, 0.5), hd = std::min(w, A(a, 1, 0.5) * mn(w, h));
        const double y0 = h * (0.5 - t / 2), y1 = h * (0.5 + t / 2), n = (y1 - y0) / 2;
        return poly({{0, y0}, {w - hd, y0}, {w - hd, 0}, {w, h / 2}, {w - hd, h}, {w - hd, y1}, {0, y1}, {n, h / 2}}); }, {0.5, 0.5}, {}, 0.2);
    add("homePlate", "Pentagon Arrow", "Block Arrows", [](double w, double h, V a) {
        const double s = std::min(w, A(a, 0, 0.5) * mn(w, h)); return poly({{0, 0}, {w - s, 0}, {w, h / 2}, {w - s, h}, {0, h}}); },
        {0.5}, {{0, 0, 2, true, 0, 0, 1}}, 0.08);
    add("chevron", "Chevron", "Block Arrows", [](double w, double h, V a) {
        const double s = std::min(w / 2, A(a, 0, 0.5) * mn(w, h)); return poly({{0, 0}, {w - s, 0}, {w, h / 2}, {w - s, h}, {0, h}, {s, h / 2}}); },
        {0.5}, {{0, 0, 2, true, 0, 0, 1}}, 0.2);
    add("rightArrowCallout", "Right Arrow Callout", "Block Arrows", [](double w, double h, V) {
        return poly({{0, 0}, {w * 0.65, 0}, {w * 0.65, h * 0.38}, {w * 0.8, h * 0.38}, {w * 0.8, h * 0.25}, {w, h / 2}, {w * 0.8, h * 0.75},
                     {w * 0.8, h * 0.62}, {w * 0.65, h * 0.62}, {w * 0.65, h}, {0, h}}); }, {}, {}, 0,
        [](double w, double h, V) { return QRectF(w * 0.03, h * 0.05, w * 0.6, h * 0.9); });
    add("downArrowCallout", "Down Arrow Callout", "Block Arrows", [](double w, double h, V) {
        return poly({{0, 0}, {w, 0}, {w, h * 0.65}, {w * 0.62, h * 0.65}, {w * 0.62, h * 0.8}, {w * 0.75, h * 0.8}, {w / 2, h}, {w * 0.25, h * 0.8},
                     {w * 0.38, h * 0.8}, {w * 0.38, h * 0.65}, {0, h * 0.65}}); }, {}, {}, 0,
        [](double w, double h, V) { return QRectF(w * 0.05, h * 0.03, w * 0.9, h * 0.6); });
    add("circularArrow", "Circular Arrow", "Block Arrows", [](double w, double h, V a) {
        const double t = A(a, 0, 0.125) * mn(w, h);
        QPainterPath p; p.arcMoveTo(QRectF(0, 0, w, h), 180); p.arcTo(QRectF(0, 0, w, h), 180, -150);
        const QPointF tip(w / 2 + std::cos(qDegreesToRadians(-30.0)) * (w / 2 - t / 2), h / 2 + std::sin(qDegreesToRadians(-30.0)) * (h / 2 - t / 2));
        p.lineTo(tip.x() + t, tip.y()); p.lineTo(tip.x(), tip.y() + t * 2.2); p.lineTo(tip.x() - t * 2, tip.y() + t * 0.2);
        p.arcTo(QRectF(t, t, w - 2 * t, h - 2 * t), 30, 150); p.closeSubpath(); return p; }, {0.125}, {}, 0.3);

    // ---------------- Equation Shapes ----------------
    add("mathPlus", "Plus", "Equation Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.235) * mn(w, h), cx = w / 2, cy = h / 2, ex = w * 0.38, ey = h * 0.38;
        return poly({{cx - t / 2, cy - ey}, {cx + t / 2, cy - ey}, {cx + t / 2, cy - t / 2}, {cx + ex, cy - t / 2}, {cx + ex, cy + t / 2},
                     {cx + t / 2, cy + t / 2}, {cx + t / 2, cy + ey}, {cx - t / 2, cy + ey}, {cx - t / 2, cy + t / 2}, {cx - ex, cy + t / 2},
                     {cx - ex, cy - t / 2}, {cx - t / 2, cy - t / 2}}); }, {0.235}, {}, 0.3);
    add("mathMinus", "Minus", "Equation Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.235) * h; QPainterPath p; p.addRect(w * 0.12, h / 2 - t / 2, w * 0.76, t); return p; }, {0.235}, {}, 0.3);
    add("mathMultiply", "Multiply", "Equation Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.235) * mn(w, h); QPainterPath bar; bar.addRect(-w * 0.4, -t / 2, w * 0.8, t);
        QTransform r1; r1.translate(w / 2, h / 2); r1.rotate(45);
        QTransform r2; r2.translate(w / 2, h / 2); r2.rotate(-45);
        return r1.map(bar).united(r2.map(bar)); }, {0.235}, {}, 0.3);
    add("mathDivide", "Division", "Equation Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.235) * h * 0.6; QPainterPath p; p.addRect(w * 0.12, h / 2 - t / 2, w * 0.76, t);
        const double d = t * 1.1; p.addEllipse(QRectF(w / 2 - d / 2, h / 2 - t / 2 - d * 1.6, d, d)); p.addEllipse(QRectF(w / 2 - d / 2, h / 2 + t / 2 + d * 0.6, d, d));
        return p; }, {0.235}, {}, 0.3);
    add("mathEqual", "Equal", "Equation Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.235) * h * 0.8, g = t * 0.6; QPainterPath p;
        p.addRect(w * 0.12, h / 2 - g / 2 - t, w * 0.76, t); p.addRect(w * 0.12, h / 2 + g / 2, w * 0.76, t); return p; }, {0.235}, {}, 0.3);
    add("mathNotEqual", "Not Equal", "Equation Shapes", [](double w, double h, V a) {
        const double t = A(a, 0, 0.235) * h * 0.8, g = t * 0.6; QPainterPath p;
        p.addRect(w * 0.12, h / 2 - g / 2 - t, w * 0.76, t); p.addRect(w * 0.12, h / 2 + g / 2, w * 0.76, t);
        QPainterPath s; s.addRect(-t / 2, -h * 0.42, t, h * 0.84); QTransform r; r.translate(w / 2, h / 2); r.rotate(20);
        return p.united(r.map(s)); }, {0.235}, {}, 0.3);

    // ---------------- Flowchart ----------------
    add("fcProcess", "Flowchart: Process", "Flowchart", [](double w, double h, V) { QPainterPath p; p.addRect(0, 0, w, h); return p; }, {}, {}, 0.05);
    add("fcAltProcess", "Flowchart: Alternate Process", "Flowchart", [](double w, double h, V) { return roundRect(w, h, mn(w, h) * 0.17); }, {}, {}, 0.06);
    add("fcDecision", "Flowchart: Decision", "Flowchart", [](double w, double h, V) { return poly({{w / 2, 0}, {w, h / 2}, {w / 2, h}, {0, h / 2}}); }, {}, {}, 0.25);
    add("fcData", "Flowchart: Data", "Flowchart", [](double w, double h, V) { return poly({{w * 0.2, 0}, {w, 0}, {w * 0.8, h}, {0, h}}); }, {}, {}, 0.2);
    add("fcPredefined", "Flowchart: Predefined Process", "Flowchart", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h); p.moveTo(w * 0.125, 0); p.lineTo(w * 0.125, h); p.moveTo(w * 0.875, 0); p.lineTo(w * 0.875, h); return p; },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.13, 0, w * 0.74, h); });
    add("fcInternalStorage", "Flowchart: Internal Storage", "Flowchart", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h); p.moveTo(w * 0.15, 0); p.lineTo(w * 0.15, h); p.moveTo(0, h * 0.15); p.lineTo(w, h * 0.15); return p; },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.15, h * 0.15, w * 0.85, h * 0.85); });
    add("fcDocument", "Flowchart: Document", "Flowchart", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, 0); p.lineTo(w, 0); p.lineTo(w, h * 0.8); p.cubicTo(w * 0.75, h * 0.62, w * 0.5, h * 0.95, w * 0.25, h * 1.0);
        p.cubicTo(w * 0.12, h * 1.02, w * 0.04, h * 0.95, 0, h * 0.9); p.closeSubpath(); return p; }, {}, {}, 0,
        [](double w, double h, V) { return QRectF(0, 0, w, h * 0.78); });
    add("fcMultidocument", "Flowchart: Multidocument", "Flowchart", [](double w, double h, V) {
        QPainterPath p;
        for (int i = 2; i >= 0; --i) {
            const double ox = w * 0.06 * i, oy = h * 0.08 * (2 - i), ww = w * 0.88, hh = h * 0.84;
            QPainterPath d; d.moveTo(ox, oy); d.lineTo(ox + ww, oy); d.lineTo(ox + ww, oy + hh * 0.8);
            d.cubicTo(ox + ww * 0.75, oy + hh * 0.62, ox + ww * 0.5, oy + hh * 0.95, ox + ww * 0.25, oy + hh);
            d.cubicTo(ox + ww * 0.12, oy + hh * 1.02, ox + ww * 0.04, oy + hh * 0.95, ox, oy + hh * 0.9); d.closeSubpath();
            p = p.united(d);
        }
        return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(0, h * 0.2, w * 0.86, h * 0.6); });
    add("fcTerminator", "Flowchart: Terminator", "Flowchart", [](double w, double h, V) { return roundRect(w, h, h / 2); }, {}, {}, 0.1);
    add("fcPreparation", "Flowchart: Preparation", "Flowchart", [](double w, double h, V) {
        return poly({{w * 0.2, 0}, {w * 0.8, 0}, {w, h / 2}, {w * 0.8, h}, {w * 0.2, h}, {0, h / 2}}); }, {}, {}, 0.18);
    add("fcManualInput", "Flowchart: Manual Input", "Flowchart", [](double w, double h, V) { return poly({{0, h * 0.2}, {w, 0}, {w, h}, {0, h}}); },
        {}, {}, 0, [](double w, double h, V) { return QRectF(0, h * 0.2, w, h * 0.8); });
    add("fcManualOperation", "Flowchart: Manual Operation", "Flowchart", [](double w, double h, V) { return poly({{0, 0}, {w, 0}, {w * 0.8, h}, {w * 0.2, h}}); },
        {}, {}, 0.18);
    add("fcConnector", "Flowchart: Connector", "Flowchart", [](double w, double h, V) { return ellipse(w, h); }, {}, {}, 0.15);
    add("fcOffpage", "Flowchart: Off-page Connector", "Flowchart", [](double w, double h, V) {
        return poly({{0, 0}, {w, 0}, {w, h * 0.8}, {w / 2, h}, {0, h * 0.8}}); }, {}, {}, 0, [](double w, double h, V) { return QRectF(0, 0, w, h * 0.8); });
    add("fcCard", "Flowchart: Card", "Flowchart", [](double w, double h, V) { return poly({{w * 0.2, 0}, {w, 0}, {w, h}, {0, h}, {0, h * 0.2}}); },
        {}, {}, 0, [](double w, double h, V) { return QRectF(0, h * 0.2, w, h * 0.8); });
    add("fcPunchedTape", "Flowchart: Punched Tape", "Flowchart", [](double w, double h, V) { return wavePath(w, h, 0.1, 1); }, {}, {}, 0.2);
    add("fcSummingJunction", "Flowchart: Summing Junction", "Flowchart", [](double w, double h, V) {
        QPainterPath p = ellipse(w, h); const double k = 0.3536;
        p.moveTo(w * (0.5 - k), h * (0.5 - k)); p.lineTo(w * (0.5 + k), h * (0.5 + k)); p.moveTo(w * (0.5 + k), h * (0.5 - k)); p.lineTo(w * (0.5 - k), h * (0.5 + k));
        return p; }, {}, {}, 0.15);
    add("fcOr", "Flowchart: Or", "Flowchart", [](double w, double h, V) {
        QPainterPath p = ellipse(w, h); p.moveTo(w / 2, 0); p.lineTo(w / 2, h); p.moveTo(0, h / 2); p.lineTo(w, h / 2); return p; }, {}, {}, 0.15);
    add("fcCollate", "Flowchart: Collate", "Flowchart", [](double w, double h, V) {
        QPainterPath p = poly({{0, 0}, {w, 0}, {w / 2, h / 2}}); p.addPolygon(QPolygonF({QPointF(w / 2, h / 2), QPointF(w, h), QPointF(0, h), QPointF(w / 2, h / 2)}));
        return p; }, {}, {}, 0.25);
    add("fcSort", "Flowchart: Sort", "Flowchart", [](double w, double h, V) {
        QPainterPath p = poly({{w / 2, 0}, {w, h / 2}, {w / 2, h}, {0, h / 2}}); p.moveTo(0, h / 2); p.lineTo(w, h / 2); return p; }, {}, {}, 0.25);
    add("fcExtract", "Flowchart: Extract", "Flowchart", [](double w, double h, V) { return poly({{w / 2, 0}, {w, h}, {0, h}}); },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.25, h * 0.5, w * 0.5, h * 0.5); });
    add("fcMerge", "Flowchart: Merge", "Flowchart", [](double w, double h, V) { return poly({{0, 0}, {w, 0}, {w / 2, h}}); },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.25, 0, w * 0.5, h * 0.5); });
    add("fcStoredData", "Flowchart: Stored Data", "Flowchart", [](double w, double h, V) {
        QPainterPath p; p.moveTo(w * 0.17, 0); p.lineTo(w, 0); p.quadTo(w * 0.83, h / 2, w, h); p.lineTo(w * 0.17, h); p.quadTo(-w * 0.17, h / 2, w * 0.17, 0);
        p.closeSubpath(); return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.17, 0, w * 0.66, h); });
    add("fcDelay", "Flowchart: Delay", "Flowchart", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, 0); p.lineTo(w / 2, 0); p.arcTo(QRectF(0, 0, w, h), 90, -180); p.lineTo(0, h); p.closeSubpath(); return p; },
        {}, {}, 0, [](double w, double h, V) { return QRectF(0, h * 0.15, w * 0.85, h * 0.7); });
    add("fcSequential", "Flowchart: Sequential Access Storage", "Flowchart", [](double w, double h, V) {
        QPainterPath p = ellipse(w, h); p.moveTo(w / 2, h); p.lineTo(w, h); p.lineTo(w, h * 0.85); return p; }, {}, {}, 0.15);
    add("fcMagneticDisk", "Flowchart: Magnetic Disk", "Flowchart", [](double w, double h, V) {
        const double e = h * 0.25; QPainterPath p; p.moveTo(0, e / 2); p.arcTo(QRectF(0, 0, w, e), 180, -180); p.lineTo(w, h - e / 2);
        p.arcTo(QRectF(0, h - e, w, e), 0, -180); p.closeSubpath(); p.moveTo(0, e / 2); p.arcTo(QRectF(0, 0, w, e), 180, 180); return p; },
        {}, {}, 0, [](double w, double h, V) { return QRectF(0, h * 0.25, w, h * 0.62); });
    add("fcDirectAccess", "Flowchart: Direct Access Storage", "Flowchart", [](double w, double h, V) {
        const double e = w * 0.25; QPainterPath p; p.moveTo(e / 2, 0); p.lineTo(w - e / 2, 0); p.arcTo(QRectF(w - e, 0, e, h), 90, -180);
        p.lineTo(e / 2, h); p.arcTo(QRectF(0, 0, e, h), 270, -180); p.closeSubpath(); p.moveTo(w - e / 2, 0); p.arcTo(QRectF(w - e, 0, e, h), 90, 180); return p; },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.12, 0, w * 0.62, h); });
    add("fcDisplay", "Flowchart: Display", "Flowchart", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, h / 2); p.lineTo(w * 0.17, 0); p.lineTo(w * 0.83, 0); p.quadTo(w * 1.08, h / 2, w * 0.83, h); p.lineTo(w * 0.17, h);
        p.closeSubpath(); return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.17, 0, w * 0.66, h); });

    // ---------------- Stars and Banners ----------------
    add("irregularSeal1", "Explosion 1", "Stars and Banners", [](double w, double h, V) {
        static const double pts[][2] = {{0.42, 0.25}, {0.56, 0}, {0.6, 0.24}, {0.82, 0.12}, {0.74, 0.32}, {1, 0.36}, {0.78, 0.48}, {0.95, 0.68},
                                        {0.7, 0.62}, {0.72, 0.9}, {0.55, 0.7}, {0.46, 1}, {0.38, 0.72}, {0.18, 0.86}, {0.24, 0.62}, {0, 0.6},
                                        {0.18, 0.46}, {0.02, 0.28}, {0.26, 0.32}, {0.2, 0.06}};
        QVector<QPointF> v; for (const auto &p : pts) v << QPointF(p[0] * w, p[1] * h); return polyV(v); }, {}, {}, 0.3);
    add("irregularSeal2", "Explosion 2", "Stars and Banners", [](double w, double h, V) {
        static const double pts[][2] = {{0.5, 0.18}, {0.62, 0}, {0.66, 0.2}, {0.86, 0.06}, {0.8, 0.28}, {1, 0.32}, {0.84, 0.44}, {0.98, 0.58},
                                        {0.8, 0.6}, {0.9, 0.82}, {0.68, 0.72}, {0.64, 0.96}, {0.5, 0.76}, {0.38, 1}, {0.32, 0.76}, {0.1, 0.88},
                                        {0.18, 0.64}, {0, 0.56}, {0.16, 0.44}, {0.04, 0.22}, {0.28, 0.28}, {0.3, 0.04}};
        QVector<QPointF> v; for (const auto &p : pts) v << QPointF(p[0] * w, p[1] * h); return polyV(v); }, {}, {}, 0.3);
    add("star4", "4-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 4, A(a, 0, 0.25) * 2); }, {0.19}, {}, 0.3);
    add("star5", "5-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 5, A(a, 0, 0.38) * 1.0); }, {0.38}, {}, 0.32);
    add("star6", "6-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 6, A(a, 0, 0.58)); }, {0.58}, {}, 0.25);
    add("star7", "7-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 7, A(a, 0, 0.6)); }, {0.6}, {}, 0.25);
    add("star8", "8-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 8, A(a, 0, 0.72)); }, {0.72}, {}, 0.22);
    add("star10", "10-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 10, A(a, 0, 0.76)); }, {0.76}, {}, 0.2);
    add("star12", "12-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 12, A(a, 0, 0.78)); }, {0.78}, {}, 0.2);
    add("star16", "16-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 16, A(a, 0, 0.82)); }, {0.82}, {}, 0.18);
    add("star24", "24-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 24, A(a, 0, 0.86)); }, {0.86}, {}, 0.16);
    add("star32", "32-Point Star", "Stars and Banners", [](double w, double h, V a) { return starPath(w, h, 32, A(a, 0, 0.88)); }, {0.88}, {}, 0.15);
    add("ribbon2", "Up Ribbon", "Stars and Banners", [](double w, double h, V) {
        QPainterPath p = poly({{0, h * 0.2}, {w * 0.12, h * 0.2}, {w * 0.12, 0}, {w * 0.88, 0}, {w * 0.88, h * 0.2}, {w, h * 0.2}, {w * 0.93, h * 0.55},
                               {w, h * 0.9}, {w * 0.88, h * 0.9}, {w * 0.88, h * 0.7}, {w * 0.12, h * 0.7}, {w * 0.12, h * 0.9}, {0, h * 0.9}, {w * 0.07, h * 0.55}});
        return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.14, h * 0.03, w * 0.72, h * 0.64); });
    add("ribbon", "Down Ribbon", "Stars and Banners", [](double w, double h, V) {
        return poly({{0, h * 0.1}, {w * 0.12, h * 0.1}, {w * 0.12, h * 0.3}, {w * 0.88, h * 0.3}, {w * 0.88, h * 0.1}, {w, h * 0.1}, {w * 0.93, h * 0.45},
                     {w, h * 0.8}, {w * 0.88, h * 0.8}, {w * 0.88, h}, {w * 0.12, h}, {w * 0.12, h * 0.8}, {0, h * 0.8}, {w * 0.07, h * 0.45}}); },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.14, h * 0.33, w * 0.72, h * 0.64); });
    add("ellipseRibbon2", "Curved Up Ribbon", "Stars and Banners", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, h * 0.3); p.lineTo(w * 0.12, h * 0.3); p.lineTo(w * 0.12, h * 0.1); p.quadTo(w / 2, -h * 0.08, w * 0.88, h * 0.1);
        p.lineTo(w * 0.88, h * 0.3); p.lineTo(w, h * 0.3); p.lineTo(w * 0.93, h * 0.62); p.lineTo(w, h * 0.95); p.lineTo(w * 0.88, h * 0.95);
        p.lineTo(w * 0.88, h * 0.75); p.quadTo(w / 2, h * 0.57, w * 0.12, h * 0.75); p.lineTo(w * 0.12, h * 0.95); p.lineTo(0, h * 0.95);
        p.lineTo(w * 0.07, h * 0.62); p.closeSubpath(); return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.14, h * 0.12, w * 0.72, h * 0.5); });
    add("ellipseRibbon", "Curved Down Ribbon", "Stars and Banners", [](double w, double h, V) {
        QPainterPath p; p.moveTo(0, h * 0.05); p.lineTo(w * 0.12, h * 0.05); p.lineTo(w * 0.12, h * 0.25); p.quadTo(w / 2, h * 0.43, w * 0.88, h * 0.25);
        p.lineTo(w * 0.88, h * 0.05); p.lineTo(w, h * 0.05); p.lineTo(w * 0.93, h * 0.38); p.lineTo(w, h * 0.7); p.lineTo(w * 0.88, h * 0.7);
        p.lineTo(w * 0.88, h * 0.9); p.quadTo(w / 2, h * 1.08, w * 0.12, h * 0.9); p.lineTo(w * 0.12, h * 0.7); p.lineTo(0, h * 0.7);
        p.lineTo(w * 0.07, h * 0.38); p.closeSubpath(); return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.14, h * 0.4, w * 0.72, h * 0.5); });
    add("verticalScroll", "Vertical Scroll", "Stars and Banners", [](double w, double h, V a) {
        const double r = A(a, 0, 0.125) * mn(w, h); QPainterPath p;
        p.addRoundedRect(QRectF(r, r, w - 2 * r, h - 2 * r), 2, 2);
        p.addEllipse(QRectF(r, 0, w - r, 2 * r)); p.addEllipse(QRectF(0, h - 2 * r, w - r, 2 * r)); return p.simplified(); },
        {0.125}, {}, 0.2);
    add("horizontalScroll", "Horizontal Scroll", "Stars and Banners", [](double w, double h, V a) {
        const double r = A(a, 0, 0.125) * mn(w, h); QPainterPath p;
        p.addRoundedRect(QRectF(r, r, w - 2 * r, h - 2 * r), 2, 2);
        p.addEllipse(QRectF(0, r, 2 * r, h - r)); p.addEllipse(QRectF(w - 2 * r, 0, 2 * r, h - r)); return p.simplified(); },
        {0.125}, {}, 0.2);
    add("wave", "Wave", "Stars and Banners", [](double w, double h, V a) { return wavePath(w, h, A(a, 0, 0.125), 1); }, {0.125}, {}, 0.2);
    add("doubleWave", "Double Wave", "Stars and Banners", [](double w, double h, V a) { return wavePath(w, h, A(a, 0, 0.0625), 2); }, {0.0625}, {}, 0.15);

    // ---------------- Callouts ----------------
    add("wedgeRectCallout", "Rectangular Callout", "Callouts", [](double w, double h, V a) {
        QPainterPath b; b.addRect(0, 0, w, h * 0.78);
        return calloutTail(b, w, h, A(a, 0, 0.2), A(a, 1, 1.0), w * 0.3, h * 0.78 - 0.5, w * 0.08); }, {0.2, 1.0}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.04, h * 0.04, w * 0.92, h * 0.7); });
    add("wedgeRoundRectCallout", "Rounded Rectangular Callout", "Callouts", [](double w, double h, V a) {
        QPainterPath b = roundRect(w, h * 0.78, mn(w, h * 0.78) * 0.16);
        return calloutTail(b, w, h, A(a, 0, 0.2), A(a, 1, 1.0), w * 0.3, h * 0.78 - 1, w * 0.08); }, {0.2, 1.0}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.06, h * 0.05, w * 0.88, h * 0.68); });
    add("wedgeEllipseCallout", "Oval Callout", "Callouts", [](double w, double h, V a) {
        QPainterPath b = ellipse(w, h * 0.8);
        return calloutTail(b, w, h, A(a, 0, 0.18), A(a, 1, 1.0), w * 0.32, h * 0.66, w * 0.08); }, {0.18, 1.0}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.15, h * 0.12, w * 0.7, h * 0.56); });
    add("cloudCallout", "Cloud Callout", "Callouts", [](double w, double h, V) {
        QPainterPath p;
        const QRectF c[] = {{0.05, 0.25, 0.35, 0.35}, {0.2, 0.05, 0.35, 0.35}, {0.45, 0.03, 0.35, 0.37}, {0.62, 0.2, 0.36, 0.38},
                            {0.45, 0.42, 0.38, 0.36}, {0.15, 0.42, 0.38, 0.36}, {0.0, 0.35, 0.3, 0.28}};
        for (const QRectF &r : c) { QPainterPath e; e.addEllipse(QRectF(r.x() * w, r.y() * h, r.width() * w, r.height() * h)); p = p.united(e); }
        p.addEllipse(QRectF(w * 0.18, h * 0.82, w * 0.08, h * 0.07)); p.addEllipse(QRectF(w * 0.1, h * 0.92, w * 0.05, h * 0.05)); return p; },
        {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.18, h * 0.15, w * 0.64, h * 0.52); });
    add("borderCallout1", "Line Callout 1", "Callouts", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h * 0.7); p.moveTo(w * 0.1, h * 0.7); p.lineTo(-w * 0.05, h); return p; }, {}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.04, h * 0.04, w * 0.92, h * 0.62); });
    add("borderCallout2", "Line Callout 2", "Callouts", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h * 0.7); p.moveTo(0, h * 0.35); p.lineTo(-w * 0.1, h * 0.35); p.lineTo(-w * 0.2, h); return p; }, {}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.04, h * 0.04, w * 0.92, h * 0.62); });
    add("accentCallout1", "Line Callout 1 (Accent Bar)", "Callouts", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h * 0.7); p.moveTo(-w * 0.04, 0); p.lineTo(-w * 0.04, h * 0.7); p.moveTo(-w * 0.04, h * 0.35);
        p.lineTo(-w * 0.2, h); return p; }, {}, {}, 0, [](double w, double h, V) { return QRectF(w * 0.04, h * 0.04, w * 0.92, h * 0.62); });
    add("callout1", "Line Callout 1 (No Border)", "Callouts", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h * 0.7); p.moveTo(w * 0.1, h * 0.7); p.lineTo(-w * 0.05, h); return p; }, {}, {},
        0, [](double w, double h, V) { return QRectF(w * 0.04, h * 0.04, w * 0.92, h * 0.62); });

    // ---------------- Action / misc ----------------
    add("actionButtonBlank", "Action Button: Blank", "Action Buttons", [](double w, double h, V) {
        QPainterPath p; p.addRect(0, 0, w, h); const double d = mn(w, h) * 0.08; p.addRect(d, d, w - 2 * d, h - 2 * d); return p; }, {}, {}, 0.1);
    add("tab", "Tab", "Basic Shapes", [](double w, double h, V) { return poly({{w * 0.15, 0}, {w * 0.85, 0}, {w, h}, {0, h}}); }, {}, {}, 0.18);
    add("flowArrow", "Flow Arrow", "Block Arrows", [](double w, double h, V) {
        return poly({{0, h * 0.2}, {w * 0.6, h * 0.2}, {w * 0.6, 0}, {w, h / 2}, {w * 0.6, h}, {w * 0.6, h * 0.8}, {0, h * 0.8}, {w * 0.15, h / 2}}); }, {}, {}, 0.2);
    return L;
}

const QVector<ShapeDef> &shapeLibrary()
{
    static const QVector<ShapeDef> lib = build();
    return lib;
}

const ShapeDef *shapeDef(const QString &id)
{
    for (const auto &d : shapeLibrary())
        if (d.id == id) return &d;
    return nullptr;
}

QStringList shapeCategories()
{
    QStringList out;
    for (const auto &d : shapeLibrary())
        if (!out.contains(d.category)) out << d.category;
    return out;
}

QVector<double> shapeAdj(const QString &id, const QVector<double> &adj)
{
    const ShapeDef *d = shapeDef(id);
    QVector<double> out = d ? d->defaults : QVector<double>();
    for (int i = 0; i < adj.size() && i < out.size(); ++i) out[i] = adj[i];
    return out;
}

QPainterPath shapePath(const QString &id, const QSizeF &size, const QVector<double> &adj)
{
    const ShapeDef *d = shapeDef(id);
    if (!d) d = &shapeLibrary().front();
    const double w = std::max(0.5, size.width()), h = std::max(0.5, size.height());
    QPainterPath p = d->path(w, h, shapeAdj(d->id, adj));
    // Shapes with holes need even-odd; unions of overlapping parts need winding.
    static const QStringList holes = {"frame", "donut", "noSmoking", "bevel", "halfFrame"};
    p.setFillRule(holes.contains(d->id) ? Qt::OddEvenFill : Qt::WindingFill);
    return p;
}

QRectF shapeTextRect(const QString &id, const QSizeF &size, const QVector<double> &adj)
{
    const ShapeDef *d = shapeDef(id);
    const double w = size.width(), h = size.height();
    if (!d) return QRectF(0, 0, w, h);
    if (d->textRect) return d->textRect(w, h, shapeAdj(id, adj));
    return insetRect(w, h, d->inset, d->inset);
}

QPointF adjHandlePos(const AdjHandle &hd, const QSizeF &s, const QVector<double> &adj)
{
    const double basis = hd.basis == 0 ? s.width() : hd.basis == 1 ? s.height() : std::min(s.width(), s.height());
    const double v = adj.value(hd.index) * basis;
    if (hd.axis == 0) return QPointF(hd.fromEnd ? s.width() - v : v, hd.other * s.height());
    return QPointF(hd.other * s.width(), hd.fromEnd ? s.height() - v : v);
}

double adjFromPoint(const AdjHandle &hd, const QSizeF &s, const QPointF &p)
{
    const double basis = std::max(0.01, hd.basis == 0 ? s.width() : hd.basis == 1 ? s.height() : std::min(s.width(), s.height()));
    double v = hd.axis == 0 ? p.x() : p.y();
    if (hd.fromEnd) v = (hd.axis == 0 ? s.width() : s.height()) - v;
    return std::clamp(v / basis, hd.min, hd.max);
}

} // namespace jp
