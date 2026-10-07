#include "core/items.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QRandomGenerator>
#include <QTransform>
#include <QUuid>
#include <QtMath>
#include <atomic>

namespace jp {

QString newId(const char *prefix)
{
    static std::atomic<quint32> counter{0};
    const quint32 n = counter++;
    return QString::fromLatin1(prefix) + QString::number(QRandomGenerator::global()->generate() & 0xffffff, 36) +
           QString::number(n, 36);
}

QString itemTypeName(ItemType t)
{
    switch (t) {
    case ItemType::Text: return QStringLiteral("Text Box");
    case ItemType::Picture: return QStringLiteral("Picture");
    case ItemType::Shape: return QStringLiteral("Shape");
    case ItemType::Line: return QStringLiteral("Line");
    case ItemType::Table: return QStringLiteral("Table");
    case ItemType::TextArt: return QStringLiteral("Text Art");
    case ItemType::Group: return QStringLiteral("Group");
    }
    return {};
}

static const char *kTypeKeys[] = {"text", "picture", "shape", "line", "table", "textart", "group"};

static QJsonArray rectJson(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
// Geometry from a file is kept within 1,000,000 points (about 350 m) and
// finite: drawing turns these into pixel integers, and a larger or NaN value
// would overflow them.
static double sane(double v) { return std::isfinite(v) ? std::clamp(v, -1e6, 1e6) : 0.0; }
static QRectF rectFrom(const QJsonValue &v)
{
    const auto a = v.toArray();
    return a.size() == 4 ? QRectF(sane(a[0].toDouble()), sane(a[1].toDouble()), sane(a[2].toDouble()), sane(a[3].toDouble())) : QRectF();
}
static QJsonArray marginsJson(const QMarginsF &m) { return {m.left(), m.top(), m.right(), m.bottom()}; }
static QMarginsF marginsFrom(const QJsonValue &v, const QMarginsF &def)
{
    const auto a = v.toArray();
    return a.size() == 4 ? QMarginsF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : def;
}

// ---------- Item ----------
QTransform Item::transform() const
{
    QTransform t;
    t.translate(rect.center().x(), rect.center().y());
    t.rotate(rotation);
    t.scale(flipH ? -1 : 1, flipV ? -1 : 1);
    t.translate(-rect.width() / 2, -rect.height() / 2);
    return t;
}

QPolygonF Item::outline() const
{
    return transform().map(QPolygonF(QRectF(0, 0, rect.width(), rect.height())));
}

QRectF Item::bounds() const
{
    return rotation == 0 ? rect : outline().boundingRect();
}

void Item::moveBy(double dx, double dy) { rect.translate(dx, dy); }

void Item::scaleInto(const QRectF &from, const QRectF &to)
{
    const double sx = from.width() > 0 ? to.width() / from.width() : 1;
    const double sy = from.height() > 0 ? to.height() / from.height() : 1;
    const QPointF c = rect.center();
    const QPointF nc(to.left() + (c.x() - from.left()) * sx, to.top() + (c.y() - from.top()) * sy);
    const bool swap = std::abs(std::sin(qDegreesToRadians(rotation))) > 0.7;
    const double nw = std::max(1.0, rect.width() * (swap ? sy : sx));
    const double nh = std::max(1.0, rect.height() * (swap ? sx : sy));
    rect = QRectF(nc.x() - nw / 2, nc.y() - nh / 2, nw, nh);
}

void Item::rotateAround(double deg, const QPointF &c)
{
    QTransform t;
    t.translate(c.x(), c.y());
    t.rotate(deg);
    t.translate(-c.x(), -c.y());
    const QPointF nc = t.map(rect.center());
    rect.moveCenter(nc);
    rotation = std::fmod(std::fmod(rotation + deg, 360.0) + 360.0, 360.0);
}

void Item::copyBase(const Item &o)
{
    id = newId();
    name = o.name; altText = o.altText; hyperlink = o.hyperlink;
    rect = o.rect; rotation = o.rotation; flipH = o.flipH; flipV = o.flipV; locked = o.locked;
    fill = o.fill; stroke = o.stroke; fx = o.fx; wrap = o.wrap;
}

QJsonObject Item::toJson() const
{
    QJsonObject o;
    o["type"] = QString::fromLatin1(kTypeKeys[int(type())]);
    o["id"] = id;
    if (!name.isEmpty()) o["name"] = name;
    if (!altText.isEmpty()) o["alt"] = altText;
    if (!hyperlink.isEmpty()) o["link"] = hyperlink;
    o["rect"] = rectJson(rect);
    if (rotation) o["rot"] = rotation;
    if (flipH) o["flipH"] = true;
    if (flipV) o["flipV"] = true;
    if (locked) o["locked"] = true;
    if (fill.type != Fill::NoFill) o["fill"] = fill.toJson();
    if (!stroke.isNone()) o["stroke"] = stroke.toJson();
    if (fx.any()) o["fx"] = fx.toJson();
    o["wrap"] = wrap.toJson();
    return o;
}

void Item::fromJson(const QJsonObject &o)
{
    id = o["id"].toString(id);
    name = o["name"].toString();
    altText = o["alt"].toString();
    hyperlink = o["link"].toString();
    rect = rectFrom(o["rect"]);
    rotation = o["rot"].toDouble();
    flipH = o["flipH"].toBool();
    flipV = o["flipV"].toBool();
    locked = o["locked"].toBool();
    fill = Fill::fromJson(o["fill"].toObject());
    stroke = Stroke::fromJson(o["stroke"].toObject());
    fx = Effects::fromJson(o["fx"].toObject());
    wrap = Wrap::fromJson(o["wrap"].toObject());
}

ItemPtr Item::create(ItemType t)
{
    switch (t) {
    case ItemType::Text: return std::make_shared<TextItem>();
    case ItemType::Picture: return std::make_shared<PictureItem>();
    case ItemType::Shape: return std::make_shared<ShapeItem>();
    case ItemType::Line: return std::make_shared<LineItem>();
    case ItemType::Table: return std::make_shared<TableItem>();
    case ItemType::TextArt: return std::make_shared<TextArtItem>();
    case ItemType::Group: return std::make_shared<GroupItem>();
    }
    return nullptr;
}

ItemPtr Item::fromJsonAny(const QJsonObject &o)
{
    const QString t = o["type"].toString();
    for (int i = 0; i < 7; ++i)
        if (t == QLatin1String(kTypeKeys[i])) {
            auto it = create(ItemType(i));
            it->fromJson(o);
            return it;
        }
    return nullptr;
}

QRectF unionBounds(const ItemList &items)
{
    QRectF r;
    for (const auto &it : items) r = r.isNull() ? it->bounds() : r.united(it->bounds());
    return r;
}

// ---------- TextItem ----------
TextItem::TextItem() { wrap.mode = Wrap::Square; }

ItemPtr TextItem::clone() const
{
    auto c = std::make_shared<TextItem>(*this);
    c->copyBase(*this);
    c->nextId.clear();
    return c;   // caller assigns a copied story
}

QJsonObject TextItem::toJson() const
{
    QJsonObject o = Item::toJson();
    o["story"] = storyId;
    if (!nextId.isEmpty()) o["next"] = nextId;
    o["insets"] = marginsJson(insets);
    if (columns > 1) { o["columns"] = columns; o["columnGap"] = columnGap; }
    if (valign != VAlign::Top) o["valign"] = int(valign);
    if (autofit) o["autofit"] = int(autofit);
    if (vertical) o["vertical"] = true;
    if (continuedOn) o["contOn"] = true;
    if (!hyphenate) o["hyph"] = false;
    if (std::abs(hyphenZone - 18) > 1e-6) o["hyphZone"] = hyphenZone;
    if (continuedFrom) o["contFrom"] = true;
    if (fitScale != 1.0) o["fitScale"] = fitScale;
    return o;
}

void TextItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    storyId = o["story"].toString();
    nextId = o["next"].toString();
    insets = marginsFrom(o["insets"], insets);
    columns = std::max(1, o["columns"].toInt(1));
    columnGap = o["columnGap"].toDouble(9);
    valign = VAlign(o["valign"].toInt());
    autofit = Autofit(o["autofit"].toInt());
    vertical = o["vertical"].toBool();
    continuedOn = o["contOn"].toBool();
    hyphenate = o["hyph"].toBool(true);
    hyphenZone = o["hyphZone"].toDouble(18);
    continuedFrom = o["contFrom"].toBool();
    fitScale = o["fitScale"].toDouble(1.0);
}

// ---------- PictureItem ----------
PictureItem::PictureItem() { wrap.mode = Wrap::Square; }

ItemPtr PictureItem::clone() const
{
    auto c = std::make_shared<PictureItem>(*this);
    c->copyBase(*this);
    return c;
}

void PictureItem::fitImage(const QSize &px, bool fillFrame)
{
    if (px.isEmpty()) { imgRect = QRectF(0, 0, rect.width(), rect.height()); return; }
    const double fw = rect.width(), fh = rect.height();
    const double s = fillFrame ? std::max(fw / px.width(), fh / px.height()) : std::min(fw / px.width(), fh / px.height());
    const double w = px.width() * s, h = px.height() * s;
    imgRect = QRectF((fw - w) / 2, (fh - h) / 2, w, h);
}

void PictureItem::scaleInto(const QRectF &from, const QRectF &to)
{
    const QSizeF before = rect.size();
    Item::scaleInto(from, to);
    const double kx = rect.width() / before.width(), ky = rect.height() / before.height();
    imgRect = QRectF(imgRect.x() * kx, imgRect.y() * ky, imgRect.width() * kx, imgRect.height() * ky);
}

QJsonObject PictureItem::toJson() const
{
    QJsonObject o = Item::toJson();
    if (!imageId.isEmpty()) o["image"] = imageId;
    o["imgRect"] = rectJson(imgRect);
    if (maskShape != QLatin1String("rect")) o["mask"] = maskShape;
    if (brightness) o["brightness"] = brightness;
    if (contrast) o["contrast"] = contrast;
    if (recolor) { o["recolor"] = int(recolor); o["recolorColor"] = recolorColor.toString(); }
    if (hasTransparentColor) o["transparentColor"] = transparentColor.name();
    if (transparency) o["transparency"] = transparency;
    if (!caption.isEmpty()) o["caption"] = caption;
    return o;
}

void PictureItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    imageId = o["image"].toString();
    imgRect = rectFrom(o["imgRect"]);
    if (imgRect.isNull()) imgRect = QRectF(0, 0, rect.width(), rect.height());
    maskShape = o["mask"].toString(QStringLiteral("rect"));
    brightness = o["brightness"].toDouble();
    contrast = o["contrast"].toDouble();
    recolor = Recolor(o["recolor"].toInt());
    recolorColor = ColorRef::fromString(o["recolorColor"].toString());
    hasTransparentColor = o.contains("transparentColor");
    transparentColor = QColor(o["transparentColor"].toString());
    transparency = o["transparency"].toDouble();
    caption = o["caption"].toString();
}

// ---------- ShapeItem ----------
ShapeItem::ShapeItem()
{
    fill = Fill::solid(ColorRef::scheme(Accent1));
    stroke = Stroke::line(ColorRef::scheme(Accent1, 0, 25), 1);
}

ItemPtr ShapeItem::clone() const
{
    auto c = std::make_shared<ShapeItem>(*this);
    c->copyBase(*this);
    return c;
}

void ShapeItem::resized(const QSizeF &before)
{
    if (before.width() <= 0 || before.height() <= 0) return;
    const double kx = rect.width() / before.width(), ky = rect.height() / before.height();
    if (kx == 1 && ky == 1) return;
    if (!customPath.isEmpty()) customPath = QTransform::fromScale(kx, ky).map(customPath);
    if (isArt()) stroke.width *= std::sqrt(kx * ky);
}

void ShapeItem::scaleInto(const QRectF &from, const QRectF &to)
{
    const QSizeF before = rect.size();
    Item::scaleInto(from, to);
    resized(before);
}

static QJsonArray pathJson(const QPainterPath &p)
{
    QJsonArray a;
    for (int i = 0; i < p.elementCount(); ++i) {
        const auto e = p.elementAt(i);
        a.append(QJsonArray{int(e.type), e.x, e.y});
    }
    return a;
}

static QPainterPath pathFrom(const QJsonArray &a)
{
    QPainterPath p;
    for (int i = 0; i < a.size(); ++i) {
        const auto e = a[i].toArray();
        const int t = e[0].toInt();
        const QPointF pt(e[1].toDouble(), e[2].toDouble());
        if (t == QPainterPath::MoveToElement) p.moveTo(pt);
        else if (t == QPainterPath::LineToElement) p.lineTo(pt);
        else if (t == QPainterPath::CurveToElement && i + 2 < a.size()) {
            const auto c2 = a[i + 1].toArray(), c3 = a[i + 2].toArray();
            p.cubicTo(pt, QPointF(c2[1].toDouble(), c2[2].toDouble()), QPointF(c3[1].toDouble(), c3[2].toDouble()));
            i += 2;
        }
    }
    return p;
}

QJsonObject ShapeItem::toJson() const
{
    QJsonObject o = Item::toJson();
    o["shape"] = shape;
    if (!adj.isEmpty()) {
        QJsonArray a;
        for (double v : adj) a.append(v);
        o["adj"] = a;
    }
    if (!customPath.isEmpty()) o["path"] = pathJson(customPath);
    // Overlapping outlines fill by winding (as SVG does by default).
    if (!customPath.isEmpty() && customPath.fillRule() == Qt::WindingFill) o["winding"] = true;
    if (!storyId.isEmpty()) {
        o["story"] = storyId;
        o["insets"] = marginsJson(insets);
        o["valign"] = int(valign);
    }
    return o;
}

void ShapeItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    shape = o["shape"].toString(QStringLiteral("rect"));
    adj.clear();
    for (const auto &v : o["adj"].toArray()) adj.push_back(v.toDouble());
    customPath = pathFrom(o["path"].toArray());
    if (o["winding"].toBool()) customPath.setFillRule(Qt::WindingFill);
    storyId = o["story"].toString();
    insets = marginsFrom(o["insets"], insets);
    valign = VAlign(o["valign"].toInt(int(VAlign::Middle)));
}

// ---------- LineItem ----------
LineItem::LineItem()
{
    stroke = Stroke::line(ColorRef::scheme(Main), 1);
    wrap.mode = Wrap::None;
    syncRect();
}

ItemPtr LineItem::clone() const
{
    auto c = std::make_shared<LineItem>(*this);
    c->copyBase(*this);
    return c;
}

void LineItem::syncRect()
{
    rect = bounds();
    rotation = 0;
}

QVector<QPointF> LineItem::routePoints() const
{
    if (route == Straight) return {p1, p2};
    const QPointF d = p2 - p1;
    // Both ends level: out, across at the bend, and in (an S for a curve).
    if (!startVertical && !endVertical) {
        const double x = p1.x() + bend * d.x();
        return {p1, {x, p1.y()}, {x, p2.y()}, p2};
    }
    if (startVertical && endVertical) {
        const double y = p1.y() + bend * d.y();
        return {p1, {p1.x(), y}, {p2.x(), y}, p2};
    }
    // One end level, the other upright: a single corner (a quarter curve).
    const QPointF corner = startVertical ? QPointF(p1.x(), p2.y()) : QPointF(p2.x(), p1.y());
    if (route == Elbow) return {p1, corner, p2};
    const double k = 0.5523;
    return {p1, p1 + (corner - p1) * k, p2 + (corner - p2) * k, p2};
}

QPainterPath LineItem::path() const
{
    const QVector<QPointF> pts = routePoints();
    QPainterPath p(pts.first());
    if (route == Curved && pts.size() == 4) p.cubicTo(pts[1], pts[2], pts[3]);
    else for (int i = 1; i < pts.size(); ++i) p.lineTo(pts[i]);
    return p;
}

QRectF LineItem::bounds() const
{
    if (route == Straight) return QRectF(p1, p2).normalized();
    return path().boundingRect().united(QRectF(p1, p2).normalized());
}
void LineItem::moveBy(double dx, double dy) { p1 += QPointF(dx, dy); p2 += QPointF(dx, dy); syncRect(); }

void LineItem::scaleInto(const QRectF &from, const QRectF &to)
{
    auto m = [&](QPointF p) {
        const double sx = from.width() > 0 ? to.width() / from.width() : 1, sy = from.height() > 0 ? to.height() / from.height() : 1;
        return QPointF(to.left() + (p.x() - from.left()) * sx, to.top() + (p.y() - from.top()) * sy);
    };
    p1 = m(p1); p2 = m(p2);
    syncRect();
}

void LineItem::rotateAround(double deg, const QPointF &c)
{
    QTransform t;
    t.translate(c.x(), c.y());
    t.rotate(deg);
    t.translate(-c.x(), -c.y());
    p1 = t.map(p1); p2 = t.map(p2);
    // A quarter turn makes level ends upright and upright ends level.
    if (std::abs(std::remainder(deg, 180.0)) > 45) {
        startVertical = !startVertical;
        endVertical = !endVertical;
    }
    syncRect();
}

QJsonObject LineItem::toJson() const
{
    QJsonObject o = Item::toJson();
    o["p1"] = QJsonArray{p1.x(), p1.y()};
    o["p2"] = QJsonArray{p2.x(), p2.y()};
    if (route != Straight) {
        o["route"] = route == Elbow ? QStringLiteral("elbow") : QStringLiteral("curved");
        if (bend != 0.5) o["bend"] = bend;
    }
    if (startVertical) o["startVertical"] = true;
    if (endVertical) o["endVertical"] = true;
    auto glue = [&](const char *key, const Glue &g) {
        if (!g.id.isEmpty()) o[QLatin1String(key)] = QJsonObject{{"id", g.id}, {"site", g.site}};
    };
    glue("startGlue", start);
    glue("endGlue", end);
    return o;
}

void LineItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    const auto a = o["p1"].toArray(), b = o["p2"].toArray();
    p1 = QPointF(a[0].toDouble(), a[1].toDouble());
    p2 = QPointF(b[0].toDouble(), b[1].toDouble());
    const QString r = o["route"].toString();
    route = r == QLatin1String("elbow") ? Elbow : r == QLatin1String("curved") ? Curved : Straight;
    bend = o["bend"].toDouble(0.5);
    startVertical = o["startVertical"].toBool();
    endVertical = o["endVertical"].toBool();
    auto glue = [&](const char *key) {
        const QJsonObject g = o[QLatin1String(key)].toObject();
        return Glue{g["id"].toString(), g["site"].toInt(-1)};
    };
    start = glue("startGlue");
    end = glue("endGlue");
    syncRect();
}

// ---------- TableItem ----------
TableItem::TableItem() { wrap.mode = Wrap::Square; }

ItemPtr TableItem::clone() const
{
    auto c = std::make_shared<TableItem>(*this);
    c->copyBase(*this);
    return c;   // caller copies cell stories
}

QRectF TableItem::cellRect(int r, int c) const
{
    double x = 0, y = 0;
    for (int i = 0; i < c; ++i) x += colW[i];
    for (int i = 0; i < r; ++i) y += rowH[i];
    const auto &cl = cell(r, c);
    double w = 0, h = 0;
    for (int i = c; i < std::min(cols, c + cl.colSpan); ++i) w += colW[i];
    for (int i = r; i < std::min(rows, r + cl.rowSpan); ++i) h += rowH[i];
    return QRectF(x, y, w, h);
}

void TableItem::syncRect()
{
    double w = 0, h = 0;
    for (double v : colW) w += v;
    for (double v : rowH) h += v;
    rect.setSize(QSizeF(w, h));
}

void TableItem::scaleInto(const QRectF &from, const QRectF &to)
{
    const QSizeF before = rect.size();
    Item::scaleInto(from, to);
    const double kx = rect.width() / before.width(), ky = rect.height() / before.height();
    for (double &w : colW) w *= kx;
    for (double &h : rowH) h *= ky;
}

static QJsonObject borderJson(const CellBorder &b)
{
    QJsonObject o;
    if (!b.top.isNone()) o["t"] = b.top.toJson();
    if (!b.bottom.isNone()) o["b"] = b.bottom.toJson();
    if (!b.left.isNone()) o["l"] = b.left.toJson();
    if (!b.right.isNone()) o["r"] = b.right.toJson();
    return o;
}

QJsonObject TableItem::toJson() const
{
    QJsonObject o = Item::toJson();
    o["rows"] = rows;
    o["cols"] = cols;
    QJsonArray cw, rh, cs;
    for (double v : colW) cw.append(v);
    for (double v : rowH) rh.append(v);
    for (const auto &c : cells) {
        QJsonObject co{{"story", c.storyId}};
        if (c.fill.type != Fill::NoFill) co["fill"] = c.fill.toJson();
        if (c.rowSpan > 1) co["rs"] = c.rowSpan;
        if (c.colSpan > 1) co["cs"] = c.colSpan;
        if (c.covered) co["covered"] = true;
        if (c.diagonal) co["diag"] = c.diagonal;
        if (c.valign != VAlign::Top) co["valign"] = int(c.valign);
        co["margins"] = marginsJson(c.margins);
        const auto b = borderJson(c.border);
        if (!b.isEmpty()) co["border"] = b;
        cs.append(co);
    }
    o["colW"] = cw;
    o["rowH"] = rh;
    o["cells"] = cs;
    if (!format.isEmpty()) o["format"] = format;
    o["grow"] = growToFit;
    if (lockSize) o["lockSize"] = true;
    o["header"] = header;
    o["banded"] = banded;
    return o;
}

void TableItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    // Twice the other program's largest table: more is a damaged file, and
    // rows x cols cells are made below.
    rows = std::clamp(o["rows"].toInt(), 0, 256);
    cols = std::clamp(o["cols"].toInt(), 0, 256);
    colW.clear(); rowH.clear(); cells.clear();
    for (const auto &v : o["colW"].toArray()) colW.push_back(v.toDouble());
    for (const auto &v : o["rowH"].toArray()) rowH.push_back(v.toDouble());
    for (const auto &v : o["cells"].toArray()) {
        const auto co = v.toObject();
        TableCell c;
        c.storyId = co["story"].toString();
        c.fill = Fill::fromJson(co["fill"].toObject());
        c.rowSpan = co["rs"].toInt(1);
        c.colSpan = co["cs"].toInt(1);
        c.covered = co["covered"].toBool();
        c.diagonal = co["diag"].toInt();
        c.valign = VAlign(co["valign"].toInt());
        c.margins = marginsFrom(co["margins"], c.margins);
        const auto b = co["border"].toObject();
        c.border.top = Stroke::fromJson(b["t"].toObject());
        c.border.bottom = Stroke::fromJson(b["b"].toObject());
        c.border.left = Stroke::fromJson(b["l"].toObject());
        c.border.right = Stroke::fromJson(b["r"].toObject());
        cells.push_back(c);
    }
    cells.resize(rows * cols);
    colW.resize(cols);
    rowH.resize(rows);
    format = o["format"].toString();
    growToFit = o["grow"].toBool(true);
    lockSize = o["lockSize"].toBool();
    header = o["header"].toBool(true);
    banded = o["banded"].toBool(true);
}

// ---------- TextArtItem ----------
TextArtItem::TextArtItem()
{
    fill = Fill::solid(ColorRef::scheme(Accent1));
    stroke = Stroke::none();
    wrap.mode = Wrap::Square;
}

ItemPtr TextArtItem::clone() const
{
    auto c = std::make_shared<TextArtItem>(*this);
    c->copyBase(*this);
    return c;
}

QJsonObject TextArtItem::toJson() const
{
    QJsonObject o = Item::toJson();
    o["text"] = text;
    o["font"] = font;
    if (bold) o["bold"] = true;
    if (italic) o["italic"] = true;
    o["size"] = size;
    if (evenHeight) o["even"] = true;
    if (vertical) o["vertical"] = true;
    o["spacing"] = spacing;
    o["align"] = align;
    o["transform"] = transform_;
    o["adj"] = transformAdj;
    if (!styleId.isEmpty()) o["style"] = styleId;
    return o;
}

void TextArtItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    text = o["text"].toString();
    font = o["font"].toString(font);
    bold = o["bold"].toBool();
    italic = o["italic"].toBool();
    size = o["size"].toDouble(36);
    evenHeight = o["even"].toBool();
    vertical = o["vertical"].toBool();
    spacing = o["spacing"].toDouble(1.0);
    align = o["align"].toInt(1);
    transform_ = o["transform"].toString(QStringLiteral("plain"));
    transformAdj = o["adj"].toDouble(0.5);
    styleId = o["style"].toString();
}

// ---------- GroupItem ----------
GroupItem::GroupItem() { wrap.mode = Wrap::Square; }

ItemPtr GroupItem::clone() const
{
    auto g = std::make_shared<GroupItem>();
    g->copyBase(*this);
    for (const auto &c : children) g->children.push_back(c->clone());
    g->barcode = barcode;
    return g;
}

QRectF GroupItem::bounds() const { return unionBounds(children); }

void GroupItem::moveBy(double dx, double dy)
{
    for (auto &c : children) c->moveBy(dx, dy);
    syncRect();
}

void GroupItem::scaleInto(const QRectF &from, const QRectF &to)
{
    for (auto &c : children) c->scaleInto(from, to);
    syncRect();
}

void GroupItem::rotateAround(double deg, const QPointF &c)
{
    for (auto &ch : children) ch->rotateAround(deg, c);
    syncRect();
}

QJsonObject GroupItem::toJson() const
{
    QJsonObject o = Item::toJson();
    QJsonArray a;
    for (const auto &c : children) a.append(c->toJson());
    o["children"] = a;
    if (!barcode.isEmpty()) o["barcode"] = barcode;
    return o;
}

void GroupItem::fromJson(const QJsonObject &o)
{
    Item::fromJson(o);
    children.clear();
    for (const auto &v : o["children"].toArray())
        if (auto it = Item::fromJsonAny(v.toObject())) children.push_back(it);
    barcode = o["barcode"].toObject();
    syncRect();
}

QPointF connectionSite(const Item &it, int site, bool *vertical)
{
    const bool group = it.type() == ItemType::Group;
    const QSizeF sz = group ? it.bounds().size() : it.rect.size();
    const QTransform t = group ? QTransform::fromTranslate(it.bounds().left(), it.bounds().top()) : it.transform();
    static const QPointF at[kConnectionSites] = {{0.5, 0}, {0, 0.5}, {0.5, 1}, {1, 0.5}};
    static const QPointF out[kConnectionSites] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
    site = std::clamp(site, 0, kConnectionSites - 1);
    const QPointF l(at[site].x() * sz.width(), at[site].y() * sz.height());
    const QPointF p = t.map(l);
    if (vertical) {
        const QPointF n = t.map(l + out[site]) - p;
        *vertical = std::abs(n.y()) > std::abs(n.x());
    }
    return p;
}

void remapGlue(const ItemList &items, const QHash<QString, QString> &ids)
{
    walkItems(items, [&](const ItemPtr &it) {
        if (it->type() != ItemType::Line) return;
        auto *l = static_cast<LineItem *>(it.get());
        for (LineItem::Glue *g : {&l->start, &l->end})
            if (!g->id.isEmpty()) *g = ids.contains(g->id) ? LineItem::Glue{ids.value(g->id), g->site} : LineItem::Glue();
    });
}

} // namespace jp
