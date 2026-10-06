#include "io/pubimport.h"

#include "render/renderer.h"
#include "io/pubshapes.h"
#include "text/textprops.h"

#include <QFile>
#include <QHash>
#include <QFont>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QtMath>
#include <cmath>
#include <functional>

#include <libmspub/jp_hooks.h>
#include <libmspub/libmspub.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge/librevenge.h>

namespace jp {

namespace {

using librevenge::RVNGProperty;
using librevenge::RVNGPropertyList;
using librevenge::RVNGPropertyListVector;

bool decodeWithQt(const char *encoding, const unsigned char *data, std::size_t len, std::u32string &out)
{
    QStringDecoder dec(encoding);
    if (!dec.isValid()) return false;
    const QString s = dec.decode(QByteArrayView(reinterpret_cast<const char *>(data), qsizetype(len)));
    for (const char32_t c : s.toUcs4()) out.push_back(c);
    return true;
}

double toPt(const RVNGProperty *p, double def = 0)
{
    if (!p) return def;
    switch (p->getUnit()) {
    case librevenge::RVNG_INCH: return p->getDouble() * 72.0;
    case librevenge::RVNG_POINT: return p->getDouble();
    case librevenge::RVNG_TWIP: return p->getDouble() / 20.0;
    case librevenge::RVNG_PERCENT: return p->getDouble();
    default: return p->getDouble() * 72.0;
    }
}

QString str(const RVNGProperty *p) { return p ? QString::fromUtf8(p->getStr().cstr()) : QString(); }

QColor color(const RVNGProperty *p)
{
    const QString s = str(p);
    return s.startsWith('#') ? QColor(s) : QColor();
}

double percent(const RVNGProperty *p, double def = 1.0)
{
    if (!p) return def;
    if (p->getUnit() == librevenge::RVNG_PERCENT) return p->getDouble();
    const QString s = str(p);
    if (s.endsWith('%')) return s.chopped(1).toDouble() / 100.0;
    return p->getDouble();
}

// SVG elliptical arc to cubic Béziers.
void arcTo(QPainterPath &path, QPointF p0, double rx, double ry, double phiDeg, bool large, bool sweep, QPointF p1)
{
    if (rx == 0 || ry == 0) { path.lineTo(p1); return; }
    rx = std::abs(rx); ry = std::abs(ry);
    const double phi = qDegreesToRadians(phiDeg), c = std::cos(phi), s = std::sin(phi);
    const double dx = (p0.x() - p1.x()) / 2, dy = (p0.y() - p1.y()) / 2;
    const double x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
    double lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double k = den == 0 ? 0 : std::sqrt(std::max(0.0, num / den));
    if (large == sweep) k = -k;
    const double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
    const double cx = c * cx1 - s * cy1 + (p0.x() + p1.x()) / 2, cy = s * cx1 + c * cy1 + (p0.y() + p1.y()) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) {
        const double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    const double t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && dt > 0) dt -= 2 * M_PI;
    if (sweep && dt < 0) dt += 2 * M_PI;
    const int segs = std::max(1, int(std::ceil(std::abs(dt) / (M_PI / 2))));
    const double d = dt / segs;
    const double alpha = 4.0 / 3.0 * std::tan(d / 4);
    double t = t1;
    for (int i = 0; i < segs; ++i) {
        const double c1 = std::cos(t), s1 = std::sin(t), c2 = std::cos(t + d), s2 = std::sin(t + d);
        auto pt = [&](double x, double y) { return QPointF(cx + c * rx * x - s * ry * y, cy + s * rx * x + c * ry * y); };
        path.cubicTo(pt(c1 - alpha * s1, s1 + alpha * c1), pt(c2 + alpha * s2, s2 - alpha * c2), pt(c2, s2));
        t += d;
    }
}

QPainterPath pathFromVector(const RVNGPropertyListVector &v, bool *open)
{
    QPainterPath path;
    QPointF cur;
    bool closed = false;
    for (unsigned long i = 0; i < v.count(); ++i) {
        const RVNGPropertyList &e = v[i];
        const QString act = str(e["librevenge:path-action"]);
        const QPointF pt(toPt(e["svg:x"]), toPt(e["svg:y"]));
        if (act == "M") { path.moveTo(pt); cur = pt; }
        else if (act == "L") { path.lineTo(pt); cur = pt; }
        else if (act == "C") { path.cubicTo(QPointF(toPt(e["svg:x1"]), toPt(e["svg:y1"])), QPointF(toPt(e["svg:x2"]), toPt(e["svg:y2"])), pt); cur = pt; }
        else if (act == "Q") { path.quadTo(QPointF(toPt(e["svg:x1"]), toPt(e["svg:y1"])), pt); cur = pt; }
        else if (act == "A") {
            arcTo(path, cur, toPt(e["svg:rx"]), toPt(e["svg:ry"]), e["librevenge:rotate"] ? e["librevenge:rotate"]->getDouble() : 0,
                  e["librevenge:large-arc"] && e["librevenge:large-arc"]->getInt(), e["librevenge:sweep"] && e["librevenge:sweep"]->getInt(), pt);
            cur = pt;
        } else if (act == "Z") { path.closeSubpath(); closed = true; }
    }
    if (open) *open = !closed;
    return path;
}

QPainterPath pathFromPoints(const RVNGPropertyListVector &v, bool close)
{
    QPainterPath path;
    for (unsigned long i = 0; i < v.count(); ++i) {
        const QPointF pt(toPt(v[i]["svg:x"]), toPt(v[i]["svg:y"]));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    if (close) path.closeSubpath();
    return path;
}

QString formatForMime(const QString &mime)
{
    if (mime.contains("png")) return "png";
    if (mime.contains("jpeg") || mime.contains("jpg")) return "jpg";
    if (mime.contains("gif")) return "gif";
    if (mime.contains("bmp")) return "bmp";
    if (mime.contains("tif")) return "tif";
    if (mime.contains("wmf")) return "wmf";
    if (mime.contains("emf")) return "emf";
    if (mime.contains("svg")) return "svg";
    return "bin";
}

class Collector : public librevenge::RVNGDrawingInterface {
public:
    Collector(Document &d, PubImportReport &r) : m_doc(d), m_rep(r) {}

    void startDocument(const RVNGPropertyList &) override {}
    void endDocument() override {}
    void setDocumentMetaData(const RVNGPropertyList &p) override
    {
        m_doc.props.title = str(p["dc:title"]);
        m_doc.props.author = str(p["meta:initial-creator"]);
        if (m_doc.props.author.isEmpty()) m_doc.props.author = str(p["dc:creator"]);
        m_doc.props.subject = str(p["dc:subject"]);
        m_doc.props.keywords = str(p["meta:keyword"]);
        m_doc.props.comments = str(p["dc:description"]);
    }
    void defineEmbeddedFont(const RVNGPropertyList &p) override { m_rep.warnings << QStringLiteral("Embedded font \"%1\" was not imported.").arg(str(p["librevenge:name"])); }

    void startPage(const RVNGPropertyList &p) override
    {
        const double w = toPt(p["svg:width"], 612), h = toPt(p["svg:height"], 792);
        if (m_doc.pages.isEmpty()) {
            m_doc.setup.size = QSizeF(w, h);
            m_doc.setup.sheet = m_doc.setup.size;
            m_doc.setup.sizeName = QStringLiteral("Custom");
            const double m = std::min(w, h) < 216 ? 9 : 36;
            m_doc.setup.margins = QMarginsF(m, m, m, m);
        }
        auto page = m_doc.addPage();
        // The page's master: the one written under that sequence number (A when the file names none).
        page->masterId = p["jp:master-seq"] ? m_masterIds.value(p["jp:master-seq"]->getInt(), QStringLiteral("A")) : QStringLiteral("A");
        m_page = page.get();
        m_stack.clear();
        ++m_rep.pages;
    }
    void endPage() override { m_page = nullptr; }
    // Master pages arrive first, each once: A, B, C... in order.
    void startMasterPage(const RVNGPropertyList &p) override
    {
        const int n = int(m_masterIds.size());
        const QString id = n < 26 ? QString(QChar('A' + n)) : QStringLiteral("M%1").arg(n + 1);
        if (p["jp:master-seq"]) m_masterIds.insert(p["jp:master-seq"]->getInt(), id);
        MasterPage *m = m_doc.master(id);
        if (!m) {
            auto mp = std::make_shared<MasterPage>();
            mp->id = id;
            mp->abbr = id;
            mp->name = QStringLiteral("Master Page %1").arg(id);
            m_doc.masters << mp;
            m = mp.get();
        }
        m_master = m;
        m_stack.clear();
    }
    void endMasterPage() override { m_master = nullptr; }

    void setStyle(const RVNGPropertyList &p) override { m_style = p; }

    void startLayer(const RVNGPropertyList &) override { m_stack.push_back(std::make_shared<GroupItem>()); }
    void endLayer() override { closeGroup(); }
    void startEmbeddedGraphics(const RVNGPropertyList &) override {}
    void endEmbeddedGraphics() override {}
    void openGroup(const RVNGPropertyList &) override { m_stack.push_back(std::make_shared<GroupItem>()); }
    void closeGroup() override
    {
        if (m_stack.empty()) return;
        auto g = m_stack.back();
        m_stack.pop_back();
        if (g->children.empty()) return;
        if (g->children.size() == 1) { add(g->children.front()); return; }
        g->syncRect();
        add(g);
    }

    void drawRectangle(const RVNGPropertyList &p) override
    {
        if (p["jp:textart-text"]) {
            addTextArt(p);
            return;
        }
        QPainterPath path;
        const QRectF r(toPt(p["svg:x"]), toPt(p["svg:y"]), toPt(p["svg:width"]), toPt(p["svg:height"]));
        const double rx = toPt(p["svg:rx"]);
        if (rx > 0) path.addRoundedRect(r, rx, toPt(p["svg:ry"], rx));
        else path.addRect(r);
        addPath(path, false);
    }
    void drawEllipse(const RVNGPropertyList &p) override
    {
        QPainterPath path;
        const QPointF c(toPt(p["svg:cx"]), toPt(p["svg:cy"]));
        path.addEllipse(c, toPt(p["svg:rx"]), toPt(p["svg:ry"]));
        addPath(path, false);
    }
    void drawPolygon(const RVNGPropertyList &p) override
    {
        if (const RVNGPropertyListVector *v = p.child("svg:points")) addPath(pathFromPoints(*v, true), false);
    }
    void drawPolyline(const RVNGPropertyList &p) override
    {
        if (const RVNGPropertyListVector *v = p.child("svg:points")) addPath(pathFromPoints(*v, false), true);
    }
    void drawPath(const RVNGPropertyList &p) override
    {
        if (const RVNGPropertyListVector *v = p.child("svg:d")) {
            bool open = false;
            const QPainterPath path = pathFromVector(*v, &open);
            addPath(path, open);
        }
    }
    // A .pub picture's recolor arrives as greyscale plus draw:red/green/blue: the
    // picture's dark parts take that color and white stays white.
    static void applyRecolor(PictureItem *pic, const RVNGPropertyList &p)
    {
        if (str(p["draw:color-mode"]) != "greyscale") return;
        if (p["draw:red"] && p["draw:green"] && p["draw:blue"]) {
            const QColor c = QColor::fromRgbF(float(std::clamp(p["draw:red"]->getDouble(), 0.0, 1.0)), float(std::clamp(p["draw:green"]->getDouble(), 0.0, 1.0)),
                                              float(std::clamp(p["draw:blue"]->getDouble(), 0.0, 1.0)));
            pic->recolor = PictureItem::ColorTint;
            pic->recolorColor = ColorRef::rgb(c);
        } else {
            pic->recolor = PictureItem::Grayscale;
        }
    }
    void drawGraphicObject(const RVNGPropertyList &p) override
    {
        const QRectF r(toPt(p["svg:x"]), toPt(p["svg:y"]), toPt(p["svg:width"]), toPt(p["svg:height"]));
        const QByteArray bytes = QByteArray::fromBase64(QByteArray(p["office:binary-data"] ? p["office:binary-data"]->getStr().cstr() : ""));
        auto pic = makePicture(r, 0, bytes, str(p["librevenge:mime-type"]));
        applyRecolor(pic.get(), p);
        add(pic);
    }
    void drawConnector(const RVNGPropertyList &p) override
    {
        if (const RVNGPropertyListVector *v = p.child("svg:d")) {
            bool open = true;
            addPath(pathFromVector(*v, &open), true);
        }
    }

    // ---------- text ----------
    void startTextObject(const RVNGPropertyList &p) override
    {
        if (qEnvironmentVariableIsSet("JP_PUB_TRACE")) fprintf(stderr, "TEXT %s\n", p.getPropString().cstr());
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(toPt(p["svg:x"]), toPt(p["svg:y"]), toPt(p["svg:width"]), toPt(p["svg:height"]));
        // librevenge gives the angle counterclockwise as seen on the page.
        if (p["librevenge:rotate"]) t->rotation = std::fmod(360.0 + p["librevenge:rotate"]->getDouble(), 360.0);
        t->insets = QMarginsF(toPt(p["fo:padding-left"]), toPt(p["fo:padding-top"]), toPt(p["fo:padding-right"]), toPt(p["fo:padding-bottom"]));
        const QString va = str(p["draw:textarea-vertical-align"]);
        t->valign = va == "middle" ? VAlign::Middle : va == "bottom" ? VAlign::Bottom : VAlign::Top;
        if (p["fo:column-count"]) t->columns = std::max(1, p["fo:column-count"]->getInt());
        if (p["fo:column-gap"]) t->columnGap = toPt(p["fo:column-gap"]);
        // Text direction: 1 and 3 run top to bottom (turned 90 degrees clockwise),
        // 2 bottom to top (the same, turned a further 180 about the center).
        if (p["jp:text-flow"]) {
            const int flow = p["jp:text-flow"]->getInt();
            if (flow == 1 || flow == 3 || flow == 2) t->vertical = true;
            if (flow == 2) t->rotation = std::fmod(t->rotation + 180.0, 360.0);
        }
        t->wrap.mode = Wrap::None;
        m_skipText = false;
        // Boxes sharing a story become a linked chain.
        if (p["jp:text-id"]) {
            const int tid = p["jp:text-id"]->getInt();
            auto it = m_chains.find(tid);
            if (it != m_chains.end() && it->second) {
                TextItem *prev = it->second;
                t->storyId = prev->storyId;
                prev->nextId = t->id;
                it->second = t.get();
                m_skipText = true;
                ++m_linkCount[tid];
            } else {
                t->storyId = m_doc.createStory();
                m_chains[tid] = t.get();
            }
        } else {
            t->storyId = m_doc.createStory();
        }
        // A plain rectangle drawn just before at the same place is this box's fill/border.
        ItemList &list = currentList();
        if (!list.empty() && list.back()->type() == ItemType::Shape) {
            auto *s = static_cast<ShapeItem *>(list.back().get());
            if (s->shape == "rect" && s->customPath.isEmpty() && s->storyId.isEmpty() && std::abs(s->rect.x() - t->rect.x()) < 1 &&
                std::abs(s->rect.y() - t->rect.y()) < 1 && std::abs(s->rect.width() - t->rect.width()) < 1 &&
                std::abs(s->rect.height() - t->rect.height()) < 1 && s->rotation == t->rotation) {
                t->fill = s->fill;
                t->stroke = s->stroke;
                t->fx = s->fx;
                list.pop_back();
                --m_rep.shapes;
            }
        }
        m_text = t;
        m_cursor = QTextCursor(m_doc.storyDoc(t->storyId));
        m_firstPara = !m_skipText;
        m_list = nullptr;
        add(t);
        ++m_rep.textBoxes;
    }
    void endTextObject() override
    {
        m_text.reset();
        m_cursor = QTextCursor();
        m_skipText = false;
    }

    void defineParagraphStyle(const RVNGPropertyList &) override {}
    void defineCharacterStyle(const RVNGPropertyList &) override {}

    void openParagraph(const RVNGPropertyList &p) override
    {
        if (qEnvironmentVariableIsSet("JP_PUB_TRACE")) fprintf(stderr, "PARA %s\n", p.getPropString().cstr());
        if (m_cursor.isNull() || m_skipText) return;
        QTextBlockFormat bf;
        const QString al = str(p["fo:text-align"]);
        if (al == "center") bf.setAlignment(Qt::AlignHCenter);
        else if (al == "end" || al == "right") bf.setAlignment(Qt::AlignRight);
        else if (al == "justify") bf.setAlignment(Qt::AlignJustify);
        else bf.setAlignment(Qt::AlignLeft);
        if (p["fo:margin-left"]) bf.setLeftMargin(toPt(p["fo:margin-left"]));
        if (p["fo:margin-right"]) bf.setRightMargin(toPt(p["fo:margin-right"]));
        if (p["fo:text-indent"]) bf.setTextIndent(toPt(p["fo:text-indent"]));
        if (p["fo:margin-top"]) bf.setTopMargin(toPt(p["fo:margin-top"]));
        if (p["fo:margin-bottom"]) bf.setBottomMargin(toPt(p["fo:margin-bottom"]));
        // Spacing in sp is always recorded (an explicit 1 sp differs from the
        // inherited Normal style), so layout can scale the font's line height.
        if (const RVNGProperty *sp = p["jp:line-spacing-sp"]) bf.setLineHeight(sp->getDouble() * 100.0, QTextBlockFormat::ProportionalHeight);
        else if (const RVNGProperty *lh = p["fo:line-height"]) {
            if (lh->getUnit() == librevenge::RVNG_PERCENT || lh->getUnit() == librevenge::RVNG_GENERIC) bf.setLineHeight(lh->getDouble() * 100.0, QTextBlockFormat::ProportionalHeight);
            else bf.setLineHeight(toPt(lh), QTextBlockFormat::FixedHeight);
        }
        if (const RVNGPropertyListVector *dc = p.child("style:drop-cap")) {
            if (dc->count() > 0) {
                const RVNGPropertyList &d = (*dc)[0];
                bf.setProperty(tp::DropCapLines, d["style:lines"] ? d["style:lines"]->getInt() : 3);
                bf.setProperty(tp::DropCapChars, d["style:length"] ? d["style:length"]->getInt() : 1);
            }
        } else if (p["style:drop-cap"]) {
            bf.setProperty(tp::DropCapLines, 3);
            bf.setProperty(tp::DropCapChars, 1);
        }
        if (const RVNGPropertyListVector *tabs = p.child("style:tab-stops")) {
            QList<QTextOption::Tab> tl;
            for (unsigned long i = 0; i < tabs->count(); ++i) {
                const RVNGPropertyList &t = (*tabs)[i];
                const QString type = str(t["style:type"]);
                QTextOption::Tab tab;
                tab.position = toPt(t["style:position"]);
                tab.type = type == "right" ? QTextOption::RightTab : type == "center" ? QTextOption::CenterTab : type == "char" ? QTextOption::DelimiterTab : QTextOption::LeftTab;
                tl << tab;
            }
            bf.setTabPositions(tl);
        }
        if (m_firstPara) {
            m_cursor.setBlockFormat(bf);
            m_firstPara = false;
        } else {
            m_cursor.insertBlock(bf);
        }
        applyList(p);
    }

    // A .pub list paragraph (kind 23 bulleted, else a numbering style) joins
    // the list of the paragraph before it when the settings match.
    void applyList(const RVNGPropertyList &p)
    {
        if (!p["jp:list-kind"]) {
            m_list = nullptr;
            return;
        }
        const int kind = p["jp:list-kind"]->getInt();
        const int ch = p["jp:list-char"] ? p["jp:list-char"]->getInt() : 0;
        const int delim = p["jp:list-delim"] ? p["jp:list-delim"]->getInt() : -1;
        const QString key = QStringLiteral("%1/%2/%3").arg(kind).arg(ch).arg(delim);
        const QTextBlock prev = m_cursor.block().previous();
        if (m_list && key == m_listKey && prev.isValid() && prev.textList() == m_list) {
            m_list->add(m_cursor.block());
            return;
        }
        QTextListFormat lf;
        lf.setIndent(0);
        if (kind == 23) {
            lf.setStyle(QTextListFormat::ListDisc);
            // 0xB7 is the round bullet in the Symbol font; others keep their Symbol character.
            if (ch && ch != 0xB7) {
                lf.setProperty(tp::BulletChar, QString(QChar(ch)));
                lf.setProperty(tp::BulletFont, QStringLiteral("Symbol"));
            }
        } else {
            // Publisher's numbering: 0 1 2 3, 1 I II, 2 i ii, 3 A B, 4 a b; punctuation
            // 2 "1.", 0 "1)", 1 "(1)".
            static const QTextListFormat::Style styles[] = {QTextListFormat::ListDecimal, QTextListFormat::ListUpperRoman, QTextListFormat::ListLowerRoman,
                                                           QTextListFormat::ListUpperAlpha, QTextListFormat::ListLowerAlpha};
            lf.setStyle(kind >= 0 && kind <= 4 ? styles[kind] : QTextListFormat::ListDecimal);
            lf.setNumberSuffix(delim == 0 || delim == 1 ? QStringLiteral(")") : QStringLiteral("."));
            if (delim == 1) lf.setNumberPrefix(QStringLiteral("("));
            static const int formats[] = {1, 5, 4, 3, 2};
            const int format = kind == 0 && delim == 0 ? 6 : kind == 0 && delim == 1 ? 7 : kind >= 0 && kind <= 4 ? formats[kind] : 1;
            lf.setProperty(tp::NumberFormat, format);
        }
        m_list = m_cursor.createList(lf);
        m_listKey = key;
    }
    void closeParagraph() override {}

    void openSpan(const RVNGPropertyList &p) override
    {
        if (qEnvironmentVariableIsSet("JP_PUB_TRACE")) fprintf(stderr, "SPAN %s\n", p.getPropString().cstr());
        QTextCharFormat cf;
        const QString font = str(p["style:font-name"]);
        if (!font.isEmpty()) {
            cf.setFontFamilies(QStringList{font});
            if (!m_rep.fontsUsed.contains(font)) m_rep.fontsUsed << font;
        }
        if (p["fo:font-size"]) cf.setFontPointSize(toPt(p["fo:font-size"]));
        const QString w = str(p["fo:font-weight"]);
        if (w == "bold" || w.toInt() >= 600) cf.setFontWeight(QFont::Bold);
        if (str(p["fo:font-style"]) == "italic") cf.setFontItalic(true);
        const QString ut = str(p["style:text-underline-type"]), us = str(p["style:text-underline-style"]);
        if (!ut.isEmpty() && ut != "none") {
            QTextCharFormat::UnderlineStyle u = QTextCharFormat::SingleUnderline;
            if (us == "dotted") u = QTextCharFormat::DotLine;
            else if (us == "dash") u = QTextCharFormat::DashUnderline;
            else if (us == "wave") u = QTextCharFormat::WaveUnderline;
            cf.setUnderlineStyle(u);
        }
        if (str(p["style:text-line-through-type"]) == "single" || str(p["style:text-line-through-style"]) == "solid") cf.setFontStrikeOut(true);
        const QColor c = color(p["fo:color"]);
        if (c.isValid()) cf.setProperty(tp::ColorRefP, c.name().toUpper());
        const QColor bg = color(p["fo:background-color"]);
        if (bg.isValid()) cf.setProperty(tp::HighlightRefP, bg.name().toUpper());
        // "super", "sub", or a raise like "50% 67%" (positive up, negative down).
        const QString pos = str(p["style:text-position"]);
        const double raise = pos.section(' ', 0, 0).remove('%').toDouble();
        if (pos.startsWith("super") || raise > 0) cf.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        else if (pos.startsWith("sub") || raise < 0) cf.setVerticalAlignment(QTextCharFormat::AlignSubScript);
        if (str(p["fo:font-variant"]) == "small-caps") cf.setFontCapitalization(QFont::SmallCaps);
        if (str(p["fo:text-transform"]) == "uppercase") cf.setFontCapitalization(QFont::AllUppercase);
        if (str(p["style:text-outline"]) == "true") cf.setProperty(tp::OutlineRef, c.isValid() ? c.name().toUpper() : QStringLiteral("#000000"));
        // Text outline stroke (a .pub text box's text line).
        const QColor oc = color(p["jp:text-outline-color"]);
        if (oc.isValid() && p["jp:text-outline-width"]) {
            cf.setProperty(tp::OutlineRef, oc.name().toUpper());
            cf.setProperty(tp::OutlineWidth, toPt(p["jp:text-outline-width"]));
        }
        if (!str(p["fo:text-shadow"]).isEmpty() && str(p["fo:text-shadow"]) != "none") cf.setProperty(tp::Shadow, true);
        const QString relief = str(p["style:font-relief"]);
        if (relief == "embossed") cf.setProperty(tp::Emboss, true);
        if (relief == "engraved") cf.setProperty(tp::Engrave, true);
        if (p["fo:text-scale"]) cf.setFontStretch(std::clamp(int(std::round(percent(p["fo:text-scale"]) * 100)), 1, 4000));
        if (p["fo:letter-spacing"]) {
            cf.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            cf.setFontLetterSpacing(toPt(p["fo:letter-spacing"]));
        } else if (p["jp:tracking"]) {
            // Publisher's tracking, as a percentage of normal spacing.
            cf.setFontLetterSpacingType(QFont::PercentageSpacing);
            cf.setFontLetterSpacing(p["jp:tracking"]->getDouble());
        }
        const QString lang = str(p["fo:language"]), country = str(p["fo:country"]);
        if (!lang.isEmpty()) cf.setProperty(tp::Language, country.isEmpty() ? lang : lang + '-' + country);
        m_span = cf;
    }
    void closeSpan() override { m_span = QTextCharFormat(); }
    void openLink(const RVNGPropertyList &p) override { m_link = str(p["xlink:href"]); }
    void closeLink() override { m_link.clear(); }

    void insertText(const librevenge::RVNGString &text) override
    {
        if (qEnvironmentVariableIsSet("JP_PUB_TRACE")) fprintf(stderr, "TEXT %s\n", QString::fromUtf8(text.cstr()).toUtf8().toPercentEncoding(" ").constData());
        put(QString::fromUtf8(text.cstr()));
    }
    void insertTab() override { put(QStringLiteral("\t")); }
    void insertSpace() override { put(QStringLiteral(" ")); }
    void insertLineBreak() override { put(QString(QChar(QChar::LineSeparator))); }
    void insertField(const RVNGPropertyList &p) override
    {
        if (m_cursor.isNull() || m_skipText) return;
        const QString type = str(p["librevenge:field-type"]);
        QTextCharFormat cf = m_span;
        if (type == "text:page-number") cf.setProperty(tp::Field, QStringLiteral("page"));
        else if (type == "text:page-count") cf.setProperty(tp::Field, QStringLiteral("pages"));
        else if (type == "text:date") cf.setProperty(tp::Field, QStringLiteral("date"));
        else if (type == "text:time") cf.setProperty(tp::Field, QStringLiteral("time"));
        else return;
        m_cursor.insertText(QString(QChar::ObjectReplacementCharacter), cf);
    }

    void openOrderedListLevel(const RVNGPropertyList &) override {}
    void openUnorderedListLevel(const RVNGPropertyList &) override {}
    void closeOrderedListLevel() override {}
    void closeUnorderedListLevel() override {}
    void openListElement(const RVNGPropertyList &p) override { openParagraph(p); }
    void closeListElement() override {}

    // ---------- tables ----------
    void startTableObject(const RVNGPropertyList &p) override
    {
        m_table = std::make_shared<TableItem>();
        m_table->rect = QRectF(toPt(p["svg:x"]), toPt(p["svg:y"]), toPt(p["svg:width"]), toPt(p["svg:height"]));
        m_table->colW.clear();
        if (const RVNGPropertyListVector *cols = p.child("librevenge:table-columns"))
            for (unsigned long i = 0; i < cols->count(); ++i) m_table->colW << toPt((*cols)[i]["style:column-width"], 72);
        m_cells.clear();
        m_row = -1;
        m_table->rowH.clear();
        m_table->format = QStringLiteral("None");
        m_table->header = false;
        m_table->banded = false;
        m_table->growToFit = false;
    }
    void openTableRow(const RVNGPropertyList &p) override
    {
        if (!m_table) return;
        ++m_row;
        double h = toPt(p["librevenge:row-height"]);
        if (h <= 0) h = toPt(p["style:row-height"]);
        if (h <= 0) h = toPt(p["style:min-row-height"], 18);
        m_table->rowH << h;
        m_cells.push_back({});
    }
    void closeTableRow() override {}
    void openTableCell(const RVNGPropertyList &p) override
    {
        if (!m_table || m_row < 0) return;
        TableCell c;
        c.storyId = m_doc.createStory();
        if (p["table:number-columns-spanned"]) c.colSpan = std::max(1, p["table:number-columns-spanned"]->getInt());
        if (p["table:number-rows-spanned"]) c.rowSpan = std::max(1, p["table:number-rows-spanned"]->getInt());
        const QColor bg = color(p["fo:background-color"]);
        if (bg.isValid()) c.fill = Fill::solid(ColorRef::rgb(bg));
        auto border = [&](const char *key) {
            const QString s = str(p[key]);
            if (s.isEmpty() || s == "none") return Stroke::none();
            const QStringList parts = s.split(' ', Qt::SkipEmptyParts);
            Stroke st;
            st.width = 0.75;
            for (const auto &part : parts) {
                if (part.startsWith('#')) st.color = ColorRef::rgb(QColor(part));
                else if (part.endsWith("in")) st.width = part.chopped(2).toDouble() * 72;
                else if (part.endsWith("pt")) st.width = part.chopped(2).toDouble();
                else if (part == "dashed") st.dash = Stroke::DashLine;
                else if (part == "dotted") st.dash = Stroke::RoundDot;
                else if (part == "double") st.compound = Stroke::Double;
            }
            if (st.color.isNone()) st.color = ColorRef::rgb(Qt::black);
            return st;
        };
        c.border.left = border("fo:border-left");
        c.border.right = border("fo:border-right");
        c.border.top = border("fo:border-top");
        c.border.bottom = border("fo:border-bottom");
        const QString va = str(p["style:vertical-align"]);
        c.valign = va == "middle" ? VAlign::Middle : va == "bottom" ? VAlign::Bottom : VAlign::Top;
        if (p["fo:padding-left"])
            c.margins = QMarginsF(toPt(p["fo:padding-left"]), toPt(p["fo:padding-top"]), toPt(p["fo:padding-right"]), toPt(p["fo:padding-bottom"]));
        m_cells.back().push_back(c);
        m_cursor = QTextCursor(m_doc.storyDoc(c.storyId));
        m_firstPara = true;
        m_list = nullptr;
        m_skipText = false;
    }
    void closeTableCell() override { m_cursor = QTextCursor(); }
    void insertCoveredTableCell(const RVNGPropertyList &) override
    {
        if (!m_table || m_row < 0) return;
        TableCell c;
        c.storyId = m_doc.createStory();
        c.covered = true;
        m_cells.back().push_back(c);
    }
    void endTableObject() override
    {
        if (!m_table) return;
        auto t = m_table;
        m_table.reset();
        t->rows = int(m_cells.size());
        int cols = t->colW.size();
        for (const auto &r : m_cells) cols = std::max(cols, int(r.size()));
        t->cols = cols;
        if (t->rows == 0 || cols == 0) return;
        while (t->colW.size() < cols) t->colW << t->rect.width() / cols;
        while (t->rowH.size() < t->rows) t->rowH << 18;
        t->cells.clear();
        for (auto &r : m_cells) {
            while (int(r.size()) < cols) { TableCell c; c.storyId = m_doc.createStory(); r.push_back(c); }
            for (auto &c : r) t->cells.push_back(c);
        }
        // Scale columns and rows to the table frame libmspub gave us.
        double sw = 0, sh = 0;
        for (double w : t->colW) sw += w;
        for (double h : t->rowH) sh += h;
        if (sw > 0 && t->rect.width() > 0) for (double &w : t->colW) w *= t->rect.width() / sw;
        if (sh > 0 && t->rect.height() > 0) for (double &h : t->rowH) h *= t->rect.height() / sh;
        t->syncRect();
        add(t);
        ++m_rep.tables;
    }

    void finish()
    {
        for (const auto &kv : m_linkCount)
            if (kv.second > 0) ++m_rep.linkedChains;
        if (m_doc.pages.isEmpty()) m_doc.addPage();
    }

private:
    ItemList &currentList()
    {
        if (!m_stack.empty()) return m_stack.back()->children;
        if (m_master) return m_master->items;
        if (m_page) return m_page->items;
        return m_doc.scratch;
    }

    void add(const ItemPtr &it)
    {
        currentList().push_back(it);
        m_fillOnly = nullptr;
    }

    // Publisher's Text Art: the warp from its shape number, the words and
    // settings, the unturned frame. A shape flipped one way turns the other
    // way round in Publisher (JeffPub always turns clockwise).
    void addTextArt(const RVNGPropertyList &p)
    {
        auto ta = std::make_shared<TextArtItem>();
        ta->rect = QRectF(toPt(p["svg:x"]), toPt(p["svg:y"]), toPt(p["svg:width"]), toPt(p["svg:height"]));
        ta->text = str(p["jp:textart-text"]);
        if (p["jp:textart-font"]) ta->font = str(p["jp:textart-font"]);
        const QString tf = p["jp:textart-type"] ? textArtTransformForPubType(p["jp:textart-type"]->getInt()) : QString();
        ta->transform_ = tf.isEmpty() ? QStringLiteral("plain") : tf;
        if (p["jp:textart-size"]) ta->size = (p["jp:textart-size"]->getInt() & 0xffffffff) / 65536.0;
        if (p["jp:textart-spacing"]) ta->spacing = (p["jp:textart-spacing"]->getInt() & 0xffffffff) / 65536.0;
        // Publisher's alignment: stretch 0, center 1, left 2, right 3, letter 4, word 5.
        static const int kAlign[] = {5, 1, 0, 2, 4, 3};
        if (p["jp:textart-align"]) ta->align = kAlign[std::clamp(p["jp:textart-align"]->getInt(), 0, 5)];
        if (p["jp:textart-flags"]) {
            const unsigned f = unsigned(p["jp:textart-flags"]->getInt());
            ta->bold = f & 0x20;
            ta->italic = f & 0x10;
            ta->evenHeight = f & 0x80;
            ta->vertical = f & 0x2000;
        }
        ta->flipH = p["jp:flip-h"] && p["jp:flip-h"]->getInt();
        ta->flipV = p["jp:flip-v"] && p["jp:flip-v"]->getInt();
        double rot = p["jp:rotation"] ? p["jp:rotation"]->getDouble() : 0;
        if (ta->flipH != ta->flipV) rot = -rot;
        ta->rotation = std::fmod(std::fmod(rot, 360.0) + 360.0, 360.0);
        QByteArray bitmap;
        QString mime;
        ta->fill = fillFromStyle(&bitmap, &mime);
        if (ta->fill.type == Fill::Picture || ta->fill.type == Fill::Texture) ta->fill = Fill::solid(ColorRef::rgb(Qt::black));
        ta->stroke = strokeFromStyle();
        ta->wrap.mode = Wrap::None;
        add(ta);
        ++m_rep.shapes;
    }

    void put(const QString &s)
    {
        if (m_cursor.isNull() || m_skipText) return;
        QTextCharFormat cf = m_span;
        if (!m_link.isEmpty()) {
            cf.setAnchor(true);
            cf.setAnchorHref(m_link);
        }
        // .pub text keeps each paragraph's own mark (\r) in the text; paragraphs are
        // already delimited by openParagraph, so the mark must not make another.
        // Shift+Enter is \v (or \n): a line break inside the paragraph.
        QString t = s;
        t.remove(QLatin1Char('\r'));
        t.replace(QLatin1Char('\v'), QChar::LineSeparator);
        t.replace(QLatin1Char('\n'), QChar::LineSeparator);
        if (t.isEmpty()) {
            // A bare paragraph mark still sizes an empty paragraph.
            if (m_cursor.block().length() <= 1) m_cursor.setBlockCharFormat(cf);
            return;
        }
        m_cursor.insertText(t, cf);
    }

    Fill fillFromStyle(QByteArray *bitmap, QString *mime) const
    {
        const QString f = str(m_style["draw:fill"]);
        const double opacity = m_style["draw:opacity"] ? percent(m_style["draw:opacity"]) : 1.0;
        if (f == "solid") {
            const QColor c = color(m_style["draw:fill-color"]);
            return Fill::solid(ColorRef::rgb(c.isValid() ? c : QColor(Qt::white)), 1 - opacity);
        }
        if (f == "gradient") {
            Fill g;
            g.type = Fill::Gradient;
            g.angle = 90 - (m_style["draw:angle"] ? m_style["draw:angle"]->getDouble() : 0);
            const QString shade = str(m_style["libmspub:shade"]);
            if (shade == "center" || shade == "shape") g.gradType = Fill::Radial;
            if (const RVNGPropertyListVector *stops = m_style.child("svg:linearGradient")) {
                for (unsigned long i = 0; i < stops->count(); ++i) {
                    const RVNGPropertyList &s = (*stops)[i];
                    g.stops << GradientStop{percent(s["svg:offset"], 0), ColorRef::rgb(color(s["svg:stop-color"])), 1 - percent(s["svg:stop-opacity"], 1)};
                }
            }
            if (g.stops.size() >= 2) {
                g.color = g.stops.first().color;
                g.color2 = g.stops.last().color;
            } else {
                g.color = ColorRef::rgb(color(m_style["draw:fill-color"]));
                g.color2 = ColorRef::rgb(Qt::white);
                g.stops.clear();
            }
            return g;
        }
        if (f == "bitmap") {
            if (bitmap) *bitmap = QByteArray::fromBase64(QByteArray(m_style["draw:fill-image"] ? m_style["draw:fill-image"]->getStr().cstr() : ""));
            if (mime) *mime = str(m_style["librevenge:mime-type"]);
            // A texture repeats the picture; anything else stretches it.
            Fill pf;
            pf.type = str(m_style["style:repeat"]) == "stretch" ? Fill::Picture : Fill::Texture;
            return pf;
        }
        return Fill::none();
    }

    Stroke strokeFromStyle() const
    {
        const QString s = str(m_style["draw:stroke"]);
        if (s.isEmpty() || s == "none") return Stroke::none();
        Stroke st;
        const QColor c = color(m_style["svg:stroke-color"]);
        st.color = ColorRef::rgb(c.isValid() ? c : QColor(Qt::black));
        st.width = m_style["svg:stroke-width"] ? toPt(m_style["svg:stroke-width"]) : 0.75;
        if (st.width <= 0) st.width = 0.25;
        if (m_style["svg:stroke-opacity"]) st.transparency = 1 - percent(m_style["svg:stroke-opacity"]);
        if (s == "dash") st.dash = m_style["draw:dots1-length"] && toPt(m_style["draw:dots1-length"]) < st.width * 1.5 ? Stroke::RoundDot : Stroke::DashLine;
        if (str(m_style["svg:stroke-linecap"]) == "round") st.cap = Qt::RoundCap;
        if (!str(m_style["draw:marker-start-path"]).isEmpty()) st.startArrow = Arrow::Triangle;
        if (!str(m_style["draw:marker-end-path"]).isEmpty()) st.endArrow = Arrow::Triangle;
        // Arrowheads as .pub files store them: 1 triangle, 2 stealth,
        // 3 diamond, 4 oval, 5 open; sizes 0-2.
        static const Arrow kArrows[] = {Arrow::None, Arrow::Triangle, Arrow::Stealth, Arrow::Diamond, Arrow::Oval, Arrow::Open};
        if (m_style["jp:arrow-start"]) {
            st.startArrow = kArrows[std::clamp(m_style["jp:arrow-start"]->getInt(), 0, 5)];
            if (m_style["jp:arrow-start-size"]) st.startSize = std::clamp(m_style["jp:arrow-start-size"]->getInt(), 0, 2);
        }
        if (m_style["jp:arrow-end"]) {
            st.endArrow = kArrows[std::clamp(m_style["jp:arrow-end"]->getInt(), 0, 5)];
            if (m_style["jp:arrow-end-size"]) st.endSize = std::clamp(m_style["jp:arrow-end-size"]->getInt(), 0, 2);
        }
        return st;
    }

    void applyShadow(Item &it) const
    {
        if (str(m_style["draw:shadow"]) != "visible") return;
        it.fx.shadow.on = true;
        const double dx = toPt(m_style["draw:shadow-offset-x"]), dy = toPt(m_style["draw:shadow-offset-y"]);
        it.fx.shadow.distance = std::hypot(dx, dy);
        it.fx.shadow.angle = qRadiansToDegrees(std::atan2(dy, dx));
        it.fx.shadow.blur = 0;
        const QColor c = color(m_style["draw:shadow-color"]);
        if (c.isValid()) it.fx.shadow.color = ColorRef::rgb(c);
        it.fx.shadow.transparency = 1 - percent(m_style["draw:shadow-opacity"], 1.0);
    }

    std::shared_ptr<PictureItem> makePicture(const QRectF &r, double rotation, const QByteArray &bytes, const QString &mime)
    {
        auto pic = std::make_shared<PictureItem>();
        pic->rect = r;
        pic->rotation = rotation;
        pic->imgRect = QRectF(QPointF(0, 0), r.size());
        pic->wrap.mode = Wrap::None;
        if (!bytes.isEmpty()) {
            const QString fmt = formatForMime(mime);
            pic->imageId = m_doc.addImage(bytes, fmt);
            if (m_doc.image(pic->imageId).isNull())
                m_rep.warnings << QStringLiteral("A %1 picture could not be displayed.").arg(fmt.toUpper());
        }
        ++m_rep.pictures;
        return pic;
    }

    void addPath(const QPainterPath &pathIn, bool open)
    {
        const QRectF b = pathIn.boundingRect();
        if (b.width() < 0.01 && b.height() < 0.01) return;
        if (qEnvironmentVariableIsSet("JP_PUB_TRACE"))
            fprintf(stderr, "PATH %.1f,%.1f %.1fx%.1f open=%d n=%d style=%s\n", b.x(), b.y(), b.width(), b.height(), int(open), pathIn.elementCount(), m_style.getPropString().cstr());
        QByteArray bitmap;
        QString mime;
        Fill fill = open ? Fill::none() : fillFromStyle(&bitmap, &mime);
        const Stroke stroke = strokeFromStyle();
        if (fill.type == Fill::NoFill && stroke.isNone() && !open) return;
        // A line explicitly drawn without a stroke (a box's absent border side) shows nothing.
        if (open && stroke.isNone() && str(m_style["draw:stroke"]) == "none") return;
        // The reader draws a shape's fill and then its outline as a second
        // path (a rectangle's pushed out by up to half the line). Give the
        // outline back to the filled shape rather than making two objects.
        if (!open && fill.type == Fill::NoFill && m_fillOnly && pathIn.elementCount() == m_fillOnlyCount && !currentList().empty()
            && currentList().back().get() == m_fillOnly) {
            const double slack = stroke.width / 2 + 0.3;
            const QRectF &f = m_fillOnlyBounds;
            if (std::abs(b.left() - f.left()) <= slack && std::abs(b.top() - f.top()) <= slack && std::abs(b.right() - f.right()) <= slack
                && std::abs(b.bottom() - f.bottom()) <= slack) {
                m_fillOnly->stroke = stroke;
                m_fillOnly = nullptr;
                return;
            }
        }

        // Straight lines become line objects.
        if (pathIn.elementCount() == 2 && open) {
            auto l = std::make_shared<LineItem>();
            l->p1 = pathIn.elementAt(0);
            l->p2 = pathIn.elementAt(1);
            l->stroke = stroke.isNone() ? Stroke::line(ColorRef::rgb(Qt::black), 0.75) : stroke;
            l->syncRect();
            applyShadow(*l);
            add(l);
            ++m_rep.shapes;
            return;
        }

        // Detect (possibly rotated) rectangles.
        QPolygonF poly = pathIn.toFillPolygon();
        if (poly.size() > 1 && poly.first() == poly.last()) poly.removeLast();
        bool isRect = false;
        double rot = 0, rw = 0, rh = 0;
        if (poly.size() == 4) {
            const QPointF e1 = poly[1] - poly[0], e2 = poly[2] - poly[1], e3 = poly[3] - poly[2];
            const double dot = e1.x() * e2.x() + e1.y() * e2.y();
            const double l1 = std::hypot(e1.x(), e1.y()), l2 = std::hypot(e2.x(), e2.y());
            if (l1 > 0 && l2 > 0 && std::abs(dot) / (l1 * l2) < 0.01 && std::abs(std::hypot(e3.x(), e3.y()) - l1) < 0.5) {
                isRect = true;
                rot = qRadiansToDegrees(std::atan2(e1.y(), e1.x()));
                rw = l1;
                rh = l2;
                // Normalize so a near-axis rectangle has rotation 0.
                while (rot <= -45) { rot += 90; std::swap(rw, rh); }
                while (rot > 45) { rot -= 90; std::swap(rw, rh); }
                if (std::abs(rot) < 0.05) rot = 0;
            }
        }
        const QPointF center = isRect ? (poly[0] + poly[2]) / 2 : b.center();

        if (fill.type == Fill::Texture) {
            fill.imageId = bitmap.isEmpty() ? QString() : m_doc.addImage(bitmap, formatForMime(mime));
            if (fill.imageId.isEmpty() || m_doc.image(fill.imageId).isNull()) fill = Fill::solid(ColorRef::rgb(QColor(220, 220, 220)));
        }
        if (fill.type == Fill::Picture) {
            const QRectF r = isRect ? QRectF(center.x() - rw / 2, center.y() - rh / 2, rw, rh) : b;
            auto pic = makePicture(r, isRect ? rot : 0, bitmap, mime);
            pic->stroke = stroke;
            if (!isRect) {
                // Non-rectangular picture frames: approximate with an oval mask when it fits.
                pic->maskShape = QStringLiteral("ellipse");
            }
            applyRecolor(pic.get(), m_style);
            applyShadow(*pic);
            add(pic);
            if (stroke.isNone()) rememberFillOnly(pic.get(), b, pathIn.elementCount());
            return;
        }

        auto s = std::make_shared<ShapeItem>();
        s->fill = fill;
        s->stroke = stroke;
        s->wrap.mode = Wrap::None;
        if (isRect) {
            s->shape = QStringLiteral("rect");
            s->rect = QRectF(center.x() - rw / 2, center.y() - rh / 2, rw, rh);
            s->rotation = rot;
        } else {
            s->shape = QStringLiteral("rect");
            s->rect = b.width() < 0.5 || b.height() < 0.5 ? b.adjusted(-0.25, -0.25, 0.25, 0.25) : b;
            s->customPath = pathIn.translated(-s->rect.topLeft());
            if (open) s->fill = Fill::none();
        }
        applyShadow(*s);
        add(s);
        ++m_rep.shapes;
        if (stroke.isNone() && !open && s->fill.type != Fill::NoFill) rememberFillOnly(s.get(), b, pathIn.elementCount());
    }

    void rememberFillOnly(Item *it, const QRectF &bounds, int count)
    {
        m_fillOnly = it;
        m_fillOnlyBounds = bounds;
        m_fillOnlyCount = count;
    }

    Document &m_doc;
    Item *m_fillOnly = nullptr;           // the last shape added with a fill and no outline
    QRectF m_fillOnlyBounds;
    int m_fillOnlyCount = 0;
    PubImportReport &m_rep;
    Page *m_page = nullptr;
    MasterPage *m_master = nullptr;       // set while a master page's objects arrive
    QHash<int, QString> m_masterIds;      // .pub master page sequence number -> master id
    RVNGPropertyList m_style;
    std::vector<std::shared_ptr<GroupItem>> m_stack;
    std::shared_ptr<TextItem> m_text;
    QTextCursor m_cursor;
    QTextCharFormat m_span;
    QString m_link;
    bool m_firstPara = true;
    QTextList *m_list = nullptr;   // the list the last paragraph joined
    QString m_listKey;
    bool m_skipText = false;
    std::map<int, TextItem *> m_chains;
    std::map<int, int> m_linkCount;
    std::shared_ptr<TableItem> m_table;
    std::vector<std::vector<TableCell>> m_cells;
    int m_row = -1;
};

} // namespace

bool isPublisherFile(const QByteArray &head)
{
    return head.size() >= 8 && quint8(head[0]) == 0xD0 && quint8(head[1]) == 0xCF && quint8(head[2]) == 0x11 && quint8(head[3]) == 0xE0;
}

std::unique_ptr<Document> importPublisher(const QByteArray &data, QString *error, PubImportReport *report)
{
    static bool hooked = [] { libmspub::setDecodeHook(&decodeWithQt); return true; }();
    Q_UNUSED(hooked);
    PubImportReport localReport;
    PubImportReport &rep = report ? *report : localReport;
    if (!isPublisherFile(data)) {
        if (error) *error = QStringLiteral("This is not a .pub publication file.");
        return nullptr;
    }
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char *>(data.constData()), (unsigned)data.size());
    if (!libmspub::MSPUBDocument::isSupported(&input)) {
        if (error) *error = QStringLiteral("JeffPub 79 can't read this version of .pub file.");
        return nullptr;
    }
    auto doc = std::make_unique<Document>();
    doc->pages.clear();
    Collector c(*doc, rep);
    bool ok = false;
    try {
        input.seek(0, librevenge::RVNG_SEEK_SET);
        ok = libmspub::MSPUBDocument::parse(&input, &c);
    } catch (const std::exception &e) {
        if (error) *error = QStringLiteral("The .pub file is damaged: %1").arg(QString::fromUtf8(e.what()));
        return nullptr;
    } catch (...) {
        if (error) *error = QStringLiteral("The .pub file is damaged.");
        return nullptr;
    }
    c.finish();
    if (!ok) {
        if (error) *error = QStringLiteral("JeffPub 79 could not read this .pub file. It may be damaged or use features that aren't supported yet.");
        return nullptr;
    }
    doc->props.created = QDateTime::currentDateTime();
    // The .pub "shrink text on overflow" setting is not exposed by libmspub.
    // Finished publications don't hide text, so a lone box whose text overflows
    // with our fonts is set to shrink to fit.
    LayoutCache cache;
    RenderOptions opt;
    QVector<TextItem *> boxes;
    doc->forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Text) boxes << static_cast<TextItem *>(it);
    });
    int fitted = 0;
    for (TextItem *t : boxes) {
        if (!t->nextId.isEmpty() || doc->prevFrame(t->id)) continue;
        const auto fl = cache.textFrame(*doc, *t, 1, opt);
        if (fl.layout && fl.layout->overflow() && !qEnvironmentVariableIsSet("JP_NO_SHRINK")) {
            t->autofit = TextItem::ShrinkOnOverflow;
            const auto fit = cache.textFrame(*doc, *t, 1, opt);
            // Heavy shrinking means the author left text hidden on purpose.
            if (fit.fitScale < 0.6) t->autofit = TextItem::NoAutofit;
            else ++fitted;
        }
    }
    if (fitted) rep.warnings << QStringLiteral("%1 text box(es) set to shrink text slightly so it fits.").arg(fitted);
    return doc;
}

std::unique_ptr<Document> importPublisherFile(const QString &path, QString *error, PubImportReport *report)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return nullptr;
    }
    return importPublisher(f.readAll(), error, report);
}

} // namespace jp
