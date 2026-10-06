#include "core/style.h"

#include <QImage>
#include <QJsonArray>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

namespace jp {

static QColor withTransparency(QColor c, double t)
{
    c.setAlphaF(float(c.alphaF() * (1.0 - qBound(0.0, t, 1.0))));
    return c;
}

QVector<Qt::BrushStyle> patternBrushes()
{
    return {Qt::Dense1Pattern, Qt::Dense2Pattern, Qt::Dense3Pattern, Qt::Dense4Pattern, Qt::Dense5Pattern, Qt::Dense6Pattern,
            Qt::Dense7Pattern, Qt::HorPattern, Qt::VerPattern, Qt::CrossPattern, Qt::BDiagPattern, Qt::FDiagPattern, Qt::DiagCrossPattern};
}

QString dashName(Stroke::Dash d)
{
    static const char *n[] = {"Solid", "Round Dot", "Square Dot", "Dash", "Dash Dot", "Long Dash", "Long Dash Dot", "Long Dash Dot Dot"};
    return QString::fromLatin1(n[int(d)]);
}

QImage Fill::patternTile(const ColorScheme &s) const
{
    QImage tile(8, 8, QImage::Format_ARGB32_Premultiplied);
    tile.fill(color2.isNone() ? QColor(Qt::transparent) : withTransparency(color2.resolve(s), transparency));
    QPainter p(&tile);
    p.fillRect(tile.rect(), QBrush(withTransparency(color.resolve(s), transparency), patternBrushes().value(pattern, Qt::Dense4Pattern)));
    return tile;
}

QBrush Fill::brush(const QRectF &r, const ColorScheme &s, const ImageLookup &img) const
{
    switch (type) {
    case NoFill: return Qt::NoBrush;
    case Solid: return color.isNone() ? QBrush(Qt::NoBrush) : QBrush(withTransparency(color.resolve(s), transparency));
    case Gradient: {
        QGradientStops gs;
        if (stops.isEmpty()) {
            gs << QGradientStop(0, withTransparency(color.resolve(s), transparency))
               << QGradientStop(1, withTransparency(color2.isNone() ? QColor(Qt::white) : color2.resolve(s), transparency));
        } else {
            for (const auto &st : stops) gs << QGradientStop(st.pos, withTransparency(st.color.resolve(s), st.transparency));
        }
        if (gradType == Linear) {
            // Angle measured clockwise from "left to right"; 90 = top to bottom.
            const double a = qDegreesToRadians(angle);
            const QPointF c = r.center();
            const double half = (std::abs(std::cos(a)) * r.width() + std::abs(std::sin(a)) * r.height()) / 2;
            QLinearGradient g(c - QPointF(std::cos(a), std::sin(a)) * half, c + QPointF(std::cos(a), std::sin(a)) * half);
            g.setStops(gs);
            return QBrush(g);
        }
        const double rad = std::hypot(r.width(), r.height()) / 2;
        QRadialGradient g(r.center(), gradType == Rectangular ? std::max(r.width(), r.height()) / 2 * 1.15 : rad);
        g.setStops(gs);
        return QBrush(g);
    }
    case Picture:
    case Texture: {
        if (!img || imageId.isEmpty()) return QBrush(withTransparency(QColor(220, 220, 220), transparency));
        QImage im = img(imageId);
        if (im.isNull()) return Qt::NoBrush;
        QBrush b;
        QTransform t;
        if (type == Texture || tile) {
            b = QBrush(im);
            t.translate(r.left(), r.top());
            t.scale(tileScale * 72.0 / 96.0, tileScale * 72.0 / 96.0);
        } else {
            b = QBrush(im);
            t.translate(r.left(), r.top());
            t.scale(r.width() / im.width(), r.height() / im.height());
        }
        b.setTransform(t);
        return b;
    }
    case Pattern: {
        // An 8-pixel tile at 96 per inch, so the pattern keeps its size
        // when zoomed and printed.
        QBrush b(patternTile(s));
        QTransform t;
        t.translate(r.left(), r.top());
        t.scale(72.0 / 96.0, 72.0 / 96.0);
        b.setTransform(t);
        return b;
    }
    }
    return Qt::NoBrush;
}

QJsonObject Fill::toJson() const
{
    QJsonObject o;
    static const char *types[] = {"none", "solid", "gradient", "picture", "texture", "pattern"};
    o["type"] = QString::fromLatin1(types[type]);
    if (type == NoFill) return o;
    o["color"] = color.toString();
    if (transparency) o["transparency"] = transparency;
    if (type == Gradient) {
        static const char *gt[] = {"linear", "radial", "rectangular", "path"};
        o["gradType"] = QString::fromLatin1(gt[gradType]);
        o["angle"] = angle;
        o["color2"] = color2.toString();
        if (!stops.isEmpty()) {
            QJsonArray a;
            for (const auto &s : stops) a.append(QJsonObject{{"pos", s.pos}, {"color", s.color.toString()}, {"t", s.transparency}});
            o["stops"] = a;
        }
    }
    if (type == Picture || type == Texture) {
        o["image"] = imageId;
        o["tile"] = tile;
        o["tileScale"] = tileScale;
    }
    if (type == Pattern) {
        o["pattern"] = pattern;
        o["color2"] = color2.toString();
    }
    return o;
}

Fill Fill::fromJson(const QJsonObject &o)
{
    Fill f;
    const QString t = o["type"].toString();
    static const QStringList types = {"none", "solid", "gradient", "picture", "texture", "pattern"};
    f.type = Type(qMax(0, types.indexOf(t)));
    f.color = ColorRef::fromString(o["color"].toString());
    f.transparency = o["transparency"].toDouble();
    static const QStringList gt = {"linear", "radial", "rectangular", "path"};
    f.gradType = GradType(qMax(0, gt.indexOf(o["gradType"].toString())));
    f.angle = o["angle"].toDouble(90);
    f.color2 = ColorRef::fromString(o["color2"].toString());
    for (const auto &v : o["stops"].toArray()) {
        const auto s = v.toObject();
        f.stops.push_back({s["pos"].toDouble(), ColorRef::fromString(s["color"].toString()), s["t"].toDouble()});
    }
    f.imageId = o["image"].toString();
    f.tile = o["tile"].toBool();
    f.tileScale = o["tileScale"].toDouble(1.0);
    f.pattern = o["pattern"].toInt();
    return f;
}

QPen Stroke::pen(const ColorScheme &s) const
{
    if (isNone()) return Qt::NoPen;
    QPen p(withTransparency(color.resolve(s), transparency), width);
    p.setJoinStyle(join);
    p.setCapStyle(dash == RoundDot ? Qt::RoundCap : cap);
    switch (dash) {
    case SolidLine: break;
    case RoundDot: p.setDashPattern({0.01, 2}); break;
    case SquareDot: p.setDashPattern({1, 1}); break;
    case DashLine: p.setDashPattern({4, 3}); break;
    case DashDot: p.setDashPattern({4, 3, 1, 3}); break;
    case LongDash: p.setDashPattern({8, 3}); break;
    case LongDashDot: p.setDashPattern({8, 3, 1, 3}); break;
    case LongDashDotDot: p.setDashPattern({8, 3, 1, 3, 1, 3}); break;
    }
    return p;
}

QJsonObject Stroke::toJson() const
{
    QJsonObject o{{"color", color.toString()}, {"width", width}};
    if (dash) o["dash"] = int(dash);
    if (compound) o["compound"] = int(compound);
    if (transparency) o["transparency"] = transparency;
    if (join != Qt::MiterJoin) o["join"] = int(join);
    if (cap != Qt::FlatCap) o["cap"] = int(cap);
    if (startArrow != Arrow::None) { o["startArrow"] = int(startArrow); o["startSize"] = startSize; }
    if (endArrow != Arrow::None) { o["endArrow"] = int(endArrow); o["endSize"] = endSize; }
    return o;
}

Stroke Stroke::fromJson(const QJsonObject &o)
{
    Stroke s;
    if (o.isEmpty()) return s;
    s.color = ColorRef::fromString(o["color"].toString());
    s.width = o["width"].toDouble(0.75);
    s.dash = Dash(o["dash"].toInt());
    s.compound = Compound(o["compound"].toInt());
    s.transparency = o["transparency"].toDouble();
    s.join = Qt::PenJoinStyle(o["join"].toInt(Qt::MiterJoin));
    s.cap = Qt::PenCapStyle(o["cap"].toInt(Qt::FlatCap));
    s.startArrow = Arrow(o["startArrow"].toInt());
    s.endArrow = Arrow(o["endArrow"].toInt());
    s.startSize = o["startSize"].toInt(1);
    s.endSize = o["endSize"].toInt(1);
    return s;
}

QPointF ShadowFx::offset() const
{
    const double a = qDegreesToRadians(angle);
    return QPointF(std::cos(a), std::sin(a)) * distance;
}

QJsonObject Effects::toJson() const
{
    QJsonObject o;
    if (shadow.on)
        o["shadow"] = QJsonObject{{"color", shadow.color.toString()}, {"t", shadow.transparency}, {"blur", shadow.blur},
                                  {"dist", shadow.distance}, {"angle", shadow.angle}, {"inner", shadow.inner}};
    if (glow.on) o["glow"] = QJsonObject{{"color", glow.color.toString()}, {"size", glow.size}, {"t", glow.transparency}};
    if (softEdge > 0) o["softEdge"] = softEdge;
    if (reflection.on)
        o["reflection"] = QJsonObject{{"t", reflection.transparency}, {"size", reflection.size}, {"dist", reflection.distance}, {"blur", reflection.blur}};
    if (bevel.type) o["bevel"] = QJsonObject{{"type", bevel.type}, {"w", bevel.width}, {"h", bevel.height}};
    if (rot3d.x || rot3d.y) o["rot3d"] = QJsonObject{{"x", rot3d.x}, {"y", rot3d.y}, {"p", rot3d.perspective}};
    return o;
}

Effects Effects::fromJson(const QJsonObject &o)
{
    Effects e;
    if (o.contains("shadow")) {
        const auto s = o["shadow"].toObject();
        e.shadow.on = true;
        e.shadow.color = ColorRef::fromString(s["color"].toString());
        e.shadow.transparency = s["t"].toDouble(0.6);
        e.shadow.blur = s["blur"].toDouble(4);
        e.shadow.distance = s["dist"].toDouble(3);
        e.shadow.angle = s["angle"].toDouble(45);
        e.shadow.inner = s["inner"].toBool();
    }
    if (o.contains("glow")) {
        const auto g = o["glow"].toObject();
        e.glow.on = true;
        e.glow.color = ColorRef::fromString(g["color"].toString());
        e.glow.size = g["size"].toDouble(6);
        e.glow.transparency = g["t"].toDouble(0.4);
    }
    e.softEdge = o["softEdge"].toDouble();
    if (o.contains("reflection")) {
        const auto r = o["reflection"].toObject();
        e.reflection.on = true;
        e.reflection.transparency = r["t"].toDouble(0.5);
        e.reflection.size = r["size"].toDouble(0.5);
        e.reflection.distance = r["dist"].toDouble();
        e.reflection.blur = r["blur"].toDouble(0.5);
    }
    if (o.contains("bevel")) {
        const auto b = o["bevel"].toObject();
        e.bevel.type = b["type"].toInt();
        e.bevel.width = b["w"].toDouble(6);
        e.bevel.height = b["h"].toDouble(6);
    }
    if (o.contains("rot3d")) {
        const auto r = o["rot3d"].toObject();
        e.rot3d.x = r["x"].toDouble();
        e.rot3d.y = r["y"].toDouble();
        e.rot3d.perspective = r["p"].toDouble();
    }
    return e;
}

QJsonObject Wrap::toJson() const
{
    QJsonObject o{{"mode", int(mode)}, {"side", int(side)}, {"t", top}, {"b", bottom}, {"l", left}, {"r", right}};
    if (!points.isEmpty()) {
        QJsonArray a;
        for (const auto &p : points) { a.append(p.x()); a.append(p.y()); }
        o["points"] = a;
    }
    return o;
}

Wrap Wrap::fromJson(const QJsonObject &o)
{
    Wrap w;
    if (o.isEmpty()) return w;
    w.mode = Mode(o["mode"].toInt(Square));
    w.side = Side(o["side"].toInt());
    w.top = o["t"].toDouble(2.88);
    w.bottom = o["b"].toDouble(2.88);
    w.left = o["l"].toDouble(2.88);
    w.right = o["r"].toDouble(2.88);
    const auto a = o["points"].toArray();
    for (int i = 0; i + 1 < a.size(); i += 2) w.points << QPointF(a[i].toDouble(), a[i + 1].toDouble());
    return w;
}

} // namespace jp
