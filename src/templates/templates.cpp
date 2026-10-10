#include "templates/templates.h"

#include "core/presets.h"
#include "render/textart.h"
#include "templates/changetemplate.h"
#include "text/textprops.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDate>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QSet>
#include <QtMath>

#include <future>
#include <iterator>
#include <thread>
#include <vector>

namespace jp {

namespace {

constexpr double IN = 72.0;

// ---------------- generated artwork ----------------
// Abstract pictures painted in the scheme's colors stand in for photos.

// The same picture in the same colors is made once per session (the New page
// builds every template to show it).
QHash<QString, QByteArray> &artCache()
{
    static QHash<QString, QByteArray> done;
    return done;
}

QString artKey(const ColorScheme &s, const QString &kind, const QSize &px, quint32 seed)
{
    QString key = kind + QLatin1Char('|') + QString::number(px.width()) + QLatin1Char('x') + QString::number(px.height()) + QLatin1Char('|') +
                  QString::number(seed);
    for (int i = 0; i < SlotCount; ++i) key += QLatin1Char('|') + s.c[i].name();
    return key;
}

// One picture as PNG. Touches nothing shared, so several can be made at once.
QByteArray paintArt(const ColorScheme &s, const QString &kind, const QSize &px, quint32 seed)
{
    // Made at most 3,000 pixels on its longest side: decorative art on a
    // 5-foot banner needs no more (60 dpi there), and every pixel past that
    // cost time and file size. It is drawn in the size asked for and scaled
    // down, so it looks the same, only less finely.
    const int longest = std::max(px.width(), px.height());
    const double fit = longest > 3000 ? 3000.0 / longest : 1.0;
    const QSize made = QSize(std::max(1, int(std::lround(px.width() * fit))), std::max(1, int(std::lround(px.height() * fit))));
    QImage img(made, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(double(made.width()) / std::max(1, px.width()), double(made.height()) / std::max(1, px.height()));
    const QRectF all(0, 0, px.width(), px.height());
    // qHash is randomized per process; artwork must be the same every run.
    QRandomGenerator rng(seed * 7919 + qChecksum(kind.toUtf8()));
    const double w = px.width(), h = px.height();
    auto c = [&](int slot, int lighten = 0) { return ColorRef::scheme(slot, lighten).resolve(s); };
    if (kind == "hills") {
        QLinearGradient sky(0, 0, 0, h);
        sky.setColorAt(0, c(Accent2, 50));
        sky.setColorAt(1, c(Accent5));
        p.fillRect(all, sky);
        p.setPen(Qt::NoPen);
        p.setBrush(c(Accent3, 10));
        p.drawEllipse(QPointF(w * 0.72, h * 0.3), w * 0.09, w * 0.09);
        for (int layer = 0; layer < 3; ++layer) {
            QPainterPath hill;
            const double base = h * (0.55 + layer * 0.14);
            hill.moveTo(0, h);
            hill.lineTo(0, base);
            for (int i = 0; i <= 8; ++i) {
                const double x = w * i / 8.0;
                hill.quadTo(x - w / 16, base - h * (0.08 + 0.1 * rng.generateDouble()), x, base + h * 0.02 * (i % 2));
            }
            hill.lineTo(w, h);
            p.setBrush(layer == 0 ? c(Accent1, 40) : layer == 1 ? c(Accent1, 15) : ColorRef::scheme(Accent1, 0, 20).resolve(s));
            p.drawPath(hill);
        }
    } else if (kind == "bokeh") {
        QLinearGradient bg(0, 0, w, h);
        bg.setColorAt(0, ColorRef::scheme(Accent1, 0, 40).resolve(s));
        bg.setColorAt(1, ColorRef::scheme(Main).resolve(s));
        p.fillRect(all, bg);
        for (int i = 0; i < 46; ++i) {
            const double r = w * (0.02 + 0.07 * rng.generateDouble());
            QColor col = c(1 + int(rng.bounded(4)), 30);
            col.setAlphaF(float(0.15 + 0.35 * rng.generateDouble()));
            QRadialGradient g(QPointF(rng.bounded(int(w)), rng.bounded(int(h))), r);
            g.setColorAt(0, col);
            QColor edge = col;
            edge.setAlphaF(0);
            g.setColorAt(1, edge);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(g.center(), r, r);
        }
    } else if (kind == "confetti") {
        p.fillRect(all, c(Accent5));
        for (int i = 0; i < 160; ++i) {
            p.save();
            p.translate(rng.bounded(int(w)), rng.bounded(int(h)));
            p.rotate(rng.bounded(360));
            p.setPen(Qt::NoPen);
            p.setBrush(c(1 + int(rng.bounded(4))));
            const double sz = w * (0.008 + 0.014 * rng.generateDouble());
            if (i % 3 == 0) p.drawEllipse(QPointF(0, 0), sz, sz);
            else p.drawRect(QRectF(-sz, -sz / 3, sz * 2, sz * 0.7));
            p.restore();
        }
    } else if (kind == "waves") {
        p.fillRect(all, c(Accent5));
        for (int i = 0; i < 7; ++i) {
            QPainterPath wave;
            const double y0 = h * (0.25 + i * 0.11);
            wave.moveTo(0, h);
            wave.lineTo(0, y0);
            for (int k = 0; k <= 4; ++k) wave.quadTo(w * (k + 0.5) / 4, y0 + (k % 2 ? -1 : 1) * h * 0.06, w * (k + 1) / 4, y0);
            wave.lineTo(w, h);
            QColor col = c(i % 2 ? Accent2 : Accent1, 60 - i * 8);
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawPath(wave);
        }
    } else if (kind == "geo") {
        p.fillRect(all, c(Main));
        const int n = 8;
        const double cw = w / n, ch = h / (n * h / w);
        for (int yy = 0; yy * ch < h + ch; ++yy)
            for (int xx = 0; xx < n; ++xx) {
                QPainterPath tri;
                const double x0 = xx * cw, y0 = yy * ch;
                if (rng.bounded(2)) tri.addPolygon(QPolygonF({QPointF(x0, y0), QPointF(x0 + cw, y0), QPointF(x0, y0 + ch), QPointF(x0, y0)}));
                else tri.addPolygon(QPolygonF({QPointF(x0 + cw, y0), QPointF(x0 + cw, y0 + ch), QPointF(x0, y0 + ch), QPointF(x0 + cw, y0)}));
                p.fillPath(tri, c(1 + int(rng.bounded(4)), int(rng.bounded(3)) * 20));
            }
    } else if (kind == "sunburst") {
        p.fillRect(all, c(Accent3, 20));
        const QPointF ctr(w / 2, h * 0.9);
        for (int i = 0; i < 24; ++i) {
            QPainterPath ray;
            const double a0 = M_PI * i / 24 - M_PI, a1 = M_PI * (i + 0.5) / 24 - M_PI;
            ray.moveTo(ctr);
            ray.lineTo(ctr + QPointF(std::cos(a0), std::sin(a0)) * w * 1.2);
            ray.lineTo(ctr + QPointF(std::cos(a1), std::sin(a1)) * w * 1.2);
            ray.closeSubpath();
            p.fillPath(ray, c(Accent3, 45));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c(Accent1));
        p.drawEllipse(ctr, w * 0.18, w * 0.18);
    } else if (kind == "skyline") {
        QLinearGradient sky(0, 0, 0, h);
        sky.setColorAt(0, c(Accent1, 0));
        sky.setColorAt(1, c(Accent3, 40));
        p.fillRect(all, sky);
        double x = 0;
        while (x < w) {
            const double bw = w * (0.05 + 0.08 * rng.generateDouble()), bh = h * (0.25 + 0.45 * rng.generateDouble());
            p.fillRect(QRectF(x, h - bh, bw, bh), ColorRef::scheme(Main).resolve(s));
            for (double wy = h - bh + 6; wy < h - 8; wy += 10)
                for (double wx = x + 4; wx < x + bw - 6; wx += 8)
                    if (rng.bounded(3)) p.fillRect(QRectF(wx, wy, 3, 4), c(Accent3, 30));
            x += bw + 2;
        }
    } else if (kind == "leaves") {
        p.fillRect(all, c(Accent5));
        for (int i = 0; i < 26; ++i) {
            p.save();
            p.translate(rng.bounded(int(w)), rng.bounded(int(h)));
            p.rotate(rng.bounded(360));
            const double L = w * (0.06 + 0.08 * rng.generateDouble());
            QPainterPath leaf;
            leaf.moveTo(0, 0);
            leaf.quadTo(L * 0.5, -L * 0.3, L, 0);
            leaf.quadTo(L * 0.5, L * 0.3, 0, 0);
            p.fillPath(leaf, c(i % 3 == 0 ? Accent3 : i % 3 == 1 ? Accent1 : Accent2, int(rng.bounded(3)) * 15));
            p.setPen(QPen(c(Main, 60), 1));
            p.drawLine(QPointF(0, 0), QPointF(L, 0));
            p.restore();
        }
    } else {   // "stripes"
        for (int i = 0; i * 18 < w + h; ++i) {
            p.save();
            p.rotate(-30);
            p.fillRect(QRectF(-w, i * 36 - h, w * 3, 18), c(1 + i % 4, 30));
            p.restore();
        }
    }
    p.end();
    QByteArray png;
    QBuffer b(&png);
    b.open(QIODevice::WriteOnly);
    // Qt's quality 80 is zlib's fastest compression, level 1: as quick to
    // write as none (quality 90 and up), and 10 to 50 times smaller.
    img.save(&b, "PNG", 80);
    return png;
}

void keepArt(const QString &key, const QByteArray &png)
{
    QHash<QString, QByteArray> &done = artCache();
    if (done.size() > 64) done.clear();   // a session's worth; templates use a few dozen
    done.insert(key, png);
}

// Pictures prepareArt() made or decoded ahead, by key, until art() takes them.
QHash<QString, QImage> &decodedArt()
{
    static QHash<QString, QImage> ready;
    return ready;
}

QImage decodePng(const QByteArray &png)
{
    QBuffer buf;
    buf.setData(png);
    QImageReader rd(&buf);
    rd.setAutoTransform(true);   // as ImageData::image() reads it
    return rd.read();
}

QString art(Document &d, const QString &kind, const QSize &px, quint32 seed = 1)
{
    const QString key = artKey(d.colors, kind, px, seed);
    auto hit = artCache().constFind(key);
    const QByteArray png = hit != artCache().constEnd() ? *hit : paintArt(d.colors, kind, px, seed);
    if (hit == artCache().constEnd()) keepArt(key, png);
    return d.addImage(png, "png", "art:" + kind, decodedArt().take(key));
}

// Makes and decodes several pictures at once, a processor core each, for the
// art() calls that follow (the year calendar's twelve took a third of a
// second one after another, and decoding them again most of a tenth).
struct ArtRequest {
    QString kind;
    QSize px;
    quint32 seed;
};
void prepareArt(const ColorScheme &s, const QVector<ArtRequest> &want)
{
    struct Job {
        QString key;
        ArtRequest a;
        QByteArray png;   // empty: still to make
    };
    QVector<Job> todo;
    QSet<QString> keys;
    for (const ArtRequest &a : want) {
        const QString key = artKey(s, a.kind, a.px, a.seed);
        if (keys.contains(key)) continue;
        keys.insert(key);
        todo << Job{key, a, artCache().value(key)};
    }
    decodedArt().clear();
    const int cores = std::max(1, int(std::thread::hardware_concurrency()));
    for (int from = 0; from < todo.size(); from += cores) {
        const int to = std::min(int(todo.size()), from + cores);
        std::vector<std::future<std::pair<QByteArray, QImage>>> jobs;
        for (int i = from; i < to; ++i)
            jobs.push_back(std::async(std::launch::async, [s, job = todo[i]] {
                const QByteArray png = job.png.isEmpty() ? paintArt(s, job.a.kind, job.a.px, job.a.seed) : job.png;
                return std::make_pair(png, decodePng(png));
            }));
        for (int i = from; i < to; ++i) {
            auto made = jobs[size_t(i - from)].get();
            if (todo[i].png.isEmpty()) keepArt(todo[i].key, made.first);
            decodedArt().insert(todo[i].key, made.second);
        }
    }
}

// ---------------- builder ----------------
struct Para {
    QString text;
    QString style;          // paragraph style name
    double size = 0;        // 0 = style's size
    int align = -1;         // 0 left, 1 center, 2 right, 3 justify
    int weight = -1;        // QFont weight, -1 = style
    int italic = -1;
    ColorRef color;         // none = style color
    QString font;           // "+major", "+minor" or a family
    double spaceAfter = -1;
};

struct B {
    Document &d;
    Page *pg;
    QSizeF ps;
    B(Document &doc, int page = 0) : d(doc), pg(page < doc.pages.size() ? doc.pages[page].get() : nullptr), ps(doc.pageSize()) {}
    void onPage(int i) { pg = d.pages[i].get(); }

    void fillStory(QTextDocument *doc, const QVector<Para> &paras)
    {
        QTextCursor c(doc);
        bool first = true;
        for (const Para &p : paras) {
            const TextStyle *st = d.style(p.style.isEmpty() ? QStringLiteral("Normal") : p.style);
            QTextBlockFormat bf = st ? st->blk : QTextBlockFormat();
            QTextCharFormat cf = st ? st->chr : QTextCharFormat();
            if (p.align >= 0) {
                const Qt::Alignment al[] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight, Qt::AlignJustify};
                bf.setAlignment(al[p.align]);
            }
            if (p.spaceAfter >= 0) bf.setBottomMargin(p.spaceAfter);
            if (p.size > 0) cf.setFontPointSize(p.size);
            if (p.weight >= 0) cf.setFontWeight(p.weight);
            if (p.italic >= 0) cf.setFontItalic(p.italic);
            if (!p.color.isNone()) cf.setProperty(tp::ColorRefP, p.color.toString());
            if (p.font == "+major") { cf.clearProperty(QTextFormat::FontFamilies); cf.setProperty(tp::ThemeFont, QStringLiteral("major")); }
            else if (p.font == "+minor") { cf.clearProperty(QTextFormat::FontFamilies); cf.setProperty(tp::ThemeFont, QStringLiteral("minor")); }
            else if (!p.font.isEmpty()) cf.setFontFamilies(QStringList{p.font});
            if (first) { c.setBlockFormat(bf); c.setBlockCharFormat(cf); first = false; }
            else c.insertBlock(bf, cf);
            // {biz:name}, {page}, {merge:Field} become fields.
            const QString t = p.text;
            int i = 0;
            while (i < t.size()) {
                const int open = t.indexOf('{', i);
                if (open < 0) { c.insertText(t.mid(i), cf); break; }
                c.insertText(t.mid(i, open - i), cf);
                const int close = t.indexOf('}', open);
                if (close < 0) { c.insertText(t.mid(open), cf); break; }
                QTextCharFormat ff = cf;
                ff.setProperty(tp::Field, t.mid(open + 1, close - open - 1));
                c.insertText(QString(QChar::ObjectReplacementCharacter), ff);
                i = close + 1;
            }
        }
    }

    std::shared_ptr<TextItem> text(const QRectF &r, const QVector<Para> &paras, VAlign va = VAlign::Top)
    {
        auto t = std::make_shared<TextItem>();
        t->rect = r;
        t->valign = va;
        t->storyId = d.createStory();
        fillStory(d.storyDoc(t->storyId), paras);
        pg->items.push_back(t);
        return t;
    }
    std::shared_ptr<ShapeItem> shape(const QString &id, const QRectF &r, const Fill &fill, const Stroke &stroke = Stroke::none())
    {
        auto s = std::make_shared<ShapeItem>();
        s->shape = id;
        s->rect = r;
        s->fill = fill;
        s->stroke = stroke;
        s->wrap.mode = Wrap::None;
        pg->items.push_back(s);
        return s;
    }
    std::shared_ptr<ShapeItem> shapeText(const QString &id, const QRectF &r, const Fill &fill, const QVector<Para> &paras)
    {
        auto s = shape(id, r, fill);
        s->storyId = d.createStory();
        fillStory(d.storyDoc(s->storyId), paras);
        return s;
    }
    std::shared_ptr<PictureItem> picture(const QRectF &r, const QString &artKind, quint32 seed = 1, const QString &mask = QStringLiteral("rect"))
    {
        auto pic = std::make_shared<PictureItem>();
        pic->rect = r;
        pic->maskShape = mask;
        pic->imageId = artKind.isEmpty() ? QString() : art(d, artKind, QSize(int(r.width() * 2), int(r.height() * 2)), seed);
        pic->imgRect = QRectF(QPointF(0, 0), r.size());
        pg->items.push_back(pic);
        return pic;
    }
    std::shared_ptr<LineItem> line(QPointF a, QPointF b, const ColorRef &c, double w = 1)
    {
        auto l = std::make_shared<LineItem>();
        l->p1 = a;
        l->p2 = b;
        l->stroke = Stroke::line(c, w);
        l->syncRect();
        pg->items.push_back(l);
        return l;
    }
    // The business logo (or an empty picture frame for it) when the
    // template option asks for one.
    std::shared_ptr<PictureItem> logo(const QRectF &r, const TemplateOptions &o)
    {
        if (!o.options.value(QStringLiteral("logo")).toBool()) return nullptr;
        auto pic = std::make_shared<PictureItem>();
        pic->rect = r;
        pic->name = QStringLiteral("Logo");
        pic->imgRect = QRectF(QPointF(0, 0), r.size());
        if (!o.logoBytes.isEmpty()) {
            pic->imageId = d.addImage(o.logoBytes, o.logoFormat);
            pic->fitImage(d.imageSize(pic->imageId), false);
            if (!d.biz.isEmpty()) d.biz.first().logoImageId = pic->imageId;
        }
        pg->items.push_back(pic);
        return pic;
    }
    std::shared_ptr<TextArtItem> textart(const QRectF &r, const QString &text, const QString &styleId, const QString &font = QString())
    {
        auto w = std::make_shared<TextArtItem>();
        for (const auto &st : textArtStyles()) if (st.id == styleId) applyTextArtStyle(*w, st);
        w->text = text;
        if (!font.isEmpty()) w->font = font;
        w->rect = r;
        pg->items.push_back(w);
        return w;
    }
};

bool wantsLogo(const TemplateOptions &o) { return o.options.value(QStringLiteral("logo")).toBool(); }
bool wantsMailing(const TemplateOptions &o) { return o.options.value(QStringLiteral("address")).toBool(true); }

std::unique_ptr<Document> base(const TemplateOptions &o, QSizeF size, int pages, const QString &defScheme, const QString &defFonts, const QString &sizeName = QString())
{
    auto d = Document::blank(size, sizeName.isEmpty() ? QStringLiteral("Custom") : sizeName, pages);
    const ColorScheme *cs = findColorScheme(o.colorScheme.isEmpty() ? defScheme : o.colorScheme);
    if (o.colors) d->colors = *o.colors;
    else if (cs) d->colors = *cs;
    const FontScheme *fs = findFontScheme(o.fontScheme.isEmpty() ? defFonts : o.fontScheme);
    if (o.fonts) d->fonts = *o.fonts;
    else if (fs) d->fonts = *fs;
    BusinessInfo biz = o.business;
    if (biz.name.isEmpty()) {
        biz.name = QCoreApplication::translate("Templates", "Riverbend Community Arts");
        biz.tagline = QCoreApplication::translate("Templates", "Bringing art to every block");
        biz.person = QCoreApplication::translate("Templates", "Dana Whitfield");
        biz.title = QCoreApplication::translate("Templates", "Program Director");
        biz.address = QCoreApplication::translate("Templates", "1450 Mill Street\nRiverbend, MI 49001");
        biz.phone = QStringLiteral("(269) 555-0142");
        biz.fax = QStringLiteral("(269) 555-0143");
        biz.email = QStringLiteral("hello@riverbendarts.org");
        biz.web = QStringLiteral("riverbendarts.org");
        biz.setName = QCoreApplication::translate("Templates", "Sample business information");
    }
    d->biz = {biz};
    return d;
}

QRectF content(const Document &d) { return QRectF(QPointF(0, 0), d.pageSize()).marginsRemoved(d.setup.margins); }

Para P(const QString &t, const QString &style = QString()) { Para p; p.text = t; p.style = style; return p; }
Para H(const QString &t, double size, int align = 0, int slot = -1)
{
    Para p;
    p.text = t;
    p.style = QStringLiteral("Heading 1");
    p.size = size;
    p.align = align;
    if (slot >= 0) p.color = ColorRef::scheme(slot);
    return p;
}
Para C(const QString &t, double size, int slot = Main, int align = 1, bool major = false, int weight = -1)
{
    Para p;
    p.text = t;
    p.size = size;
    p.align = align;
    p.color = slot >= 0 ? ColorRef::scheme(slot) : ColorRef::rgb(Qt::white);
    if (major) p.font = "+major";
    p.weight = weight;
    p.spaceAfter = 2;
    return p;
}
Para W(const QString &t, double size, int align = 1, bool major = false, int weight = -1)
{
    Para p = C(t, size, Main, align, major, weight);
    p.color = ColorRef::rgb(Qt::white);
    return p;
}

// Sample paragraphs. Functions, not constants: they are translated when a
// template is built, after the program has loaded its translation.
QString lorem1()
{
    return QCoreApplication::translate(
        "Templates",
        "Every Saturday this summer, the riverfront lawn turns into an open-air stage. Local bands, a youth orchestra and a jazz trio take turns from late afternoon into the evening. "
        "Bring a blanket or a lawn chair; food trucks line Mill Street from five o'clock.");
}
QString lorem2()
{
    return QCoreApplication::translate(
        "Templates",
        "The program began twelve years ago with a single folding stage and a borrowed sound system. Today more than forty volunteers set up the lawn each week, and the "
        "concerts draw neighbors from across the county. Admission is free, and donations go directly to music lessons for students in the district.");
}
QString lorem3()
{
    return QCoreApplication::translate(
        "Templates",
        "Volunteers help with parking, seating, the information table and cleanup. Shifts are two hours long, and no experience is needed. Sign up at the welcome tent or "
        "on our website, and bring a friend.");
}

// ---------------- templates ----------------
std::unique_ptr<Document> flyerEvent(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Seventy-Nine", "Poster");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height() * 0.48), "sunburst", 3);
    b.shape("rect", QRectF(0, ps.height() * 0.48, ps.width(), ps.height() * 0.52), Fill::solid(ColorRef::scheme(Accent5)));
    b.shape("wave", QRectF(-10, ps.height() * 0.44, ps.width() + 20, 50), Fill::solid(ColorRef::scheme(Accent5)));
    b.text(QRectF(40, 40, ps.width() - 80, 40), {C(QCoreApplication::translate("Templates", "{biz:name} presents"), 14, Main, 1, false, 700)});
    b.textart(QRectF(54, 92, ps.width() - 108, 150), QCoreApplication::translate("Templates", "SUMMER CONCERTS"), "fill-main", d->fonts.heading);
    b.text(QRectF(72, ps.height() * 0.53, ps.width() - 144, 90), {C(QCoreApplication::translate("Templates", "Saturdays · June 6 – August 29"), 26, Accent1, 1, true), C(QCoreApplication::translate("Templates", "Riverfront Lawn · 5:00 – 9:00 PM · Free admission"), 15, Main, 1)});
    b.line(QPointF(160, ps.height() * 0.53 + 96), QPointF(ps.width() - 160, ps.height() * 0.53 + 96), ColorRef::scheme(Accent1), 2);
    auto body = b.text(QRectF(72, ps.height() * 0.53 + 110, ps.width() - 144, 150), {P(lorem1(), "Body Text"), P(lorem3(), "Body Text")});
    body->columns = 2;
    body->columnGap = 22;
    b.shapeText("irregularSeal1", QRectF(ps.width() - 200, ps.height() * 0.39, 170, 170), Fill::solid(ColorRef::scheme(Accent1)),
                {W(QCoreApplication::translate("Templates", "FREE"), 22, 1, true), W(QCoreApplication::translate("Templates", "all ages"), 11)});
    b.shape("rect", QRectF(0, ps.height() - 72, ps.width(), 72), Fill::solid(ColorRef::scheme(Main)));
    b.text(QRectF(40, ps.height() - 64, ps.width() - 80, 56), {W(QStringLiteral("{biz:name}"), 13, 1, true, 700), W(QStringLiteral("{biz:phone}  ·  {biz:web}"), 11)}, VAlign::Middle);
    return d;
}

std::unique_ptr<Document> flyerSale(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Cherry", "Headline");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
    b.shape("rect", QRectF(28, 28, ps.width() - 56, ps.height() - 56), Fill::none(), Stroke::line(ColorRef::rgb(Qt::white), 3));
    b.textart(QRectF(60, 70, ps.width() - 120, 210), QCoreApplication::translate("Templates", "SALE"), "fill-main", d->fonts.heading)->fill = Fill::solid(ColorRef::rgb(Qt::white));
    b.shapeText("star24", QRectF(ps.width() / 2 - 160, 290, 320, 300), Fill::solid(ColorRef::scheme(Accent3)),
                {W(QCoreApplication::translate("Templates", "UP TO"), 16, 1, true), W(QStringLiteral("50%"), 44, 1, true, 700), W(QCoreApplication::translate("Templates", "OFF"), 22, 1, true)});
    b.text(QRectF(70, 594, ps.width() - 140, 76), {W(QCoreApplication::translate("Templates", "Friday through Sunday only"), 24, 1, true), W(QCoreApplication::translate("Templates", "Every jacket, sweater and boot in the store"), 16)});
    b.text(QRectF(70, ps.height() - 116, ps.width() - 140, 72), {W(QStringLiteral("{biz:name}"), 18, 1, true, 700), W(QStringLiteral("{biz:address:oneline}"), 12), W(QStringLiteral("{biz:phone}"), 12)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> flyerAnnouncement(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Harbor", "Editorial");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.picture(QRectF(c.left(), c.top(), c.width(), 300), "waves", 2);
    b.text(QRectF(c.left(), 350, c.width(), 120), {H(QCoreApplication::translate("Templates", "Now Open on Mill Street"), 40, 0, Accent1), P(QCoreApplication::translate("Templates", "A new home for classes, studios and community events."), "Subtitle")});
    b.line(QPointF(c.left(), 476), QPointF(c.right(), 476), ColorRef::scheme(Accent3), 2);
    auto body = b.text(QRectF(c.left(), 492, c.width() * 0.62, 220), {P(lorem2(), "Body Text"), P(lorem3(), "Body Text")});
    body->wrap.mode = Wrap::Square;
    b.shapeText("rect", QRectF(c.left() + c.width() * 0.66, 492, c.width() * 0.34, 220), Fill::solid(ColorRef::scheme(Accent5)),
                {C(QCoreApplication::translate("Templates", "Open House"), 20, Accent1, 1, true), C(QCoreApplication::translate("Templates", "Saturday, June 13\n10 AM – 4 PM"), 13, Main), C(QCoreApplication::translate("Templates", "Tours, demos and refreshments"), 11, Main)});
    b.text(QRectF(c.left(), ps.height() - 100, c.width(), 60), {C(QStringLiteral("{biz:name}  ·  {biz:phone}  ·  {biz:web}"), 11, Accent1, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> businessCard(const TemplateOptions &o, int variant)
{
    auto d = base(o, QSizeF(3.5 * IN, 2 * IN), 1, variant == 0 ? "Ink" : variant == 1 ? "Brick" : "Meadow", variant == 2 ? "Elegant" : "Modern", "Business Card");
    d->setup.margins = QMarginsF(9, 9, 9, 9);
    B b(*d);
    const QSizeF ps = d->pageSize();
    if (variant == 0) {
        b.shape("rect", QRectF(0, 0, 70, ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
        if (!b.logo(QRectF(12, ps.height() / 2 - 23, 46, 46), o)) b.shape("ellipse", QRectF(14, ps.height() / 2 - 21, 42, 42), Fill::solid(ColorRef::scheme(Accent3)));
        b.text(QRectF(84, 14, ps.width() - 94, 60), {C(QStringLiteral("{biz:person}"), 14, Main, 0, true, 700), C(QStringLiteral("{biz:title}"), 8, Accent1, 0)});
        b.text(QRectF(84, 74, ps.width() - 94, 66), {C(QStringLiteral("{biz:name}"), 8.5, Main, 0, false, 700), C(QStringLiteral("{biz:phone}"), 7.5, Main, 0), C(QStringLiteral("{biz:email}"), 7.5, Main, 0), C(QStringLiteral("{biz:web}"), 7.5, Main, 0)});
    } else if (variant == 1) {
        b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Main)));
        b.shape("rect", QRectF(0, ps.height() - 10, ps.width(), 10), Fill::solid(ColorRef::scheme(Accent1)));
        b.text(QRectF(14, 18, ps.width() - (wantsLogo(o) ? 76 : 28), 50), {W(QStringLiteral("{biz:name}"), 14, 0, true, 700), W(QStringLiteral("{biz:tagline}"), 7.5, 0)})->autofit =
            TextItem::ShrinkOnOverflow;
        b.logo(QRectF(ps.width() - 56, 16, 42, 42), o);
        b.text(QRectF(14, 76, ps.width() - 28, 58), {W(QStringLiteral("{biz:person}, {biz:title}"), 8.5, 0, false, 700), W(QStringLiteral("{biz:phone}  ·  {biz:email}"), 7.5, 0), W(QStringLiteral("{biz:web}"), 7.5, 0)});
    } else {
        b.shape("rect", QRectF(6, 6, ps.width() - 12, ps.height() - 12), Fill::none(), Stroke::line(ColorRef::scheme(Accent1), 0.75));
        // With a logo: the logo small and centered on top, the rest a little lower.
        const double dy = wantsLogo(o) ? 12 : 0;
        b.logo(QRectF(ps.width() / 2 - 11, 8, 22, 22), o);
        b.text(QRectF(14, 22 + dy, ps.width() - 28, 52), {C(QStringLiteral("{biz:person}"), 16, Accent1, 1, true), C(QStringLiteral("{biz:title}"), 8, Main, 1)});
        b.line(QPointF(ps.width() / 2 - 30, 80 + dy), QPointF(ps.width() / 2 + 30, 80 + dy), ColorRef::scheme(Accent3), 1);
        b.text(QRectF(14, 86 + dy, ps.width() - 28, 50 - dy), {C(QStringLiteral("{biz:name}"), 8, Main, 1, false, 700), C(QStringLiteral("{biz:phone}  ·  {biz:email}"), 7, Main, 1)});
    }
    return d;
}

std::unique_ptr<Document> brochure(const TemplateOptions &o, int variant)
{
    auto d = base(o, QSizeF(11 * IN, 8.5 * IN), 2, variant == 0 ? "Lagoon" : "Plum", variant == 0 ? "Modern" : "Editorial", "Letter Landscape");
    d->setup.margins = QMarginsF(27, 27, 27, 27);
    for (auto &m : d->masters) m->grid.cols = 3, m->grid.colGap = 54;
    B b(*d);
    const QSizeF ps = d->pageSize();
    const double panel = ps.width() / 3;
    // Outside: inside flap, back, cover.
    b.shape("rect", QRectF(0, 0, panel, ps.height()), Fill::solid(ColorRef::scheme(Accent5)));
    b.text(QRectF(27, 36, panel - 54, ps.height() - 72), {H(QCoreApplication::translate("Templates", "Classes for every age"), 18, 0, Accent1), P(lorem3(), "Body Text"),
                                                          H(QCoreApplication::translate("Templates", "Studio hours"), 14, 0, Accent1), P(QCoreApplication::translate("Templates", "Monday – Friday\t10 AM – 8 PM\nSaturday\t9 AM – 5 PM\nSunday\tClosed"), "Body Text")});
    b.logo(QRectF(panel + panel / 2 - 45, ps.height() * 0.55 - 100, 90, 90), o);
    b.text(QRectF(panel + 27, ps.height() * 0.55, panel - 54, ps.height() * 0.35), {C(QStringLiteral("{biz:name}"), 13, Accent1, 1, true, 700), C(QStringLiteral("{biz:address}"), 10, Main, 1),
                                                                                    C(QCoreApplication::translate("Templates", "Phone: {biz:phone}"), 10, Main, 1), C(QStringLiteral("{biz:email}"), 10, Main, 1)});
    b.picture(QRectF(2 * panel, 0, panel, ps.height() * 0.58), variant == 0 ? "waves" : "bokeh", 4);
    b.shape("rect", QRectF(2 * panel, ps.height() * 0.58, panel, ps.height() * 0.42), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(2 * panel + 24, ps.height() * 0.62, panel - 48, ps.height() * 0.3), {W(QStringLiteral("{biz:name}"), 24, 0, true, 700), W(QStringLiteral("{biz:tagline}"), 12, 0)});
    // Inside: three panels of linked text.
    b.onPage(1);
    b.text(QRectF(27, 30, ps.width() - 54, 74), {H(QCoreApplication::translate("Templates", "Make something this season"), 28, 1, Accent1)});
    auto t1 = b.text(QRectF(27, 110, panel - 54, ps.height() - 146), {H(QCoreApplication::translate("Templates", "Painting"), 15, 0, Accent2), P(lorem2(), "Body Text"), H(QCoreApplication::translate("Templates", "Ceramics"), 15, 0, Accent2), P(lorem1(), "Body Text"),
                                                                     H(QCoreApplication::translate("Templates", "Printmaking"), 15, 0, Accent2), P(lorem3(), "Body Text"), P(lorem2(), "Body Text")});
    auto t2 = std::make_shared<TextItem>();
    t2->rect = QRectF(panel + 27, 110, panel - 54, ps.height() * 0.45);
    t2->storyId = t1->storyId;
    t1->nextId = t2->id;
    b.pg->items.push_back(t2);
    b.picture(QRectF(panel + 27, 110 + ps.height() * 0.47, panel - 54, ps.height() * 0.36), "leaves", 9);
    auto t3 = std::make_shared<TextItem>();
    t3->rect = QRectF(2 * panel + 27, 110, panel - 54, ps.height() - 146);
    t3->storyId = t1->storyId;
    t2->nextId = t3->id;
    b.pg->items.push_back(t3);
    return d;
}

std::unique_ptr<Document> newsletter(const TemplateOptions &o, int variant)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 2, variant == 0 ? "Regatta" : "Orchard", variant == 0 ? "Gazette" : "Friendly", "Letter");
    for (auto &m : d->masters) m->grid.cols = 3;
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    // Master: footer with page number.
    {
        Page tmp;
        b.pg = &tmp;
        b.line(QPointF(c.left(), ps.height() - 40), QPointF(c.right(), ps.height() - 40), ColorRef::scheme(Accent4), 0.75);
        b.text(QRectF(c.left(), ps.height() - 36, c.width(), 20), {C(QCoreApplication::translate("Templates", "{biz:name} Newsletter\tPage {page}"), 8.5, Accent4, 0)});
        auto &mi = d->masters.first()->items;
        for (auto &it : tmp.items) mi.push_back(it);
        // Right-aligned tab for the page number.
        if (auto *t = dynamic_cast<TextItem *>(mi.back().get())) {
            QTextCursor cur(d->storyDoc(t->storyId));
            QTextBlockFormat bf;
            bf.setTabPositions({QTextOption::Tab(c.width() - 6, QTextOption::RightTab)});
            cur.mergeBlockFormat(bf);
        }
        b.onPage(0);
    }
    // Masthead.
    b.shape("rect", QRectF(0, 0, ps.width(), 150), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(c.left(), 36, c.width() - (wantsLogo(o) ? 100 : 0), 70), {W(QStringLiteral("{biz:name}"), 34, 0, true, 700)})->autofit = TextItem::ShrinkOnOverflow;
    b.logo(QRectF(c.right() - 84, 24, 84, 76), o);
    b.text(QRectF(c.left(), 104, c.width() / 2, 24), {W(QCoreApplication::translate("Templates", "Volume 3, Issue 6"), 10, 0)});
    b.text(QRectF(c.left() + c.width() / 2, 104, c.width() / 2, 24), {W(QStringLiteral("{date:MMMM yyyy}"), 10, 2)});
    const double colW = (c.width() - 2 * 14) / 3;
    // Sidebar.
    b.shapeText("rect", QRectF(c.left(), 170, colW, 300), Fill::solid(ColorRef::scheme(Accent5)),
                {C(QCoreApplication::translate("Templates", "Inside this issue"), 14, Accent1, 0, true, 700), C(QCoreApplication::translate("Templates", "Summer concert lineup\t1\nVolunteer spotlight\t2\nNew studio classes\t2\nCalendar\t2"), 10, Main, 0)});
    if (auto *s = dynamic_cast<ShapeItem *>(b.pg->items.back().get())) { s->valign = VAlign::Top; s->insets = QMarginsF(10, 10, 10, 10); }
    // Lead story across two columns, continued on page 2.
    b.text(QRectF(c.left() + colW + 14, 170, 2 * colW + 14, 60), {H(QCoreApplication::translate("Templates", "Concert season opens on the river"), 22, 0, Accent1)})->autofit = TextItem::ShrinkOnOverflow;
    b.picture(QRectF(c.left() + colW + 14, 236, 2 * colW + 14, 180), "skyline", 5);
    b.text(QRectF(c.left() + colW + 14, 417, 2 * colW + 14, 24), {P(QCoreApplication::translate("Templates", "The riverfront lawn at dusk, before the first concert of the season."), "Caption")});
    auto lead = b.text(QRectF(c.left() + colW + 14, 444, 2 * colW + 14, c.bottom() - 444 - 40),
                       {P(lorem1(), "Body Text"), P(lorem2(), "Body Text"), P(lorem3(), "Body Text"), P(lorem1(), "Body Text"), P(lorem2(), "Body Text"), P(lorem3(), "Body Text")});
    lead->columns = 2;
    lead->columnGap = 14;
    lead->continuedOn = true;
    b.text(QRectF(c.left(), 490, colW, c.bottom() - 530), {P(QCoreApplication::translate("Templates", "“The lawn fills up by six. Come early and bring a blanket.”"), "Quote"), P(QCoreApplication::translate("Templates", "— A regular since the first season"), "Caption")}, VAlign::Middle);
    // Page 2.
    b.onPage(1);
    auto cont = std::make_shared<TextItem>();
    cont->rect = QRectF(c.left(), c.top() + 10, c.width(), 230);
    cont->columns = 3;
    cont->columnGap = 14;
    cont->continuedFrom = true;
    cont->storyId = lead->storyId;
    lead->nextId = cont->id;
    b.pg->items.push_back(cont);
    b.picture(QRectF(c.left(), 290, colW * 2 + 14, 200), "leaves", 6);
    b.text(QRectF(c.left() + 2 * colW + 28, 290, colW, 200), {H(QCoreApplication::translate("Templates", "Volunteer spotlight"), 16, 0, Accent1), P(lorem3(), "Body Text")});
    b.text(QRectF(c.left(), 506, c.width(), 36), {C(QCoreApplication::translate("Templates", "Coming up"), 18, Accent1, 0, true, 700)}, VAlign::Bottom);
    // A small calendar-style table.
    auto tbl = std::make_shared<TableItem>();
    tbl->rows = 5;
    tbl->cols = 3;
    tbl->rect = QRectF(c.left(), 548, c.width(), 150);
    tbl->colW = {c.width() * 0.22, c.width() * 0.5, c.width() * 0.28};
    tbl->rowH = QVector<double>(5, 30);
    tbl->cells.resize(15);
    const QString rowsText[5][3] = {
        {QCoreApplication::translate("Templates", "Date"), QCoreApplication::translate("Templates", "Event"), QCoreApplication::translate("Templates", "Where")},
        {QCoreApplication::translate("Templates", "June 6"), QCoreApplication::translate("Templates", "Opening night: Riverbend Youth Orchestra"), QCoreApplication::translate("Templates", "Riverfront Lawn")},
        {QCoreApplication::translate("Templates", "June 13"), QCoreApplication::translate("Templates", "Open house at the new studio"), QCoreApplication::translate("Templates", "1450 Mill Street")},
        {QCoreApplication::translate("Templates", "June 20"), QCoreApplication::translate("Templates", "Jazz on the lawn"), QCoreApplication::translate("Templates", "Riverfront Lawn")},
        {QCoreApplication::translate("Templates", "June 27"), QCoreApplication::translate("Templates", "Family printmaking workshop"), QCoreApplication::translate("Templates", "Studio B")}};
    for (int r = 0; r < 5; ++r)
        for (int col = 0; col < 3; ++col) tbl->cell(r, col).storyId = d->createStory(rowsText[r][col]);
    tbl->syncRect();
    applyTableFormatCells(tbl.get(), QStringLiteral("Table Style 1"), [&](TableCell &cell, bool head, bool) {
        if (!head) return;
        QTextCursor cur(d->storyDoc(cell.storyId));
        cur.select(QTextCursor::Document);
        QTextCharFormat f;
        f.setProperty(tp::ColorRefP, QStringLiteral("#FFFFFF"));
        f.setFontWeight(QFont::Bold);
        cur.mergeCharFormat(f);
    });
    b.pg->items.push_back(tbl);
    return d;
}

std::unique_ptr<Document> greetingCard(const TemplateOptions &o, int variant)
{
    auto d = base(o, QSizeF(5.5 * IN, 4.25 * IN), 4, variant == 0 ? "Carnival" : "Sage", variant == 0 ? "Festive" : "Script", "Quarter-fold Card");
    d->setup.layout = PageSetup::FoldedCard;
    d->setup.sheet = QSizeF(11 * IN, 8.5 * IN);
    d->setup.margins = QMarginsF(18, 18, 18, 18);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height()), variant == 0 ? "confetti" : "leaves", 7);
    b.textart(QRectF(30, ps.height() / 2 - 70, ps.width() - 60, 120), variant == 0 ? QCoreApplication::translate("Templates", "Happy Birthday!") : QCoreApplication::translate("Templates", "Thank You"),
              variant == 0 ? "arch-a1" : "script", d->fonts.heading);
    b.onPage(1);
    b.onPage(2);
    b.text(QRectF(36, ps.height() / 2 - 60, ps.width() - 72, 120),
           {C(variant == 0 ? QCoreApplication::translate("Templates", "Wishing you a year full of good surprises.") : QCoreApplication::translate("Templates", "Your kindness meant more than you know."), 16, Accent1, 1, true),
            C(QStringLiteral("— {biz:person}"), 11, Main, 1)}, VAlign::Middle)->role = role::Body;
    b.onPage(3);
    b.text(QRectF(36, ps.height() - 70, ps.width() - 72, 36), {C(QCoreApplication::translate("Templates", "Made with JeffPub"), 8, Accent4, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> certificate(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(11 * IN, 8.5 * IN), 1, "Regatta", "Monument", "Letter Landscape");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(24, 24, ps.width() - 48, ps.height() - 48), Fill::solid(ColorRef::scheme(Accent5)), Stroke::line(ColorRef::scheme(Accent1), 6));
    auto inner = b.shape("rect", QRectF(40, 40, ps.width() - 80, ps.height() - 80), Fill::none(), Stroke::line(ColorRef::scheme(Accent3), 1.5));
    inner->stroke.compound = Stroke::Double;
    inner->stroke.width = 4;
    b.text(QRectF(80, 84, ps.width() - 160, 132), {C(QCoreApplication::translate("Templates", "Certificate"), 46, Accent1, 1, true), C(QCoreApplication::translate("Templates", "of Achievement"), 22, Accent3, 1, true)});
    b.text(QRectF(80, 220, ps.width() - 160, 30), {C(QCoreApplication::translate("Templates", "This certificate is presented to"), 14, Main, 1, false)});
    b.text(QRectF(120, 256, ps.width() - 240, 64), {C(QCoreApplication::translate("Templates", "Recipient Name"), 36, Main, 1, false, -1)})->autofit = TextItem::ShrinkOnOverflow;
    b.line(QPointF(170, 326), QPointF(ps.width() - 170, 326), ColorRef::scheme(Accent4), 1);
    b.text(QRectF(120, 336, ps.width() - 240, 60), {C(QCoreApplication::translate("Templates", "in recognition of outstanding dedication to the summer concert series and the students it supports."), 13, Main, 1)});
    for (int i = 0; i < 2; ++i) {
        const double x = i == 0 ? 120 : ps.width() - 330;
        b.line(QPointF(x, ps.height() - 140), QPointF(x + 210, ps.height() - 140), ColorRef::scheme(Main), 0.75);
        b.text(QRectF(x, ps.height() - 134, 210, 40), {C(i == 0 ? QStringLiteral("{biz:person}, {biz:title}") : QStringLiteral("{date:MMMM d, yyyy}"), 10, Main, 1)});
    }
    b.shapeText("star24", QRectF(ps.width() / 2 - 60, ps.height() - 190, 120, 120), Fill::solid(ColorRef::scheme(Accent3)), {W(QStringLiteral("{biz:name}"), 8, 1, true, 700)});
    return d;
}

std::unique_ptr<Document> postcard(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(6 * IN, 4 * IN), 2, "Tangerine", "Retro", "Postcard");
    d->setup.margins = QMarginsF(18, 18, 18, 18);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height()), "sunburst", 11);
    b.textart(QRectF(30, ps.height() / 2 - 50, ps.width() - 60, 80), QCoreApplication::translate("Templates", "Greetings from Riverbend"), "retro", d->fonts.heading);
    b.onPage(1);
    b.line(QPointF(ps.width() / 2, 24), QPointF(ps.width() / 2, ps.height() - 24), ColorRef::scheme(Accent4), 0.75);
    b.text(QRectF(18, 18, ps.width() / 2 - 32, ps.height() - 36), {C(QCoreApplication::translate("Templates", "Wish you were here!"), 14, Accent1, 0, true), P(lorem1().left(160) + QStringLiteral("…"), "Body Text"),
                                                                   C(QStringLiteral("{biz:name} · {biz:web}"), 7.5, Accent4, 0)});
    if (wantsMailing(o)) {
        b.shape("rect", QRectF(ps.width() - 78, 18, 60, 70), Fill::none(), Stroke::line(ColorRef::scheme(Accent4), 0.75))->stroke.dash = Stroke::DashLine;
        b.text(QRectF(ps.width() - 78, 34, 60, 40), {C(QCoreApplication::translate("Templates", "Place\nstamp\nhere"), 7, Accent4, 1)}, VAlign::Middle);
        b.text(QRectF(ps.width() / 2 + 18, ps.height() / 2, ps.width() / 2 - 36, 90), {C(QStringLiteral("{mergeblock:address}"), 10, Main, 0)});
    }
    return d;
}

std::unique_ptr<Document> sign(const TemplateOptions &o, int variant)
{
    auto d = base(o, QSizeF(11 * IN, 8.5 * IN), 1, variant == 0 ? "Graphite" : "Marigold", "Headline", "Letter Landscape");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(variant == 0 ? Accent3 : Accent1)));
    b.shape("roundRect", QRectF(36, 36, ps.width() - 72, ps.height() - 72), Fill::solid(ColorRef::rgb(Qt::white)));
    b.textart(QRectF(80, 90, ps.width() - 160, 220), variant == 0 ? QCoreApplication::translate("Templates", "CLOSED") : QCoreApplication::translate("Templates", "WELCOME"), "fill-main", d->fonts.heading);
    b.text(QRectF(80, 330, ps.width() - 160, 120),
           {C(variant == 0 ? QCoreApplication::translate("Templates", "for the holiday") : QCoreApplication::translate("Templates", "Come on in — we're open!"), 34, variant == 0 ? Accent3 : Accent1, 1, true),
            C(variant == 0 ? QCoreApplication::translate("Templates", "We reopen Monday at 9 AM.") : QCoreApplication::translate("Templates", "Monday – Saturday · 9 AM – 6 PM"), 20, Main, 1)});
    b.text(QRectF(80, ps.height() - 110, ps.width() - 160, 40), {C(QStringLiteral("{biz:name}  ·  {biz:phone}"), 14, Accent4, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> banner(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(60 * IN, 24 * IN), 1, "Lagoon", "Poster", "Banner 5 × 2 ft");
    d->setup.margins = QMarginsF(72, 72, 72, 72);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height()), "waves", 13);
    b.textart(QRectF(200, 300, ps.width() - 400, 900), QCoreApplication::translate("Templates", "WELCOME HOME"), "fill-main", d->fonts.heading)->fill = Fill::solid(ColorRef::scheme(Accent1));
    b.text(QRectF(200, 1250, ps.width() - 400, 300), {C(QStringLiteral("{biz:name}"), 120, Main, 1, true, 700)});
    return d;
}

std::unique_ptr<Document> menu(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 14 * IN), 1, "Espresso", "Elegant", "Legal");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(18, 18, ps.width() - 36, ps.height() - 36), Fill::none(), Stroke::line(ColorRef::scheme(Accent2), 1.5));
    b.text(QRectF(c.left(), c.top() + 20, c.width(), 110), {C(QStringLiteral("{biz:name}"), 40, Accent1, 1, true), C(QStringLiteral("{biz:tagline}"), 13, Accent2, 1)});
    struct Sec { const char *title; QVector<QPair<const char *, const char *>> items; };
    const QVector<Sec> secs = {{QT_TRANSLATE_NOOP("Templates", "Starters"), {{QT_TRANSLATE_NOOP("Templates", "Tomato basil soup"), "6"}, {QT_TRANSLATE_NOOP("Templates", "Garden salad with lemon vinaigrette"), "8"}, {QT_TRANSLATE_NOOP("Templates", "Roasted squash and burrata"), "11"}}},
                               {QT_TRANSLATE_NOOP("Templates", "Mains"), {{QT_TRANSLATE_NOOP("Templates", "Herb roast chicken, root vegetables"), "19"}, {QT_TRANSLATE_NOOP("Templates", "Mushroom risotto, aged parmesan"), "17"}, {QT_TRANSLATE_NOOP("Templates", "Seared trout, brown butter, capers"), "22"}, {QT_TRANSLATE_NOOP("Templates", "Braised short rib, polenta"), "24"}}},
                               {QT_TRANSLATE_NOOP("Templates", "Desserts"), {{QT_TRANSLATE_NOOP("Templates", "Apple crisp with cream"), "8"}, {QT_TRANSLATE_NOOP("Templates", "Dark chocolate tart"), "9"}, {QT_TRANSLATE_NOOP("Templates", "Seasonal sorbet"), "6"}}}};
    double y = c.top() + 150;
    for (const Sec &s : secs) {
        QVector<Para> paras{C(QCoreApplication::translate("Templates", s.title), 20, Accent1, 1, true)};
        for (const auto &it : s.items) {
            Para p;
            p.text = QCoreApplication::translate("Templates", it.first) + "\t" + QString::fromLatin1(it.second);
            p.style = "Body Text";
            paras << p;
        }
        auto t = b.text(QRectF(c.left() + 30, y, c.width() - 60, 40 + 26 * s.items.size()), paras);
        QTextCursor cur(d->storyDoc(t->storyId));
        cur.select(QTextCursor::Document);
        QTextBlockFormat bf;
        bf.setTabPositions({QTextOption::Tab(c.width() - 72, QTextOption::RightTab)});
        bf.setProperty(tp::TabLeaders, QStringLiteral("."));
        cur.mergeBlockFormat(bf);
        y += 70 + 26 * s.items.size();
        b.line(QPointF(ps.width() / 2 - 40, y - 18), QPointF(ps.width() / 2 + 40, y - 18), ColorRef::scheme(Accent2), 1);
    }
    b.text(QRectF(c.left(), c.bottom() - 50, c.width(), 40), {C(QStringLiteral("{biz:address:oneline}  ·  {biz:phone}"), 10, Accent4, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> giftCertificate(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(7 * IN, 3 * IN), 1, "Berry", "Elegant", "Gift Certificate");
    d->setup.margins = QMarginsF(14, 14, 14, 14);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::gradient(ColorRef::scheme(Accent1), ColorRef::scheme(Accent2), 0));
    b.shape("rect", QRectF(12, 12, ps.width() - 24, ps.height() - 24), Fill::solid(ColorRef::rgb(Qt::white)));
    b.text(QRectF(30, 22, ps.width() * 0.6, 60), {C(QCoreApplication::translate("Templates", "Gift Certificate"), 28, Accent1, 0, true)});
    b.text(QRectF(30, 84, ps.width() * 0.6, 100), {C(QCoreApplication::translate("Templates", "To: ______________________"), 11, Main, 0), C(QCoreApplication::translate("Templates", "From: ____________________"), 11, Main, 0),
                                                    C(QCoreApplication::translate("Templates", "Expires: _________________"), 11, Main, 0)});
    b.shapeText("ellipse", QRectF(ps.width() - 170, 40, 130, 130), Fill::solid(ColorRef::scheme(Accent2)), {W(QStringLiteral("$50"), 34, 1, true, 700)});
    b.text(QRectF(30, ps.height() - 40, ps.width() - 60, 22), {C(QStringLiteral("{biz:name} · {biz:phone} · {biz:web}"), 8, Accent4, 0)});
    return d;
}

std::unique_ptr<Document> invitation(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(5 * IN, 7 * IN), 1, "Lilac", "Script", "5 × 7 Invitation");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height() * 0.42), "bokeh", 21);
    b.shape("wave", QRectF(-6, ps.height() * 0.36, ps.width() + 12, 40), Fill::solid(ColorRef::rgb(Qt::white)));
    b.text(QRectF(28, ps.height() * 0.44, ps.width() - 56, 70), {C(QCoreApplication::translate("Templates", "You're Invited"), 34, Accent1, 1, true)});
    b.text(QRectF(36, ps.height() * 0.56, ps.width() - 72, 160),
           {C(QCoreApplication::translate("Templates", "to an evening of music and dessert"), 12, Main, 1), C(QCoreApplication::translate("Templates", "Saturday, the twentieth of June"), 13, Accent1, 1, false, 700),
            C(QCoreApplication::translate("Templates", "seven o'clock in the evening"), 12, Main, 1), C(QStringLiteral("{biz:address:oneline}"), 11, Main, 1), C(QCoreApplication::translate("Templates", "Kindly reply to {biz:email}"), 9, Accent4, 1)})->role = role::Date;
    return d;
}

std::unique_ptr<Document> letterhead(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Ink", "Classic", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, 14, ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
    const double lx = b.logo(QRectF(c.left(), 34, 60, 60), o) ? 70 : 0;
    b.text(QRectF(c.left() + lx, 36, c.width() * 0.6 - lx, 60), {C(QStringLiteral("{biz:name}"), 22, Accent1, 0, true, 700), C(QStringLiteral("{biz:tagline}"), 10, Accent4, 0)})->autofit =
        TextItem::ShrinkOnOverflow;
    b.text(QRectF(c.left() + c.width() * 0.6, 36, c.width() * 0.4, 70), {C(QStringLiteral("{biz:address}"), 8.5, Main, 2), C(QStringLiteral("{biz:phone}"), 8.5, Main, 2), C(QStringLiteral("{biz:email}"), 8.5, Main, 2)});
    b.line(QPointF(c.left(), 112), QPointF(c.right(), 112), ColorRef::scheme(Accent1), 1.5);
    b.text(QRectF(c.left(), 140, c.width(), c.bottom() - 200), {P(QStringLiteral("{date:MMMM d, yyyy}"), "Normal"), P(QString()), P(QCoreApplication::translate("Templates", "Dear neighbor,"), "Normal"), P(lorem2(), "Body Text"), P(lorem3(), "Body Text"),
                                                                 P(QCoreApplication::translate("Templates", "Sincerely,"), "Normal"), P(QString()), P(QCoreApplication::translate("Templates", "{biz:person}\n{biz:title}"), "Normal")});
    b.text(QRectF(c.left(), ps.height() - 50, c.width(), 18), {C(QStringLiteral("{biz:web}"), 8.5, Accent1, 1)});
    return d;
}

std::unique_ptr<Document> resume(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Slate", "Clean", "Letter");
    B b(*d);
    const QRectF c = content(*d);
    b.text(QRectF(c.left(), c.top(), c.width(), 70), {C(QStringLiteral("{biz:person}"), 30, Accent1, 0, true, 700), C(QStringLiteral("{biz:address:oneline} · {biz:phone} · {biz:email}"), 9.5, Accent4, 0)});
    b.line(QPointF(c.left(), c.top() + 76), QPointF(c.right(), c.top() + 76), ColorRef::scheme(Accent1), 2);
    double y = c.top() + 92;
    const QVector<QPair<QString, QVector<Para>>> secs = {
        {QCoreApplication::translate("Templates", "Objective"), {P(QCoreApplication::translate("Templates", "Program director with ten years of experience building community arts programs and the volunteer teams that run them."), "Body Text")}},
        {QCoreApplication::translate("Templates", "Experience"), {C(QCoreApplication::translate("Templates", "Program Director, {biz:name}\t2019 – present"), 11, Main, 0, false, 700),
                        P(QCoreApplication::translate("Templates", "Grew the summer concert series from 8 to 13 weeks; recruited and trained 40 volunteers; secured three foundation grants."), "Body Text"),
                        C(QCoreApplication::translate("Templates", "Outreach Coordinator, Kalamazoo Arts Council\t2014 – 2019"), 11, Main, 0, false, 700),
                        P(QCoreApplication::translate("Templates", "Ran school partnerships reaching 2,000 students a year."), "Body Text")}},
        {QCoreApplication::translate("Templates", "Education"), {C(QCoreApplication::translate("Templates", "B.A., Music Education, Western Michigan University\t2013"), 11, Main, 0, false, 700)}},
        {QCoreApplication::translate("Templates", "Skills"), {P(QCoreApplication::translate("Templates", "Grant writing · Event production · Volunteer management · Budgeting · Desktop publishing"), "Body Text")}},
    };
    for (const auto &s : secs) {
        b.text(QRectF(c.left(), y, 120, 24), {C(s.first, 12, Accent1, 0, true, 700)});
        auto t = b.text(QRectF(c.left() + 130, y, c.width() - 130, 30 + 30 * s.second.size()), s.second);
        QTextCursor cur(d->storyDoc(t->storyId));
        cur.select(QTextCursor::Document);
        QTextBlockFormat bf;
        bf.setTabPositions({QTextOption::Tab(c.width() - 136, QTextOption::RightTab)});
        cur.mergeBlockFormat(bf);
        t->autofit = TextItem::GrowBox;
        y += 46 + 34 * s.second.size();
    }
    return d;
}

std::unique_ptr<Document> calendarTemplate(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(11 * IN, 8.5 * IN), 12, "Meadow", "Modern", "Letter Landscape");
    const int year = QDate::currentDate().year() + (QDate::currentDate().month() > 10 ? 1 : 0);
    const QStringList arts{"leaves", "bokeh", "waves", "hills", "sunburst", "confetti", "geo", "skyline", "leaves", "hills", "bokeh", "confetti"};
    {
        const QRectF c = content(*d);
        QVector<ArtRequest> want;
        for (int m = 1; m <= 12; ++m) want << ArtRequest{arts[m - 1], QSize(int(c.width() * 0.36 * 2), int(c.height() * 2)), quint32(100 + m)};
        prepareArt(d->colors, want);
    }
    for (int m = 1; m <= 12; ++m) {
        B b(*d, m - 1);
        const QSizeF ps = d->pageSize();
        const QRectF c = content(*d);
        b.picture(QRectF(c.left(), c.top(), c.width() * 0.36, c.height()), arts[m - 1], 100 + m);
        ItemList items = makeCalendar(*d, QRectF(c.left() + c.width() * 0.4, c.top(), c.width() * 0.6, c.height()), year, m, 1);
        for (auto &it : items) b.pg->items.push_back(it);
        Q_UNUSED(ps);
    }
    return d;
}

std::unique_ptr<Document> program(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(5.5 * IN, 8.5 * IN), 4, "Regatta", "Bookish", "Half Letter");
    d->setup.layout = PageSetup::Booklet;
    d->setup.sheet = QSizeF(11 * IN, 8.5 * IN);
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(c.left(), 120, c.width(), 220), {W(QCoreApplication::translate("Templates", "Spring Recital"), 32, 1, true), W(QCoreApplication::translate("Templates", "Students of {biz:name}"), 13, 1), W(QCoreApplication::translate("Templates", "Sunday, May 17 · 3:00 PM"), 12, 1)});
    b.onPage(1);
    b.text(c, {H(QCoreApplication::translate("Templates", "Welcome"), 20, 0, Accent1), P(lorem2(), "Body Text"), P(lorem3(), "Body Text")});
    b.onPage(2);
    auto t = b.text(c, {H(QCoreApplication::translate("Templates", "Program"), 20, 0, Accent1),
                        P(QCoreApplication::translate("Templates", "Minuet in G\tJ. S. Bach\nFür Elise\tL. van Beethoven\nThe Entertainer\tS. Joplin\nClair de Lune\tC. Debussy\nPrelude in C\tJ. S. Bach"), "Body Text"),
                        P(QCoreApplication::translate("Templates", "Intermission"), "Caption"),
                        P(QCoreApplication::translate("Templates", "Gymnopédie No. 1\tE. Satie\nRondo alla Turca\tW. A. Mozart\nMaple Leaf Rag\tS. Joplin"), "Body Text")});
    QTextCursor cur(d->storyDoc(t->storyId));
    cur.select(QTextCursor::Document);
    QTextBlockFormat bf;
    bf.setTabPositions({QTextOption::Tab(c.width() - 6, QTextOption::RightTab)});
    bf.setProperty(tp::TabLeaders, QStringLiteral("."));
    cur.mergeBlockFormat(bf);
    b.onPage(3);
    b.text(QRectF(c.left(), c.bottom() - 140, c.width(), 140), {C(QCoreApplication::translate("Templates", "Thank you for coming"), 16, Accent1, 1, true), C(QCoreApplication::translate("Templates", "{biz:name}\n{biz:address}\n{biz:web}"), 10, Main, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> labels(const TemplateOptions &o)
{
    // Address labels: 2-5/8" x 1", 30 per sheet (common 3 x 10 layout).
    auto d = base(o, QSizeF(2.625 * IN, 1 * IN), 1, "Ink", "Modern", "Address Label");
    d->setup.layout = PageSetup::MultiplePerSheet;
    d->setup.sheet = QSizeF(8.5 * IN, 11 * IN);
    d->setup.gridCols = 3;
    d->setup.gridRows = 10;
    d->setup.gapH = 0.125 * IN;
    d->setup.margins = QMarginsF(6, 6, 6, 6);
    B b(*d);
    b.text(QRectF(8, 6, d->pageSize().width() - 16, d->pageSize().height() - 12), {C(QStringLiteral("{mergeblock:address}"), 9, Main, 0)}, VAlign::Middle);
    return d;
}

std::unique_ptr<Document> envelope(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(9.5 * IN, 4.125 * IN), 1, "Ink", "Classic", "#10 Envelope");
    d->setup.layout = PageSetup::Envelope;
    d->setup.margins = QMarginsF(18, 18, 18, 18);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.text(QRectF(22, 20, 220, 70), {C(QStringLiteral("{biz:name}"), 10, Accent1, 0, true, 700), C(QStringLiteral("{biz:address}"), 9, Main, 0)});
    b.shape("rect", QRectF(18, 18, 4, 60), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(ps.width() * 0.42, ps.height() * 0.45, ps.width() * 0.5, 90), {C(QStringLiteral("{mergeblock:address}"), 11, Main, 0)});
    return d;
}

std::unique_ptr<Document> businessForm(const TemplateOptions &o, int variant)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Slate", "Corporate", "Letter");
    B b(*d);
    const QRectF c = content(*d);
    const double lx = b.logo(QRectF(c.left(), c.top(), 64, 64), o) ? 74 : 0;
    b.text(QRectF(c.left() + lx, c.top(), c.width() * 0.5 - lx, 84), {C(QStringLiteral("{biz:name}"), 18, Accent1, 0, true, 700), C(QStringLiteral("{biz:address}"), 9, Main, 0)});
    b.text(QRectF(c.left() + c.width() * 0.5, c.top(), c.width() * 0.5, 84),
           {C(variant == 0 ? QCoreApplication::translate("Templates", "INVOICE") : QCoreApplication::translate("Templates", "PURCHASE ORDER"), 22, Accent1, 2, true, 700), C(QCoreApplication::translate("Templates", "No. 1001  ·  {date:M/d/yyyy}"), 9, Main, 2)});
    b.text(QRectF(c.left(), c.top() + 90, c.width() / 2, 80), {C(variant == 0 ? QCoreApplication::translate("Templates", "Bill to:") : QCoreApplication::translate("Templates", "Vendor:"), 10, Accent4, 0, false, 700), C(QCoreApplication::translate("Templates", "Customer name\nStreet address\nCity, ST 00000"), 10, Main, 0)});
    auto tbl = std::make_shared<TableItem>();
    tbl->rows = 10;
    tbl->cols = 4;
    tbl->rect = QRectF(c.left(), c.top() + 190, c.width(), 300);
    tbl->colW = {c.width() * 0.12, c.width() * 0.52, c.width() * 0.18, c.width() * 0.18};
    tbl->rowH = QVector<double>(10, 26);
    tbl->cells.resize(40);
    const QStringList head{QCoreApplication::translate("Templates", "Qty"), QCoreApplication::translate("Templates", "Description"),
                           QCoreApplication::translate("Templates", "Unit price"), QCoreApplication::translate("Templates", "Amount")};
    for (int r = 0; r < 10; ++r)
        for (int col = 0; col < 4; ++col) tbl->cell(r, col).storyId = d->createStory(r == 0 ? head[col] : QString());
    tbl->syncRect();
    applyTableFormatCells(tbl.get(), QStringLiteral("Table Style 5"), [&](TableCell &cell, bool head, bool) {
        if (!head) return;
        QTextCursor cur(d->storyDoc(cell.storyId));
        cur.select(QTextCursor::Document);
        QTextCharFormat f;
        f.setProperty(tp::ColorRefP, QStringLiteral("#FFFFFF"));
        f.setFontWeight(QFont::Bold);
        cur.mergeCharFormat(f);
    });
    b.pg->items.push_back(tbl);
    b.text(QRectF(c.left() + c.width() * 0.6, c.top() + 510, c.width() * 0.4, 80), {C(QCoreApplication::translate("Templates", "Subtotal\nTax\nTotal"), 11, Main, 2, false, 700)});
    b.text(QRectF(c.left(), c.bottom() - 60, c.width(), 50), {C(QCoreApplication::translate("Templates", "Thank you for your business!  {biz:phone} · {biz:email}"), 10, Accent4, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> catalog(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 4, "Earth", "Boutique", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.picture(QRectF(0, 0, ps.width(), ps.height() * 0.65), "leaves", 31);
    b.text(QRectF(c.left(), ps.height() * 0.7, c.width() - (wantsLogo(o) ? 120 : 0), 140), {C(QStringLiteral("{biz:name}"), 36, Accent1, 0, true), C(QCoreApplication::translate("Templates", "Fall Catalog"), 18, Accent2, 0, true)});
    b.logo(QRectF(c.right() - 100, ps.height() * 0.7, 100, 100), o);
    for (int pgi = 1; pgi < 4; ++pgi) {
        b.onPage(pgi);
        for (int k = 0; k < 4; ++k) {
            const double x = c.left() + (k % 2) * (c.width() / 2 + 8), y = c.top() + (k / 2) * (c.height() / 2 + 8);
            const double w = c.width() / 2 - 8, h = c.height() / 2 - 8;
            b.picture(QRectF(x, y, w, h * 0.62), k % 2 ? "geo" : "bokeh", pgi * 10 + k);
            b.text(QRectF(x, y + h * 0.65, w, h * 0.35), {C(QCoreApplication::translate("Templates", "Product name"), 13, Accent1, 0, true, 700), P(QCoreApplication::translate("Templates", "Short description of the product, its materials and sizes."), "Body Text"),
                                                          C(QCoreApplication::translate("Templates", "$00.00  ·  Item #%1%2").arg(pgi).arg(k + 1), 11, Main, 0, false, 700)});
        }
    }
    return d;
}

std::unique_ptr<Document> emailNewsletter(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 15 * IN), 1, "Glacier", "Modern", "Email");
    d->setup.margins = QMarginsF(36, 36, 36, 36);
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, ps.width(), 120), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(c.left(), 30, c.width() - (wantsLogo(o) ? 90 : 0), 70), {W(QStringLiteral("{biz:name}"), 28, 0, true, 700), W(QCoreApplication::translate("Templates", "News for friends and neighbors"), 12, 0)});
    b.logo(QRectF(c.right() - 76, 22, 76, 76), o);
    b.picture(QRectF(c.left(), 140, c.width(), 240), "hills", 41);
    b.text(QRectF(c.left(), 396, c.width(), 260), {H(QCoreApplication::translate("Templates", "This month on the riverfront"), 22, 0, Accent1), P(lorem1(), "Body Text"), P(lorem2(), "Body Text")});
    b.shapeText("roundRect", QRectF(ps.width() / 2 - 110, 670, 220, 44), Fill::solid(ColorRef::scheme(Accent3)), {W(QCoreApplication::translate("Templates", "Get tickets"), 14, 1, true, 700)});
    b.text(QRectF(c.left(), 740, c.width(), 300), {H(QCoreApplication::translate("Templates", "Volunteer with us"), 18, 0, Accent1), P(lorem3(), "Body Text")});
    b.text(QRectF(c.left(), ps.height() - 90, c.width(), 50), {C(QStringLiteral("{biz:address:oneline} · {biz:web}"), 9, Accent4, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> complimentsCard(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.27 * IN, 3.9 * IN), 1, "Sage", "Elegant", "With Compliments");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width(), 24), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(36, 60, ps.width() - 72, 80), {C(QCoreApplication::translate("Templates", "With Compliments"), 30, Accent1, 1, true)});
    b.text(QRectF(36, ps.height() - 90, ps.width() - 72, 60), {C(QStringLiteral("{biz:name}"), 12, Main, 1, false, 700), C(QStringLiteral("{biz:address:oneline}  ·  {biz:phone}"), 9, Main, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> paperAirplane(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Sky", "Retro", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height()), "stripes", 51);
    // Fold lines for a classic dart.
    const ColorRef fold = ColorRef::scheme(Main);
    auto mk = [&](QPointF a, QPointF bb) { auto l = b.line(a, bb, fold, 1); l->stroke.dash = Stroke::DashLine; };
    mk(QPointF(ps.width() / 2, 0), QPointF(ps.width() / 2, ps.height()));
    mk(QPointF(ps.width() / 2, 0), QPointF(0, ps.width() / 2));
    mk(QPointF(ps.width() / 2, 0), QPointF(ps.width(), ps.width() / 2));
    mk(QPointF(ps.width() / 2, 0), QPointF(ps.width() * 0.12, ps.height()));
    mk(QPointF(ps.width() / 2, 0), QPointF(ps.width() * 0.88, ps.height()));
    b.shapeText("roundRect", QRectF(ps.width() / 2 - 150, ps.height() - 120, 300, 70), Fill::solid(ColorRef::rgb(Qt::white)),
                {C(QCoreApplication::translate("Templates", "Fold along the dashed lines, center line first."), 11, Main, 1)});
    return d;
}

// ---------------- second designs ----------------
// Right tab at `at` with dot leaders, on every paragraph of a text box.
void dotLeaders(Document &d, const TextItem &t, double at, const QString &leader = QStringLiteral("."))
{
    QTextCursor cur(d.storyDoc(t.storyId));
    cur.select(QTextCursor::Document);
    QTextBlockFormat bf;
    bf.setTabPositions({QTextOption::Tab(at, QTextOption::RightTab)});
    if (!leader.isEmpty()) bf.setProperty(tp::TabLeaders, leader);
    cur.mergeBlockFormat(bf);
}

std::unique_ptr<Document> certificateAppreciation(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Garnet", "Script", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(18, 18, ps.width() - 36, ps.height() - 36), Fill::none(), Stroke::line(ColorRef::scheme(Accent1), 10));
    auto inner = b.shape("rect", QRectF(36, 36, ps.width() - 72, ps.height() - 72), Fill::none(), Stroke::line(ColorRef::scheme(Accent3), 3));
    inner->stroke.compound = Stroke::ThinThick;
    for (int i = 0; i < 4; ++i)
        b.shape("diamond", QRectF(i % 2 ? ps.width() - 52 : 28, i / 2 ? ps.height() - 52 : 28, 24, 24), Fill::solid(ColorRef::scheme(Accent3)));
    b.text(QRectF(72, 110, ps.width() - 144, 150), {C(QCoreApplication::translate("Templates", "Certificate"), 54, Accent1, 1, true), C(QCoreApplication::translate("Templates", "OF APPRECIATION"), 16, Accent3, 1, false, 700)});
    b.text(QRectF(72, 300, ps.width() - 144, 30), {C(QCoreApplication::translate("Templates", "With gratitude, this certificate is presented to"), 13, Main, 1)});
    b.text(QRectF(90, 340, ps.width() - 180, 70), {C(QCoreApplication::translate("Templates", "Recipient Name"), 40, Accent1, 1, true)})->autofit = TextItem::ShrinkOnOverflow;
    b.line(QPointF(120, 418), QPointF(ps.width() - 120, 418), ColorRef::scheme(Accent3), 1);
    b.text(QRectF(100, 432, ps.width() - 200, 90), {C(QCoreApplication::translate("Templates", "for generously giving time, talent and heart to our community this year."), 13, Main, 1)});
    b.shapeText("ribbon2", QRectF(ps.width() / 2 - 120, 540, 240, 70), Fill::solid(ColorRef::scheme(Accent1)), {W(QCoreApplication::translate("Templates", "THANK YOU"), 15, 1, false, 700)});
    for (int i = 0; i < 2; ++i) {
        const double x = i == 0 ? 90 : ps.width() - 290;
        b.line(QPointF(x, ps.height() - 170), QPointF(x + 200, ps.height() - 170), ColorRef::scheme(Main), 0.75);
        b.text(QRectF(x, ps.height() - 164, 200, 40), {C(i == 0 ? QStringLiteral("{biz:person}") : QStringLiteral("{date:MMMM d, yyyy}"), 10, Main, 1)});
    }
    b.text(QRectF(72, ps.height() - 110, ps.width() - 144, 30), {C(QStringLiteral("{biz:name}"), 12, Accent3, 1, false, 700)});
    return d;
}

std::unique_ptr<Document> postcardEvent(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(6 * IN, 4 * IN), 2, "Midnight", "Poster", "Postcard");
    d->setup.margins = QMarginsF(18, 18, 18, 18);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height()), "bokeh", 17);
    b.text(QRectF(24, 30, ps.width() * 0.62, 180), {W(QCoreApplication::translate("Templates", "SAVE"), 54, 0, true), W(QCoreApplication::translate("Templates", "THE DATE"), 54, 0, true)});
    b.shapeText("ellipse", QRectF(ps.width() - 150, 40, 120, 120), Fill::solid(ColorRef::scheme(Accent3)), {C(QCoreApplication::translate("Templates", "JUNE"), 14, Main, 1, true), C(QStringLiteral("20"), 40, Main, 1, true)});
    b.text(QRectF(24, ps.height() - 70, ps.width() - 48, 50), {W(QCoreApplication::translate("Templates", "{biz:name} · Annual Gala · {biz:address:oneline}"), 10, 0)}, VAlign::Bottom);
    b.onPage(1);
    b.shape("rect", QRectF(0, 0, 10, ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(26, 20, ps.width() / 2 - 40, ps.height() - 40),
           {C(QCoreApplication::translate("Templates", "Annual Gala"), 18, Accent1, 0, true), P(QCoreApplication::translate("Templates", "Dinner, music and a silent auction to support our programs. Formal invitation to follow."), "Body Text"),
            C(QStringLiteral("{biz:web}"), 9, Accent4, 0)});
    b.line(QPointF(ps.width() / 2, 24), QPointF(ps.width() / 2, ps.height() - 24), ColorRef::scheme(Accent4), 0.75);
    if (wantsMailing(o)) {
        b.shape("rect", QRectF(ps.width() - 78, 18, 60, 70), Fill::none(), Stroke::line(ColorRef::scheme(Accent4), 0.75))->stroke.dash = Stroke::DashLine;
        b.text(QRectF(ps.width() - 78, 34, 60, 40), {C(QCoreApplication::translate("Templates", "Place\nstamp\nhere"), 7, Accent4, 1)}, VAlign::Middle);
        b.text(QRectF(ps.width() / 2 + 18, ps.height() / 2, ps.width() / 2 - 36, 90), {C(QStringLiteral("{mergeblock:address}"), 10, Main, 0)});
    }
    return d;
}

std::unique_ptr<Document> bannerGrandOpening(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(72 * IN, 24 * IN), 1, "Fiesta", "Heavy", "Banner 6 × 2 ft");
    d->setup.margins = QMarginsF(72, 72, 72, 72);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Accent5)));
    b.picture(QRectF(0, 0, ps.width(), 260), "stripes", 7);
    b.picture(QRectF(0, ps.height() - 260, ps.width(), 260), "stripes", 8);
    b.textart(QRectF(260, 360, ps.width() - 520, 760), QCoreApplication::translate("Templates", "GRAND OPENING"), "fill-main", d->fonts.heading)->fill = Fill::solid(ColorRef::scheme(Accent1));
    b.text(QRectF(260, 1140, ps.width() - 520, 240), {C(QCoreApplication::translate("Templates", "{biz:name}  ·  Saturday 10 AM"), 110, Main, 1, true, 700)});
    return d;
}

std::unique_ptr<Document> menuCafe(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Chalkboard", "Chalk", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Main)));
    auto frame = b.shape("roundRect", QRectF(20, 20, ps.width() - 40, ps.height() - 40), Fill::none(), Stroke::line(ColorRef::rgb(Qt::white), 2));
    frame->stroke.dash = Stroke::RoundDot;
    b.text(QRectF(c.left(), c.top() + 10, c.width(), 90), {W(QStringLiteral("{biz:name}"), 40, 1, true), W(QCoreApplication::translate("Templates", "COFFEE · TEA · PASTRY"), 12, 1)});
    struct Col { const char *title; QVector<QPair<const char *, const char *>> items; };
    const QVector<Col> cols = {{QT_TRANSLATE_NOOP("Templates", "Coffee"), {{QT_TRANSLATE_NOOP("Templates", "Drip coffee"), "2.50"}, {QT_TRANSLATE_NOOP("Templates", "Espresso"), "3.00"}, {QT_TRANSLATE_NOOP("Templates", "Cappuccino"), "4.25"}, {QT_TRANSLATE_NOOP("Templates", "Latte"), "4.50"}, {QT_TRANSLATE_NOOP("Templates", "Mocha"), "4.75"}, {QT_TRANSLATE_NOOP("Templates", "Cold brew"), "4.00"}}},
                               {QT_TRANSLATE_NOOP("Templates", "Tea"), {{QT_TRANSLATE_NOOP("Templates", "Black or green"), "2.75"}, {QT_TRANSLATE_NOOP("Templates", "Chai latte"), "4.50"}, {QT_TRANSLATE_NOOP("Templates", "Herbal"), "2.75"}, {QT_TRANSLATE_NOOP("Templates", "Iced tea"), "3.00"}}},
                               {QT_TRANSLATE_NOOP("Templates", "Bakery"), {{QT_TRANSLATE_NOOP("Templates", "Butter croissant"), "3.25"}, {QT_TRANSLATE_NOOP("Templates", "Blueberry muffin"), "3.00"}, {QT_TRANSLATE_NOOP("Templates", "Cinnamon roll"), "3.75"}, {QT_TRANSLATE_NOOP("Templates", "Banana bread"), "3.25"}}},
                               {QT_TRANSLATE_NOOP("Templates", "Lunch"), {{QT_TRANSLATE_NOOP("Templates", "Soup of the day"), "6.00"}, {QT_TRANSLATE_NOOP("Templates", "Grilled cheese"), "7.50"}, {QT_TRANSLATE_NOOP("Templates", "Turkey and swiss"), "9.00"}, {QT_TRANSLATE_NOOP("Templates", "Garden salad"), "8.00"}}}};
    const double colW = (c.width() - 30) / 2;
    for (int k = 0; k < cols.size(); ++k) {
        const double x = c.left() + (k % 2) * (colW + 30), y = c.top() + 130 + (k / 2) * 290;
        QVector<Para> paras{W(QCoreApplication::translate("Templates", cols[k].title), 22, 0, true)};
        for (const auto &it : cols[k].items) paras << W(QCoreApplication::translate("Templates", it.first) + "\t" + QString::fromLatin1(it.second), 12, 0);
        auto t = b.text(QRectF(x, y, colW, 270), paras);
        dotLeaders(*d, *t, colW - 8);
    }
    b.text(QRectF(c.left(), c.bottom() - 40, c.width(), 30), {W(QStringLiteral("{biz:address:oneline}  ·  {biz:phone}"), 10, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> giftCertificateClassic(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(7 * IN, 3 * IN), 1, "Forest", "Monument", "Gift Certificate");
    d->setup.margins = QMarginsF(14, 14, 14, 14);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Accent5)));
    auto border = b.shape("rect", QRectF(10, 10, ps.width() - 20, ps.height() - 20), Fill::none(), Stroke::line(ColorRef::scheme(Accent1), 5));
    border->stroke.compound = Stroke::Triple;
    b.shape("rect", QRectF(ps.width() * 0.68, 10, ps.width() * 0.32 - 10, ps.height() - 20), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(30, 26, ps.width() * 0.6, 50), {C(QCoreApplication::translate("Templates", "GIFT CERTIFICATE"), 24, Accent1, 0, true)})->role = role::Title;
    auto f = b.text(QRectF(30, 84, ps.width() * 0.6, 90), {C(QCoreApplication::translate("Templates", "Presented to\t"), 11, Main, 0), C(QCoreApplication::translate("Templates", "From\t"), 11, Main, 0), C(QCoreApplication::translate("Templates", "Valid until\t"), 11, Main, 0)});
    dotLeaders(*d, *f, ps.width() * 0.6 - 10, QStringLiteral("_"));
    b.text(QRectF(ps.width() * 0.68 + 10, 40, ps.width() * 0.32 - 30, 140), {W(QStringLiteral("$100"), 38, 1, true), W(QStringLiteral("{biz:name}"), 10, 1, false, 700), W(QStringLiteral("{biz:phone}"), 8, 1)}, VAlign::Middle)->role = role::Label;
    b.text(QRectF(30, ps.height() - 42, ps.width() * 0.6, 20), {C(QCoreApplication::translate("Templates", "No. 0001 · Not redeemable for cash"), 7.5, Accent4, 0)});
    return d;
}

std::unique_ptr<Document> invitationParty(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(5 * IN, 7 * IN), 1, "Carnival", "Festive", "5 × 7 Invitation");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.picture(QRectF(0, 0, ps.width(), ps.height()), "confetti", 27);
    b.shape("roundRect", QRectF(26, 70, ps.width() - 52, ps.height() - 140), Fill::solid(ColorRef::rgb(Qt::white)));
    b.text(QRectF(40, 96, ps.width() - 80, 110), {C(QCoreApplication::translate("Templates", "Let's"), 26, Accent2, 1, true), C(QCoreApplication::translate("Templates", "Celebrate!"), 40, Accent1, 1, true)});
    b.text(QRectF(46, 220, ps.width() - 92, 190),
           {C(QCoreApplication::translate("Templates", "Join us for a birthday party"), 13, Main, 1), C(QCoreApplication::translate("Templates", "Saturday, August 8"), 15, Accent1, 1, false, 700), C(QCoreApplication::translate("Templates", "2:00 – 5:00 PM"), 13, Main, 1),
            C(QStringLiteral("{biz:address:oneline}"), 11, Main, 1), C(QCoreApplication::translate("Templates", "RSVP {biz:phone}"), 10, Accent4, 1)})->role = role::Date;
    return d;
}

std::unique_ptr<Document> letterheadModern(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Cobalt", "Modern", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, ps.width(), 96), Fill::gradient(ColorRef::scheme(Accent1), ColorRef::scheme(Accent2), 0));
    b.text(QRectF(c.left(), 18, c.width() - (wantsLogo(o) ? 84 : 0), 64), {W(QStringLiteral("{biz:name}"), 24, 0, true, 700), W(QStringLiteral("{biz:tagline}"), 10, 0)}, VAlign::Middle);
    b.logo(QRectF(c.right() - 70, 13, 70, 70), o);
    b.text(QRectF(c.left(), 130, c.width(), c.bottom() - 220), {P(QStringLiteral("{date:MMMM d, yyyy}"), "Normal"), P(QString()), P(QCoreApplication::translate("Templates", "Dear neighbor,"), "Normal"), P(lorem1(), "Body Text"),
                                                                P(lorem2(), "Body Text"), P(QCoreApplication::translate("Templates", "Best regards,"), "Normal"), P(QString()), P(QCoreApplication::translate("Templates", "{biz:person}\n{biz:title}"), "Normal")});
    b.line(QPointF(c.left(), ps.height() - 74), QPointF(c.right(), ps.height() - 74), ColorRef::scheme(Accent1), 0.75);
    b.text(QRectF(c.left(), ps.height() - 66, c.width(), 30), {C(QStringLiteral("{biz:address:oneline}  ·  {biz:phone}  ·  {biz:email}  ·  {biz:web}"), 8.5, Accent4, 1)});
    return d;
}

std::unique_ptr<Document> resumeSidebar(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Fjord", "Studio", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const double side = 200;
    b.shape("rect", QRectF(0, 0, side, ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
    b.picture(QRectF(40, 40, side - 80, side - 80), "hills", 61, QStringLiteral("ellipse"));
    b.text(QRectF(24, side - 20, side - 48, 400),
           {W(QCoreApplication::translate("Templates", "CONTACT"), 11, 0, true, 700), W(QCoreApplication::translate("Templates", "{biz:phone}\n{biz:email}\n{biz:address}"), 9.5, 0), W(QString(), 6, 0),
            W(QCoreApplication::translate("Templates", "SKILLS"), 11, 0, true, 700), W(QCoreApplication::translate("Templates", "Grant writing\nEvent production\nVolunteer management\nBudgeting\nDesktop publishing"), 9.5, 0)});
    const double x = side + 30, w = ps.width() - side - 66;
    b.text(QRectF(x, 40, w, 80), {C(QStringLiteral("{biz:person}"), 30, Accent1, 0, true, 700), C(QCoreApplication::translate("Templates", "Program Director"), 13, Accent4, 0)});
    auto body = b.text(QRectF(x, 140, w, ps.height() - 190),
                       {C(QCoreApplication::translate("Templates", "PROFILE"), 12, Accent1, 0, true, 700),
                        P(QCoreApplication::translate("Templates", "Program director with ten years of experience building community arts programs and the volunteer teams that run them."), "Body Text"),
                        C(QCoreApplication::translate("Templates", "EXPERIENCE"), 12, Accent1, 0, true, 700),
                        C(QCoreApplication::translate("Templates", "Program Director, {biz:name}\t2019 – present"), 10.5, Main, 0, false, 700),
                        P(QCoreApplication::translate("Templates", "Grew the summer concert series from 8 to 13 weeks; recruited and trained 40 volunteers; secured three foundation grants."), "Body Text"),
                        C(QCoreApplication::translate("Templates", "Outreach Coordinator, Kalamazoo Arts Council\t2014 – 2019"), 10.5, Main, 0, false, 700),
                        P(QCoreApplication::translate("Templates", "Ran school partnerships reaching 2,000 students a year."), "Body Text"),
                        C(QCoreApplication::translate("Templates", "EDUCATION"), 12, Accent1, 0, true, 700),
                        C(QCoreApplication::translate("Templates", "B.A., Music Education, Western Michigan University\t2013"), 10.5, Main, 0, false, 700)});
    dotLeaders(*d, *body, w - 6, QString());
    return d;
}

std::unique_ptr<Document> calendarMonth(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Harvest", "Friendly", "Letter");
    B b(*d);
    const QRectF c = content(*d);
    const QDate today = QDate::currentDate();
    b.picture(QRectF(c.left(), c.top(), c.width(), c.height() * 0.38), "sunburst", 71);
    ItemList items = makeCalendar(*d, QRectF(c.left(), c.top() + c.height() * 0.42, c.width(), c.height() * 0.58), today.year(), today.month(), 2);
    for (auto &it : items) b.pg->items.push_back(it);
    return d;
}

std::unique_ptr<Document> programEvent(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Nautical", "Civic", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, ps.width(), 150), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(c.left(), 30, c.width(), 100), {W(QCoreApplication::translate("Templates", "Annual Meeting"), 34, 1, true), W(QStringLiteral("{biz:name} · {date:MMMM d, yyyy}"), 12, 1)}, VAlign::Middle);
    auto t = b.text(QRectF(c.left() + 20, 190, c.width() - 40, 420),
                    {C(QCoreApplication::translate("Templates", "Order of Events"), 20, Accent1, 1, true),
                     C(QCoreApplication::translate("Templates", "Welcome and introductions\t6:00 PM"), 13, Main, 0), C(QCoreApplication::translate("Templates", "Year in review\t6:15 PM"), 13, Main, 0),
                     C(QCoreApplication::translate("Templates", "Treasurer's report\t6:35 PM"), 13, Main, 0), C(QCoreApplication::translate("Templates", "Volunteer awards\t6:50 PM"), 13, Main, 0),
                     C(QCoreApplication::translate("Templates", "Election of officers\t7:15 PM"), 13, Main, 0), C(QCoreApplication::translate("Templates", "Closing remarks\t7:40 PM"), 13, Main, 0),
                     C(QCoreApplication::translate("Templates", "Refreshments\t7:45 PM"), 13, Main, 0)});
    dotLeaders(*d, *t, c.width() - 46);
    b.shape("rect", QRectF(c.left(), 640, c.width(), 1.5), Fill::solid(ColorRef::scheme(Accent3)));
    b.text(QRectF(c.left(), 660, c.width(), 80), {C(QCoreApplication::translate("Templates", "Thank you to our sponsors and volunteers."), 12, Accent4, 1), C(QStringLiteral("{biz:web}"), 10, Accent1, 1)});
    return d;
}

std::unique_ptr<Document> labelsShipping(const TemplateOptions &o)
{
    // Shipping labels: 4" x 2", 10 per sheet (2 x 5).
    auto d = base(o, QSizeF(4 * IN, 2 * IN), 1, "Slate", "Corporate", "Shipping Label");
    d->setup.layout = PageSetup::MultiplePerSheet;
    d->setup.sheet = QSizeF(8.5 * IN, 11 * IN);
    d->setup.gridCols = 2;
    d->setup.gridRows = 5;
    d->setup.gapH = 0.1875 * IN;
    d->setup.margins = QMarginsF(8, 8, 8, 8);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, 8, ps.height()), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(16, 8, ps.width() * 0.45, 60), {C(QCoreApplication::translate("Templates", "FROM"), 6.5, Accent4, 0, false, 700), C(QCoreApplication::translate("Templates", "{biz:name}\n{biz:address}"), 7.5, Main, 0)});
    b.text(QRectF(ps.width() * 0.38, ps.height() * 0.42, ps.width() * 0.58, ps.height() * 0.54), {C(QCoreApplication::translate("Templates", "SHIP TO"), 7, Accent4, 0, false, 700), C(QStringLiteral("{mergeblock:address}"), 10, Main, 0)});
    return d;
}

std::unique_ptr<Document> envelopeA7(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(7.25 * IN, 5.25 * IN), 1, "Lilac", "Script", "A7 Envelope");
    d->setup.layout = PageSetup::Envelope;
    d->setup.margins = QMarginsF(18, 18, 18, 18);
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.text(QRectF(24, 22, 200, 60), {C(QStringLiteral("{biz:name}"), 10, Accent1, 0, true), C(QStringLiteral("{biz:address}"), 8, Main, 0)});
    b.shape("heart", QRectF(ps.width() - 60, 24, 30, 26), Fill::solid(ColorRef::scheme(Accent1)));
    b.text(QRectF(ps.width() * 0.25, ps.height() * 0.42, ps.width() * 0.5, 100), {C(QStringLiteral("{mergeblock:address}"), 12, Main, 1)}, VAlign::Middle);
    return d;
}

// A catalog made with Catalog Merge: a product list fills the catalog
// area on page 2, four products to a page.
std::unique_ptr<Document> catalogMerged(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 2, "Juniper", "Clean", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.shape("rect", QRectF(0, 0, ps.width(), ps.height()), Fill::solid(ColorRef::scheme(Accent5)));
    b.picture(QRectF(c.left(), c.top(), c.width(), c.height() * 0.6), "geo", 81);
    b.text(QRectF(c.left(), c.top() + c.height() * 0.64, c.width() - (wantsLogo(o) ? 120 : 0), 140), {C(QStringLiteral("{biz:name}"), 40, Accent1, 0, true, 700), C(QCoreApplication::translate("Templates", "Product Catalog"), 20, Accent2, 0, true)});
    b.logo(QRectF(c.right() - 100, c.top() + c.height() * 0.64, 100, 100), o);
    d->merge.fields = {QStringLiteral("Product"), QStringLiteral("Price"), QStringLiteral("Description"), QStringLiteral("Item")};
    const QVector<QStringList> rows = {{QCoreApplication::translate("Templates", "Canvas tote"), "$24",
                                        QCoreApplication::translate("Templates", "Heavy cotton canvas with leather handles."), "101"},
                                       {QCoreApplication::translate("Templates", "Wool throw"), "$68",
                                        QCoreApplication::translate("Templates", "Soft merino wool, 50 by 60 inches."), "102"},
                                       {QCoreApplication::translate("Templates", "Ceramic mug"), "$16",
                                        QCoreApplication::translate("Templates", "Glazed stoneware, holds 12 ounces."), "103"},
                                       {QCoreApplication::translate("Templates", "Notebook set"), "$18",
                                        QCoreApplication::translate("Templates", "Three lined notebooks with recycled covers."), "104"},
                                       {QCoreApplication::translate("Templates", "Desk lamp"), "$45",
                                        QCoreApplication::translate("Templates", "Adjustable arm and a warm LED bulb."), "105"},
                                       {QCoreApplication::translate("Templates", "Plant pot"), "$22",
                                        QCoreApplication::translate("Templates", "Terracotta with a drainage saucer."), "106"}};
    d->merge.rows = rows;
    b.onPage(1);
    b.text(QRectF(c.left(), c.top(), c.width(), 40), {C(QCoreApplication::translate("Templates", "New this season"), 20, Accent1, 0, true)});
    d->catalog.pageId = d->pages[1]->id;
    d->catalog.rect = QRectF(c.left(), c.top() + 50, c.width(), c.height() - 50);
    d->catalog.rows = 2;
    d->catalog.cols = 2;
    const QRectF cell = d->catalog.cell(0).adjusted(8, 8, -8, -8);
    auto pic = std::make_shared<PictureItem>();
    pic->rect = QRectF(cell.left(), cell.top(), cell.width(), cell.height() * 0.6);
    pic->imageId = art(*d, "bokeh", QSize(int(pic->rect.width() * 2), int(pic->rect.height() * 2)), 82);
    pic->imgRect = QRectF(QPointF(0, 0), pic->rect.size());
    b.pg->items.push_back(pic);
    b.text(QRectF(cell.left(), cell.top() + cell.height() * 0.63, cell.width(), cell.height() * 0.37),
           {C(QStringLiteral("{merge:Product}"), 14, Accent1, 0, true, 700), P(QStringLiteral("{merge:Description}"), "Body Text"),
            C(QCoreApplication::translate("Templates", "{merge:Price}  ·  Item {merge:Item}"), 11, Main, 0, false, 700)});
    return d;
}

std::unique_ptr<Document> emailPromo(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 12 * IN), 1, "Nectarine", "Heavy", "Email");
    d->setup.margins = QMarginsF(36, 36, 36, 36);
    B b(*d);
    const QSizeF ps = d->pageSize();
    const QRectF c = content(*d);
    b.text(QRectF(c.left(), 30, c.width(), 40), {C(QStringLiteral("{biz:name}"), 16, Accent1, 1, true, 700)});
    b.picture(QRectF(0, 80, ps.width(), 300), "sunburst", 91);
    b.text(QRectF(c.left(), 400, c.width(), 120), {C(QCoreApplication::translate("Templates", "20% OFF"), 54, Accent1, 1, true), C(QCoreApplication::translate("Templates", "everything this weekend"), 18, Main, 1)});
    auto coupon = b.shapeText("roundRect", QRectF(ps.width() / 2 - 150, 540, 300, 80), Fill::solid(ColorRef::scheme(Accent5)),
                              {C(QCoreApplication::translate("Templates", "Use code"), 11, Main, 1), C(QCoreApplication::translate("Templates", "WEEKEND20"), 24, Accent1, 1, true, 700)});
    coupon->stroke = Stroke::line(ColorRef::scheme(Accent1), 2);
    coupon->stroke.dash = Stroke::DashLine;
    b.text(QRectF(c.left(), 650, c.width(), 120), {P(lorem3(), "Body Text")});
    b.shapeText("roundRect", QRectF(ps.width() / 2 - 100, 780, 200, 44), Fill::solid(ColorRef::scheme(Accent1)), {W(QCoreApplication::translate("Templates", "Shop now"), 14, 1, true, 700)});
    b.text(QRectF(c.left(), ps.height() - 70, c.width(), 40), {C(QStringLiteral("{biz:address:oneline} · {biz:web}"), 8.5, Accent4, 1)}, VAlign::Bottom);
    return d;
}

std::unique_ptr<Document> complimentsModern(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.27 * IN, 3.9 * IN), 1, "Kingfisher", "Modern", "With Compliments");
    B b(*d);
    const QSizeF ps = d->pageSize();
    b.shape("rect", QRectF(0, 0, ps.width() * 0.32, ps.height()), Fill::gradient(ColorRef::scheme(Accent1), ColorRef::scheme(Accent2), 90));
    b.logo(QRectF(18, 18, 64, 64), o);
    const double top = wantsLogo(o) ? 90 : 30;
    b.text(QRectF(18, top, ps.width() * 0.32 - 36, ps.height() - 30 - top), {W(QStringLiteral("{biz:name}"), 16, 0, true, 700), W(QStringLiteral("{biz:address}"), 8.5, 0), W(QStringLiteral("{biz:phone}"), 8.5, 0)}, VAlign::Bottom);
    b.text(QRectF(ps.width() * 0.32 + 30, 40, ps.width() * 0.68 - 60, 70), {C(QCoreApplication::translate("Templates", "With Compliments"), 28, Accent1, 0, true)});
    b.line(QPointF(ps.width() * 0.32 + 30, 116), QPointF(ps.width() - 30, 116), ColorRef::scheme(Accent3), 1);
    return d;
}

// An origami fortune teller: fold lines, four colors and eight numbers.
std::unique_ptr<Document> fortuneTeller(const TemplateOptions &o)
{
    auto d = base(o, QSizeF(8.5 * IN, 11 * IN), 1, "Carnival", "Playful", "Letter");
    B b(*d);
    const QSizeF ps = d->pageSize();
    const double side = 7.5 * IN;
    const QRectF sq((ps.width() - side) / 2, 36, side, side);
    b.shape("rect", sq, Fill::none(), Stroke::line(ColorRef::scheme(Main), 1));
    const QPointF ctr = sq.center();
    // Outer corner triangles: the four colors.
    const QPointF corners[4] = {sq.topLeft(), sq.topRight(), sq.bottomRight(), sq.bottomLeft()};
    const char *colorNames[4] = {QT_TRANSLATE_NOOP("Templates", "RED"), QT_TRANSLATE_NOOP("Templates", "BLUE"), QT_TRANSLATE_NOOP("Templates", "GREEN"),
                                 QT_TRANSLATE_NOOP("Templates", "YELLOW")};
    const int slots[4] = {Accent1, Accent2, Accent3, Accent4};
    for (int k = 0; k < 4; ++k) {
        const QPointF a = corners[k], m1 = (corners[k] + corners[(k + 1) % 4]) / 2, m0 = (corners[k] + corners[(k + 3) % 4]) / 2;
        auto tri = std::make_shared<ShapeItem>();
        QPolygonF poly({a, m1, m0, a});
        const QRectF br = poly.boundingRect();
        tri->rect = br;
        QPainterPath path;
        path.addPolygon(poly.translated(-br.topLeft()));
        tri->customPath = path;
        tri->fill = Fill::solid(ColorRef::scheme(slots[k], 30));
        tri->stroke = Stroke::none();
        b.pg->items.push_back(tri);
        const QPointF tc = (a + m1 + m0) / 3;
        b.text(QRectF(tc.x() - 50, tc.y() - 12, 100, 24), {C(QCoreApplication::translate("Templates", colorNames[k]), 12, Main, 1, true, 700)}, VAlign::Middle)->role = role::Label;
    }
    // Numbers in the middle ring, and fold lines.
    for (int k = 0; k < 8; ++k) {
        const double a = M_PI / 8 + k * M_PI / 4;
        const QPointF p = ctr + QPointF(std::cos(a), std::sin(a)) * side * 0.27;
        b.text(QRectF(p.x() - 20, p.y() - 14, 40, 28), {C(QString::number(k + 1), 18, Main, 1, true, 700)}, VAlign::Middle)->role = role::Label;
    }
    auto fold = [&](QPointF a, QPointF z) { auto l = b.line(a, z, ColorRef::scheme(Main, 50), 0.75); l->stroke.dash = Stroke::DashLine; };
    fold(sq.topLeft(), sq.bottomRight());
    fold(sq.topRight(), sq.bottomLeft());
    fold(QPointF(ctr.x(), sq.top()), QPointF(ctr.x(), sq.bottom()));
    fold(QPointF(sq.left(), ctr.y()), QPointF(sq.right(), ctr.y()));
    for (int k = 0; k < 4; ++k) fold((corners[k] + corners[(k + 1) % 4]) / 2, (corners[(k + 1) % 4] + corners[(k + 2) % 4]) / 2);
    b.text(QRectF(sq.left(), sq.bottom() + 14, sq.width(), 60),
           {C(QCoreApplication::translate("Templates", "Cut out the square. Fold the corners to the center, turn it over, fold the corners in again, then fold in half both ways."), 10, Accent4, 1)});
    return d;
}

// Every design comes out with its boxes tagged for Change Template.
QVector<TemplateInfo> tagged(QVector<TemplateInfo> list)
{
    for (TemplateInfo &t : list)
        t.build = [make = t.build](const TemplateOptions &o) {
            auto d = make(o);
            tagRoles(*d);
            return d;
        };
    return list;
}

} // namespace

const QVector<TemplateInfo> &templates()
{
    static const QVector<TemplateInfo> list = tagged({
        {"flyer-event", QCoreApplication::translate("Templates", "Flyers"), QCoreApplication::translate("Templates", "Event Flyer"),
         QCoreApplication::translate("Templates", "Bold top artwork, event details and a starburst callout."), flyerEvent, {}},
        {"flyer-sale", QCoreApplication::translate("Templates", "Flyers"), QCoreApplication::translate("Templates", "Sale Flyer"),
         QCoreApplication::translate("Templates", "Full-color sale sign with a percentage burst."), flyerSale, {}},
        {"flyer-announce", QCoreApplication::translate("Templates", "Flyers"), QCoreApplication::translate("Templates", "Announcement Flyer"),
         QCoreApplication::translate("Templates", "Picture, headline, story and a sidebar."), flyerAnnouncement, {}},
        {"bizcard-band", QCoreApplication::translate("Templates", "Business Cards"), QCoreApplication::translate("Templates", "Accent Band Business Card"),
         QCoreApplication::translate("Templates", "Color band with your logo spot."), [](const TemplateOptions &o) { return businessCard(o, 0); }, {"logo"}},
        {"bizcard-dark", QCoreApplication::translate("Templates", "Business Cards"), QCoreApplication::translate("Templates", "Bold Business Card"),
         QCoreApplication::translate("Templates", "Dark card with an accent stripe."), [](const TemplateOptions &o) { return businessCard(o, 1); }, {"logo"}},
        {"bizcard-classic", QCoreApplication::translate("Templates", "Business Cards"), QCoreApplication::translate("Templates", "Classic Business Card"),
         QCoreApplication::translate("Templates", "Centered, framed and elegant."), [](const TemplateOptions &o) { return businessCard(o, 2); }, {"logo"}},
        {"brochure-trifold", QCoreApplication::translate("Templates", "Brochures"), QCoreApplication::translate("Templates", "Tri-fold Brochure"),
         QCoreApplication::translate("Templates", "Letter landscape, three panels, linked story inside."), [](const TemplateOptions &o) { return brochure(o, 0); }, {"logo"}},
        {"brochure-gallery", QCoreApplication::translate("Templates", "Brochures"), QCoreApplication::translate("Templates", "Gallery Brochure"),
         QCoreApplication::translate("Templates", "Tri-fold with a full-height cover picture."), [](const TemplateOptions &o) { return brochure(o, 1); }, {"logo"}},
        {"newsletter-classic", QCoreApplication::translate("Templates", "Newsletters"), QCoreApplication::translate("Templates", "Classic Newsletter"),
         QCoreApplication::translate("Templates", "Masthead, three columns, continued story, calendar table."), [](const TemplateOptions &o) { return newsletter(o, 0); }, {"logo"}},
        {"newsletter-friendly", QCoreApplication::translate("Templates", "Newsletters"), QCoreApplication::translate("Templates", "Friendly Newsletter"),
         QCoreApplication::translate("Templates", "Warm colors and rounded type."), [](const TemplateOptions &o) { return newsletter(o, 1); }, {"logo"}},
        {"greeting-birthday", QCoreApplication::translate("Templates", "Greeting Cards"), QCoreApplication::translate("Templates", "Birthday Card"),
         QCoreApplication::translate("Templates", "Quarter-fold card with confetti."), [](const TemplateOptions &o) { return greetingCard(o, 0); }, {}},
        {"greeting-thanks", QCoreApplication::translate("Templates", "Greeting Cards"), QCoreApplication::translate("Templates", "Thank You Card"),
         QCoreApplication::translate("Templates", "Quarter-fold card with leaves."), [](const TemplateOptions &o) { return greetingCard(o, 1); }, {}},
        {"certificate-achievement", QCoreApplication::translate("Templates", "Award Certificates"), QCoreApplication::translate("Templates", "Certificate of Achievement"),
         QCoreApplication::translate("Templates", "Landscape certificate with a seal."), certificate, {}},
        {"postcard-greetings", QCoreApplication::translate("Templates", "Postcards"), QCoreApplication::translate("Templates", "Greetings Postcard"),
         QCoreApplication::translate("Templates", "Front artwork, back with message and address."), postcard, {"address"}},
        {"sign-closed", QCoreApplication::translate("Templates", "Signs"), QCoreApplication::translate("Templates", "Closed Sign"),
         QCoreApplication::translate("Templates", "Big, readable door sign."), [](const TemplateOptions &o) { return sign(o, 0); }, {}},
        {"sign-welcome", QCoreApplication::translate("Templates", "Signs"), QCoreApplication::translate("Templates", "Welcome Sign"),
         QCoreApplication::translate("Templates", "Friendly open-hours sign."), [](const TemplateOptions &o) { return sign(o, 1); }, {}},
        {"banner-welcome", QCoreApplication::translate("Templates", "Banners"), QCoreApplication::translate("Templates", "Welcome Banner"),
         QCoreApplication::translate("Templates", "5 × 2 ft banner, prints tiled."), banner, {}},
        {"menu-bistro", QCoreApplication::translate("Templates", "Menus"), QCoreApplication::translate("Templates", "Bistro Menu"),
         QCoreApplication::translate("Templates", "Legal-size menu with dot leaders."), menu, {}},
        {"gift-certificate", QCoreApplication::translate("Templates", "Gift Certificates"), QCoreApplication::translate("Templates", "Gift Certificate"),
         QCoreApplication::translate("Templates", "Gradient border and amount badge."), giftCertificate, {}},
        {"invitation-evening", QCoreApplication::translate("Templates", "Invitation Cards"), QCoreApplication::translate("Templates", "Evening Invitation"),
         QCoreApplication::translate("Templates", "5 × 7 invitation."), invitation, {}},
        {"letterhead", QCoreApplication::translate("Templates", "Letterhead"), QCoreApplication::translate("Templates", "Letterhead"),
         QCoreApplication::translate("Templates", "Header, contact block and letter body."), letterhead, {"logo"}},
        {"resume", QCoreApplication::translate("Templates", "Resumes"), QCoreApplication::translate("Templates", "Résumé"),
         QCoreApplication::translate("Templates", "Clean one-page résumé."), resume, {}},
        {"calendar-year", QCoreApplication::translate("Templates", "Calendars"), QCoreApplication::translate("Templates", "Wall Calendar"),
         QCoreApplication::translate("Templates", "Twelve months, picture on each page."), calendarTemplate, {}},
        {"program-recital", QCoreApplication::translate("Templates", "Programs"), QCoreApplication::translate("Templates", "Recital Program"),
         QCoreApplication::translate("Templates", "Four-page booklet that prints folded."), program, {}},
        {"labels-address", QCoreApplication::translate("Templates", "Labels"), QCoreApplication::translate("Templates", "Address Labels"),
         QCoreApplication::translate("Templates", "30 per sheet, ready for mail merge."), labels, {}},
        {"envelope-10", QCoreApplication::translate("Templates", "Envelopes"), QCoreApplication::translate("Templates", "#10 Envelope"),
         QCoreApplication::translate("Templates", "Return address and merge address block."), envelope, {}},
        {"form-invoice", QCoreApplication::translate("Templates", "Business Forms"), QCoreApplication::translate("Templates", "Invoice"),
         QCoreApplication::translate("Templates", "Itemized invoice table."), [](const TemplateOptions &o) { return businessForm(o, 0); }, {"logo"}},
        {"form-po", QCoreApplication::translate("Templates", "Business Forms"), QCoreApplication::translate("Templates", "Purchase Order"),
         QCoreApplication::translate("Templates", "Purchase order table."), [](const TemplateOptions &o) { return businessForm(o, 1); }, {"logo"}},
        {"catalog-fall", QCoreApplication::translate("Templates", "Catalogs"), QCoreApplication::translate("Templates", "Product Catalog"),
         QCoreApplication::translate("Templates", "Cover plus four products per page."), catalog, {"logo"}},
        {"email-newsletter", QCoreApplication::translate("Templates", "Email"), QCoreApplication::translate("Templates", "Email Newsletter"),
         QCoreApplication::translate("Templates", "Tall single page for email."), emailNewsletter, {"logo"}},
        {"compliments", QCoreApplication::translate("Templates", "With Compliments Cards"), QCoreApplication::translate("Templates", "With Compliments"),
         QCoreApplication::translate("Templates", "DL compliments slip."), complimentsCard, {}},
        {"paper-airplane", QCoreApplication::translate("Templates", "Paper Folding Projects"), QCoreApplication::translate("Templates", "Paper Airplane"),
         QCoreApplication::translate("Templates", "Printable fold lines."), paperAirplane, {}},
        {"certificate-appreciation", QCoreApplication::translate("Templates", "Award Certificates"), QCoreApplication::translate("Templates", "Certificate of Appreciation"),
         QCoreApplication::translate("Templates", "Portrait certificate with a ribbon."), certificateAppreciation, {}},
        {"postcard-save-date", QCoreApplication::translate("Templates", "Postcards"), QCoreApplication::translate("Templates", "Save the Date Postcard"),
         QCoreApplication::translate("Templates", "Night-sky front with a date badge."), postcardEvent, {"address"}},
        {"banner-grand-opening", QCoreApplication::translate("Templates", "Banners"), QCoreApplication::translate("Templates", "Grand Opening Banner"),
         QCoreApplication::translate("Templates", "6 × 2 ft banner with striped edges, prints tiled."), bannerGrandOpening, {}},
        {"menu-cafe", QCoreApplication::translate("Templates", "Menus"), QCoreApplication::translate("Templates", "Café Menu"),
         QCoreApplication::translate("Templates", "Chalkboard menu in four columns with dot leaders."), menuCafe, {}},
        {"gift-certificate-classic", QCoreApplication::translate("Templates", "Gift Certificates"), QCoreApplication::translate("Templates", "Classic Gift Certificate"),
         QCoreApplication::translate("Templates", "Triple border and an amount panel."), giftCertificateClassic, {}},
        {"invitation-party", QCoreApplication::translate("Templates", "Invitation Cards"), QCoreApplication::translate("Templates", "Party Invitation"),
         QCoreApplication::translate("Templates", "Confetti birthday invitation."), invitationParty, {}},
        {"letterhead-modern", QCoreApplication::translate("Templates", "Letterhead"), QCoreApplication::translate("Templates", "Modern Letterhead"),
         QCoreApplication::translate("Templates", "Gradient header, contact line at the foot."), letterheadModern, {"logo"}},
        {"resume-sidebar", QCoreApplication::translate("Templates", "Resumes"), QCoreApplication::translate("Templates", "Sidebar Résumé"),
         QCoreApplication::translate("Templates", "Colored sidebar with photo, contact and skills."), resumeSidebar, {}},
        {"calendar-month", QCoreApplication::translate("Templates", "Calendars"), QCoreApplication::translate("Templates", "Monthly Calendar"),
         QCoreApplication::translate("Templates", "One month with a picture, portrait."), calendarMonth, {}},
        {"program-event", QCoreApplication::translate("Templates", "Programs"), QCoreApplication::translate("Templates", "Event Program"),
         QCoreApplication::translate("Templates", "One-page order of events with dot leaders."), programEvent, {}},
        {"labels-shipping", QCoreApplication::translate("Templates", "Labels"), QCoreApplication::translate("Templates", "Shipping Labels"),
         QCoreApplication::translate("Templates", "4 × 2 in, 10 per sheet, ready for mail merge."), labelsShipping, {}},
        {"envelope-a7", QCoreApplication::translate("Templates", "Envelopes"), QCoreApplication::translate("Templates", "A7 Invitation Envelope"),
         QCoreApplication::translate("Templates", "Centered address and a heart."), envelopeA7, {}},
        {"catalog-merge", QCoreApplication::translate("Templates", "Catalogs"), QCoreApplication::translate("Templates", "Merged Product Catalog"),
         QCoreApplication::translate("Templates", "Cover, then a catalog page filled from a product list."), catalogMerged, {"logo"}},
        {"email-promo", QCoreApplication::translate("Templates", "Email"), QCoreApplication::translate("Templates", "Promotion Email"),
         QCoreApplication::translate("Templates", "Sale headline, coupon code and a button."), emailPromo, {}},
        {"compliments-modern", QCoreApplication::translate("Templates", "With Compliments Cards"), QCoreApplication::translate("Templates", "Modern Compliments Slip"),
         QCoreApplication::translate("Templates", "Gradient side panel."), complimentsModern, {"logo"}},
        {"fortune-teller", QCoreApplication::translate("Templates", "Paper Folding Projects"), QCoreApplication::translate("Templates", "Fortune Teller"),
         QCoreApplication::translate("Templates", "Origami fortune teller with colors and numbers."), fortuneTeller, {}},
    });
    return list;
}

QStringList templateCategories()
{
    QStringList c;
    for (const auto &t : templates())
        if (!c.contains(t.category)) c << t.category;
    c.sort();
    return c;
}

const TemplateInfo *findTemplate(const QString &id)
{
    for (const auto &t : templates())
        if (t.id == id) return &t;
    return nullptr;
}

const QVector<BlankSize> &blankSizes()
{
    static const QVector<BlankSize> s = {
        {"Letter", "Standard", {612, 792}}, {"Letter Landscape", "Standard", {792, 612}}, {"Legal", "Standard", {612, 1008}}, {"Tabloid", "Standard", {792, 1224}},
        {"Half Letter", "Standard", {396, 612}}, {"A3", "Standard", {841.89, 1190.55}}, {"A4", "Standard", {595.28, 841.89}}, {"A5", "Standard", {419.53, 595.28}},
        {"Business Card", "Cards", {252, 144}}, {"Postcard", "Cards", {432, 288}}, {"Index Card", "Cards", {360, 216}}, {"Quarter-fold Card", "Cards", {396, 306}},
        {"5 × 7 Card", "Cards", {360, 504}}, {"Square 8 × 8", "Other", {576, 576}}, {"#10 Envelope", "Envelopes", {684, 297}}, {"A2 Envelope", "Envelopes", {414, 315}},
        {"Poster 18 × 24", "Large", {1296, 1728}}, {"Poster 24 × 36", "Large", {1728, 2592}}, {"Banner 5 × 2 ft", "Large", {4320, 1728}},
        {"Social Square", "Web", {810, 810}}, {"Email", "Web", {612, 1080}},
    };
    return s;
}

// ---------------- calendars ----------------
ItemList makeCalendar(Document &doc, const QRectF &area, int year, int month, int style)
{
    ItemList out;
    const QDate first(year, month, 1);
    auto title = std::make_shared<TextItem>();
    title->rect = QRectF(area.left(), area.top(), area.width(), 56);
    title->storyId = doc.createStory();
    {
        QTextCursor c(doc.storyDoc(title->storyId));
        QTextCharFormat cf;
        cf.setFontPointSize(style == 2 ? 22 : 30);
        cf.setProperty(tp::ThemeFont, QStringLiteral("major"));
        cf.setProperty(tp::ColorRefP, ColorRef::scheme(Accent1).toString());
        c.insertText(QLocale().monthName(month) + QStringLiteral(" %1").arg(year), cf);
    }
    out.push_back(title);
    const int lead = first.dayOfWeek() % 7;   // Sunday first
    const int days = first.daysInMonth();
    const int weeks = (lead + days + 6) / 7;
    auto t = std::make_shared<TableItem>();
    t->rows = weeks + 1;
    t->cols = 7;
    t->rect = QRectF(area.left(), area.top() + 62, area.width(), area.height() - 62);
    t->colW = QVector<double>(7, area.width() / 7);
    const double rowH = (area.height() - 62 - 24) / weeks;
    t->rowH = QVector<double>(weeks + 1, rowH);
    t->rowH[0] = 24;
    t->cells.resize(t->rows * 7);
    t->growToFit = false;
    const QStringList names{QCoreApplication::translate("Templates", "Sun"), QCoreApplication::translate("Templates", "Mon"), QCoreApplication::translate("Templates", "Tue"),
                            QCoreApplication::translate("Templates", "Wed"), QCoreApplication::translate("Templates", "Thu"), QCoreApplication::translate("Templates", "Fri"),
                            QCoreApplication::translate("Templates", "Sat")};
    for (int r = 0; r < t->rows; ++r)
        for (int c = 0; c < 7; ++c) {
            QString text;
            if (r == 0) text = names[c];
            else {
                const int day = (r - 1) * 7 + c - lead + 1;
                if (day >= 1 && day <= days) text = QString::number(day);
            }
            t->cell(r, c).storyId = doc.createStory(text);
            if (r > 0) {
                QTextCursor cur(doc.storyDoc(t->cell(r, c).storyId));
                QTextBlockFormat bf;
                bf.setAlignment(Qt::AlignRight);
                cur.mergeBlockFormat(bf);
            }
        }
    t->syncRect();
    const QString fmt = style == 0 ? QStringLiteral("Table Style 1") : style == 1 ? QStringLiteral("Table Style 6") : style == 2 ? QStringLiteral("None") : QStringLiteral("Table Style 11");
    applyTableFormatCells(t.get(), fmt, [&](TableCell &cell, bool head, bool) {
        QTextCursor cur(doc.storyDoc(cell.storyId));
        cur.select(QTextCursor::Document);
        QTextCharFormat f;
        if (head) { f.setProperty(tp::ColorRefP, style == 2 ? ColorRef::scheme(Accent1).toString() : QStringLiteral("#FFFFFF")); f.setFontWeight(QFont::Bold); }
        f.setFontPointSize(head ? 10 : 11);
        cur.mergeCharFormat(f);
        QTextBlockFormat bf;
        bf.setAlignment(head ? Qt::AlignHCenter : Qt::AlignRight);
        cur.mergeBlockFormat(bf);
    });
    if (style == 2)
        for (int r = 0; r < t->rows; ++r)
            for (int c = 0; c < 7; ++c) t->cell(r, c).border.bottom = Stroke::line(ColorRef::scheme(Accent4, 50), 0.5);
    out.push_back(t);
    return out;
}

// ---------------- building blocks ----------------
const QVector<BuildingBlock> &buildingBlocks()
{
    static const QVector<BuildingBlock> list = [] {
        QVector<BuildingBlock> v;
        auto mkText = [](Document &d, const QRectF &r, const QVector<Para> &paras, VAlign va = VAlign::Top) {
            auto t = std::make_shared<TextItem>();
            t->rect = r;
            t->valign = va;
            t->storyId = d.createStory();
            Page tmp;
            B b(d);
            b.pg = &tmp;
            b.fillStory(d.storyDoc(t->storyId), paras);
            return t;
        };
        auto shapeOf = [](const QString &id, const QRectF &r, const Fill &f, const Stroke &s = Stroke::none()) {
            auto x = std::make_shared<ShapeItem>();
            x->shape = id;
            x->rect = r;
            x->fill = f;
            x->stroke = s;
            x->wrap.mode = Wrap::None;
            return x;
        };
        // Headings.
        static const char *const headingNames[] = {QT_TRANSLATE_NOOP("BuildingBlocks", "Accent Bar Heading"), QT_TRANSLATE_NOOP("BuildingBlocks", "Underlined Heading"), QT_TRANSLATE_NOOP("BuildingBlocks", "Boxed Heading"), QT_TRANSLATE_NOOP("BuildingBlocks", "Tab Heading"), QT_TRANSLATE_NOOP("BuildingBlocks", "Centered Rule Heading"), QT_TRANSLATE_NOOP("BuildingBlocks", "Banner Heading")};
        for (int i = 0; i < int(std::size(headingNames)); ++i) {
            v << BuildingBlock{QStringLiteral("heading.%1").arg(i), "Page Parts", QCoreApplication::translate("BuildingBlocks", headingNames[i]), [i, mkText, shapeOf](Document &d, const QRectF &c) {
                ItemList out;
                const QRectF r(c.left(), c.top() + 36, c.width(), 54);
                switch (i) {
                case 0: out.push_back(shapeOf("rect", QRectF(r.left(), r.top(), 8, r.height()), Fill::solid(ColorRef::scheme(Accent1)))); break;
                case 2: out.push_back(shapeOf("rect", r, Fill::solid(ColorRef::scheme(Accent1)))); break;
                case 3: out.push_back(shapeOf("snip2same", QRectF(r.left(), r.top(), r.width() * 0.6, r.height()), Fill::solid(ColorRef::scheme(Accent2)))); break;
                case 5: out.push_back(shapeOf("ribbon2", r.adjusted(-10, -6, 10, 6), Fill::solid(ColorRef::scheme(Accent1)))); break;
                default: break;
                }
                const bool white = i == 2 || i == 3 || i == 5;
                Para hp = H(QCoreApplication::translate("BuildingBlocks", "Heading Text"), 24, i == 4 || i == 5 ? 1 : 0, white ? -1 : Accent1);
                if (white) hp.color = ColorRef::rgb(Qt::white);
                auto t = mkText(d, QRectF(r.left() + (i == 0 ? 16 : 10), r.top(), r.width() - 26, r.height()), {hp}, VAlign::Middle);
                out.push_back(t);
                if (i == 1) {
                    auto l = std::make_shared<LineItem>();
                    l->p1 = QPointF(r.left(), r.bottom()); l->p2 = QPointF(r.right(), r.bottom());
                    l->stroke = Stroke::line(ColorRef::scheme(Accent1), 2); l->syncRect();
                    out.push_back(l);
                }
                if (i == 4) {
                    for (double x : {r.left(), r.right() - r.width() * 0.25}) {
                        auto l = std::make_shared<LineItem>();
                        l->p1 = QPointF(x, r.center().y()); l->p2 = QPointF(x + r.width() * 0.25, r.center().y());
                        l->stroke = Stroke::line(ColorRef::scheme(Accent3), 1.5); l->syncRect();
                        out.push_back(l);
                    }
                }
                return out;
            }};
        }
        // Pull quotes.
        static const char *const quoteNames[] = {QT_TRANSLATE_NOOP("BuildingBlocks", "Quote Box"), QT_TRANSLATE_NOOP("BuildingBlocks", "Quote Rule"), QT_TRANSLATE_NOOP("BuildingBlocks", "Quote Marks"), QT_TRANSLATE_NOOP("BuildingBlocks", "Quote Circle")};
        for (int i = 0; i < int(std::size(quoteNames)); ++i) {
            v << BuildingBlock{QStringLiteral("quote.%1").arg(i), "Page Parts", QCoreApplication::translate("BuildingBlocks", quoteNames[i]), [i, mkText, shapeOf](Document &d, const QRectF &c) {
                ItemList out;
                const QRectF r(c.center().x() - 120, c.center().y() - 80, 240, i == 3 ? 240 : 150);
                if (i == 0) out.push_back(shapeOf("rect", r, Fill::solid(ColorRef::scheme(Accent5)), Stroke::line(ColorRef::scheme(Accent1), 1)));
                if (i == 3) out.push_back(shapeOf("ellipse", r, Fill::solid(ColorRef::scheme(Accent1, 70))));
                if (i == 1) for (double y : {r.top(), r.bottom()}) {
                    auto l = std::make_shared<LineItem>();
                    l->p1 = QPointF(r.left(), y); l->p2 = QPointF(r.right(), y);
                    l->stroke = Stroke::line(ColorRef::scheme(Accent1), 2); l->syncRect();
                    out.push_back(l);
                }
                QVector<Para> paras;
                if (i == 2) paras << C(QStringLiteral("“"), 60, Accent1, 0, true);
                paras << Para{QCoreApplication::translate("BuildingBlocks", "“A pull quote highlights a key sentence from the story.”"), QStringLiteral("Quote"), 0, 1};
                auto t = mkText(d, r.adjusted(16, 16, -16, -16), paras, VAlign::Middle);
                t->wrap.mode = Wrap::Square;
                out.push_back(t);
                return out;
            }};
        }
        // Sidebars.
        static const char *const sideNames[] = {QT_TRANSLATE_NOOP("BuildingBlocks", "Shaded Sidebar"), QT_TRANSLATE_NOOP("BuildingBlocks", "Bordered Sidebar"), QT_TRANSLATE_NOOP("BuildingBlocks", "Tab Sidebar"), QT_TRANSLATE_NOOP("BuildingBlocks", "Checklist Sidebar")};
        for (int i = 0; i < int(std::size(sideNames)); ++i) {
            v << BuildingBlock{QStringLiteral("sidebar.%1").arg(i), "Page Parts", QCoreApplication::translate("BuildingBlocks", sideNames[i]), [i, mkText, shapeOf](Document &d, const QRectF &c) {
                ItemList out;
                const QRectF r(c.right() - 170, c.top() + 60, 170, 300);
                if (i == 0) out.push_back(shapeOf("rect", r, Fill::solid(ColorRef::scheme(Accent5))));
                if (i == 1) out.push_back(shapeOf("rect", r, Fill::none(), Stroke::line(ColorRef::scheme(Accent1), 1.5)));
                if (i == 2) { out.push_back(shapeOf("rect", r, Fill::solid(ColorRef::scheme(Accent5)))); out.push_back(shapeOf("rect", QRectF(r.left(), r.top(), r.width(), 34), Fill::solid(ColorRef::scheme(Accent1)))); }
                if (i == 3) out.push_back(shapeOf("roundRect", r, Fill::solid(ColorRef::scheme(Accent3, 80))));
                QVector<Para> paras;
                Para h = C(QCoreApplication::translate("BuildingBlocks", "Sidebar Heading"), 14, i == 2 ? -1 : Accent1, 0, true, 700);
                if (i == 2) h.color = ColorRef::rgb(Qt::white);
                paras << h;
                if (i == 3) paras << P(QCoreApplication::translate("BuildingBlocks", "✔ First item\n✔ Second item\n✔ Third item"), "Body Text");
                else paras << P(QCoreApplication::translate("BuildingBlocks", "Use a sidebar for short, related information such as dates, contacts or a list of what's inside."), "Body Text");
                auto t = mkText(d, r.adjusted(10, i == 2 ? 6 : 10, -10, -10), paras);
                out.push_back(t);
                return out;
            }};
        }
        // Stories and table of contents.
        v << BuildingBlock{"story.2col", "Page Parts", QCoreApplication::translate("BuildingBlocks", "Two-Column Story"), [mkText](Document &d, const QRectF &c) {
            ItemList out;
            out.push_back(mkText(d, QRectF(c.left(), c.top(), c.width(), 50), {H(QCoreApplication::translate("BuildingBlocks", "Story Headline"), 26, 0, Accent1)}));
            auto t = mkText(d, QRectF(c.left(), c.top() + 56, c.width(), 280), {P(lorem1(), "Body Text"), P(lorem2(), "Body Text"), P(lorem3(), "Body Text")});
            t->columns = 2;
            t->columnGap = 18;
            out.push_back(t);
            return out;
        }};
        v << BuildingBlock{"story.3col", "Page Parts", QCoreApplication::translate("BuildingBlocks", "Three-Column Story"), [mkText](Document &d, const QRectF &c) {
            ItemList out;
            out.push_back(mkText(d, QRectF(c.left(), c.top(), c.width(), 50), {H(QCoreApplication::translate("BuildingBlocks", "Story Headline"), 26, 0, Accent1)}));
            auto t = mkText(d, QRectF(c.left(), c.top() + 56, c.width(), 260), {P(lorem1(), "Body Text"), P(lorem2(), "Body Text"), P(lorem3(), "Body Text"), P(lorem1(), "Body Text")});
            t->columns = 3;
            t->columnGap = 14;
            out.push_back(t);
            return out;
        }};
        v << BuildingBlock{"toc", "Page Parts", QCoreApplication::translate("BuildingBlocks", "Table of Contents"), [mkText](Document &d, const QRectF &c) {
            ItemList out;
            auto t = mkText(d, QRectF(c.left(), c.top() + 40, 220, 160), {C(QCoreApplication::translate("BuildingBlocks", "Inside This Issue"), 14, Accent1, 0, true, 700),
                                                                          P(QCoreApplication::translate("BuildingBlocks", "Lead story\t1\nInside story\t2\nInside story\t2\nCalendar\t3\nBack page story\t4"), "Body Text")});
            QTextCursor cur(d.storyDoc(t->storyId));
            cur.select(QTextCursor::Document);
            QTextBlockFormat bf;
            bf.setTabPositions({QTextOption::Tab(200, QTextOption::RightTab)});
            bf.setProperty(tp::TabLeaders, QStringLiteral("."));
            cur.mergeBlockFormat(bf);
            out.push_back(t);
            return out;
        }};
        // Borders & accents.
        static const char *const borderNames[] = {QT_TRANSLATE_NOOP("BuildingBlocks", "Thick Bar"), QT_TRANSLATE_NOOP("BuildingBlocks", "Double Rule"), QT_TRANSLATE_NOOP("BuildingBlocks", "Corner Accent"), QT_TRANSLATE_NOOP("BuildingBlocks", "Frame"), QT_TRANSLATE_NOOP("BuildingBlocks", "Rounded Frame"), QT_TRANSLATE_NOOP("BuildingBlocks", "Dotted Frame"), QT_TRANSLATE_NOOP("BuildingBlocks", "Wave Accent"), QT_TRANSLATE_NOOP("BuildingBlocks", "Chevron Bar")};
        for (int i = 0; i < int(std::size(borderNames)); ++i) {
            v << BuildingBlock{QStringLiteral("border.%1").arg(i), "Borders & Accents", QCoreApplication::translate("BuildingBlocks", borderNames[i]), [i, shapeOf](Document &d, const QRectF &c) {
                Q_UNUSED(d);
                ItemList out;
                switch (i) {
                case 0: out.push_back(shapeOf("rect", QRectF(c.left(), c.top(), c.width(), 14), Fill::solid(ColorRef::scheme(Accent1)))); break;
                case 1: for (double y : {c.top(), c.top() + 6}) {
                        auto l = std::make_shared<LineItem>();
                        l->p1 = QPointF(c.left(), y); l->p2 = QPointF(c.right(), y);
                        l->stroke = Stroke::line(ColorRef::scheme(Accent1), y == c.top() ? 3 : 1); l->syncRect();
                        out.push_back(l);
                    } break;
                case 2: out.push_back(shapeOf("rtTriangle", QRectF(c.left() - 36, c.top() - 36, 120, 120), Fill::solid(ColorRef::scheme(Accent2)))); break;
                case 3: out.push_back(shapeOf("rect", c.adjusted(-18, -18, 18, 18), Fill::none(), Stroke::line(ColorRef::scheme(Accent1), 4))); break;
                case 4: out.push_back(shapeOf("roundRect", c.adjusted(-18, -18, 18, 18), Fill::none(), Stroke::line(ColorRef::scheme(Accent2), 3))); break;
                case 5: { auto s = shapeOf("rect", c.adjusted(-18, -18, 18, 18), Fill::none(), Stroke::line(ColorRef::scheme(Accent3), 3)); s->stroke.dash = Stroke::RoundDot; s->stroke.cap = Qt::RoundCap; out.push_back(s); break; }
                case 6: out.push_back(shapeOf("wave", QRectF(c.left() - 36, c.bottom() - 30, c.width() + 72, 80), Fill::solid(ColorRef::scheme(Accent1, 40)))); break;
                default: for (int k = 0; k < 6; ++k) out.push_back(shapeOf("chevron", QRectF(c.left() + k * 40, c.top(), 44, 26), Fill::solid(ColorRef::scheme(1 + k % 4)))); break;
                }
                return out;
            }};
        }
        // Advertisements and attention getters.
        static const char *const adNames[] = {QT_TRANSLATE_NOOP("BuildingBlocks", "Coupon"), QT_TRANSLATE_NOOP("BuildingBlocks", "Sale Ad"), QT_TRANSLATE_NOOP("BuildingBlocks", "Event Ad"), QT_TRANSLATE_NOOP("BuildingBlocks", "Starburst"), QT_TRANSLATE_NOOP("BuildingBlocks", "Price Tag"), QT_TRANSLATE_NOOP("BuildingBlocks", "Ribbon Banner"), QT_TRANSLATE_NOOP("BuildingBlocks", "New!")};
        for (int i = 0; i < int(std::size(adNames)); ++i) {
            v << BuildingBlock{QStringLiteral("ad.%1").arg(i), "Advertisements", QCoreApplication::translate("BuildingBlocks", adNames[i]), [i, mkText, shapeOf](Document &d, const QRectF &c) {
                ItemList out;
                const QRectF r(c.center().x() - 140, c.center().y() - 80, 280, 160);
                if (i == 0) {
                    auto s = shapeOf("rect", r, Fill::solid(ColorRef::scheme(Accent5)), Stroke::line(ColorRef::scheme(Main), 1.5));
                    s->stroke.dash = Stroke::DashLine;
                    out.push_back(s);
                    out.push_back(mkText(d, r.adjusted(12, 12, -12, -12), {C(QCoreApplication::translate("BuildingBlocks", "$5 OFF"), 34, Accent1, 1, true, 700), C(QCoreApplication::translate("BuildingBlocks", "any purchase of $25 or more"), 11, Main, 1),
                                                                           C(QCoreApplication::translate("BuildingBlocks", "{biz:name} · Expires June 30"), 8, Accent4, 1)}, VAlign::Middle));
                } else if (i == 1 || i == 2) {
                    out.push_back(shapeOf("rect", r, Fill::solid(ColorRef::scheme(i == 1 ? Accent1 : Accent2))));
                    out.push_back(mkText(d, r.adjusted(12, 12, -12, -12), {W(i == 1 ? QCoreApplication::translate("BuildingBlocks", "SPRING SALE") : QCoreApplication::translate("BuildingBlocks", "OPEN HOUSE"), 26, 1, true, 700),
                                                                           W(i == 1 ? QCoreApplication::translate("BuildingBlocks", "This weekend only") : QCoreApplication::translate("BuildingBlocks", "Saturday 10–4"), 13, 1)}, VAlign::Middle));
                } else {
                    const QString shapeId = i == 3 ? "irregularSeal1" : i == 4 ? "pentagon" : i == 5 ? "ribbon2" : "star8";
                    auto s = shapeOf(shapeId, QRectF(c.center().x() - 80, c.center().y() - 80, 160, i == 5 ? 80 : 160), Fill::solid(ColorRef::scheme(Accent3)));
                    s->storyId = d.createStory();
                    Page tmp;
                    B b(d);
                    b.pg = &tmp;
                    b.fillStory(d.storyDoc(s->storyId), {W(i == 3 ? QCoreApplication::translate("BuildingBlocks", "FREE!") : i == 4 ? QStringLiteral("$19.99") : i == 5 ? QCoreApplication::translate("BuildingBlocks", "Best Seller") : QCoreApplication::translate("BuildingBlocks", "NEW!"), 20, 1, true, 700)});
                    out.push_back(s);
                }
                return out;
            }};
        }
        // Calendars.
        for (int k = 0; k < 4; ++k) {
            v << BuildingBlock{QStringLiteral("calendar.%1").arg(k), "Calendars", k < 2 ? QCoreApplication::translate("BuildingBlocks", "This Month") : QCoreApplication::translate("BuildingBlocks", "Next Month"), [k](Document &d, const QRectF &c) {
                const QDate dt = QDate::currentDate().addMonths(k < 2 ? 0 : 1);
                return makeCalendar(d, QRectF(c.left(), c.top() + c.height() * 0.4, c.width(), c.height() * 0.55), dt.year(), dt.month(), k % 2 ? 2 : 0);
            }};
        }
        return v;
    }();
    return list;
}

const BuildingBlock *findBlock(const QString &id)
{
    for (const auto &b : buildingBlocks())
        if (b.id == id) return &b;
    return nullptr;
}

} // namespace jp
