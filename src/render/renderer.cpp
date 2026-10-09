#include "render/renderer.h"

#include "render/metafile.h"
#include "render/pdfpage.h"
#include "render/shapes.h"
#include "render/textart.h"
#include "core/fonts.h"

#include <QCache>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QPainter>
#include <QSvgRenderer>
#include <QPainterPathStroker>
#include <QTextDocument>
#include <QtMath>
#include <cmath>

namespace jp {

// ---------- helpers ----------
QColor grayOf(const QColor &c)
{
    const int g = qGray(c.rgb());
    return QColor(g, g, g, c.alpha());
}

void boxBlur(QImage &img, int radius)
{
    if (radius < 1 || img.isNull()) return;
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = img.width(), h = img.height();
    QVector<int> buf(std::max(w, h) * 4);
    auto pass = [&](bool horizontal) {
        const int n = horizontal ? w : h, lines = horizontal ? h : w;
        for (int l = 0; l < lines; ++l) {
            auto px = [&](int i) -> QRgb & {
                return horizontal ? reinterpret_cast<QRgb *>(img.scanLine(l))[i] : reinterpret_cast<QRgb *>(img.scanLine(i))[l];
            };
            for (int i = 0; i < n; ++i) {
                const QRgb c = px(i);
                buf[i * 4] = qAlpha(c); buf[i * 4 + 1] = qRed(c); buf[i * 4 + 2] = qGreen(c); buf[i * 4 + 3] = qBlue(c);
            }
            int s[4] = {0, 0, 0, 0};
            const int win = radius * 2 + 1;
            for (int i = -radius; i <= radius; ++i) {
                const int k = std::clamp(i, 0, n - 1);
                for (int c = 0; c < 4; ++c) s[c] += buf[k * 4 + c];
            }
            for (int i = 0; i < n; ++i) {
                px(i) = qRgba(s[1] / win, s[2] / win, s[3] / win, s[0] / win);
                const int out = std::clamp(i - radius, 0, n - 1), in = std::clamp(i + radius + 1, 0, n - 1);
                for (int c = 0; c < 4; ++c) s[c] += buf[in * 4 + c] - buf[out * 4 + c];
            }
        }
    };
    for (int k = 0; k < 3; ++k) { pass(true); pass(false); }
}

static double deviceScale(const QPainter *p)
{
    const QTransform t = p->deviceTransform();
    return std::sqrt(std::abs(t.determinant()));
}

static ImageLookup lookupFor(const Document &doc)
{
    return [&doc](const QString &id) { return doc.image(id); };
}

void Renderer::strokePath(QPainter *p, const QPainterPath &path, const Stroke &s, const ColorScheme &cs)
{
    if (s.isNone()) return;
    const QPen pen = s.pen(cs);
    // Inside a closed outline's edge: the outline twice as wide, clipped to
    // the shape, so its inner half shows (dashes keep their lengths).
    if (s.inset && s.compound == Stroke::Single && path.elementCount() > 2 &&
        QLineF(QPointF(path.elementAt(0)), path.currentPosition()).length() < 0.01) {
        QPen wide = pen;
        wide.setWidthF(pen.widthF() * 2);
        if (wide.style() == Qt::CustomDashLine) {
            QList<qreal> d = wide.dashPattern();
            for (qreal &x : d) x /= 2;
            wide.setDashPattern(d);
        }
        p->save();
        p->setClipPath(path, Qt::IntersectClip);
        p->strokePath(path, wide);
        p->restore();
        return;
    }
    if (s.compound == Stroke::Single) {
        p->strokePath(path, pen);
        return;
    }
    QPainterPathStroker st;
    st.setJoinStyle(pen.joinStyle());
    st.setCapStyle(pen.capStyle());
    if (pen.style() == Qt::CustomDashLine) st.setDashPattern(pen.dashPattern());
    auto ring = [&](double w) { st.setWidth(w); return st.createStroke(path); };
    const double w = std::max(1.5, s.width);
    QPainterPath region;
    switch (s.compound) {
    case Stroke::Double: region = ring(w).subtracted(ring(w / 3)); break;
    case Stroke::ThickThin: region = ring(w).subtracted(ring(w * 0.25)).subtracted(ring(w * 0.5).subtracted(ring(w * 0.25)) ); break;
    case Stroke::ThinThick: region = ring(w).subtracted(ring(w * 0.6)).united(ring(w * 0.3)); break;
    case Stroke::Triple: region = ring(w).subtracted(ring(w * 0.6)).united(ring(w * 0.2)); break;
    default: break;
    }
    p->fillPath(region, pen.brush());
}

// ---------- layout cache ----------
QVector<QPolygonF> Renderer::wrapObstacles(const Document &doc, const TextItem &frame)
{
    QVector<QPolygonF> out;
    const auto loc = doc.find(frame.id);
    if (!loc.item) return out;
    const ItemList *list = nullptr;
    if (loc.page >= 0) list = &doc.pages[loc.page]->items;
    else if (!loc.masterId.isEmpty()) { if (auto *m = doc.master(loc.masterId)) list = &m->items; }
    else list = &doc.scratch;
    if (!list) return out;
    // Index of the top-level item that holds this frame.
    int idx = -1;
    for (int i = 0; i < int(list->size()); ++i) {
        bool hit = (*list)[i]->id == frame.id;
        if (!hit && (*list)[i]->type() == ItemType::Group)
            walkItems(static_cast<GroupItem *>((*list)[i].get())->children, [&](const ItemPtr &c) { hit |= c->id == frame.id; });
        if (hit) { idx = i; break; }
    }
    if (idx < 0) return out;
    const QTransform inv = frame.transform().inverted();
    const QRectF fb = frame.bounds();
    std::function<void(const ItemPtr &)> consider = [&](const ItemPtr &o) {
        if (o->type() == ItemType::Group) {
            for (const auto &c : static_cast<GroupItem *>(o.get())->children) consider(c);
            return;
        }
        if (o->id == frame.id || o->wrap.mode == Wrap::None) return;
        // A text box pushes this box's text aside only if its own frame
        // reaches into the text area; then the wrap distance widens the gap.
        // Publisher's designs butt boxes together, their distances (2.88 pt)
        // reaching past their insets (2.85): a name in a 12.7-pt-high box
        // lost its only line to a box beside it.
        if (o->type() == ItemType::Text && !o->bounds().adjusted(1, 1, -1, -1).intersects(fb.marginsRemoved(frame.insets))) return;
        const Wrap &w = o->wrap;
        QPainterPath pagePath;
        if (w.mode == Wrap::Square || w.mode == Wrap::TopBottom) {
            QRectF b = o->bounds().adjusted(-w.left, -w.top, w.right, w.bottom);
            if (w.mode == Wrap::TopBottom) { b.setLeft(-1e5); b.setRight(1e5); }
            pagePath.addRect(b);
        } else {
            QPainterPath local;
            if (!w.points.isEmpty()) local.addPolygon(w.points);
            else local = silhouette(doc, *o);
            pagePath = o->transform().map(local);
            const double d = std::max({w.left, w.right, w.top, w.bottom});
            if (d > 0) {
                QPainterPathStroker st;
                st.setWidth(d * 2);
                pagePath = pagePath.united(st.createStroke(pagePath));
            }
        }
        const QRectF pb = pagePath.boundingRect();
        if (!pb.intersects(fb)) return;
        if (w.side == Wrap::LeftOnly) { QPainterPath r; r.addRect(QRectF(pb.left(), pb.top(), 1e5, pb.height())); pagePath = pagePath.united(r); }
        else if (w.side == Wrap::RightOnly) { QPainterPath r; r.addRect(QRectF(-1e5, pb.top(), pb.right() + 1e5, pb.height())); pagePath = pagePath.united(r); }
        else if (w.side == Wrap::Largest) {
            const double leftSpace = pb.left() - fb.left(), rightSpace = fb.right() - pb.right();
            QPainterPath r;
            if (leftSpace >= rightSpace) r.addRect(QRectF(pb.left(), pb.top(), 1e5, pb.height()));
            else r.addRect(QRectF(-1e5, pb.top(), pb.right() + 1e5, pb.height()));
            pagePath = pagePath.united(r);
        }
        out << inv.map(pagePath.toFillPolygon());
    };
    for (int i = idx + 1; i < int(list->size()); ++i) consider((*list)[i]);
    return out;
}

FrameSpec Renderer::frameSpec(const Document &doc, const TextItem &t, int pageNumber, const RenderOptions &opt)
{
    FrameSpec s;
    s.size = t.vertical ? QSizeF(t.rect.height(), t.rect.width()) : t.rect.size();
    s.insets = t.insets;
    s.columns = std::max(1, t.columns);
    s.gap = t.columnGap;
    s.valign = t.valign;
    s.hyphenate = t.hyphenate;
    s.hyphenZone = t.hyphenZone;
    // Baseline guides of the page's master (for "align text to baseline guides").
    const auto at = doc.find(t.id);
    const MasterPage *mp = at.page >= 0 ? doc.masterFor(*doc.pages[at.page]) : (!at.masterId.isEmpty() ? doc.master(at.masterId) : nullptr);
    if (mp && mp->grid.baseline > 1 && !t.vertical && std::abs(t.rotation) < 0.01) {
        s.baselineGrid = mp->grid.baseline;
        s.baselineOrigin = doc.setup.margins.top() + mp->grid.baselineOffset - t.rect.top();
    }
    if (!t.vertical) s.obstacles = wrapObstacles(doc, t);
    const auto loc = doc.find(t.id);
    s.ctx.doc = &doc;
    s.ctx.pageNumber = loc.page >= 0 ? loc.page + 1 : pageNumber;
    s.ctx.pageCount = doc.pages.size();
    s.ctx.mergeRecord = opt.mergeRecord;
    // For "next page" and "previous page" numbers: the linked boxes' pages.
    if (!t.nextId.isEmpty()) {
        const auto nl = doc.find(t.nextId);
        if (nl.page >= 0) s.ctx.nextPage = nl.page + 1;
    }
    if (TextItem *pf = doc.prevFrame(t.id)) {
        const auto pl = doc.find(pf->id);
        if (pl.page >= 0) s.ctx.prevPage = pl.page + 1;
    }
    if (t.continuedOn && !t.nextId.isEmpty()) {
        const auto nl = doc.find(t.nextId);
        s.ctx.continuedOnPage = nl.page + 1;
        s.insets.setBottom(s.insets.bottom() + 13);
    }
    if (t.continuedFrom) {
        if (TextItem *pf = doc.prevFrame(t.id)) s.ctx.continuedFromPage = doc.find(pf->id).page + 1;
        s.insets.setTop(s.insets.top() + 13);
    }
    return s;
}

static QString specSig(const FrameSpec &s)
{
    QString k = QStringLiteral("%1x%2|%3,%4,%5,%6|%7|%8|%9|%10;").arg(s.size.width()).arg(s.size.height()).arg(s.insets.left()).arg(s.insets.top())
                    .arg(s.insets.right()).arg(s.insets.bottom()).arg(s.columns).arg(s.gap).arg(int(s.valign)).arg(s.ctx.key());
    k += QStringLiteral("h%1,%2|b%3,%4;").arg(int(s.hyphenate)).arg(s.hyphenZone).arg(s.baselineGrid).arg(s.baselineOrigin);
    for (const auto &o : s.obstacles) {
        const QRectF b = o.boundingRect();
        // The points' sum too, so moving a point inside the same bounds re-wraps.
        double sum = 0;
        for (const QPointF &pt : o) sum += pt.x() * 1.7 + pt.y();
        k += QStringLiteral("o%1,%2,%3,%4,%5,%6").arg(b.x(), 0, 'f', 1).arg(b.y(), 0, 'f', 1).arg(b.width(), 0, 'f', 1).arg(b.height(), 0, 'f', 1).arg(o.size()).arg(sum, 0, 'f', 1);
    }
    return k;
}

LayoutCache::FrameLayout LayoutCache::textFrame(const Document &doc, const TextItem &frame, int pageNumber, const RenderOptions &opt)
{
    FrameLayout out;
    QVector<TextItem *> chain = doc.chainOf(frame.id);
    // A text box set in text is on no page: it's a chain of its own.
    if (chain.isEmpty()) chain << const_cast<TextItem *>(&frame);
    const TextItem *head = chain.first();
    Story *story = doc.story(head->storyId);
    if (!story) return out;
    QVector<FrameSpec> specs;
    QString sig = QStringLiteral("%1/%2/").arg(story->serial).arg(story->doc->revision());
    for (int i = 0; i < chain.size(); ++i) {
        if (chain[i]->id == frame.id) out.frame = i;
        specs << Renderer::frameSpec(doc, *chain[i], pageNumber, opt);
        sig += specSig(specs.last());
    }
    LayoutEnv env;
    env.colors = doc.colors;
    env.fonts = doc.fonts;
    sig += env.key();
    const bool autofit = chain.size() == 1 && (head->autofit == TextItem::BestFit || head->autofit == TextItem::ShrinkOnOverflow);
    if (autofit) sig += QStringLiteral("|fit%1%2").arg(int(head->autofit)).arg(head->fitAsStored ? "s" : "");
    const QString key = head->id + QStringLiteral("@") + (doc.find(head->id).page < 0 ? QString::number(pageNumber) : QString());
    Entry &e = m_entries[key];
    if (e.sig != sig || !e.layout) {
        auto lay = std::make_shared<StoryLayout>();
        double scale = 1.0;
        if (autofit) {
            // Text fits when none is left over and the box holds its lines:
            // a line taller than the box (a banner's headline grown to fill
            // its width) doesn't fit, though the layout still places it.
            auto overflows = [&] {
                if (lay->overflow()) return true;
                for (int f = 0; f < specs.size(); ++f) {
                    const auto lines = lay->lineInfo(f);
                    if (lines.isEmpty()) continue;
                    double top = lines.first().rect.top(), bottom = lines.first().rect.bottom();
                    for (const auto &li : lines) {
                        top = std::min(top, li.rect.top());
                        bottom = std::max(bottom, li.rect.bottom());
                    }
                    if (bottom - top > specs[f].size.height() - specs[f].insets.top() - specs[f].insets.bottom() + 0.5) return true;
                }
                return false;
            };
            // Binary search the largest font scale that fits.
            double lo = 0.05, hi = head->autofit == TextItem::BestFit && !head->fitAsStored ? 8.0 : 1.0;
            env.fontScale = hi;
            lay->build(story->doc.get(), specs, env);
            if (overflows()) {
                for (int it = 0; it < 14; ++it) {
                    const double mid = (lo + hi) / 2;
                    env.fontScale = mid;
                    lay->build(story->doc.get(), specs, env);
                    if (overflows()) hi = mid; else lo = mid;
                }
                scale = lo;
                env.fontScale = lo;
                lay->build(story->doc.get(), specs, env);
            } else {
                scale = hi;
            }
        } else {
            lay->build(story->doc.get(), specs, env);
        }
        e.sig = sig;
        e.layout = lay;
        e.fitScale = scale;
    }
    out.layout = e.layout.get();
    out.fitScale = e.fitScale;
    return out;
}

const StoryLayout *LayoutCache::storyBox(const Document &doc, const QString &storyId, const FrameSpec &spec, const QString &key)
{
    Story *story = doc.story(storyId);
    if (!story) return nullptr;
    LayoutEnv env;
    env.colors = doc.colors;
    env.fonts = doc.fonts;
    const QString sig = QStringLiteral("%1/%2/").arg(story->serial).arg(story->doc->revision()) + specSig(spec) + env.key();
    Entry &e = m_entries[key + QStringLiteral("#") + storyId];
    if (e.sig != sig || !e.layout) {
        e.layout = std::make_shared<StoryLayout>();
        e.layout->build(story->doc.get(), {spec}, env);
        e.sig = sig;
    }
    return e.layout.get();
}

// ---------- silhouettes ----------
namespace {
// Douglas-Peucker: drop points closer than tol to the line between neighbors.
void simplifyRun(const QPolygonF &in, int a, int b, double tol, QVector<bool> &keep)
{
    if (b <= a + 1) return;
    const QPointF p = in[a], q = in[b];
    const QPointF d = q - p;
    const double len = std::hypot(d.x(), d.y());
    double best = -1;
    int bi = -1;
    for (int i = a + 1; i < b; ++i) {
        const QPointF r = in[i] - p;
        const double dist = len < 1e-9 ? std::hypot(r.x(), r.y()) : std::abs(d.x() * r.y() - d.y() * r.x()) / len;
        if (dist > best) { best = dist; bi = i; }
    }
    if (best > tol) {
        keep[bi] = true;
        simplifyRun(in, a, bi, tol, keep);
        simplifyRun(in, bi, b, tol, keep);
    }
}

QPolygonF convexHull(QPolygonF pts)
{
    std::sort(pts.begin(), pts.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    if (pts.size() < 3) return pts;
    auto cross = [](const QPointF &o, const QPointF &a, const QPointF &b) { return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x()); };
    QPolygonF h(2 * pts.size());
    int k = 0;
    for (int i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (int i = pts.size() - 2, t = k + 1; i >= 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}
} // namespace

QPolygonF Renderer::defaultWrapPolygon(const Document &doc, const Item &it)
{
    const QPainterPath path = silhouette(doc, it);
    const QList<QPolygonF> polys = path.toFillPolygons();
    QPolygonF poly;
    if (polys.size() == 1) {
        poly = polys.first();
    } else {
        // Several pieces (Text Art letters, groups): wrap around all of them.
        QPolygonF all;
        for (const QPolygonF &p : polys) all += p;
        poly = convexHull(all);
    }
    if (poly.size() > 1 && poly.first() == poly.last()) poly.removeLast();
    if (poly.size() < 3) return QPolygonF(QRectF(QPointF(0, 0), it.rect.size()));
    // Simplify until a handful of points remain, so each is easy to grab.
    const double diag = std::hypot(it.rect.width(), it.rect.height());
    for (double tol = diag * 0.012; ; tol *= 1.5) {
        QPolygonF closed = poly;
        closed << poly.first();
        QVector<bool> keep(closed.size(), false);
        keep.first() = keep.last() = true;
        simplifyRun(closed, 0, closed.size() - 1, tol, keep);
        QPolygonF out;
        for (int i = 0; i < closed.size() - 1; ++i)
            if (keep[i]) out << closed[i];
        if (out.size() <= 24 || tol > diag) return out.size() >= 3 ? out : poly;
    }
}

QPainterPath Renderer::textArtOutline(const TextArtItem &w)
{
    static QCache<QString, QPainterPath> cache(200);
    QJsonObject o = w.toJson();
    o.remove("id"); o.remove("rect"); o.remove("fill"); o.remove("stroke"); o.remove("fx"); o.remove("wrap"); o.remove("rot");
    const QString key = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) + QStringLiteral("%1x%2").arg(w.rect.width()).arg(w.rect.height());
    if (QPainterPath *p = cache.object(key)) return *p;
    QPainterPath path = textArtPath(w, w.rect.size());
    cache.insert(key, new QPainterPath(path));
    return path;
}

QPainterPath Renderer::silhouette(const Document &doc, const Item &it)
{
    const QSizeF sz = it.rect.size();
    QPainterPath p;
    switch (it.type()) {
    case ItemType::Shape: {
        const auto &s = static_cast<const ShapeItem &>(it);
        p = s.customPath.isEmpty() ? shapePath(s.shape, sz, s.adj) : s.customPath;
        break;
    }
    case ItemType::Picture: {
        const auto &pic = static_cast<const PictureItem &>(it);
        p = shapePath(pic.maskShape, sz);
        break;
    }
    case ItemType::TextArt: p = textArtOutline(static_cast<const TextArtItem &>(it)); break;
    case ItemType::Line: {
        const auto &l = static_cast<const LineItem &>(it);
        const QPainterPath lp = l.path().translated(-l.rect.topLeft());
        QPainterPathStroker st;
        st.setWidth(std::max(2.0, l.stroke.width));
        p = st.createStroke(lp);
        break;
    }
    default: p.addRect(QRectF(QPointF(0, 0), sz)); break;
    }
    Q_UNUSED(doc);
    return p;
}

// ---------- pictures ----------
// A picture's recoloring of one color (0-255 a channel), as the picture
// settings dialog offers it.
static void recolorRgb(PictureItem::Recolor kind, const QColor &tint, double &r, double &g, double &b)
{
    const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    switch (kind) {
    case PictureItem::Grayscale: r = g = b = lum; break;
    case PictureItem::Sepia: { const double tr = 0.393 * r + 0.769 * g + 0.189 * b, tg = 0.349 * r + 0.686 * g + 0.168 * b, tb = 0.272 * r + 0.534 * g + 0.131 * b; r = tr; g = tg; b = tb; break; }
    case PictureItem::Washout: r = 255 - (255 - r) * 0.25 + 30; g = 255 - (255 - g) * 0.25 + 30; b = 255 - (255 - b) * 0.25 + 30; break;
    case PictureItem::BlackWhite: r = g = b = lum >= 128 ? 255 : 0; break;
    case PictureItem::ColorTint: {
        const double k = 1 - std::clamp(lum, 0.0, 255.0) / 255.0;
        r = 255 + (tint.red() - 255) * k; g = 255 + (tint.green() - 255) * k; b = 255 + (tint.blue() - 255) * k;
        break;
    }
    default: break;
    }
}

QImage Renderer::processedImage(const Document &doc, const PictureItem &pic, const QSizeF &deviceSize)
{
    static QCache<QString, QImage> cache(256 * 1024);   // KB
    QImage src = doc.image(pic.imageId);
    if (src.isNull()) return src;
    // Pick a resolution level: never more than ~1.5x what is shown (screen only).
    int level = 0;
    if (deviceSize.isValid() && deviceSize.width() > 0) {
        double w = src.width();
        while (w / 2 >= deviceSize.width() * 1.5 && w / 2 >= 64) { w /= 2; ++level; }
    }
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8").arg(pic.imageId).arg(pic.brightness).arg(pic.contrast).arg(int(pic.recolor))
                            .arg(pic.recolorColor.resolve(doc.colors).name()).arg(pic.hasTransparentColor ? pic.transparentColor.name() : QString())
                            .arg(level).arg(doc.colors.c[0].name());
    if (QImage *c = cache.object(key)) return *c;
    QImage img = level ? src.scaledToWidth(std::max(1, src.width() >> level), Qt::SmoothTransformation) : src;
    const bool adjust = pic.brightness || pic.contrast || pic.recolor || pic.hasTransparentColor;
    if (adjust) {
        img = img.convertToFormat(QImage::Format_ARGB32);
        const double bright = pic.brightness * 2.55;
        const double cf = (100.0 + pic.contrast) / 100.0;
        const QColor tint = pic.recolorColor.resolve(doc.colors);
        const QRgb tc = pic.transparentColor.rgb();
        for (int y = 0; y < img.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                QRgb c = line[x];
                int a = qAlpha(c);
                if (pic.hasTransparentColor && std::abs(qRed(c) - qRed(tc)) < 10 && std::abs(qGreen(c) - qGreen(tc)) < 10 && std::abs(qBlue(c) - qBlue(tc)) < 10) a = 0;
                double r = qRed(c), g = qGreen(c), b = qBlue(c);
                r = (r - 128) * cf * cf + 128 + bright;
                g = (g - 128) * cf * cf + 128 + bright;
                b = (b - 128) * cf * cf + 128 + bright;
                recolorRgb(pic.recolor, tint, r, g, b);
                line[x] = qRgba(std::clamp(int(r), 0, 255), std::clamp(int(g), 0, 255), std::clamp(int(b), 0, 255), a);
            }
        }
    }
    cache.insert(key, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
    return img;
}

static void drawPlaceholderIcon(QPainter *p, const QRectF &r)
{
    const double s = std::min(r.width(), r.height()) * 0.28;
    if (s < 6) return;
    const QRectF ic(r.center().x() - s / 2, r.center().y() - s * 0.4, s, s * 0.8);
    p->setPen(QPen(QColor(150, 150, 150), std::max(0.75, s / 20)));
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(ic, s * 0.06, s * 0.06);
    QPainterPath m;
    m.moveTo(ic.left() + s * 0.1, ic.bottom() - s * 0.1);
    m.lineTo(ic.left() + s * 0.38, ic.top() + s * 0.38);
    m.lineTo(ic.left() + s * 0.58, ic.top() + s * 0.58);
    m.lineTo(ic.left() + s * 0.72, ic.top() + s * 0.46);
    m.lineTo(ic.right() - s * 0.1, ic.bottom() - s * 0.1);
    p->drawPath(m);
    p->drawEllipse(QPointF(ic.right() - s * 0.25, ic.top() + s * 0.22), s * 0.07, s * 0.07);
}

// ---------- arrows ----------
static QPainterPath arrowHead(Arrow type, const QPointF &tip, const QPointF &from, double lineW, int size, double *inset)
{
    QPainterPath p;
    *inset = 0;
    if (type == Arrow::None) return p;
    const double len = std::max(4.0, lineW * (2.5 + size * 1.25)), wid = len * 0.8;
    const QPointF d = tip - from;
    const double L = std::hypot(d.x(), d.y());
    if (L < 0.01) return p;
    const QPointF u = d / L, n(-u.y(), u.x());
    const QPointF base = tip - u * len;
    switch (type) {
    case Arrow::Triangle: p.addPolygon(QPolygonF({tip, base + n * wid / 2, base - n * wid / 2, tip})); *inset = len * 0.9; break;
    case Arrow::Open: p.moveTo(base + n * wid / 2); p.lineTo(tip); p.lineTo(base - n * wid / 2); *inset = 0; break;
    case Arrow::Stealth: p.addPolygon(QPolygonF({tip, base + n * wid / 2, tip - u * len * 0.6, base - n * wid / 2, tip})); *inset = len * 0.55; break;
    case Arrow::Diamond: p.addPolygon(QPolygonF({tip + u * len * 0.0, tip - u * len / 2 + n * wid / 2, tip - u * len, tip - u * len / 2 - n * wid / 2, tip})); *inset = len * 0.5; break;
    case Arrow::Oval: p.addEllipse(tip - u * len / 2, len / 2, wid / 2); *inset = len * 0.5; break;
    default: break;
    }
    return p;
}

static void paintLine(QPainter *p, const PaintContext &ctx, const LineItem &l)
{
    const ColorScheme &cs = ctx.doc->colors;
    // Each arrowhead points along the route's end: from the nearest other
    // point of the route (a corner, or a curve's control point).
    QVector<QPointF> pts = l.routePoints();
    auto toward = [&](int from, int step) {
        for (int i = from + step; i >= 0 && i < pts.size(); i += step)
            if (QLineF(pts[from], pts[i]).length() > 0.01) return pts[i];
        return pts[from];
    };
    const QPointF a = pts.first(), b = pts.last();
    const QPointF fromA = toward(0, 1), fromB = toward(int(pts.size()) - 1, -1);
    double inA = 0, inB = 0;
    const QPainterPath ha = arrowHead(l.stroke.startArrow, a, fromA, l.stroke.width, l.stroke.startSize, &inA);
    const QPainterPath hb = arrowHead(l.stroke.endArrow, b, fromB, l.stroke.width, l.stroke.endSize, &inB);
    // The line stops short where a filled arrowhead covers its end.
    auto pull = [](QPointF &end, const QPointF &next, double by) {
        const QPointF d = next - end;
        const double L = std::hypot(d.x(), d.y());
        if (L > 0.01) end += d / L * std::min(by, L / 2);
    };
    pull(pts.first(), fromA, inA);
    pull(pts.last(), fromB, inB);
    QPainterPath path(pts.first());
    if (l.route == LineItem::Curved && pts.size() == 4) path.cubicTo(pts[1], pts[2], pts[3]);
    else for (int i = 1; i < pts.size(); ++i) path.lineTo(pts[i]);
    Renderer::strokePath(p, path, l.stroke, cs);
    QPen pen = l.stroke.pen(cs);
    pen.setStyle(Qt::SolidLine);
    pen.setJoinStyle(Qt::MiterJoin);
    for (const auto *h : {&ha, &hb}) {
        if (h->isEmpty()) continue;
        if (h == &ha ? l.stroke.startArrow == Arrow::Open : l.stroke.endArrow == Arrow::Open) p->strokePath(*h, pen);
        else p->fillPath(*h, pen.brush());
    }
}

// ---------- item content ----------
// Objects set in a story's text, where its layout put them in this frame
// (in the same coordinates as the text).
static void paintInlineObjects(QPainter *p, const PaintContext &ctx, const StoryLayout &lay, int frame)
{
    // A text box in text can hold objects too, but a damaged file can set a
    // box in its own story: a few levels, then no deeper.
    static thread_local int depth = 0;
    if (depth >= 3) return;
    ++depth;
    for (const auto &ob : lay.inlineObjects()) {
        if (ob.frame != frame) continue;
        const ItemPtr it = Item::fromJsonAny(QJsonDocument::fromJson(ob.json.toUtf8()).object());
        // The layout keeps sizes sane; an object it had to change isn't drawn.
        if (!it || std::abs(it->rect.width() - ob.rect.width()) > 0.01 || std::abs(it->rect.height() - ob.rect.height()) > 0.01) continue;
        it->moveBy(ob.rect.left() - it->rect.left(), ob.rect.top() - it->rect.top());
        Renderer::paintItem(p, ctx, *it);
    }
    --depth;
}

static void paintText(QPainter *p, const PaintContext &ctx, const TextItem &t)
{
    const QRectF r(QPointF(0, 0), t.rect.size());
    const ColorScheme &cs = ctx.doc->colors;
    if (!t.fill.isNone()) p->fillRect(r, t.fill.brush(r, cs, lookupFor(*ctx.doc)));
    const LayoutCache::FrameLayout fl = ctx.cache->textFrame(*ctx.doc, t, ctx.pageNumber, ctx.opt);
    if (fl.layout && fl.frame >= 0) {
        PaintOptions po;
        po.shadeFields = ctx.opt.shadeFields;
        po.showSpecial = ctx.opt.showSpecial;
        const QVector<TextItem *> chain = ctx.doc->chainOf(t.id);
        const QString sid = chain.isEmpty() ? t.storyId : chain.first()->storyId;
        if (ctx.opt.editStory == sid) { po.selFrom = ctx.opt.selFrom; po.selTo = ctx.opt.selTo; }
        if (ctx.opt.misspelled.contains(sid)) { po.showSpelling = true; po.misspelled = ctx.opt.misspelled.value(sid); }
        p->save();
        if (t.vertical) {
            p->translate(r.width(), 0);
            p->rotate(90);
        }
        fl.layout->paint(p, fl.frame, po);
        paintInlineObjects(p, ctx, *fl.layout, fl.frame);
        p->restore();
        // Continued notices.
        const FrameSpec spec = Renderer::frameSpec(*ctx.doc, t, ctx.pageNumber, ctx.opt);
        QFont nf(ctx.doc->fonts.body);
        nf.setPointSizeF(8 * fontPointFactor());
        nf.setItalic(true);
        const QFontMetricsF nfm(nf);
        p->setPen(cs.slot(Main));
        if (spec.ctx.continuedOnPage > 0) {
            const QString s = spec.ctx.resolve("conton");
            drawPlainText(p, QPointF(r.width() - t.insets.right() - nfm.horizontalAdvance(s), r.height() - t.insets.bottom() - 6 + (nfm.ascent() - nfm.descent()) / 2), nf, s);
        }
        if (spec.ctx.continuedFromPage > 0)
            drawPlainText(p, QPointF(t.insets.left(), t.insets.top() + 6 + (nfm.ascent() - nfm.descent()) / 2), nf, spec.ctx.resolve("contfrom"));
    }
    Renderer::strokePath(p, [&] { QPainterPath pp; pp.addRect(r); return pp; }(), t.stroke, cs);
}

static void paintPicture(QPainter *p, const PaintContext &ctx, const PictureItem &pic)
{
    if (ctx.opt.skipPictures) return;
    const QRectF r(QPointF(0, 0), pic.rect.size());
    const ColorScheme &cs = ctx.doc->colors;
    const QPainterPath mask = shapePath(pic.maskShape, r.size());
    if (!pic.fill.isNone()) p->fillPath(mask, pic.fill.brush(r, cs, lookupFor(*ctx.doc)));
    const QImage src = pic.imageId.isEmpty() ? QImage() : ctx.doc->image(pic.imageId);
    if (src.isNull()) {
        if (!ctx.opt.output) {
            p->fillPath(mask, QColor(236, 236, 236));
            drawPlaceholderIcon(p, r);
        }
    } else {
        const double ds = deviceScale(p);
        QSizeF dev = ctx.opt.output ? QSizeF() : QSizeF(pic.imgRect.width() * ds, pic.imgRect.height() * ds);
        if (ctx.opt.output && ctx.opt.maxImageDpi > 0)
            dev = QSizeF(pic.imgRect.width(), pic.imgRect.height()) * (ctx.opt.maxImageDpi / 72.0 / 1.5);
        p->save();
        p->setClipPath(mask, Qt::IntersectClip);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        if (pic.transparency > 0) p->setOpacity(p->opacity() * (1 - pic.transparency));
        // Vector clip art and PDF pages print as vectors when they need no
        // color adjustment.
        const ImageData data = ctx.doc->images.value(pic.imageId);
        const bool plain = !pic.brightness && !pic.contrast && !pic.recolor && !pic.hasTransparentColor;
        Metafile mf;
        QSvgRenderer svg;
        // Recolored clip art (border art's tinted pieces among it) plays as
        // vectors too, every color changed as the picture's pixels would be:
        // made into a small picture first, its hairlines were lost.
        const bool onlyRecolored = pic.recolor && !pic.brightness && !pic.contrast && !pic.hasTransparentColor;
        const bool metafile = data.format == QLatin1String("wmf") || data.format == QLatin1String("emf");
        if (metafile && onlyRecolored && mf.load(data.bytes)) {
            const QColor tint = pic.recolorColor.resolve(cs);
            mf.play(p, pic.imgRect, [kind = pic.recolor, tint](const QColor &c) {
                double r = c.red(), g = c.green(), b = c.blue();
                recolorRgb(kind, tint, r, g, b);
                return QColor(std::clamp(int(r), 0, 255), std::clamp(int(g), 0, 255), std::clamp(int(b), 0, 255), c.alpha());
            });
        } else if (ctx.opt.output && plain && metafile && mf.load(data.bytes)) {
            mf.play(p, pic.imgRect);
        } else if (ctx.opt.output && plain && data.format == QLatin1String("svg") && svg.load(data.bytes)) {
            svg.render(p, pic.imgRect);
        } else if (ctx.opt.output && plain && data.format == QLatin1String("pdf") && PdfDocument::shared(data.bytes)->isValid()) {
            PdfDocument::shared(data.bytes)->play(p, 0, pic.imgRect);
        } else {
            p->drawImage(pic.imgRect, Renderer::processedImage(*ctx.doc, pic, dev));
        }
        p->restore();
    }
    Renderer::strokePath(p, mask, pic.stroke, cs);
}

static void paintShape(QPainter *p, const PaintContext &ctx, const ShapeItem &s)
{
    const QRectF r(QPointF(0, 0), s.rect.size());
    const ColorScheme &cs = ctx.doc->colors;
    const ShapeDef *def = shapeDef(s.shape);
    const QPainterPath path = s.customPath.isEmpty() ? shapePath(s.shape, r.size(), s.adj) : s.customPath;
    if (!(def && def->open) && !s.fill.isNone()) p->fillPath(path, s.fill.brush(path.boundingRect(), cs, lookupFor(*ctx.doc)));
    Renderer::strokePath(p, path, s.stroke, cs);
    if (!s.storyId.isEmpty()) {
        QPointF origin;
        const StoryLayout *lay = Renderer::shapeTextLayout(ctx, s, &origin);
        if (lay) {
            PaintOptions po;
            po.shadeFields = ctx.opt.shadeFields;
            po.showSpecial = ctx.opt.showSpecial;
            if (ctx.opt.editStory == s.storyId) { po.selFrom = ctx.opt.selFrom; po.selTo = ctx.opt.selTo; }
            p->save();
            p->translate(origin);
            lay->paint(p, 0, po);
            paintInlineObjects(p, ctx, *lay, 0);
            p->restore();
        }
    }
}

const StoryLayout *Renderer::shapeTextLayout(const PaintContext &ctx, const ShapeItem &s, QPointF *origin)
{
    const QRectF r(QPointF(0, 0), s.rect.size());
    const QRectF tr = s.customPath.isEmpty() ? shapeTextRect(s.shape, r.size(), s.adj) : r;
    FrameSpec spec;
    spec.size = tr.size();
    spec.insets = s.insets;
    spec.valign = s.valign;
    spec.ctx.doc = ctx.doc;
    spec.ctx.pageNumber = ctx.pageNumber;
    spec.ctx.pageCount = ctx.pageCount;
    spec.ctx.mergeRecord = ctx.opt.mergeRecord;
    if (origin) *origin = tr.topLeft();
    return ctx.cache->storyBox(*ctx.doc, s.storyId, spec, s.id + QString::number(ctx.pageNumber));
}

const StoryLayout *Renderer::cellLayout(const PaintContext &ctx, const TableItem &t, int row, int col, QPointF *origin)
{
    const TableCell &c = t.cell(row, col);
    const QRectF cr = t.cellRect(row, col);
    FrameSpec spec;
    spec.size = cr.size();
    spec.insets = c.margins;
    spec.valign = c.valign;
    spec.ctx.doc = ctx.doc;
    spec.ctx.pageNumber = ctx.pageNumber;
    spec.ctx.pageCount = ctx.pageCount;
    spec.ctx.mergeRecord = ctx.opt.mergeRecord;
    if (origin) *origin = cr.topLeft();
    return ctx.cache->storyBox(*ctx.doc, c.storyId, spec, t.id + QStringLiteral("/%1/%2/%3").arg(row).arg(col).arg(ctx.pageNumber));
}

static void paintTable(QPainter *p, const PaintContext &ctx, const TableItem &t)
{
    const ColorScheme &cs = ctx.doc->colors;
    const QRectF r(QPointF(0, 0), t.rect.size());
    if (!t.fill.isNone()) p->fillRect(r, t.fill.brush(r, cs, lookupFor(*ctx.doc)));
    for (int row = 0; row < t.rows; ++row)
        for (int col = 0; col < t.cols; ++col) {
            const TableCell &c = t.cell(row, col);
            if (c.covered) continue;
            const QRectF cr = t.cellRect(row, col);
            if (!c.fill.isNone()) p->fillRect(cr, c.fill.brush(cr, cs, lookupFor(*ctx.doc)));
        }
    if (!ctx.opt.output && ctx.opt.tableGridlines) {
        QPen g(QColor(160, 160, 160), 0);
        g.setStyle(Qt::DotLine);
        p->setPen(g);
        p->setBrush(Qt::NoBrush);
        for (int row = 0; row < t.rows; ++row)
            for (int col = 0; col < t.cols; ++col)
                if (!t.cell(row, col).covered) p->drawRect(t.cellRect(row, col));
    }
    for (int row = 0; row < t.rows; ++row)
        for (int col = 0; col < t.cols; ++col) {
            const TableCell &c = t.cell(row, col);
            if (c.covered || c.storyId.isEmpty()) continue;
            const QRectF cr = t.cellRect(row, col);
            QPointF origin;
            const StoryLayout *lay = Renderer::cellLayout(ctx, t, row, col, &origin);
            if (!lay) continue;
            PaintOptions po;
            po.shadeFields = ctx.opt.shadeFields;
            po.showSpecial = ctx.opt.showSpecial;
            if (ctx.opt.editStory == c.storyId) { po.selFrom = ctx.opt.selFrom; po.selTo = ctx.opt.selTo; }
            p->save();
            p->translate(cr.topLeft());
            p->setClipRect(QRectF(QPointF(0, 0), cr.size()), Qt::IntersectClip);
            lay->paint(p, 0, po);
            paintInlineObjects(p, ctx, *lay, 0);
            p->restore();
        }
    // Borders and diagonals on top.
    for (int row = 0; row < t.rows; ++row)
        for (int col = 0; col < t.cols; ++col) {
            const TableCell &c = t.cell(row, col);
            if (c.covered) continue;
            const QRectF cr = t.cellRect(row, col);
            auto edge = [&](const Stroke &s, QPointF a, QPointF b) {
                if (s.isNone()) return;
                QPainterPath pp;
                pp.moveTo(a);
                pp.lineTo(b);
                Renderer::strokePath(p, pp, s, cs);
            };
            edge(c.border.top, cr.topLeft(), cr.topRight());
            edge(c.border.bottom, cr.bottomLeft(), cr.bottomRight());
            edge(c.border.left, cr.topLeft(), cr.bottomLeft());
            edge(c.border.right, cr.topRight(), cr.bottomRight());
            if (c.diagonal == 1) edge(c.border.top.isNone() ? Stroke::line(ColorRef::scheme(Main), 0.75) : c.border.top, cr.topLeft(), cr.bottomRight());
            if (c.diagonal == 2) edge(c.border.top.isNone() ? Stroke::line(ColorRef::scheme(Main), 0.75) : c.border.top, cr.bottomLeft(), cr.topRight());
        }
    Renderer::strokePath(p, [&] { QPainterPath pp; pp.addRect(r); return pp; }(), t.stroke, cs);
}

static void paintTextArt(QPainter *p, const PaintContext &ctx, const TextArtItem &w)
{
    const ColorScheme &cs = ctx.doc->colors;
    const QPainterPath path = Renderer::textArtOutline(w);
    if (!w.fill.isNone()) p->fillPath(path, w.fill.brush(QRectF(QPointF(0, 0), w.rect.size()), cs, lookupFor(*ctx.doc)));
    Stroke s = w.stroke;
    s.join = Qt::RoundJoin;
    Renderer::strokePath(p, path, s, cs);
}

static void paintContent(QPainter *p, const PaintContext &ctx, const Item &it)
{
    switch (it.type()) {
    case ItemType::Text: paintText(p, ctx, static_cast<const TextItem &>(it)); break;
    case ItemType::Picture: paintPicture(p, ctx, static_cast<const PictureItem &>(it)); break;
    case ItemType::Shape: paintShape(p, ctx, static_cast<const ShapeItem &>(it)); break;
    case ItemType::Table: paintTable(p, ctx, static_cast<const TableItem &>(it)); break;
    case ItemType::TextArt: paintTextArt(p, ctx, static_cast<const TextArtItem &>(it)); break;
    default: break;
    }
}

// Renders an item (local coordinates, no effects) into an image with padding.
static QImage itemImage(const PaintContext &ctx, const Item &it, double sc, double pad, QRectF *localRect)
{
    const QRectF lr = QRectF(QPointF(0, 0), it.rect.size()).adjusted(-pad, -pad, pad, pad);
    *localRect = lr;
    const QSize px = (lr.size() * sc).toSize().expandedTo(QSize(1, 1)).boundedTo(QSize(6000, 6000));
    QImage img(px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter ip(&img);
    ip.setRenderHint(QPainter::Antialiasing);
    ip.setRenderHint(QPainter::SmoothPixmapTransform);
    ip.scale(px.width() / lr.width(), px.height() / lr.height());
    ip.translate(-lr.topLeft());
    paintContent(&ip, ctx, it);
    ip.end();
    return img;
}

static QImage colorize(QImage img, const QColor &c)
{
    QPainter cp(&img);
    cp.setCompositionMode(QPainter::CompositionMode_SourceIn);
    cp.fillRect(img.rect(), c);
    cp.end();
    return img;
}

void Renderer::paintItem(QPainter *p, const PaintContext &ctx, const Item &it)
{
    if (ctx.opt.hidden.contains(it.id)) return;
    if (it.type() == ItemType::Group) {
        for (const auto &c : static_cast<const GroupItem &>(it).children) paintItem(p, ctx, *c);
        return;
    }
    if (it.type() == ItemType::Line) {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (it.fx.shadow.on) {
            p->save();
            p->translate(it.fx.shadow.offset());
            LineItem sh = static_cast<const LineItem &>(it);
            QColor sc = it.fx.shadow.color.resolve(ctx.doc->colors);
            sc.setAlphaF(float(1 - it.fx.shadow.transparency));
            sh.stroke.color = ColorRef::rgb(sc);
            paintLine(p, ctx, sh);
            p->restore();
        }
        paintLine(p, ctx, static_cast<const LineItem &>(it));
        p->restore();
        return;
    }
    const ColorScheme &cs = ctx.doc->colors;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::TextAntialiasing);
    p->setTransform(it.transform(), true);
    const Effects &fx = it.fx;
    const bool raster = fx.softEdge > 0 || fx.rot3d.x || fx.rot3d.y;
    if (fx.shadow.on || fx.glow.on || fx.reflection.on || raster) {
        const double sc = std::clamp(deviceScale(p), 0.5, 4.0);
        const double pad = std::max({fx.shadow.blur * 2 + fx.shadow.distance, fx.glow.size * 2, 4.0});
        QRectF lr;
        QImage img = itemImage(ctx, it, sc, pad, &lr);
        if (fx.glow.on) {
            QImage g = img;
            boxBlur(g, std::max(1, int(fx.glow.size * sc / 2)));
            // Boost alpha so the glow spreads.
            g = g.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < g.height(); ++y) {
                QRgb *l = reinterpret_cast<QRgb *>(g.scanLine(y));
                for (int x = 0; x < g.width(); ++x) l[x] = qRgba(0, 0, 0, std::min(255, qAlpha(l[x]) * 3));
            }
            QColor gc = fx.glow.color.resolve(cs);
            gc.setAlphaF(float(1 - fx.glow.transparency));
            p->drawImage(lr, colorize(g.convertToFormat(QImage::Format_ARGB32_Premultiplied), gc));
        }
        if (fx.shadow.on) {
            QImage s = img;
            boxBlur(s, std::max(0, int(fx.shadow.blur * sc / 2)));
            QColor c = fx.shadow.color.resolve(cs);
            c.setAlphaF(float(1 - fx.shadow.transparency));
            // Keep the shadow direction fixed on the page regardless of rotation/flip.
            QTransform inv;
            inv.scale(it.flipH ? -1 : 1, it.flipV ? -1 : 1);
            inv.rotate(-it.rotation);
            const QPointF off = inv.map(fx.shadow.offset());
            p->drawImage(lr.translated(off), colorize(s, c));
        }
        if (fx.reflection.on) {
            QImage rimg = img.mirrored(false, true);
            if (fx.reflection.blur > 0) boxBlur(rimg, int(fx.reflection.blur * sc));
            QImage mask(rimg.size(), QImage::Format_ARGB32_Premultiplied);
            mask.fill(Qt::transparent);
            {
                QPainter mp(&mask);
                QLinearGradient lg(0, 0, 0, rimg.height() * fx.reflection.size);
                lg.setColorAt(0, QColor(0, 0, 0, int(255 * (1 - fx.reflection.transparency))));
                lg.setColorAt(1, QColor(0, 0, 0, 0));
                mp.fillRect(mask.rect(), lg);
            }
            QPainter rp(&rimg);
            rp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            rp.drawImage(0, 0, mask);
            rp.end();
            const QRectF rr(lr.left(), it.rect.height() + fx.reflection.distance - (lr.bottom() - it.rect.height()), lr.width(), lr.height());
            p->drawImage(rr, rimg);
        }
        if (raster) {
            QImage body = img;
            if (fx.softEdge > 0) {
                QImage m = img;
                boxBlur(m, std::max(1, int(fx.softEdge * sc / 2)));
                m = m.convertToFormat(QImage::Format_ARGB32);
                body = body.convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < body.height(); ++y) {
                    QRgb *b = reinterpret_cast<QRgb *>(body.scanLine(y));
                    const QRgb *mm = reinterpret_cast<const QRgb *>(m.constScanLine(y));
                    for (int x = 0; x < body.width(); ++x) {
                        const int ma = std::clamp((qAlpha(mm[x]) - 128) * 2, 0, 255);
                        b[x] = qRgba(qRed(b[x]), qGreen(b[x]), qBlue(b[x]), qAlpha(b[x]) * ma / 255);
                    }
                }
            }
            if (fx.rot3d.x || fx.rot3d.y) {
                const double ay = qDegreesToRadians(fx.rot3d.y), ax = qDegreesToRadians(fx.rot3d.x);
                const double persp = 0.25 + fx.rot3d.perspective / 200.0;
                const double cw = std::cos(ay), ch = std::cos(ax);
                const double sy = std::sin(ay) * persp, sx = std::sin(ax) * persp;
                const QPointF c = lr.center();
                const double hw = lr.width() / 2 * cw, hh = lr.height() / 2 * ch;
                QPolygonF dst;
                dst << c + QPointF(-hw * (1 + sx), -hh * (1 + sy)) << c + QPointF(hw * (1 + sx), -hh * (1 - sy))
                    << c + QPointF(hw * (1 - sx), hh * (1 - sy)) << c + QPointF(-hw * (1 - sx), hh * (1 + sy));
                QTransform q;
                if (QTransform::quadToQuad(QPolygonF(QRectF(0, 0, body.width(), body.height())), dst, q)) {
                    p->save();
                    p->setTransform(q, true);
                    p->drawImage(QPointF(0, 0), body);
                    p->restore();
                }
            } else {
                p->drawImage(lr, body);
            }
        }
    }
    if (!raster) paintContent(p, ctx, it);
    if (fx.bevel.type && !raster) {
        const QPainterPath sil = silhouette(*ctx.doc, it);
        p->save();
        p->setClipPath(sil, Qt::IntersectClip);
        const QRectF b = sil.boundingRect();
        QLinearGradient lg(b.topLeft(), b.bottomRight());
        lg.setColorAt(0, QColor(255, 255, 255, 170));
        lg.setColorAt(0.5, QColor(255, 255, 255, 0));
        lg.setColorAt(0.5001, QColor(0, 0, 0, 0));
        lg.setColorAt(1, QColor(0, 0, 0, 120));
        QPen pen(QBrush(lg), fx.bevel.width * 2);
        pen.setJoinStyle(Qt::RoundJoin);
        p->strokePath(sil, pen);
        p->restore();
    }
    p->restore();
}

void Renderer::paintItems(QPainter *p, const PaintContext &ctx, const ItemList &items)
{
    for (const auto &it : items) paintItem(p, ctx, *it);
}

void Renderer::paintBackground(QPainter *p, const PaintContext &ctx, const Fill &bg, const QRectF &r)
{
    if (bg.type == Fill::NoFill) return;
    p->fillRect(r, bg.brush(r, ctx.doc->colors, lookupFor(*ctx.doc)));
}

void Renderer::paintMaster(QPainter *p, const PaintContext &ctx, const MasterPage &m, double xOffset)
{
    p->save();
    if (m.twoPage) p->setClipRect(QRectF(QPointF(0, 0), ctx.doc->pageSize()), Qt::IntersectClip);
    p->translate(xOffset, 0);
    paintItems(p, ctx, m.items);
    p->restore();
}

// Catalog merge: the catalog area's other cells repeat the objects in its
// first cell, each with the next record; while designing on screen they show
// faded, with the field names.
static void paintCatalogCells(QPainter *p, const PaintContext &c, const Page &pg)
{
    const Document &doc = *c.doc;
    const CatalogArea &cat = doc.catalog;
    if (!cat.isActive() || pg.id != cat.pageId) return;
    ItemList tmpl;
    for (const auto &it : pg.items)
        if (cat.inTemplate(it->bounds())) tmpl.push_back(it);
    if (tmpl.empty()) return;
    const QVector<int> rows = doc.merge.includedRows();
    const bool preview = c.opt.mergeRecord >= 0 && !rows.isEmpty();
    if (!preview && c.opt.output) return;
    const int start = preview ? std::max(0, int(rows.indexOf(c.opt.mergeRecord))) : 0;
    for (int k = 1; k < cat.perPage(); ++k) {
        PaintContext ck = c;
        ck.opt.editStory.clear();
        if (preview) {
            if (start + k >= rows.size()) break;
            ck.opt.mergeRecord = rows[start + k];
        }
        const QPointF d = cat.cell(k).topLeft() - cat.cell(0).topLeft();
        p->save();
        if (!preview) p->setOpacity(0.35);
        p->translate(d);
        Renderer::paintItems(p, ck, tmpl);
        p->restore();
    }
}

void Renderer::paintPage(QPainter *p, const PaintContext &ctx, int pageIndex)
{
    const Document &doc = *ctx.doc;
    if (pageIndex < 0 || pageIndex >= doc.pages.size()) return;
    const Page &pg = *doc.pages[pageIndex];
    PaintContext c = ctx;
    c.pageNumber = pageIndex + 1;
    c.pageCount = doc.pages.size();
    const QRectF pr(QPointF(0, 0), doc.pageSize());
    MasterPage *m = doc.masterFor(pg);
    const Fill bg = pg.background.type != Fill::NoFill ? pg.background : (m ? m->background : Fill());
    paintBackground(p, c, bg, pr);
    if (m) {
        const bool right = (pageIndex + 1) % 2 == 1;
        paintMaster(p, c, *m, m->twoPage && right ? -doc.pageSize().width() : 0);
    }
    if (!ctx.opt.flattenTransparency) {
        paintItems(p, c, pg.items);
        paintCatalogCells(p, c, pg);
        return;
    }
    // Flattened output: transparent items become opaque patches holding the
    // page beneath them with the item blended on top; the rest stays vector.
    const double scale = ctx.opt.flattenDpi / 72.0;
    for (size_t k = 0; k < pg.items.size(); ++k) {
        const Item &it = *pg.items[k];
        if (ctx.opt.hidden.contains(it.id)) continue;
        if (!usesTransparency(doc, it)) {
            paintItem(p, c, it);
            continue;
        }
        const double margin = it.fx.any() ? 40 : 4;
        QRectF br = it.bounds().adjusted(-margin, -margin, margin, margin).intersected(pr);
        if (br.isEmpty()) continue;
        // Snap to whole patch pixels so neighboring content lines up.
        br = QRectF(std::floor(br.left() * scale) / scale, std::floor(br.top() * scale) / scale,
                    std::ceil(br.width() * scale) / scale, std::ceil(br.height() * scale) / scale);
        QImage patch(QSize(int(std::lround(br.width() * scale)), int(std::lround(br.height() * scale))).expandedTo(QSize(1, 1)),
                     QImage::Format_RGB32);
        patch.fill(Qt::white);
        {
            QPainter pp(&patch);
            pp.setRenderHint(QPainter::Antialiasing);
            pp.setRenderHint(QPainter::TextAntialiasing);
            pp.setRenderHint(QPainter::SmoothPixmapTransform);
            pp.scale(scale, scale);
            pp.translate(-br.topLeft());
            PaintContext under = c;
            under.opt.flattenTransparency = false;
            paintBackground(&pp, under, bg, pr);
            if (m) paintMaster(&pp, under, *m, m->twoPage && (pageIndex + 1) % 2 == 1 ? -doc.pageSize().width() : 0);
            for (size_t j = 0; j <= k; ++j) paintItem(&pp, under, *pg.items[j]);
        }
        p->drawImage(br, patch);
    }
}

bool Renderer::usesTransparency(const Document &doc, const Item &it)
{
    if (it.fx.any()) return true;
    if (it.fill.transparency > 0 || it.stroke.transparency > 0) return true;
    for (const GradientStop &st : it.fill.stops)
        if (st.transparency > 0) return true;
    switch (it.type()) {
    case ItemType::Group:
        for (const auto &ch : static_cast<const GroupItem &>(it).children)
            if (usesTransparency(doc, *ch)) return true;
        return false;
    case ItemType::Picture: {
        const auto &pic = static_cast<const PictureItem &>(it);
        if (pic.transparency > 0 || pic.hasTransparentColor) return true;
        if (pic.imageId.isEmpty()) return false;
        // Cache per picture whether any pixel is see-through.
        static QHash<QString, bool> seeThrough;
        const QString key = pic.imageId + QString::number(doc.images.value(pic.imageId).bytes.size());
        auto f = seeThrough.constFind(key);
        if (f != seeThrough.constEnd()) return *f;
        const QImage img = doc.image(pic.imageId);
        bool any = false;
        if (img.hasAlphaChannel()) {
            const QImage a = img.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < a.height() && !any; ++y) {
                const QRgb *line = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                for (int x = 0; x < a.width(); ++x)
                    if (qAlpha(line[x]) < 255) { any = true; break; }
            }
        }
        seeThrough.insert(key, any);
        return any;
    }
    default:
        return false;
    }
}

QImage Renderer::renderToImage(const PaintContext &ctx, int pageIndex, double scale, bool transparent)
{
    const QSizeF ps = ctx.doc->pageSize();
    QImage img((ps * scale).toSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(transparent ? Qt::transparent : Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.scale(scale, scale);
    paintPage(&p, ctx, pageIndex);
    p.end();
    return img;
}

QImage Renderer::renderItemsToImage(const PaintContext &ctx, const ItemList &items, double scale)
{
    const QRectF b = unionBounds(items).adjusted(-4, -4, 4, 4);
    QImage img((b.size() * scale).toSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(scale, scale);
    p.translate(-b.topLeft());
    paintItems(&p, ctx, items);
    p.end();
    return img;
}

} // namespace jp
