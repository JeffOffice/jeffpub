#include "render/pdfpage.h"

#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QTransform>

#include <cmath>
#include <functional>

namespace jp {

namespace {

// PDFium keeps global state and isn't safe to use from two threads at once.
QRecursiveMutex &pdfiumLock()
{
    static QRecursiveMutex m;
    return m;
}

void initPdfium()
{
    static const bool done = [] {
        FPDF_LIBRARY_CONFIG cfg{};
        cfg.version = 2;
        FPDF_InitLibraryWithConfig(&cfg);
        return true;
    }();
    Q_UNUSED(done);
}

QTransform toQ(const FS_MATRIX &m) { return QTransform(m.a, m.b, m.c, m.d, m.e, m.f); }

// A path from PDFium's segments: moves, lines, and curves given as their
// two control points and end point, each segment perhaps closing its figure.
template <class Get>
QPainterPath pathFrom(int count, Get segment)
{
    QPainterPath path;
    QVector<QPointF> curve;
    for (int i = 0; i < count; ++i) {
        FPDF_PATHSEGMENT s = segment(i);
        if (!s) continue;
        float x = 0, y = 0;
        FPDFPathSegment_GetPoint(s, &x, &y);
        const QPointF pt(x, y);
        switch (FPDFPathSegment_GetType(s)) {
        case FPDF_SEGMENT_MOVETO: path.moveTo(pt); curve.clear(); break;
        case FPDF_SEGMENT_LINETO: path.lineTo(pt); curve.clear(); break;
        case FPDF_SEGMENT_BEZIERTO:
            curve << pt;
            if (curve.size() == 3) {
                path.cubicTo(curve[0], curve[1], curve[2]);
                curve.clear();
            }
            break;
        default: break;
        }
        if (FPDFPathSegment_GetClose(s)) path.closeSubpath();
    }
    return path;
}

QImage toImage(FPDF_BITMAP bmp)
{
    if (!bmp) return {};
    const int w = FPDFBitmap_GetWidth(bmp), h = FPDFBitmap_GetHeight(bmp), stride = FPDFBitmap_GetStride(bmp);
    const auto *buf = static_cast<const uchar *>(FPDFBitmap_GetBuffer(bmp));
    if (!buf || w <= 0 || h <= 0) return {};
    switch (FPDFBitmap_GetFormat(bmp)) {
    case FPDFBitmap_Gray: return QImage(buf, w, h, stride, QImage::Format_Grayscale8).copy();
    case FPDFBitmap_BGR: return QImage(buf, w, h, stride, QImage::Format_RGB888).rgbSwapped();
    case FPDFBitmap_BGRx: return QImage(buf, w, h, stride, QImage::Format_RGB32).copy();
    case FPDFBitmap_BGRA: return QImage(buf, w, h, stride, QImage::Format_ARGB32).copy();
    default: return {};
    }
}

QColor colorOf(unsigned r, unsigned g, unsigned b, unsigned a) { return QColor(int(r), int(g), int(b), int(a)); }

struct WriteToBytes : FPDF_FILEWRITE {
    QByteArray *out = nullptr;
    static int write(FPDF_FILEWRITE *self, const void *data, unsigned long size)
    {
        static_cast<WriteToBytes *>(self)->out->append(static_cast<const char *>(data), qsizetype(size));
        return 1;
    }
};

} // namespace

struct PdfDocument::Impl {
    QByteArray bytes;
    FPDF_DOCUMENT doc = nullptr;
    QHash<int, Check> checks;
};

PdfDocument::PdfDocument(const QByteArray &bytes) : d(std::make_unique<Impl>())
{
    QMutexLocker lock(&pdfiumLock());
    initPdfium();
    d->bytes = bytes;
    d->doc = FPDF_LoadMemDocument64(d->bytes.constData(), size_t(d->bytes.size()), nullptr);
}

PdfDocument::~PdfDocument()
{
    QMutexLocker lock(&pdfiumLock());
    if (d->doc) FPDF_CloseDocument(d->doc);
}

bool PdfDocument::isValid() const { return d->doc != nullptr; }

bool PdfDocument::looksLikePdf(const QByteArray &bytes) { return bytes.left(1024).contains("%PDF-"); }

std::shared_ptr<const PdfDocument> PdfDocument::shared(const QByteArray &bytes)
{
    QMutexLocker lock(&pdfiumLock());
    static QList<std::shared_ptr<const PdfDocument>> recent;   // most recent first
    for (int i = 0; i < recent.size(); ++i)
        if (recent[i]->d->bytes == bytes) {
            recent.move(i, 0);
            return recent.first();
        }
    auto doc = std::make_shared<const PdfDocument>(bytes);
    recent.prepend(doc);
    while (recent.size() > 6) recent.removeLast();
    return doc;
}

int PdfDocument::pageCount() const
{
    QMutexLocker lock(&pdfiumLock());
    return d->doc ? FPDF_GetPageCount(d->doc) : 0;
}

QSizeF PdfDocument::pageSize(int page) const
{
    QMutexLocker lock(&pdfiumLock());
    FS_SIZEF s{};
    if (!d->doc || !FPDF_GetPageSizeByIndexF(d->doc, page, &s)) return {};
    return QSizeF(s.width, s.height);
}

QByteArray PdfDocument::extractPage(int page) const
{
    QMutexLocker lock(&pdfiumLock());
    QByteArray out;
    if (!d->doc || page < 0 || page >= FPDF_GetPageCount(d->doc)) return out;
    FPDF_DOCUMENT one = FPDF_CreateNewDocument();
    if (!one) return out;
    const int index = page;
    if (FPDF_ImportPagesByIndex(one, d->doc, &index, 1, 0)) {
        WriteToBytes w;
        w.version = 1;
        w.WriteBlock = &WriteToBytes::write;
        w.out = &out;
        if (!FPDF_SaveAsCopy(one, &w, FPDF_NO_INCREMENTAL)) out.clear();
    }
    FPDF_CloseDocument(one);
    return out;
}

QImage PdfDocument::render(int page, const QSize &px) const
{
    QMutexLocker lock(&pdfiumLock());
    if (!d->doc || px.isEmpty() || px.width() > 20000 || px.height() > 20000) return {};
    FPDF_PAGE pg = FPDF_LoadPage(d->doc, page);
    if (!pg) return {};
    FPDF_BITMAP bmp = FPDFBitmap_Create(px.width(), px.height(), 1);
    QImage img;
    if (bmp) {
        // Clear behind it, as a picture with no background.
        FPDFBitmap_FillRect(bmp, 0, 0, px.width(), px.height(), 0x00000000);
        FPDF_RenderPageBitmap(bmp, pg, 0, 0, px.width(), px.height(), 0, 0);
        img = toImage(bmp);
        FPDFBitmap_Destroy(bmp);
    }
    FPDF_ClosePage(pg);
    return img;
}

namespace {

// The page's objects drawn into target as vectors, as well as they can be;
// what PDFium's raster shows differently is found by checkPage().
void playVectors(FPDF_DOCUMENT doc, QPainter *p, int page, const QRectF &target)
{
    FPDF_PAGE pg = FPDF_LoadPage(doc, page);
    if (!pg) return;
    // The page's own space (y up, its shown box) to the target: unturned
    // first, then turned as the page shows, then scaled into place.
    FS_RECTF box{};
    if (!FPDF_GetPageBoundingBox(pg, &box)) box = FS_RECTF{0, float(FPDF_GetPageHeightF(pg)), float(FPDF_GetPageWidthF(pg)), 0};
    const double w0 = box.right - box.left, h0 = box.top - box.bottom;
    if (w0 <= 0 || h0 <= 0) {
        FPDF_ClosePage(pg);
        return;
    }
    QTransform pageXf(1, 0, 0, -1, -box.left, box.top);
    const int turns = ((FPDFPage_GetRotation(pg) % 4) + 4) % 4;
    double dw = w0, dh = h0;
    if (turns == 1) { pageXf *= QTransform(0, 1, -1, 0, h0, 0); dw = h0; dh = w0; }
    else if (turns == 2) pageXf *= QTransform(-1, 0, 0, -1, w0, h0);
    else if (turns == 3) { pageXf *= QTransform(0, -1, 1, 0, 0, w0); dw = h0; dh = w0; }
    pageXf *= QTransform::fromScale(target.width() / dw, target.height() / dh) * QTransform::fromTranslate(target.left(), target.top());

    p->save();
    p->setClipRect(target, Qt::IntersectClip);
    p->setRenderHint(QPainter::Antialiasing);

    // Which characters each text object draws (text in groups included).
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
    QHash<FPDF_PAGEOBJECT, QVector<int>> chars;
    if (tp)
        for (int i = 0, n = FPDFText_CountChars(tp); i < n; ++i)
            if (FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, i)) chars[o] << i;

    // Shadings and anything else without a vector form: from PDFium's
    // raster of the whole page, made once when first needed.
    QImage raster;
    double rasterScale = 0;
    auto rasterOf = [&]() -> const QImage & {
        if (raster.isNull()) {
            rasterScale = std::min(300.0 / 72.0, 6000.0 / std::max(dw, dh));
            FPDF_BITMAP bmp = FPDFBitmap_Create(std::max(1, int(std::ceil(dw * rasterScale))), std::max(1, int(std::ceil(dh * rasterScale))), 1);
            if (bmp) {
                FPDFBitmap_FillRect(bmp, 0, 0, FPDFBitmap_GetWidth(bmp), FPDFBitmap_GetHeight(bmp), 0x00000000);
                FPDF_RenderPageBitmap(bmp, pg, 0, 0, FPDFBitmap_GetWidth(bmp), FPDFBitmap_GetHeight(bmp), 0, 0);
                raster = toImage(bmp);
                FPDFBitmap_Destroy(bmp);
            }
        }
        return raster;
    };
    const QTransform targetToRaster = [&] {
        // Target space to the raster's pixels (the page as shown, scaled).
        return QTransform::fromTranslate(-target.left(), -target.top()) * QTransform::fromScale(dw / target.width(), dh / target.height());
    }();

    std::function<void(FPDF_PAGEOBJECT, const QTransform &, int)> draw = [&](FPDF_PAGEOBJECT obj, const QTransform &parent, int depth) {
        if (depth > 32) return;
        const int type = FPDFPageObj_GetType(obj);
        FS_MATRIX m{1, 0, 0, 1, 0, 0};
        FPDFPageObj_GetMatrix(obj, &m);
        const QTransform objXf = toQ(m) * parent;
        if (type == FPDF_PAGEOBJ_FORM) {
            for (int k = 0, n = FPDFFormObj_CountObjects(obj); k < n; ++k)
                if (FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(obj, (unsigned long)k)) draw(c, objXf, depth + 1);
            return;
        }
        p->save();
        // Its clip: every path of it at once, in the page's space.
        if (FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(obj)) {
            for (int k = 0, n = FPDFClipPath_CountPaths(clip); k < n; ++k) {
                QPainterPath cp = pathFrom(FPDFClipPath_CountPathSegments(clip, k), [&](int s) { return FPDFClipPath_GetPathSegment(clip, k, s); });
                if (!cp.isEmpty()) p->setClipPath(pageXf.map(cp), Qt::IntersectClip);
            }
        }
        unsigned r = 0, g = 0, b = 0, a = 255;
        if (type == FPDF_PAGEOBJ_PATH) {
            QPainterPath path = pathFrom(FPDFPath_CountSegments(obj), [&](int s) { return FPDFPath_GetPathSegment(obj, s); });
            int fillMode = FPDF_FILLMODE_NONE;
            FPDF_BOOL stroke = false;
            FPDFPath_GetDrawMode(obj, &fillMode, &stroke);
            path.setFillRule(fillMode == FPDF_FILLMODE_ALTERNATE ? Qt::OddEvenFill : Qt::WindingFill);
            p->setTransform(objXf, true);
            p->setPen(Qt::NoPen);
            p->setBrush(Qt::NoBrush);
            if (fillMode != FPDF_FILLMODE_NONE && FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a)) p->fillPath(path, colorOf(r, g, b, a));
            if (stroke && FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a)) {
                float width = 1;
                FPDFPageObj_GetStrokeWidth(obj, &width);
                QPen pen(colorOf(r, g, b, a));
                // A width of 0 is the thinnest line the device draws.
                pen.setWidthF(width);
                pen.setCosmetic(width <= 0);
                switch (FPDFPageObj_GetLineJoin(obj)) {
                case FPDF_LINEJOIN_ROUND: pen.setJoinStyle(Qt::RoundJoin); break;
                case FPDF_LINEJOIN_BEVEL: pen.setJoinStyle(Qt::BevelJoin); break;
                default: pen.setJoinStyle(Qt::MiterJoin); break;
                }
                switch (FPDFPageObj_GetLineCap(obj)) {
                case FPDF_LINECAP_ROUND: pen.setCapStyle(Qt::RoundCap); break;
                case FPDF_LINECAP_PROJECTING_SQUARE: pen.setCapStyle(Qt::SquareCap); break;
                default: pen.setCapStyle(Qt::FlatCap); break;
                }
                const int dashes = FPDFPageObj_GetDashCount(obj);
                if (dashes > 0 && dashes < 64 && width > 0) {
                    QVector<float> dash(dashes);
                    if (FPDFPageObj_GetDashArray(obj, dash.data(), size_t(dashes))) {
                        // Qt measures dashes in pen widths.
                        QVector<qreal> pattern;
                        for (float v : dash) pattern << std::max<qreal>(v / width, 0.01);
                        if (pattern.size() % 2) pattern << pattern;
                        pen.setDashPattern(pattern);
                        float phase = 0;
                        if (FPDFPageObj_GetDashPhase(obj, &phase)) pen.setDashOffset(phase / width);
                    }
                }
                p->strokePath(path, pen);
            }
        } else if (type == FPDF_PAGEOBJ_TEXT && tp) {
            // Each character as its glyph's outline, where the text page
            // puts it (its matrix carries the text's size, slant and turn).
            const FPDF_TEXT_RENDERMODE mode = FPDFTextObj_GetTextRenderMode(obj);
            const bool fill = mode == FPDF_TEXTRENDERMODE_FILL || mode == FPDF_TEXTRENDERMODE_FILL_STROKE || mode == FPDF_TEXTRENDERMODE_FILL_CLIP ||
                              mode == FPDF_TEXTRENDERMODE_FILL_STROKE_CLIP || mode == FPDF_TEXTRENDERMODE_UNKNOWN;
            const bool strokeText = mode == FPDF_TEXTRENDERMODE_STROKE || mode == FPDF_TEXTRENDERMODE_FILL_STROKE ||
                                    mode == FPDF_TEXTRENDERMODE_STROKE_CLIP || mode == FPDF_TEXTRENDERMODE_FILL_STROKE_CLIP;
            FPDF_FONT font = FPDFTextObj_GetFont(obj);
            float size = 0;
            FPDFTextObj_GetFontSize(obj, &size);
            if (font && size > 0 && (fill || strokeText)) {
                QPainterPath glyphs, missing;
                const QVector<int> idx = chars.value(obj);
                for (int k = 0; k < idx.size();) {
                    // The characters one glyph stands for (a ligature's
                    // letters) share its box, which PDFium measures from
                    // the glyph the text really uses.
                    QRectF box;
                    QVector<unsigned> text;
                    for (; k < idx.size(); ++k) {
                        double l = 0, r = 0, b = 0, t = 0;
                        if (!FPDFText_GetCharBox(tp, idx[k], &l, &r, &b, &t)) break;
                        const QRectF bk(QPointF(l, b), QPointF(r, t));
                        if (!text.isEmpty() && bk != box) break;
                        box = bk;
                        text << FPDFText_GetUnicode(tp, idx[k]);
                    }
                    if (text.isEmpty()) {
                        ++k;
                        continue;
                    }
                    const int i = idx[k - 1];
                    if (box.width() <= 0 || box.height() <= 0) continue;   // draws nothing
                    unsigned u = text.size() == 1 ? text[0] : 0;
                    if (text.size() > 1) {
                        static const QHash<QString, unsigned> ligatures{{QStringLiteral("ff"), 0xFB00}, {QStringLiteral("fi"), 0xFB01}, {QStringLiteral("fl"), 0xFB02},
                                                                        {QStringLiteral("ffi"), 0xFB03}, {QStringLiteral("ffl"), 0xFB04}, {QStringLiteral("st"), 0xFB06}};
                        QString str;
                        for (unsigned c : text) str += QChar(char16_t(c < 0x10000 ? c : 0xFFFD));
                        u = ligatures.value(str);
                    }
                    QPainterPath glyph;
                    QRectF gb;
                    FS_MATRIX cm{1, 0, 0, 1, 0, 0};
                    double ox = 0, oy = 0;
                    if (u && u < 0xFFFE && FPDFText_GetMatrix(tp, i, &cm) && FPDFText_GetCharOrigin(tp, i, &ox, &oy))
                        if (FPDF_GLYPHPATH gp = FPDFFont_GetGlyphPath(font, u, size)) {
                            // Glyph paths come a font unit (one em) tall; the
                            // matrix turns, slants and scales them, and the
                            // size makes them the text's size, at its origin.
                            const QTransform at(cm.a * size, cm.b * size, cm.c * size, cm.d * size, ox, oy);
                            const QPainterPath em = pathFrom(FPDFGlyphPath_CountGlyphSegments(gp), [&](int s) { return FPDFGlyphPath_GetGlyphPathSegment(gp, s); });
                            glyph = at.map(em);
                            // PDFium's box is the glyph's own box turned.
                            gb = at.mapRect(em.boundingRect());
                        }
                    // The outline found by its character must be the glyph
                    // drawn; when it isn't (or there's none: a picture
                    // font, a character the font can't name), the glyph's
                    // box comes from the raster.
                    const double tol = 0.2 + 0.06 * size * std::sqrt(std::abs(cm.a * cm.d - cm.b * cm.c));
                    if (!glyph.isEmpty() && std::abs(gb.left() - box.left()) <= tol && std::abs(gb.right() - box.right()) <= tol &&
                        std::abs(gb.top() - box.top()) <= tol && std::abs(gb.bottom() - box.bottom()) <= tol)
                        glyphs.addPath(glyph);
                    else
                        missing.addRect(box.adjusted(-0.5, -0.5, 0.5, 0.5));
                }
                if (!missing.isEmpty() && !rasterOf().isNull()) {
                    p->save();
                    p->setClipPath(pageXf.map(missing), Qt::IntersectClip);
                    p->drawImage(target, raster);
                    p->restore();
                }
                if (!glyphs.isEmpty()) {
                    glyphs.setFillRule(Qt::WindingFill);
                    p->setTransform(pageXf, true);
                    if (fill && FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a)) p->fillPath(glyphs, colorOf(r, g, b, a));
                    if (strokeText && FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a)) {
                        float width = 1;
                        FPDFPageObj_GetStrokeWidth(obj, &width);
                        QPen pen(colorOf(r, g, b, a));
                        pen.setWidthF(width);
                        pen.setCosmetic(width <= 0);
                        p->strokePath(glyphs, pen);
                    }
                }
            }
        } else if (type == FPDF_PAGEOBJ_IMAGE) {
            // The picture as PDFium draws it (its masks applied), on its box.
            FPDF_BITMAP bmp = FPDFImageObj_GetRenderedBitmap(doc, pg, obj);
            const QImage img = toImage(bmp);
            if (bmp) FPDFBitmap_Destroy(bmp);
            float l = 0, bt = 0, rt = 0, t = 0;
            if (!img.isNull() && FPDFPageObj_GetBounds(obj, &l, &bt, &rt, &t) && rt > l && t > bt) {
                p->setTransform(QTransform((rt - l) / img.width(), 0, 0, -(t - bt) / img.height(), l, t) * pageXf, true);
                p->setRenderHint(QPainter::SmoothPixmapTransform);
                p->drawImage(QPointF(0, 0), img);
            }
        } else if (type == FPDF_PAGEOBJ_SHADING) {
            float l = 0, bt = 0, rt = 0, t = 0;
            const QImage &page = rasterOf();
            if (!page.isNull() && FPDFPageObj_GetBounds(obj, &l, &bt, &rt, &t)) {
                const QRectF where = pageXf.mapRect(QRectF(QPointF(l, bt), QPointF(rt, t)));
                const QRectF src = QTransform(targetToRaster * QTransform::fromScale(rasterScale, rasterScale)).mapRect(where);
                p->drawImage(where, page, src);
            }
        }
        p->restore();
    };
    for (int i = 0, n = FPDFPage_CountObjects(pg); i < n; ++i)
        if (FPDF_PAGEOBJECT o = FPDFPage_GetObject(pg, i)) draw(o, pageXf, 0);
    if (tp) FPDFText_ClosePage(tp);
    p->restore();
    FPDF_ClosePage(pg);
}

// The tiles (of tile pixels) where two drawings of a page differ by more
// than antialiasing: a pixel differs when its color lies well outside the
// range of the colors within a pixel of it in the other drawing.
QRegion differingTiles(const QImage &a, const QImage &b, int tile)
{
    auto white = [](const QImage &img) {
        QImage out(img.size(), QImage::Format_RGB32);
        out.fill(Qt::white);
        QPainter p(&out);
        p.drawImage(0, 0, img);
        return out;
    };
    const QImage wa = white(a), wb = white(b);
    const int w = wa.width(), h = wa.height();
    auto matched = [&](const QImage &in, int x, int y, QRgb c) {
        int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
        for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy) {
            const auto *row = reinterpret_cast<const QRgb *>(in.constScanLine(yy));
            for (int xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx) {
                const int v[3] = {qRed(row[xx]), qGreen(row[xx]), qBlue(row[xx])};
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], v[k]);
                    hi[k] = std::max(hi[k], v[k]);
                }
            }
        }
        const int v[3] = {qRed(c), qGreen(c), qBlue(c)};
        // Text edges vary most (PDFium draws small text a little bolder):
        // where the colors around vary, more leeway than in flat color.
        for (int k = 0; k < 3; ++k) {
            const int slack = std::min(96, 40 + (hi[k] - lo[k]) / 2);
            if (v[k] < lo[k] - slack || v[k] > hi[k] + slack) return false;
        }
        return true;
    };
    const int tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
    QVector<int> count(tw * th);
    for (int y = 0; y < h; ++y) {
        const auto *ra = reinterpret_cast<const QRgb *>(wa.constScanLine(y));
        const auto *rb = reinterpret_cast<const QRgb *>(wb.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (ra[x] != rb[x] && (!matched(wb, x, y, ra[x]) || !matched(wa, x, y, rb[x]))) ++count[(y / tile) * tw + x / tile];
    }
    QRegion bad;
    for (int ty = 0; ty < th; ++ty)
        for (int tx = 0; tx < tw; ++tx)
            if (count[ty * tw + tx] >= 3) bad += QRect(tx * tile, ty * tile, tile, tile);
    return bad;
}

} // namespace

PdfDocument::Check PdfDocument::check(int page) const
{
    QMutexLocker lock(&pdfiumLock());
    auto it = d->checks.find(page);
    if (it != d->checks.end()) return *it;
    // The vectors drawn at about 100 dpi beside PDFium's own raster.
    Check c;
    const QSizeF pt = pageSize(page);
    if (d->doc && !pt.isEmpty()) {
        const double scale = std::min(100.0 / 72.0, 1600.0 / std::max(pt.width(), pt.height()));
        c.size = QSize(std::max(1, qRound(pt.width() * scale)), std::max(1, qRound(pt.height() * scale)));
        const QImage ref = render(page, c.size);
        QImage played(c.size, QImage::Format_ARGB32_Premultiplied);
        played.fill(Qt::transparent);
        {
            QPainter p(&played);
            playVectors(d->doc, &p, page, QRectF(QPointF(0, 0), QSizeF(c.size)));
        }
        if (!ref.isNull()) c.raster = differingTiles(ref, played, 16);
    }
    d->checks.insert(page, c);
    return c;
}

double PdfDocument::rasterShare(int page) const
{
    const Check c = check(page);
    if (c.size.isEmpty()) return 0;
    qint64 area = 0;
    for (const QRect &r : c.raster) area += qint64(r.intersected(QRect(QPoint(0, 0), c.size)).width()) * r.intersected(QRect(QPoint(0, 0), c.size)).height();
    return double(area) / (qint64(c.size.width()) * c.size.height());
}

void PdfDocument::play(QPainter *p, int page, const QRectF &target) const
{
    QMutexLocker lock(&pdfiumLock());
    if (!d->doc || target.isEmpty()) return;
    const Check c = check(page);
    if (c.raster.isEmpty() || c.size.isEmpty()) {
        playVectors(d->doc, p, page, target);
        return;
    }
    // Where the vectors don't match PDFium's drawing, its raster is used
    // (at 300 dpi) and the vectors are left out.
    const QTransform toTarget = QTransform::fromScale(target.width() / c.size.width(), target.height() / c.size.height()) * QTransform::fromTranslate(target.left(), target.top());
    QPainterPath rasterPart;
    rasterPart.addRegion(c.raster);
    rasterPart = toTarget.map(rasterPart);
    QPainterPath vectorPart;
    vectorPart.addRect(target);
    vectorPart = vectorPart.subtracted(rasterPart);
    p->save();
    p->setClipPath(vectorPart, Qt::IntersectClip);
    playVectors(d->doc, p, page, target);
    p->restore();
    const QSizeF pt = pageSize(page);
    const double scale = std::min(300.0 / 72.0, 6000.0 / std::max(pt.width(), pt.height()));
    const QImage img = render(page, QSize(std::max(1, qRound(pt.width() * scale)), std::max(1, qRound(pt.height() * scale))));
    if (img.isNull()) return;
    p->save();
    p->setClipPath(rasterPart, Qt::IntersectClip);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    p->drawImage(target, img);
    p->restore();
}

} // namespace jp
