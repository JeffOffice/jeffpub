#include "render/metafile.h"

#include "core/fonts.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QStringDecoder>
#include <QtEndian>
#include <QtMath>
#include <cmath>

namespace jp {

namespace {

// Reads little-endian values from a picture's bytes, never outside them: a
// read past the end gives 0 and stops further reads, and moves by lengths
// from the file go through skip(), which never goes backwards.
struct Reader {
    const uchar *d = nullptr;
    qsizetype n = 0, p = 0;
    bool ok(qsizetype k) const { return p >= 0 && k >= 0 && p + k <= n; }
    void skip(qsizetype k) { if (ok(k)) p += k; else p = n; }
    quint8 u8() { return ok(1) ? d[p++] : 0; }
    quint16 u16() { if (!ok(2)) { p = n; return 0; } quint16 v = qFromLittleEndian<quint16>(d + p); p += 2; return v; }
    qint16 s16() { return qint16(u16()); }
    quint32 u32() { if (!ok(4)) { p = n; return 0; } quint32 v = qFromLittleEndian<quint32>(d + p); p += 4; return v; }
    qint32 s32() { return qint32(u32()); }
    float f32() { quint32 v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
};

QColor colorRef(quint32 c) { return QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF); }

struct Obj {
    enum Kind { None, Pen, Brush, Font, Other } kind = None;
    QPen pen;
    QBrush brush;
    QFont font;
    double height = 12;
    double escapement = 0;
    bool symbol = false;
};

struct DC {
    QPen pen = QPen(Qt::black, 0);
    QBrush brush = QBrush(Qt::white);
    QFont font;
    double fontHeight = 12;
    double escapement = 0;
    bool symbolFont = false;
    QColor textColor = Qt::black, bkColor = Qt::white;
    int bkMode = 2;
    Qt::FillRule fill = Qt::OddEvenFill;
    quint32 textAlign = 0;
    QPointF cur;
    QPointF winOrg, vpOrg;
    QSizeF winExt{1, 1}, vpExt{1, 1};
    bool winExtSet = false, vpExtSet = false;
    int mapMode = 1;
    QTransform world;
    int arcDir = 1;   // 1 counter-clockwise, 2 clockwise
};

QPen makePen(quint32 style, double width, const QColor &c)
{
    const quint32 s = style & 0x0F;
    if (s == 5) return Qt::NoPen;
    QPen p(c, width);
    if (width <= 1) p.setCosmetic(width <= 0), p.setWidthF(width <= 0 ? 1 : width);
    switch (s) {
    case 1: p.setStyle(Qt::DashLine); break;
    case 2: p.setStyle(Qt::DotLine); break;
    case 3: p.setStyle(Qt::DashDotLine); break;
    case 4: p.setStyle(Qt::DashDotDotLine); break;
    default: break;
    }
    const quint32 cap = style & 0x0F00, join = style & 0xF000;
    p.setCapStyle(cap == 0x100 ? Qt::SquareCap : cap == 0x200 ? Qt::FlatCap : Qt::RoundCap);
    p.setJoinStyle(join == 0x1000 ? Qt::BevelJoin : join == 0x2000 ? Qt::MiterJoin : Qt::RoundJoin);
    return p;
}

QBrush makeBrush(quint32 style, const QColor &c, quint32 hatch)
{
    switch (style) {
    case 1: return Qt::NoBrush;
    case 2: {
        static const Qt::BrushStyle h[] = {Qt::HorPattern, Qt::VerPattern, Qt::FDiagPattern, Qt::BDiagPattern, Qt::CrossPattern, Qt::DiagCrossPattern};
        return QBrush(c, hatch < 6 ? h[hatch] : Qt::SolidPattern);
    }
    default: return QBrush(c);
    }
}

// Wraps a device-independent bitmap into a BMP file so QImage can decode it.
QImage dibToImage(const uchar *bmi, qsizetype bmiLen, const uchar *bits, qsizetype bitsLen)
{
    if (bmiLen < 12) return {};
    const quint32 hsz = qFromLittleEndian<quint32>(bmi);
    quint32 colors = 0;
    if (hsz >= 40 && bmiLen >= 40) {
        const quint16 bpp = qFromLittleEndian<quint16>(bmi + 14);
        const quint32 compression = qFromLittleEndian<quint32>(bmi + 16);
        colors = qFromLittleEndian<quint32>(bmi + 32);
        if (!colors && bpp <= 8) colors = 1u << bpp;
        if (compression == 3 && hsz == 40) colors += 3;   // bit masks
    } else if (hsz == 12) {
        const quint16 bpp = qFromLittleEndian<quint16>(bmi + 10);
        if (bpp <= 8) colors = 1u << bpp;
    }
    QByteArray file("BM");
    const quint32 headerLen = quint32(bmiLen);
    const quint32 off = 14 + headerLen;
    file.resize(14);
    qToLittleEndian<quint32>(quint32(off + bitsLen), file.data() + 2);
    qToLittleEndian<quint32>(0, file.data() + 6);
    qToLittleEndian<quint32>(off, file.data() + 10);
    file.append(reinterpret_cast<const char *>(bmi), bmiLen);
    file.append(reinterpret_cast<const char *>(bits), bitsLen);
    Q_UNUSED(colors);
    return QImage::fromData(file, "BMP");
}

// Splits a DIB that follows a WMF record header into header and bits.
QImage dibFromPacked(const uchar *d, qsizetype len)
{
    if (len < 40) return {};
    const quint32 hsz = qFromLittleEndian<quint32>(d);
    if (hsz > quint32(len)) return {};
    quint32 colors = 0, masks = 0;
    if (hsz >= 40) {
        const quint16 bpp = qFromLittleEndian<quint16>(d + 14);
        const quint32 compression = qFromLittleEndian<quint32>(d + 16);
        colors = qFromLittleEndian<quint32>(d + 32);
        if (!colors && bpp <= 8) colors = 1u << bpp;
        if (compression == 3 && hsz == 40) masks = 12;
    } else if (hsz == 12) {
        const quint16 bpp = qFromLittleEndian<quint16>(d + 10);
        if (bpp <= 8) colors = 1u << bpp;
        const qsizetype bmiLen = hsz + colors * 3;
        return dibToImage(d, std::min(bmiLen, len), d + bmiLen, std::max<qsizetype>(0, len - bmiLen));
    }
    const qsizetype bmiLen = qsizetype(hsz) + masks + qsizetype(colors) * 4;   // colors comes from the file
    if (bmiLen > len) return {};
    return dibToImage(d, bmiLen, d + bmiLen, len - bmiLen);
}

class Player {
public:
    Player(QPainter *p, bool emf) : m_p(p), m_emf(emf) {}
    QTransform base;           // device space -> target
    std::function<QColor(const QColor &)> recolor;   // every color drawn, changed (or none)
    QRectF wmfWindowFallback;  // placeable bbox

    void play(const QByteArray &data)
    {
        if (recolor) { m_dc.textColor = recolor(m_dc.textColor); m_dc.bkColor = recolor(m_dc.bkColor); }
        if (m_emf) playEmf(data); else playWmf(data);
    }

private:
    QColor mapped(const QColor &c) const { return recolor ? recolor(c) : c; }
    QColor col(quint32 v) const { return mapped(colorRef(v)); }
    QPainter *m_p;
    bool m_emf;
    DC m_dc;
    QVector<DC> m_stack;
    QVector<Obj> m_objs;
    QPainterPath m_path;
    bool m_inPath = false;
    double m_devPerMmX = 3.78, m_devPerMmY = 3.78;

    QTransform logical() const
    {
        QTransform t;
        if (m_emf) {
            t = m_dc.world;
            QTransform page;
            switch (m_dc.mapMode) {
            case 7: case 8: {
                const double sx = m_dc.winExt.width() ? m_dc.vpExt.width() / m_dc.winExt.width() : 1;
                double sy = m_dc.winExt.height() ? m_dc.vpExt.height() / m_dc.winExt.height() : 1;
                double sxx = sx;
                if (m_dc.mapMode == 7) { const double s = std::min(std::abs(sx), std::abs(sy)); sxx = sx < 0 ? -s : s; sy = sy < 0 ? -s : s; }
                page.translate(m_dc.vpOrg.x(), m_dc.vpOrg.y());
                page.scale(sxx, sy);
                page.translate(-m_dc.winOrg.x(), -m_dc.winOrg.y());
                break;
            }
            case 2: case 3: case 4: case 5: case 6: {
                static const double mmPerUnit[] = {0, 0, 0.1, 0.01, 0.254, 0.0254, 25.4 / 1440.0};
                const double u = mmPerUnit[m_dc.mapMode];
                page.translate(m_dc.vpOrg.x(), m_dc.vpOrg.y());
                page.scale(u * m_devPerMmX, -u * m_devPerMmY);
                page.translate(-m_dc.winOrg.x(), -m_dc.winOrg.y());
                break;
            }
            default:
                page.translate(m_dc.vpOrg.x() - m_dc.winOrg.x(), m_dc.vpOrg.y() - m_dc.winOrg.y());
                break;
            }
            return t * page * base;
        }
        // WMF: map the logical window straight onto the target.
        QRectF win = m_dc.winExtSet ? QRectF(m_dc.winOrg, m_dc.winExt) : wmfWindowFallback;
        if (win.width() == 0 || win.height() == 0) win = wmfWindowFallback;
        QTransform w;
        w.translate(-win.x(), -win.y());
        QTransform s = QTransform::fromScale(1.0 / win.width(), 1.0 / win.height());
        return w * s * base;
    }

    void apply()
    {
        m_p->setTransform(logical());
        m_p->setPen(m_dc.pen);
        m_p->setBrush(m_dc.brush);
    }

    void fillAndStroke(const QPainterPath &path, bool fill = true, bool stroke = true)
    {
        if (m_inPath) { m_path.addPath(path); return; }
        apply();
        QPainterPath pp = path;
        pp.setFillRule(m_dc.fill);
        if (fill && m_dc.brush.style() != Qt::NoBrush) {
            if (m_dc.brush.style() != Qt::SolidPattern && m_dc.bkMode == 2) m_p->fillPath(pp, m_dc.bkColor);
            m_p->fillPath(pp, m_dc.brush);
        }
        if (stroke && m_dc.pen.style() != Qt::NoPen) m_p->strokePath(pp, m_dc.pen);
    }

    QPainterPath polygonPath(const QVector<QPointF> &pts, bool close)
    {
        QPainterPath pp;
        if (pts.isEmpty()) return pp;
        pp.moveTo(pts[0]);
        for (int i = 1; i < pts.size(); ++i) pp.lineTo(pts[i]);
        if (close) pp.closeSubpath();
        return pp;
    }

    QPainterPath arcPath(const QRectF &box, QPointF start, QPointF end, int kind /*0 arc,1 pie,2 chord*/)
    {
        const QPointF c = box.center();
        auto ang = [&](QPointF p) { return qRadiansToDegrees(std::atan2(-(p.y() - c.y()) * box.width(), (p.x() - c.x()) * box.height())); };
        double a0 = ang(start), a1 = ang(end);
        double sweep = a1 - a0;
        if (m_dc.arcDir == 2) { while (sweep > 0) sweep -= 360; } else { while (sweep <= 0) sweep += 360; }
        QPainterPath pp;
        if (kind == 1) pp.moveTo(c);
        else pp.arcMoveTo(box, a0);
        pp.arcTo(box, a0, sweep);
        if (kind) pp.closeSubpath();
        return pp;
    }

    void selectObject(quint32 idx)
    {
        if (m_emf && (idx & 0x80000000u)) {
            switch (idx & 0x7FFFFFFF) {
            case 0: m_dc.brush = QBrush(Qt::white); break;
            case 1: m_dc.brush = QBrush(mapped(QColor(192, 192, 192))); break;
            case 2: m_dc.brush = QBrush(mapped(QColor(128, 128, 128))); break;
            case 3: m_dc.brush = QBrush(mapped(QColor(64, 64, 64))); break;
            case 4: m_dc.brush = QBrush(Qt::black); break;
            case 5: m_dc.brush = Qt::NoBrush; break;
            case 6: m_dc.pen = QPen(Qt::white, 0); break;
            case 7: m_dc.pen = QPen(Qt::black, 0); break;
            case 8: m_dc.pen = Qt::NoPen; break;
            default: break;
            }
            return;
        }
        if (idx >= quint32(m_objs.size())) return;
        const Obj &o = m_objs[idx];
        switch (o.kind) {
        case Obj::Pen: m_dc.pen = o.pen; break;
        case Obj::Brush: m_dc.brush = o.brush; break;
        case Obj::Font: m_dc.font = o.font; m_dc.fontHeight = o.height; m_dc.escapement = o.escapement; m_dc.symbolFont = o.symbol; break;
        default: break;
        }
    }

    int newSlot()
    {
        for (int i = 0; i < m_objs.size(); ++i)
            if (m_objs[i].kind == Obj::None) return i;
        m_objs.push_back(Obj());
        return m_objs.size() - 1;
    }

    void setSlot(quint32 i, const Obj &o)
    {
        if (i > 100000) return;
        if (int(i) >= m_objs.size()) m_objs.resize(i + 1);
        m_objs[i] = o;
    }

    Obj fontObj(qint32 height, qint32 escapement, qint32 weight, bool italic, bool underline, bool strike, quint8 charset, const QString &face)
    {
        Obj o;
        o.kind = Obj::Font;
        o.font = QFont(face.isEmpty() ? QStringLiteral("Arial") : face);
        o.font.setWeight(QFont::Weight(std::clamp(weight ? weight : 400, 100, 900)));
        o.font.setItalic(italic);
        o.font.setUnderline(underline);
        o.font.setStrikeOut(strike);
        // Negative heights are character (em) heights; positive include internal leading.
        o.height = height < 0 ? -double(height) : height * 0.82;
        if (o.height <= 0) o.height = 12;
        o.escapement = escapement / 10.0;
        o.symbol = charset == 2;
        return o;
    }

    // Text in a symbol font the computer lacks (Windows has Wingdings, Webdings,
    // and Symbol; Linux has none): the pictures JeffPub knows come from
    // Unicode, and the rest from the plain codes a stand-in for Symbol has
    // them at, rather than from 0xF020-0xF0FF, where only the real fonts do.
    QString symbolPictures(const QString &s) const
    {
        const QString family = m_dc.font.family();
        if (!isSymbolFont(family) || QFontDatabase::hasFamily(family)) return s;
        QString out;
        for (QChar c : s) {
            const QString picture = symbolToUnicode(family, c.unicode());
            if (!picture.isEmpty()) out += picture;
            else out += c.unicode() >= 0xF020 && c.unicode() <= 0xF0FF ? QChar(c.unicode() - 0xF000) : c;
        }
        return out;
    }

    void text(QPointF pos, const QString &shownText, const QVector<double> &dx)
    {
        if (shownText.isEmpty()) return;
        const QString s = symbolPictures(shownText);
        apply();
        const QTransform t = logical();
        m_p->save();
        m_p->setTransform(t);
        const quint32 ta = m_dc.textAlign;
        const QPointF at = (ta & 1) ? m_dc.cur : pos;
        m_p->translate(at);
        // Keep text upright even when the mapping flips an axis.
        const bool flipY = t.m22() < 0 || (t.m22() == 0 && t.m21() > 0);
        const bool flipX = t.m11() < 0;
        m_p->scale(flipX ? -1 : 1, flipY ? -1 : 1);
        if (m_dc.escapement) m_p->rotate(-m_dc.escapement);
        QFont f = m_dc.font;
        f.setPixelSize(std::max(1, int(std::lround(m_dc.fontHeight))));
        if (m_dc.fontHeight < 4) f.setPointSizeF(m_dc.fontHeight * 0.75);
        m_p->setFont(f);
        const QFontMetricsF fm(f);
        double w = 0;
        if (!dx.isEmpty()) for (double v : dx) w += v; else w = fm.horizontalAdvance(s);
        double x = 0, y = 0;
        const quint32 h = ta & 6;
        if (h == 6) x = -w / 2; else if (h == 2) x = -w;
        const quint32 v = ta & 24;
        if (v == 0) y = fm.ascent(); else if (v == 8) y = -fm.descent();
        m_p->setPen(m_dc.textColor);
        if (!dx.isEmpty() && dx.size() == s.size()) {
            double cx = x;
            for (int i = 0; i < s.size(); ++i) { m_p->drawText(QPointF(cx, y), s.mid(i, 1)); cx += dx[i]; }
        } else {
            m_p->drawText(QPointF(x, y), s);
        }
        m_p->restore();
        if (ta & 1) m_dc.cur += QPointF(w, 0);
    }

    void image(const QImage &img, const QRectF &dst, const QRectF &src, quint32 rop)
    {
        if (img.isNull()) return;
        apply();
        m_p->save();
        QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
        if (rop == 0x008800C6) mode = QPainter::CompositionMode_Multiply;
        else if (rop == 0x00EE0086) mode = QPainter::CompositionMode_Lighten;
        else if (rop == 0x00660046) mode = QPainter::CompositionMode_Difference;
        if (m_p->paintEngine() && m_p->paintEngine()->hasFeature(QPaintEngine::BlendModes)) m_p->setCompositionMode(mode);
        else if (rop == 0x008800C6 || rop == 0x00660046) { m_p->restore(); return; }   // masks only make sense with blending
        m_p->setRenderHint(QPainter::SmoothPixmapTransform);
        QRectF s = src.isNull() ? QRectF(img.rect()) : src;
        // DIBs are bottom-up; QImage already flipped them. Source y counts from the top here.
        QImage shown = img;
        if (recolor) {
            shown = img.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < shown.height(); ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(shown.scanLine(y));
                for (int x = 0; x < shown.width(); ++x) {
                    const QColor c = recolor(QColor::fromRgb(line[x]));
                    line[x] = qRgba(c.red(), c.green(), c.blue(), qAlpha(line[x]));
                }
            }
        }
        m_p->drawImage(dst.normalized(), shown, s);
        m_p->restore();
    }

    // ---------------- WMF ----------------
    void playWmf(const QByteArray &data)
    {
        Reader r{reinterpret_cast<const uchar *>(data.constData()), data.size()};
        if (r.u32() == 0x9AC6CDD7u) r.p = 22; else r.p = 0;
        r.p += 18;   // standard header
        while (r.ok(6)) {
            const qsizetype start = r.p;
            const quint32 words = r.u32();
            const quint16 fn = r.u16();
            if (words < 3) break;
            const qsizetype end = start + qsizetype(words) * 2;
            if (end > r.n) break;
            Reader a{r.d, end, r.p};
            switch (fn) {
            case 0x0000: return;
            case 0x020B: { const double y = a.s16(), x = a.s16(); m_dc.winOrg = QPointF(x, y); break; }
            case 0x020C: { const double h = a.s16(), w = a.s16(); m_dc.winExt = QSizeF(w, h); m_dc.winExtSet = true; break; }
            case 0x0103: m_dc.mapMode = a.s16(); break;
            case 0x0201: m_dc.bkColor = col(a.u32()); break;
            case 0x0102: m_dc.bkMode = a.u16(); break;
            case 0x0106: m_dc.fill = a.u16() == 2 ? Qt::WindingFill : Qt::OddEvenFill; break;
            case 0x0209: m_dc.textColor = col(a.u32()); break;
            case 0x012E: m_dc.textAlign = a.u16(); break;
            case 0x001E: m_stack.push_back(m_dc); m_p->save(); break;
            case 0x0127: if (!m_stack.isEmpty()) { m_dc = m_stack.takeLast(); m_p->restore(); } break;
            case 0x02FA: {
                Obj o; o.kind = Obj::Pen;
                const quint16 style = a.u16(); const qint16 w = a.s16(); a.s16();
                o.pen = makePen(style, w, col(a.u32()));
                m_objs.size(); setSlot(newSlot(), o);
                break;
            }
            case 0x02FC: {
                Obj o; o.kind = Obj::Brush;
                const quint16 style = a.u16(); const quint32 c = a.u32(); const quint16 hatch = a.u16();
                o.brush = makeBrush(style, col(c), hatch);
                setSlot(newSlot(), o);
                break;
            }
            case 0x0142: case 0x01F9: case 0x00F7: case 0x06FF: {
                Obj o; o.kind = Obj::Other;
                if (fn == 0x0142) {
                    a.u16(); a.u16();
                    o.kind = Obj::Brush;
                    const QImage img = dibFromPacked(a.d + a.p, a.n - a.p);
                    o.brush = img.isNull() ? QBrush(Qt::gray) : QBrush(img);
                }
                setSlot(newSlot(), o);
                break;
            }
            case 0x02FB: {
                const qint16 h = a.s16(); a.s16(); const qint16 esc = a.s16(); a.s16(); const qint16 wt = a.s16();
                const quint8 it = a.u8(), ul = a.u8(), so = a.u8(), cs = a.u8();
                a.u8(); a.u8(); a.u8(); a.u8();
                QByteArray face;
                while (a.ok(1)) { const char c = char(a.u8()); if (!c) break; face += c; }
                setSlot(newSlot(), fontObj(h, esc, wt, it, ul, so, cs, QString::fromLatin1(face)));
                break;
            }
            case 0x012D: selectObject(a.u16()); break;
            case 0x01F0: { const quint16 i = a.u16(); if (i < m_objs.size()) m_objs[i] = Obj(); break; }
            case 0x0214: { const double y = a.s16(), x = a.s16(); m_dc.cur = QPointF(x, y); break; }
            case 0x0213: {
                const double y = a.s16(), x = a.s16();
                QPainterPath pp; pp.moveTo(m_dc.cur); pp.lineTo(x, y);
                fillAndStroke(pp, false, true);
                m_dc.cur = QPointF(x, y);
                break;
            }
            case 0x041B: case 0x0418: {
                const double b = a.s16(), rt = a.s16(), t = a.s16(), l = a.s16();
                QPainterPath pp;
                if (fn == 0x041B) pp.addRect(QRectF(QPointF(l, t), QPointF(rt, b)).normalized());
                else pp.addEllipse(QRectF(QPointF(l, t), QPointF(rt, b)).normalized());
                fillAndStroke(pp);
                break;
            }
            case 0x061C: {
                const double eh = a.s16(), ew = a.s16(), b = a.s16(), rt = a.s16(), t = a.s16(), l = a.s16();
                QPainterPath pp; pp.addRoundedRect(QRectF(QPointF(l, t), QPointF(rt, b)).normalized(), ew / 2, eh / 2);
                fillAndStroke(pp);
                break;
            }
            case 0x0817: case 0x081A: case 0x0830: {
                const double ye = a.s16(), xe = a.s16(), ys = a.s16(), xs = a.s16(), b = a.s16(), rt = a.s16(), t = a.s16(), l = a.s16();
                const int kind = fn == 0x0817 ? 0 : fn == 0x081A ? 1 : 2;
                fillAndStroke(arcPath(QRectF(QPointF(l, t), QPointF(rt, b)).normalized(), QPointF(xs, ys), QPointF(xe, ye), kind), kind != 0, true);
                break;
            }
            case 0x0324: case 0x0325: {
                const int nPts = a.s16();
                QVector<QPointF> pts;
                for (int i = 0; i < nPts && a.ok(4); ++i) { const double x = a.s16(), y = a.s16(); pts << QPointF(x, y); }
                fillAndStroke(polygonPath(pts, fn == 0x0324), fn == 0x0324, true);
                break;
            }
            case 0x0538: {
                const int nPoly = a.u16();
                QVector<int> counts;
                for (int i = 0; i < nPoly; ++i) counts << a.u16();
                QPainterPath pp;
                for (int c : counts) {
                    QVector<QPointF> pts;
                    for (int i = 0; i < c && a.ok(4); ++i) { const double x = a.s16(), y = a.s16(); pts << QPointF(x, y); }
                    pp.addPath(polygonPath(pts, true));
                }
                fillAndStroke(pp);
                break;
            }
            case 0x0521: {
                const int len = std::max(0, int(a.s16()));   // a negative length counts as none
                const QByteArray s(reinterpret_cast<const char *>(a.d + a.p), std::clamp<qsizetype>(len, 0, a.n - a.p));
                a.skip((len + 1) & ~1);
                const double y = a.s16(), x = a.s16();
                text(QPointF(x, y), decode(s), {});
                break;
            }
            case 0x0A32: {
                const double y = a.s16(), x = a.s16();
                const int len = std::max(0, int(a.s16()));   // a negative length counts as none
                const quint16 opts = a.u16();
                QRectF clip;
                if (opts & 0x06) { const double l = a.s16(), t = a.s16(), rr = a.s16(), b = a.s16(); clip = QRectF(QPointF(l, t), QPointF(rr, b)); }
                const QByteArray s(reinterpret_cast<const char *>(a.d + a.p), std::clamp<qsizetype>(len, 0, a.n - a.p));
                a.skip((len + 1) & ~1);
                QVector<double> dx;
                for (int i = 0; i < len && a.ok(2); ++i) dx << a.s16();
                if ((opts & 0x02) && !clip.isNull()) { apply(); m_p->fillRect(clip.normalized(), m_dc.bkColor); }
                text(QPointF(x, y), decode(s), dx);
                break;
            }
            case 0x0416: {
                const double b = a.s16(), rt = a.s16(), t = a.s16(), l = a.s16();
                apply();
                m_p->setClipRect(QRectF(QPointF(l, t), QPointF(rt, b)).normalized(), Qt::IntersectClip);
                break;
            }
            case 0x0B41: case 0x0F43: case 0x0940: {
                const quint32 rop = a.u32();
                qint16 srcH = 0, srcW = 0, srcY = 0, srcX = 0, dH, dW, dY, dX;
                if (fn == 0x0F43) { a.u16(); srcH = a.s16(); srcW = a.s16(); srcY = a.s16(); srcX = a.s16(); }
                else if (fn == 0x0B41) { srcY = a.s16(); srcX = a.s16(); srcH = a.s16(); srcW = a.s16(); }
                else { srcY = a.s16(); srcX = a.s16(); }
                dH = a.s16(); dW = a.s16(); dY = a.s16(); dX = a.s16();
                if (fn == 0x0940) { srcH = dH; srcW = dW; }
                const QImage img = dibFromPacked(a.d + a.p, a.n - a.p);
                if (!img.isNull()) {
                    const QRectF src(srcX, img.height() - srcY - srcH, srcW, srcH);
                    image(img, QRectF(dX, dY, dW, dH), src.intersected(QRectF(img.rect())).isEmpty() ? QRectF() : src, rop);
                } else if (rop == 0x00F00021) {
                    QPainterPath pp; pp.addRect(QRectF(dX, dY, dW, dH).normalized());
                    fillAndStroke(pp, true, false);
                }
                break;
            }
            default: break;
            }
            r.p = end;
        }
    }

    QString decode(const QByteArray &s) const
    {
        if (m_dc.symbolFont) {
            QString out;
            for (char c : s) out += QChar(0xF000 + uchar(c));
            return out;
        }
        QStringDecoder d("windows-1252");
        return d.isValid() ? d.decode(s) : QString::fromLatin1(s);
    }

    // ---------------- EMF ----------------
    void playEmf(const QByteArray &data)
    {
        Reader r{reinterpret_cast<const uchar *>(data.constData()), data.size()};
        while (r.ok(8)) {
            const qsizetype start = r.p;
            const quint32 type = r.u32();
            const quint32 size = r.u32();
            if (size < 8 || start + qsizetype(size) > r.n) break;
            Reader a{r.d, start + qsizetype(size), r.p};
            auto pt32 = [&]() { const double x = a.s32(), y = a.s32(); return QPointF(x, y); };
            auto pt16 = [&]() { const double x = a.s16(), y = a.s16(); return QPointF(x, y); };
            auto rect32 = [&]() { const double l = a.s32(), t = a.s32(), rr = a.s32(), b = a.s32(); return QRectF(QPointF(l, t), QPointF(rr, b)); };
            switch (type) {
            case 1: {   // header
                a.skip(16 + 16);   // bounds, frame
                a.u32(); a.u32(); a.u32(); a.u32(); a.u16(); a.u16();
                a.u32(); a.u32(); a.u32();
                const double devX = a.s32(), devY = a.s32(), mmX = a.s32(), mmY = a.s32();
                if (mmX > 0) m_devPerMmX = devX / mmX;
                if (mmY > 0) m_devPerMmY = devY / mmY;
                break;
            }
            case 14: return;
            case 9: { const double w = a.s32(), h = a.s32(); m_dc.winExt = QSizeF(w, h); m_dc.winExtSet = true; break; }
            case 10: m_dc.winOrg = pt32(); break;
            case 11: { const double w = a.s32(), h = a.s32(); m_dc.vpExt = QSizeF(w, h); m_dc.vpExtSet = true; break; }
            case 12: m_dc.vpOrg = pt32(); break;
            case 17: m_dc.mapMode = a.u32(); break;
            case 18: m_dc.bkMode = a.u32(); break;
            case 19: m_dc.fill = a.u32() == 2 ? Qt::WindingFill : Qt::OddEvenFill; break;
            case 22: m_dc.textAlign = a.u32(); break;
            case 24: m_dc.textColor = col(a.u32()); break;
            case 25: m_dc.bkColor = col(a.u32()); break;
            case 27: m_dc.cur = pt32(); if (m_inPath) m_path.moveTo(m_dc.cur); break;
            case 33: m_stack.push_back(m_dc); m_p->save(); break;
            case 34: {
                int rel = a.s32();
                while (rel++ < 0 && !m_stack.isEmpty()) { m_dc = m_stack.takeLast(); m_p->restore(); }
                break;
            }
            case 35: {
                const double m11 = a.f32(), m12 = a.f32(), m21 = a.f32(), m22 = a.f32(), dx = a.f32(), dy = a.f32();
                m_dc.world = QTransform(m11, m12, m21, m22, dx, dy);
                break;
            }
            case 36: {
                const double m11 = a.f32(), m12 = a.f32(), m21 = a.f32(), m22 = a.f32(), dx = a.f32(), dy = a.f32();
                const quint32 mode = a.u32();
                const QTransform x(m11, m12, m21, m22, dx, dy);
                if (mode == 1) m_dc.world = QTransform();
                else if (mode == 2) m_dc.world = x * m_dc.world;
                else if (mode == 3) m_dc.world = m_dc.world * x;
                else if (mode == 4) m_dc.world = x;
                break;
            }
            case 37: selectObject(a.u32()); break;
            case 38: {
                const quint32 ih = a.u32(), style = a.u32();
                const double w = a.s32(); a.s32();
                Obj o; o.kind = Obj::Pen; o.pen = makePen(style, w, col(a.u32()));
                setSlot(ih, o);
                break;
            }
            case 95: {
                const quint32 ih = a.u32();
                a.u32(); a.u32(); a.u32(); a.u32();
                const quint32 style = a.u32(), w = a.u32(); a.u32();
                const quint32 color = a.u32();
                Obj o; o.kind = Obj::Pen; o.pen = makePen(style, (style & 0x10000) ? w : 0, col(color));
                setSlot(ih, o);
                break;
            }
            case 39: {
                const quint32 ih = a.u32(), style = a.u32(), c = a.u32(), hatch = a.u32();
                Obj o; o.kind = Obj::Brush; o.brush = makeBrush(style, col(c), hatch);
                setSlot(ih, o);
                break;
            }
            case 93: case 94: {
                const quint32 ih = a.u32(); a.u32();
                const quint32 offBmi = a.u32(), cbBmi = a.u32(), offBits = a.u32(), cbBits = a.u32();
                Obj o; o.kind = Obj::Brush;
                if (start + offBmi + cbBmi <= a.n && start + offBits + cbBits <= a.n) {
                    const QImage img = dibToImage(a.d + start + offBmi, cbBmi, a.d + start + offBits, cbBits);
                    o.brush = img.isNull() ? QBrush(Qt::gray) : QBrush(img);
                }
                setSlot(ih, o);
                break;
            }
            case 82: {
                const quint32 ih = a.u32();
                const qint32 h = a.s32(); a.s32(); const qint32 esc = a.s32(); a.s32(); const qint32 wt = a.s32();
                const quint8 it = a.u8(), ul = a.u8(), so = a.u8(), cs = a.u8();
                a.u8(); a.u8(); a.u8(); a.u8();
                QString face;
                for (int i = 0; i < 32 && a.ok(2); ++i) { const quint16 c = a.u16(); if (!c) break; face += QChar(c); }
                setSlot(ih, fontObj(h, esc, wt, it, ul, so, cs, face));
                break;
            }
            case 40: { const quint32 ih = a.u32(); if (ih < quint32(m_objs.size())) m_objs[ih] = Obj(); break; }
            case 54: {
                const QPointF p2 = pt32();
                if (m_inPath) m_path.lineTo(p2);
                else { QPainterPath pp; pp.moveTo(m_dc.cur); pp.lineTo(p2); fillAndStroke(pp, false, true); }
                m_dc.cur = p2;
                break;
            }
            case 42: case 43: {
                const QRectF b = rect32().normalized();
                QPainterPath pp;
                if (type == 42) pp.addEllipse(b); else pp.addRect(b);
                fillAndStroke(pp);
                break;
            }
            case 44: {
                const QRectF b = rect32().normalized();
                const double w = a.s32(), h = a.s32();
                QPainterPath pp; pp.addRoundedRect(b, w / 2, h / 2);
                fillAndStroke(pp);
                break;
            }
            case 45: case 46: case 47: case 55: {
                const QRectF b = rect32().normalized();
                const QPointF s = pt32(), e = pt32();
                const int kind = type == 47 ? 1 : type == 46 ? 2 : 0;
                QPainterPath pp = arcPath(b, s, e, kind);
                if (type == 55) {
                    if (m_inPath) { m_path.lineTo(pp.pointAtPercent(0)); m_path.addPath(pp); }
                    else fillAndStroke(pp, false, true);
                    m_dc.cur = pp.currentPosition();
                } else fillAndStroke(pp, kind != 0, true);
                break;
            }
            case 57: m_dc.arcDir = a.u32(); break;
            case 2: case 3: case 4: case 5: case 6: case 85: case 86: case 87: case 88: case 89: {
                rect32();
                const quint32 count = a.u32();
                const bool s16 = type >= 85;
                QVector<QPointF> pts;
                for (quint32 i = 0; i < count && a.ok(s16 ? 4 : 8); ++i) pts << (s16 ? pt16() : pt32());
                const bool bezier = type == 2 || type == 5 || type == 85 || type == 88;
                const bool to = type == 5 || type == 6 || type == 88 || type == 89;
                QPainterPath pp;
                int i0 = 0;
                if (to) pp.moveTo(m_dc.cur);
                else if (!pts.isEmpty()) { pp.moveTo(pts[0]); i0 = 1; }
                if (bezier) { for (int i = i0; i + 2 < pts.size(); i += 3) pp.cubicTo(pts[i], pts[i + 1], pts[i + 2]); }
                else for (int i = i0; i < pts.size(); ++i) pp.lineTo(pts[i]);
                const bool polygon = type == 3 || type == 86;
                if (polygon) pp.closeSubpath();
                if (to && !pts.isEmpty()) m_dc.cur = pts.last();
                if (m_inPath) {
                    if (to) { for (int k = 1; k < pp.elementCount(); ++k) {} m_path.connectPath(pp); }
                    else m_path.addPath(pp);
                } else {
                    fillAndStroke(pp, polygon, true);
                }
                break;
            }
            case 7: case 8: case 90: case 91: {
                rect32();
                const quint32 nPolys = a.u32(); a.u32();
                QVector<quint32> counts;
                for (quint32 i = 0; i < nPolys && a.ok(4); ++i) counts << a.u32();
                const bool s16 = type >= 90;
                const bool polygon = type == 8 || type == 91;
                QPainterPath pp;
                for (quint32 c : counts) {
                    QVector<QPointF> pts;
                    for (quint32 i = 0; i < c && a.ok(s16 ? 4 : 8); ++i) pts << (s16 ? pt16() : pt32());
                    pp.addPath(polygonPath(pts, polygon));
                }
                if (m_inPath) m_path.addPath(pp); else fillAndStroke(pp, polygon, true);
                break;
            }
            case 59: m_inPath = true; m_path = QPainterPath(); m_path.moveTo(m_dc.cur); break;
            case 60: m_inPath = false; break;
            case 61: if (m_inPath) m_path.closeSubpath(); break;
            case 62: case 63: case 64: {
                rect32();
                const QPainterPath pp = m_path;
                m_inPath = false;
                fillAndStroke(pp, type != 64, type != 62);
                break;
            }
            case 67: {
                apply();
                QPainterPath pp = m_path;
                pp.setFillRule(m_dc.fill);
                m_p->setClipPath(pp, Qt::IntersectClip);
                break;
            }
            case 30: { const QRectF b = rect32().normalized(); apply(); m_p->setClipRect(b, Qt::IntersectClip); break; }
            case 75: {
                const quint32 cb = a.u32(), mode = a.u32();
                if (mode == 5 || cb == 0) { m_p->setClipping(false); break; }
                // RGNDATA: header (32 bytes) followed by rectangles in device units.
                if (cb >= 32) {
                    a.u32(); a.u32(); const quint32 nRects = a.u32(); a.u32(); a.skip(16);
                    QPainterPath region;
                    for (quint32 i = 0; i < nRects && a.ok(16); ++i) region.addRect(rect32().normalized());
                    m_p->setTransform(base);
                    m_p->setClipPath(region, mode == 1 ? Qt::IntersectClip : Qt::ReplaceClip);
                }
                break;
            }
            case 83: case 84: {
                rect32();
                a.u32(); a.f32(); a.f32();
                const QPointF ref = pt32();
                const quint32 nChars = a.u32(), offString = a.u32(), options = a.u32();
                const QRectF rcl = rect32();
                const quint32 offDx = a.u32();
                QString s;
                // In 64 bits: a count from the file could wrap a 32-bit product.
                if (start + qsizetype(offString) + qsizetype(nChars) * (type == 84 ? 2 : 1) <= a.n) {
                    if (type == 84) for (quint32 i = 0; i < nChars; ++i) s += QChar(qFromLittleEndian<quint16>(a.d + start + offString + i * 2));
                    else s = decode(QByteArray(reinterpret_cast<const char *>(a.d + start + offString), nChars));
                }
                QVector<double> dx;
                if (offDx && start + qsizetype(offDx) + qsizetype(nChars) * 4 <= a.n)
                    for (quint32 i = 0; i < nChars; ++i) dx << qFromLittleEndian<qint32>(a.d + start + offDx + i * 4);
                if ((options & 0x02) && rcl.isValid()) { apply(); m_p->fillRect(rcl.normalized(), m_dc.bkColor); }
                text(ref, s, dx);
                break;
            }
            case 76: case 77: case 81: {
                rect32();
                double xd, yd, cxd, cyd, xs = 0, ys = 0, cxs = 0, cys = 0;
                quint32 rop, offBmi, cbBmi, offBits, cbBits;
                if (type == 81) {
                    xd = a.s32(); yd = a.s32(); xs = a.s32(); ys = a.s32(); cxs = a.s32(); cys = a.s32();
                    offBmi = a.u32(); cbBmi = a.u32(); offBits = a.u32(); cbBits = a.u32();
                    a.u32(); rop = a.u32(); cxd = a.s32(); cyd = a.s32();
                } else {
                    xd = a.s32(); yd = a.s32(); cxd = a.s32(); cyd = a.s32(); rop = a.u32(); xs = a.s32(); ys = a.s32();
                    a.skip(24); a.u32(); a.u32();
                    offBmi = a.u32(); cbBmi = a.u32(); offBits = a.u32(); cbBits = a.u32();
                    if (type == 77) { cxs = a.s32(); cys = a.s32(); }
                }
                if (cbBmi == 0) {
                    QPainterPath pp; pp.addRect(QRectF(xd, yd, cxd, cyd).normalized());
                    if (rop == 0x00F00021 || rop == 0x00000042 || rop == 0x00FF0062) {
                        QBrush keep = m_dc.brush;
                        if (rop == 0x00000042) m_dc.brush = QBrush(Qt::black);
                        if (rop == 0x00FF0062) m_dc.brush = QBrush(Qt::white);
                        fillAndStroke(pp, true, false);
                        m_dc.brush = keep;
                    }
                    break;
                }
                if (start + offBmi + cbBmi > a.n || start + offBits + cbBits > a.n) break;
                const QImage img = dibToImage(a.d + start + offBmi, cbBmi, a.d + start + offBits, cbBits);
                if (img.isNull()) break;
                if (type == 76) { cxs = cxd; cys = cyd; }
                QRectF src(xs, ys, cxs, cys);
                if (type == 81) src = QRectF(xs, img.height() - ys - cys, cxs, cys);
                image(img, QRectF(xd, yd, cxd, cyd), src, rop);
                break;
            }
            default: break;
            }
            r.p = start + size;
        }
    }
};

} // namespace

bool Metafile::looksLikeMetafile(const QByteArray &d)
{
    if (d.size() < 44) return false;
    const quint32 m = qFromLittleEndian<quint32>(d.constData());
    if (m == 0x9AC6CDD7u) return true;                                   // placeable WMF
    if (m == 1 && qFromLittleEndian<quint32>(d.constData() + 40) == 0x464D4520u) return true;   // EMF " EMF"
    const quint16 t = qFromLittleEndian<quint16>(d.constData());
    const quint16 hs = qFromLittleEndian<quint16>(d.constData() + 2);
    return (t == 1 || t == 2) && hs == 9;                                // plain WMF
}

bool Metafile::load(const QByteArray &data)
{
    m_valid = false;
    m_data = data;
    if (!looksLikeMetafile(data)) return false;
    const uchar *d = reinterpret_cast<const uchar *>(data.constData());
    const quint32 m = qFromLittleEndian<quint32>(d);
    if (m == 1) {
        if (data.size() < 88) return false;   // the header is 88 bytes
        m_emf = true;
        const double bl = qFromLittleEndian<qint32>(d + 8), bt = qFromLittleEndian<qint32>(d + 12), br = qFromLittleEndian<qint32>(d + 16), bb = qFromLittleEndian<qint32>(d + 20);
        const double fl = qFromLittleEndian<qint32>(d + 24), ft = qFromLittleEndian<qint32>(d + 28), fr = qFromLittleEndian<qint32>(d + 32), fb = qFromLittleEndian<qint32>(d + 36);
        const double devX = qFromLittleEndian<qint32>(d + 72), devY = qFromLittleEndian<qint32>(d + 76);
        const double mmX = qFromLittleEndian<qint32>(d + 80), mmY = qFromLittleEndian<qint32>(d + 84);
        const double pxPerMmX = mmX > 0 ? devX / mmX : 3.78, pxPerMmY = mmY > 0 ? devY / mmY : 3.78;
        // The frame (0.01 mm) in device units is the picture's extent.
        QRectF frame(QPointF(fl / 100 * pxPerMmX, ft / 100 * pxPerMmY), QPointF(fr / 100 * pxPerMmX, fb / 100 * pxPerMmY));
        if (frame.width() <= 0 || frame.height() <= 0) frame = QRectF(QPointF(bl, bt), QPointF(br, bb));
        m_bounds = frame.normalized();
        m_sizePt = QSizeF((fr - fl) / 100.0 / 25.4 * 72.0, (fb - ft) / 100.0 / 25.4 * 72.0);
        if (m_sizePt.width() <= 0 || m_sizePt.height() <= 0) m_sizePt = m_bounds.size() * 0.75;
    } else {
        m_emf = false;
        if (m == 0x9AC6CDD7u) {
            const double l = qFromLittleEndian<qint16>(d + 6), t = qFromLittleEndian<qint16>(d + 8), r = qFromLittleEndian<qint16>(d + 10), b = qFromLittleEndian<qint16>(d + 12);
            const double inch = qFromLittleEndian<quint16>(d + 14);
            m_bounds = QRectF(QPointF(l, t), QPointF(r, b)).normalized();
            m_sizePt = inch > 0 ? QSizeF(m_bounds.width() / inch * 72, m_bounds.height() / inch * 72) : m_bounds.size();
        } else {
            m_bounds = QRectF();
            m_sizePt = QSizeF(144, 144);
        }
    }
    m_valid = true;
    return true;
}

QSizeF Metafile::naturalSize() const { return m_sizePt; }

void Metafile::play(QPainter *p, const QRectF &target, const std::function<QColor(const QColor &)> &recolor) const
{
    if (!m_valid) return;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::TextAntialiasing);
    p->setClipRect(target, Qt::IntersectClip);
    Player pl(p, m_emf);
    pl.recolor = recolor;
    const QTransform outer = p->transform();
    if (m_emf) {
        QTransform t;
        t.translate(target.x(), target.y());
        t.scale(target.width() / std::max(1e-6, m_bounds.width()), target.height() / std::max(1e-6, m_bounds.height()));
        t.translate(-m_bounds.x(), -m_bounds.y());
        pl.base = t * outer;
    } else {
        // WMF: logical window -> unit square -> target.
        QTransform t;
        t.translate(target.x(), target.y());
        t.scale(target.width(), target.height());
        pl.base = t * outer;
        pl.wmfWindowFallback = m_bounds.isValid() ? m_bounds : QRectF(0, 0, 1000, 1000);
    }
    pl.play(m_data);
    p->restore();
}

QImage Metafile::toImage(int maxSide) const
{
    if (!m_valid) return {};
    QSizeF s = m_sizePt;
    if (s.width() <= 0 || s.height() <= 0) s = QSizeF(400, 400);
    s.scale(maxSide, maxSide, Qt::KeepAspectRatio);
    QImage img(s.toSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    play(&p, QRectF(QPointF(0, 0), s));
    p.end();
    return img;
}

} // namespace jp
