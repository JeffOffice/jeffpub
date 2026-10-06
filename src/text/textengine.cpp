#include "text/textengine.h"

#include "core/document.h"
#include "core/fonts.h"
#include "text/hyphenation.h"
#include "text/textprops.h"

#include <QFontMetricsF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QtMath>
#include <QGlyphRun>
#include <QPainter>
#include <QRegularExpression>
#include <QRawFont>
#include <QTextDocument>
#include <QTextList>
#include <cmath>

namespace jp {

// ---------- fields ----------
static QString toRoman(int n)
{
    static const QPair<int, const char *> t[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
                                                {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}};
    QString s;
    for (const auto &p : t)
        while (n >= p.first) { s += QLatin1String(p.second); n -= p.first; }
    return s;
}

QString FieldContext::resolve(const QString &code) const
{
    const QString kind = code.section(':', 0, 0);
    const QString arg = code.section(':', 1);
    if (kind == "page") {
        if (arg == "roman") return toRoman(pageNumber).toLower();
        if (arg == "ROMAN") return toRoman(pageNumber);
        if (arg == "alpha") return QString(QChar('a' + (pageNumber - 1) % 26));
        return QString::number(pageNumber);
    }
    if (kind == "pages") return QString::number(pageCount);
    if (kind == "date") return now.date().toString(arg.isEmpty() ? QStringLiteral("MMMM d, yyyy") : arg);
    if (kind == "time") return now.time().toString(arg.isEmpty() ? QStringLiteral("h:mm AP") : arg);
    if (kind == "datetime") return now.toString(arg.isEmpty() ? QStringLiteral("M/d/yyyy h:mm AP") : arg);
    if (kind == "biz") {
        // "address:oneline" joins a multi-line value with commas, for use inside a line.
        const bool oneLine = arg.endsWith(QLatin1String(":oneline"));
        const QString key = oneLine ? arg.chopped(8) : arg;
        if (!doc) return QStringLiteral("[%1]").arg(key);
        QString v = doc->business().field(key);
        if (oneLine) v = v.split(QRegularExpression(QStringLiteral("\\s*\\r?\\n\\s*")), Qt::SkipEmptyParts).join(QStringLiteral(", "));
        return v;
    }
    if (kind == "merge") {
        if (!doc || mergeRecord < 0) return QStringLiteral("«%1»").arg(arg);
        return doc->merge.value(mergeRecord, arg);
    }
    if (kind == "mergeblock") {
        if (!doc || mergeRecord < 0) return arg == "greeting" ? QStringLiteral("«Greeting Line»") : QStringLiteral("«Address Block»");
        auto v = [&](const QStringList &names) {
            for (const auto &n : names)
                for (const auto &f : doc->merge.fields)
                    if (f.compare(n, Qt::CaseInsensitive) == 0) {
                        const QString x = doc->merge.value(mergeRecord, f);
                        if (!x.isEmpty()) return x;
                    }
            return QString();
        };
        const QString first = v({"First Name", "FirstName", "First"}), last = v({"Last Name", "LastName", "Last", "Surname"});
        if (arg == "greeting") {
            const QString who = (first + ' ' + last).trimmed();
            return who.isEmpty() ? QStringLiteral("Dear Friend,") : QStringLiteral("Dear %1,").arg(who);
        }
        QStringList lines;
        const QString name = (first + ' ' + last).trimmed().isEmpty() ? v({"Name", "Full Name"}) : (first + ' ' + last).trimmed();
        if (!name.isEmpty()) lines << name;
        const QString co = v({"Company", "Company Name", "Organization"});
        if (!co.isEmpty()) lines << co;
        const QString a1 = v({"Address", "Address Line 1", "Address 1", "Street"}), a2 = v({"Address Line 2", "Address 2"});
        if (!a1.isEmpty()) lines << a1;
        if (!a2.isEmpty()) lines << a2;
        const QString city = v({"City"}), st = v({"State", "ST", "Province"}), zip = v({"ZIP Code", "ZIP", "Postal Code", "Zip"});
        QString cl = city;
        if (!st.isEmpty()) cl += (cl.isEmpty() ? "" : ", ") + st;
        if (!zip.isEmpty()) cl += (cl.isEmpty() ? "" : " ") + zip;
        if (!cl.isEmpty()) lines << cl;
        const QString country = v({"Country", "Country or Region"});
        if (!country.isEmpty()) lines << country;
        return lines.join(QChar(QChar::LineSeparator));
    }
    if (kind == "conton") return continuedOnPage > 0 ? QStringLiteral("(Continued on page %1)").arg(continuedOnPage) : QString();
    if (kind == "contfrom") return continuedFromPage > 0 ? QStringLiteral("(Continued from page %1)").arg(continuedFromPage) : QString();
    return QStringLiteral("[%1]").arg(code);
}

QString FieldContext::key() const
{
    return QStringLiteral("%1/%2/%3/%4/%5/%6").arg(pageNumber).arg(pageCount).arg(mergeRecord).arg(continuedOnPage).arg(continuedFromPage)
        .arg(now.toString("yyyyMMddhhmm"));
}

QString LayoutEnv::key() const
{
    QString k = fonts.heading + '|' + fonts.body + '|' + QString::number(fontScale, 'f', 4) + (showFieldShading ? "|s" : "") + (keepInks() ? "|k" : "");
    for (int i = 0; i < SlotCount; ++i) k += colors.c[i].name();
    return k;
}

void drawPlainText(QPainter *p, const QPointF &baseline, const QFont &font, const QString &text)
{
    if (text.isEmpty()) return;
    QTextLayout tl(text, font);
    tl.setCacheEnabled(true);
    tl.beginLayout();
    QTextLine line = tl.createLine();
    if (!line.isValid()) { tl.endLayout(); return; }
    line.setLineWidth(1e6);
    line.setPosition(QPointF(0, 0));
    tl.endLayout();
    const auto runs = line.glyphRuns();
    for (const QGlyphRun &g : runs) p->drawGlyphRun(baseline - QPointF(0, line.ascent()), g);
}

// ---------- formats ----------
namespace { static double trackingSpace(const QTextCharFormat &f, const QFont &resolved); }

QTextCharFormat resolveCharFormat(const QTextCharFormat &f, const LayoutEnv &env)
{
    QTextCharFormat r = f;
    if (!f.hasProperty(QTextFormat::FontFamilies)) {
        const QString theme = f.stringProperty(tp::ThemeFont);
        r.setFontFamilies(QStringList{theme == QLatin1String("major") ? env.fonts.heading : env.fonts.body});
    }
    // Imitate condensed fonts that are missing on this computer.
    if (f.hasProperty(QTextFormat::FontFamilies) && !f.hasProperty(QTextFormat::FontStretch)) {
        const QStringList fams = f.fontFamilies().toStringList();
        if (!fams.isEmpty()) {
            const int st = substituteStretch(fams.first());
            if (st != 100) r.setFontStretch(st);
            // "Medium", "Demi", "Black"... in a missing font's name become a real weight.
            const int wt = substituteWeight(fams.first());
            if (wt > 0 && !(f.hasProperty(QTextFormat::FontWeight) && f.fontWeight() >= QFont::Bold))
                r.setFontWeight(wt);
        }
    }
    const double sz = f.hasProperty(QTextFormat::FontPointSize) ? f.fontPointSize() : 11.0;
    r.setFontPointSize(std::max(1.0, sz * env.fontScale) * fontPointFactor());
    // Automatic pair kerning applies from a size up, 14 pt unless the text
    // says otherwise (smaller text keeps the font's plain widths, as .pub
    // files are laid out); text can also turn kerning off.
    {
        const double kernFrom = f.hasProperty(tp::KernAbove) ? f.doubleProperty(tp::KernAbove) : 14.0;
        r.setFontKerning((!f.hasProperty(QTextFormat::FontKerning) || f.fontKerning()) && sz >= kernFrom);
    }
    // Unhinted design widths: hinting differs between Windows and Linux and
    // would move line breaks; publications must lay out the same everywhere.
    r.setFontHintingPreference(QFont::PreferNoHinting);
    // Kerning and tracking both become space after each letter.
    {
        const double kern = tp::kerningOf(f) * env.fontScale, track = trackingSpace(f, r.font());
        if (kern != 0 || track != 0 || f.hasProperty(QTextFormat::FontLetterSpacing)) {
            r.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            r.setFontLetterSpacing(kern + track);
        }
    }
    const QString cref = f.stringProperty(tp::ColorRefP);
    if (!cref.isEmpty()) r.setForeground(ColorRef::fromString(cref).resolve(env.colors));
    else if (!f.hasProperty(QTextFormat::ForegroundBrush)) r.setForeground(env.colors.slot(Main));
    // A gradient text fill, spread over each run of text as it is drawn.
    const QString tf = f.stringProperty(tp::TextFill);
    if (!tf.isEmpty()) {
        const Fill fill = Fill::fromJson(QJsonDocument::fromJson(tf.toUtf8()).object());
        if (fill.type == Fill::Gradient) {
            const double a = qDegreesToRadians(fill.angle);
            const QPointF dir(std::cos(a) / 2, std::sin(a) / 2);
            QLinearGradient g(QPointF(0.5, 0.5) - dir, QPointF(0.5, 0.5) + dir);
            g.setCoordinateMode(QGradient::ObjectBoundingMode);
            if (fill.stops.isEmpty()) {
                g.setColorAt(0, fill.color.resolve(env.colors));
                g.setColorAt(1, fill.color2.resolve(env.colors));
            } else {
                for (const GradientStop &st : fill.stops) g.setColorAt(st.pos, st.color.resolve(env.colors));
            }
            r.setForeground(QBrush(g));
        }
    }
    if (f.isAnchor() && cref.isEmpty()) {
        r.setForeground(env.colors.slot(Hyperlink));
        r.setFontUnderline(true);
    }
    const QString href = f.stringProperty(tp::HighlightRefP);
    if (!href.isEmpty()) r.setBackground(ColorRef::fromString(href).resolve(env.colors));
    const QString oref = f.stringProperty(tp::OutlineRef);
    if (!oref.isEmpty()) {
        QPen pen(ColorRef::fromString(oref).resolve(env.colors), f.hasProperty(tp::OutlineWidth) ? f.doubleProperty(tp::OutlineWidth) : 0.5);
        pen.setJoinStyle(Qt::RoundJoin);
        r.setTextOutline(pen);
    }
    if (f.boolProperty(tp::TrueSmallCaps)) r.setFontCapitalization(QFont::SmallCaps);
    return r;
}

QFont baseFontFor(const QTextBlock &b, const LayoutEnv &env)
{
    QTextCharFormat cf = b.charFormat();
    // Prefer the first fragment's format so empty-paragraph height matches its text.
    auto it = b.begin();
    if (!it.atEnd()) cf = it.fragment().charFormat();
    QFont f = resolveCharFormat(cf, env).font();
    // Paragraph-level OpenType features (Qt 6.8 applies features per layout font).
    const int num = cf.intProperty(tp::NumberStyle), sp = cf.intProperty(tp::NumberSpacing), lig = cf.intProperty(tp::Ligatures);
    if (num == 1) f.setFeature(QFont::Tag("lnum"), 1);
    if (num == 2) f.setFeature(QFont::Tag("onum"), 1);
    if (sp == 1) f.setFeature(QFont::Tag("pnum"), 1);
    if (sp == 2) f.setFeature(QFont::Tag("tnum"), 1);
    if (lig == 1) { f.setFeature(QFont::Tag("liga"), 0); f.setFeature(QFont::Tag("clig"), 0); }
    if (lig == 2) { f.setFeature(QFont::Tag("dlig"), 1); f.setFeature(QFont::Tag("hlig"), 1); }
    const int ss = cf.intProperty(tp::StylisticSet);
    if (ss > 0 && ss <= 20) f.setFeature(QFont::Tag::fromString(QStringLiteral("ss%1").arg(ss, 2, 10, QChar('0'))).value_or(QFont::Tag()), 1);
    if (cf.boolProperty(tp::Swash)) f.setFeature(QFont::Tag("swsh"), 1);
    if (cf.boolProperty(tp::Alternates)) f.setFeature(QFont::Tag("salt"), 1);
    if (cf.boolProperty(tp::TrueSmallCaps)) f.setFeature(QFont::Tag("smcp"), 1);
    return f;
}

// ---------- block mapping ----------
int StoryLayout::Block::dispFromDoc(int rel) const
{
    for (const Seg &s : map) {
        if (rel < s.docPos) return s.dispPos;
        if (rel < s.docPos + s.docLen) return s.docLen == s.dispLen ? s.dispPos + (rel - s.docPos) : s.dispPos;
    }
    return disp.size();
}

int StoryLayout::Block::docFromDisp(int d) const
{
    for (const Seg &s : map) {
        if (s.dispLen == 0) continue;
        if (d < s.dispPos) return s.docPos;
        if (d < s.dispPos + s.dispLen) {
            if (s.docLen == s.dispLen) return s.docPos + (d - s.dispPos);
            return d - s.dispPos > s.dispLen / 2 ? s.docPos + s.docLen : s.docPos;
        }
    }
    return docLen;
}

// ---------- layout ----------
StoryLayout::StoryLayout() = default;
StoryLayout::~StoryLayout() = default;

namespace {
struct Iv { double x0, x1; };

QVector<Iv> freeIntervals(const QVector<QPolygonF> &obstacles, double x0, double x1, double y0, double y1)
{
    QVector<Iv> res{{x0, x1}};
    for (const QPolygonF &poly : obstacles) {
        const QRectF pb = poly.boundingRect();
        if (pb.bottom() <= y0 || pb.top() >= y1 || pb.right() <= x0 || pb.left() >= x1) continue;
        double bl = pb.left(), br = pb.right();
        if (poly.size() > 4 || !poly.isClosed() || true) {
            const QPolygonF clip = poly.intersected(QPolygonF(QRectF(pb.left() - 1, y0, pb.width() + 2, y1 - y0)));
            if (clip.isEmpty()) continue;
            const QRectF cb = clip.boundingRect();
            if (cb.width() <= 0 && cb.height() <= 0) continue;
            bl = cb.left(); br = cb.right();
        }
        QVector<Iv> next;
        for (const Iv &iv : res) {
            if (br <= iv.x0 || bl >= iv.x1) { next << iv; continue; }
            if (bl > iv.x0) next << Iv{iv.x0, bl};
            if (br < iv.x1) next << Iv{br, iv.x1};
        }
        res = next;
    }
    return res;
}

// A font's single line height in ems, as .pub layouts compute it. Measured
// against reference PDFs of .pub files: for the common system fonts it uses the OS/2
// typographic metrics (typoAscender - typoDescender + typoLineGap); for every
// other font the hhea metrics (ascender - descender + lineGap). Metric-compatible
// substitutes (Arimo for Arial, Tinos for Times New Roman...) follow the font
// that was asked for, which is what the publication was laid out with.
static bool usesTypoMetrics(const QString &family)
{
    static const QStringList core = {
        "Arial", "Arial Black", "Arial Narrow", "Times New Roman", "Georgia", "Verdana", "Tahoma", "Trebuchet MS",
        "Courier New", "Calibri", "Cambria", "Candara", "Consolas", "Constantia", "Corbel", "Segoe UI", "Garamond",
        "Book Antiqua", "Bookman Old Style", "Century Gothic", "Franklin Gothic Medium", "Gill Sans MT", "Palatino Linotype",
        "Lucida Sans Unicode", "Comic Sans MS", "Impact"};
    return core.contains(family, Qt::CaseInsensitive);
}

// Line height and descent (ems) of common proprietary fonts, read from the
// fonts embedded in reference PDFs of .pub files. When such a font is missing and a
// substitute is drawn, lines keep the original's spacing and baseline.
struct KnownMetrics { double line = 0, descent = 0; };
static KnownMetrics knownMetrics(const QString &family)
{
    const QString f = family.toLower();
    // OS/2 typographic metrics of the core fonts, which their stand-ins
    // (Arimo, Tinos...) don't share: theirs give 1.15 em lines.
    if (f == "arial") return {(1491.0 + 431 + 307) / 2048, 431.0 / 2048};
    if (f == "arial narrow") return {(1491.0 + 431 + 269) / 2048, 431.0 / 2048};
    if (f == "times new roman") return {(1420.0 + 442 + 307) / 2048, 442.0 / 2048};
    if (f == "tahoma") return {(1566.0 + 423 + 59) / 2048, 423.0 / 2048};
    if (f == "garamond") return {(1339.0 + 539 + 313) / 2048, 539.0 / 2048};
    if (f == "book antiqua") return {(1489.0 + 578 + 124) / 2048, 578.0 / 2048};
    if (f == "bookman old style") return {(1467.0 + 461 + 263) / 2048, 461.0 / 2048};
    if (f == "century gothic") return {(1536.0 + 426 + 229) / 2048, 426.0 / 2048};
    if (f.startsWith("franklin gothic")) return {(1877.0 + 445) / 2048, 445.0 / 2048};   // hhea
    if (f == "georgia") return {(1549.0 + 444 + 198) / 2048, 444.0 / 2048};              // OS/2 typo
    if (f == "arial black") return {(1466.0 + 434 + 291) / 2048, 434.0 / 2048};
    if (f.startsWith("gill sans mt")) return {(1903.0 + 472) / 2048, 472.0 / 2048};
    if (f == "century schoolbook") return {(2019.0 + 443) / 2048, 443.0 / 2048};
    if (f == "calibri") return {(1536.0 + 512 + 452) / 2048, 512.0 / 2048};
    if (f == "lucida handwriting") return {(2098.0 + 727) / 2048, 727.0 / 2048};
    if (f == "juice itc") return {(1903.0 + 532) / 2048, 532.0 / 2048};
    if (f == "symbol") return {(2059.0 + 450) / 2048, 450.0 / 2048};                    // hhea
    if (f == "ag_futura") return {(4051.0 + 1081) / 4096, 1081.0 / 4096};               // win
    return {};
}
static bool isSubstituted(const QString &family) { return !substituteFor(family).isEmpty() || substituteStretch(family) != 100; }
static QString requestedFamily(const QFont &f) { return f.families().isEmpty() ? f.family() : f.families().first(); }

// A font's average character width in ems (OS/2 xAvgCharWidth), the unit of
// .pub tracking. Substituted fonts use the original's, read from fonts
// embedded in reference PDFs.
static double averageCharEm(const QFont &f, const QString &requested)
{
    if (isSubstituted(requested)) {
        const QString fam = requested.toLower();
        const bool bold = f.weight() >= QFont::DemiBold, italic = f.italic();
        if (fam == "arial") return (bold ? 980.0 : 904.0) / 2048;
        if (fam == "times new roman") return (bold ? (italic ? 844.0 : 874.0) : (italic ? 823.0 : 821.0)) / 2048;
        if (fam == "century schoolbook") return (bold ? (italic ? 1054.0 : 1073.0) : 951.0) / 2048;
        if (fam == "gill sans mt") return (bold ? 956.0 : italic ? 769.0 : 834.0) / 2048;
    }
    static QHash<QString, double> cache;
    const QString key = f.key();
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    double em = 0.5;
    const QRawFont raw = QRawFont::fromFont(f);
    const QByteArray head = raw.fontTable("head"), os2 = raw.fontTable("OS/2");
    if (head.size() >= 20 && os2.size() >= 4) {
        const int upm = (uchar(head[18]) << 8) | uchar(head[19]);
        const int avg = qint16((uchar(os2[2]) << 8) | uchar(os2[3]));
        if (upm > 0 && avg > 0) em = double(avg) / upm;
    }
    if (!(em > 0.1 && em < 2.0)) em = 0.5;
    cache.insert(key, em);
    return em;
}

// The space (layout points) tracking adds after each letter: the font's
// average character width times the tracking beyond 100%. Measured against
// reference PDFs of .pub files at 87.5%, 112.5% and 115%.
static double trackingSpace(const QTextCharFormat &f, const QFont &resolved)
{
    const double t = tp::trackingOf(f);
    if (std::abs(t - 100) < 0.01) return 0;
    return (t - 100) / 100 * averageCharEm(resolved, requestedFamily(resolved)) * resolved.pointSizeF() / fontPointFactor();
}

double naturalLineEm(const QFont &f, const QString &requestedFamily)
{
    if (isSubstituted(requestedFamily)) {
        const double known = knownMetrics(requestedFamily).line;
        if (known > 0) return known;
    }
    static QHash<QString, double> cache;
    const bool typo = usesTypoMetrics(requestedFamily);
    const QString key = f.key() + (typo ? QStringLiteral("|t") : QStringLiteral("|h"));
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    double em = 1.15;
    const QRawFont raw = QRawFont::fromFont(f);
    auto be16 = [](const QByteArray &t, int off) { return off + 2 <= t.size() ? qint16((uchar(t[off]) << 8) | uchar(t[off + 1])) : qint16(0); };
    const QByteArray head = raw.fontTable("head"), os2 = raw.fontTable("OS/2"), hhea = raw.fontTable("hhea");
    const int upm = head.size() >= 20 ? quint16(be16(head, 18)) : 0;
    if (upm > 0) {
        if (typo && os2.size() >= 74) em = double(be16(os2, 68) - be16(os2, 70) + be16(os2, 72)) / upm;
        else if (hhea.size() >= 10) em = double(be16(hhea, 4) - be16(hhea, 6) + be16(hhea, 8)) / upm;
        else if (os2.size() >= 74) em = double(be16(os2, 68) - be16(os2, 70) + be16(os2, 72)) / upm;
    }
    if (!(em > 0.6 && em < 3.0)) em = 1.15;
    cache.insert(key, em);
    return em;
}

// Single spacing in document points: the largest run's line height on the line.
double singleSpacing(const QVector<QTextLayout::FormatRange> &ranges, int from, int len, const QFont &fallback)
{
    auto lineOf = [](const QFont &f) {
        const QString fam = f.families().isEmpty() ? f.family() : f.families().first();
        return f.pointSizeF() / fontPointFactor() * naturalLineEm(f, fam);
    };
    double m = 0;
    for (const auto &r : ranges)
        if (r.start < from + len && r.start + r.length > from) m = std::max(m, lineOf(r.format.font()));
    if (m <= 0) m = lineOf(fallback);
    return m;
}

// Descent (document points) of the substituted proprietary fonts on a line, or 0
// when none is known; the baseline sits this far above the line's bottom.
double knownDescent(const QVector<QTextLayout::FormatRange> &ranges, int from, int len)
{
    double d = 0;
    for (const auto &r : ranges)
        if (r.start < from + len && r.start + r.length > from) {
            const QFont f = r.format.font();
            const QString fam = requestedFamily(f);
            if (!isSubstituted(fam)) continue;
            const double kd = knownMetrics(fam).descent;
            if (kd > 0) d = std::max(d, f.pointSizeF() / fontPointFactor() * kd);
        }
    return d;
}

// Line spacing in "spaces" (sp) scales the single spacing; points are absolute.
double lineHeightFor(const QTextBlockFormat &bf, double scale, double single)
{
    switch (bf.lineHeightType()) {
    case QTextBlockFormat::ProportionalHeight: return single * bf.lineHeight() / 100.0;
    case QTextBlockFormat::FixedHeight: return bf.lineHeight() * scale;
    case QTextBlockFormat::MinimumHeight: return std::max(single, bf.lineHeight() * scale);
    case QTextBlockFormat::LineDistanceHeight: return single + bf.lineHeight() * scale;
    default: return single;
    }
}
} // namespace

void StoryLayout::build(const QTextDocument *doc, const QVector<FrameSpec> &frames, const LayoutEnv &env)
{
    m_blocks.clear();
    m_frames = frames;
    m_env = env;
    m_overflow = false;
    m_used = QVector<double>(frames.size(), 0.0);
    const double scale = env.fontScale;
    const int nF = frames.size();

    auto colRect = [&](int f, int c) {
        const FrameSpec &fs = frames[f];
        const QRectF content(fs.insets.left(), fs.insets.top(), std::max(1.0, fs.size.width() - fs.insets.left() - fs.insets.right()),
                             std::max(1.0, fs.size.height() - fs.insets.top() - fs.insets.bottom()));
        const int n = std::max(1, fs.columns);
        const double cw = std::max(1.0, (content.width() - fs.gap * (n - 1)) / n);
        return QRectF(content.left() + c * (cw + fs.gap), content.top(), cw, content.height());
    };

    int f = 0, c = 0;
    double y = 0;              // relative to column top
    double overflowY = 0;
    QVector<Iv> row;
    int rowIdx = 0;
    double rowH = 0;
    bool rowActive = false;

    bool columnEmpty = true;   // no line placed yet in the current column
    const bool hyphenate = !frames.isEmpty() && frames.first().hyphenate;
    const double zone = (frames.isEmpty() ? 18.0 : frames.first().hyphenZone) * env.fontScale;
    auto advance = [&]() {
        rowActive = false;
        columnEmpty = true;
        y = 0;
        if (f < nF && ++c >= std::max(1, frames[f].columns)) { c = 0; ++f; }
    };

    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        auto B = std::make_unique<Block>();
        B->docStart = b.position();
        B->docLen = b.length() - 1;
        const QTextBlockFormat bf = b.blockFormat();
        const FieldContext &ctx = frames.isEmpty() ? FieldContext() : frames[std::min(f, nF - 1)].ctx;

        // Build the display string, mapping fields and the drop cap.
        const int dropLines = bf.intProperty(tp::DropCapLines);
        int dropRemaining = dropLines > 0 ? std::max(1, bf.intProperty(tp::DropCapChars)) : 0;
        QVector<QTextLayout::FormatRange> ranges;
        QTextCharFormat dropFmt;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            const int rel = frag.position() - b.position();
            const QString text = frag.text();
            const QTextCharFormat cf = frag.charFormat();
            QTextCharFormat rf = resolveCharFormat(cf, env);
            const QString field = cf.stringProperty(tp::Field);
            if (!field.isEmpty()) {
                for (int i = 0; i < text.size(); ++i) {
                    // Multi-line values (an address) break lines inside the paragraph.
                    QString v = ctx.resolve(field);
                    v.replace(QLatin1String("\r\n"), QString(QChar::LineSeparator)).replace(QLatin1Char('\n'), QChar::LineSeparator);
                    B->map << Seg{rel + i, 1, int(B->disp.size()), int(v.size())};
                    ranges << QTextLayout::FormatRange{int(B->disp.size()), int(v.size()), rf};
                    B->fieldRanges << qMakePair(int(B->disp.size()), int(v.size()));
                    B->disp += v;
                }
                continue;
            }
            int start = 0;
            if (dropRemaining > 0) {
                const int k = std::min<int>(dropRemaining, text.size());
                if (B->dropText.isEmpty()) dropFmt = rf;
                B->dropText += text.left(k);
                B->map << Seg{rel, k, int(B->disp.size()), 0};
                dropRemaining -= k;
                start = k;
            }
            if (start < text.size()) {
                const QString raw = text.mid(start);
                QString shown;
                if (hyphenate) {
                    // Automatic hyphenation: soft hyphens (shown only where a line
                    // breaks) at the allowed points of words of five letters or more.
                    static const QRegularExpression wordRe(QStringLiteral("\\p{L}{5,}"));
                    int piece = 0;
                    auto it = wordRe.globalMatch(raw);
                    while (it.hasNext()) {
                        const auto m = it.next();
                        const QString w = m.captured();
                        if (w == w.toUpper()) continue;   // leave all-caps words whole
                        if (!hyphenationKnows(w)) continue;   // names and coined words stay whole
                        for (int pt : hyphenationPoints(w)) {
                            const int cut = int(m.capturedStart()) + pt;
                            const int n = cut - piece;
                            B->map << Seg{rel + start + piece, n, int(B->disp.size() + shown.size()), n};
                            shown += raw.mid(piece, n);
                            B->map << Seg{rel + start + cut, 0, int(B->disp.size() + shown.size()), 1};
                            shown += QChar(0x00AD);
                            piece = cut;
                        }
                    }
                    const int n = int(raw.size()) - piece;
                    B->map << Seg{rel + start + piece, n, int(B->disp.size() + shown.size()), n};
                    shown += raw.mid(piece);
                } else {
                    shown = raw;
                    B->map << Seg{rel + start, int(raw.size()), int(B->disp.size()), int(raw.size())};
                }
                // All capitals (or all lowercase): Qt ignores these in a
                // layout's format ranges, so the letters are changed here, one
                // for one so positions still map.
                if (rf.fontCapitalization() == QFont::AllUppercase || rf.fontCapitalization() == QFont::AllLowercase) {
                    const bool up = rf.fontCapitalization() == QFont::AllUppercase;
                    for (QChar &ch : shown) ch = up ? ch.toUpper() : ch.toLower();
                    rf.setFontCapitalization(QFont::MixedCase);
                }
                const int len = int(shown.size());
                QTextLayout::FormatRange fr{int(B->disp.size()), len, rf};
                if (rf.fontCapitalization() == QFont::SmallCaps && !cf.boolProperty(tp::TrueSmallCaps)) {
                    // .pub small capitals: lowercase letters become capitals at
                    // 0.8 x the size (Qt would use 0.7). Display text keeps its length,
                    // so positions still map one to one.
                    QTextCharFormat big = rf, small = rf;
                    big.setFontCapitalization(QFont::MixedCase);
                    small.setFontCapitalization(QFont::MixedCase);
                    small.setFontPointSize(rf.fontPointSize() * 0.8);
                    // Tracking follows the smaller size.
                    if (const double tr = trackingSpace(cf, rf.font())) small.setFontLetterSpacing(rf.fontLetterSpacing() - 0.2 * tr);
                    int runStart = 0;
                    auto isSmall = [&](int i) { return shown[i].isLower(); };
                    for (int i = 1; i <= shown.size(); ++i)
                        if (i == shown.size() || isSmall(i) != isSmall(runStart)) {
                            ranges << QTextLayout::FormatRange{int(B->disp.size()) + runStart, i - runStart, isSmall(runStart) ? small : big};
                            runStart = i;
                        }
                    for (QChar &ch : shown)
                        if (ch.isLower()) ch = ch.toUpper();
                } else {
                    ranges << fr;
                }
                if (cf.boolProperty(tp::Shadow) || cf.boolProperty(tp::Emboss) || cf.boolProperty(tp::Engrave) ||
                    !cf.stringProperty(tp::GlowRef).isEmpty()) {
                    QTextLayout::FormatRange er = fr;
                    er.format = cf;   // keep raw props for the effect pass
                    er.format.setForeground(rf.foreground());
                    er.format.setFontPointSize(rf.fontPointSize());
                    B->effects << er;
                }
                B->disp += shown;
            }
        }
        // Qt draws U+2028 as a line break; keep it.
        const QFont base = baseFontFor(b, env);
        B->tl = std::make_unique<QTextLayout>(B->disp, base);
        B->tl->setFormats(ranges);
        QTextOption opt;
        Qt::Alignment al = bf.alignment() & Qt::AlignHorizontal_Mask;
        if (!al) al = Qt::AlignLeft;
        opt.setAlignment(al);
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        QList<QTextOption::Tab> tabs = bf.tabPositions();
        for (auto &t : tabs) t.position *= scale;
        opt.setTabs(tabs);
        opt.setTabStopDistance(36 * scale);
        B->leaders = bf.stringProperty(tp::TabLeaders);
        if (bf.layoutDirection() == Qt::RightToLeft) opt.setTextDirection(Qt::RightToLeft);
        opt.setFlags(QTextOption::IncludeTrailingSpaces);
        B->tl->setTextOption(opt);
        B->tl->setCacheEnabled(true);

        const double leftM = bf.leftMargin() * scale, rightM = bf.rightMargin() * scale, textIndent = bf.textIndent() * scale;
        const double before = bf.topMargin() * scale, after = bf.bottomMargin() * scale;

        // List marker.
        double markerW = 0, markerSingle = 0;
        if (QTextList *list = b.textList()) {
            const QTextListFormat lf = list->format();
            QString custom = bf.stringProperty(tp::BulletChar);
            if (custom.isEmpty()) custom = lf.stringProperty(tp::BulletChar);
            if (!custom.isEmpty()) B->marker = custom;
            else if (lf.style() == QTextListFormat::ListDisc) B->marker = QStringLiteral("•");
            else if (lf.style() == QTextListFormat::ListCircle) B->marker = QStringLiteral("◦");
            else if (lf.style() == QTextListFormat::ListSquare) B->marker = QStringLiteral("▪");
            else B->marker = list->itemText(b);
            QTextCharFormat mf = ranges.isEmpty() ? resolveCharFormat(b.charFormat(), env) : ranges.first().format;
            B->markerFont = mf.font();
            if (lf.hasProperty(tp::BulletSize))
                B->markerFont.setPointSizeF(std::max(1.0, lf.property(tp::BulletSize).toDouble() * env.fontScale) * fontPointFactor());
            const QString bfont = lf.stringProperty(tp::BulletFont);
            if (!bfont.isEmpty() && !custom.isEmpty()) B->markerFont.setFamily(bfont);
            B->markerColor = mf.foreground().color();
            const QString bc = lf.stringProperty(tp::BulletColor);
            if (!bc.isEmpty()) B->markerColor = ColorRef::fromString(bc).resolve(env.colors);
            markerW = QFontMetricsF(B->markerFont).horizontalAdvance(B->marker) + 4 * scale;
            // The bullet's own line height counts on the item's first line: a
            // Symbol bullet (as .pub lists have) is taller than most text.
            const QString mfam = bfont.isEmpty() ? requestedFamily(B->markerFont) : bfont;
            const double known = knownMetrics(mfam).line;
            markerSingle = B->markerFont.pointSizeF() / fontPointFactor() * (known > 0 ? known : naturalLineEm(B->markerFont, mfam));
        }

        // Drop cap metrics: cap height spans dropLines lines.
        if (!B->dropText.isEmpty()) {
            B->dropLines = dropLines;
            B->dropUp = bf.boolProperty(tp::DropCapUpper);
            QFont df = dropFmt.font();
            const QString dfam = bf.stringProperty(tp::DropCapFont);
            if (!dfam.isEmpty()) df.setFamily(dfam);
            const QFontMetricsF bm(base);
            const double lineH = bm.height() * (bf.lineHeightType() == QTextBlockFormat::ProportionalHeight ? bf.lineHeight() / 100.0 : 1.0);
            const double target = (dropLines - 1) * lineH + bm.capHeight();
            QFont probe = df;
            probe.setPointSizeF(100);
            const double cap100 = std::max(1.0, QFontMetricsF(probe).capHeight());
            df.setPointSizeF(std::max(4.0, target * 100.0 / cap100));
            B->dropFont = df;
            B->dropColor = dropFmt.foreground().color();
            const QString dc = bf.stringProperty(tp::DropCapColor);
            if (!dc.isEmpty()) B->dropColor = ColorRef::fromString(dc).resolve(env.colors);
            B->dropWidth = QFontMetricsF(df).horizontalAdvance(B->dropText) + 3 * scale;
        }

        // Placing the paragraph's lines can be redone from the same spot:
        // starting in the next column, or breaking a line early.
        struct Spot { int f, c; double y, overflowY, rowH; QVector<Iv> row; int rowIdx; bool rowActive, columnEmpty, overflow; QVector<double> used; };
        const Spot startSpot{f, c, y, overflowY, rowH, row, rowIdx, rowActive, columnEmpty, m_overflow, m_used};
        auto placeLines = [&](bool startNext, int breakAfter) {
            // Start in next text box.
            if (bf.boolProperty(tp::StartInNextBox) && f < nF && (y > 0 || c > 0)) {
                rowActive = false;
                y = 0; c = 0; ++f;
            }
            if (startNext && f < nF) advance();
            if (f < nF && y > 0) y += before;
            else if (f < nF) y += before;  // space before also applies at the top of a frame
            // A raised cap rises above the first line: room for it.
            if (f < nF && B->dropUp && !B->dropText.isEmpty())
                y += std::max(0.0, QFontMetricsF(B->dropFont).capHeight() - QFontMetricsF(base).capHeight());

            // Estimate with the same rule the placed line will use, so wrap checks
            // see the obstacles the real line will meet.
            const double estH = std::max(1.0, lineHeightFor(bf, scale, singleSpacing({}, 0, 0, base)));
            B->tl->beginLayout();
            int lineNo = 0;
            while (true) {
                QTextLine line = B->tl->createLine();
                if (!line.isValid()) break;
                double indL = leftM, indR = rightM;
                if (lineNo == 0) {
                    if (!B->marker.isEmpty()) indL = textIndent < 0 ? leftM : leftM + textIndent + markerW;
                    else indL = leftM + textIndent;
                }
                if (B->dropLines > 0 && lineNo < (B->dropUp ? 1 : B->dropLines)) indL += B->dropWidth;
                if (lineNo == breakAfter && f < nF && !columnEmpty) advance();   // widow control: carry this line over

                bool placed = false;
                for (int guard = 0; guard < 2000 && !placed; ++guard) {
                    if (f >= nF) {
                        line.setLineWidth(nF ? std::max(10.0, colRect(nF - 1, 0).width() - indL - indR) : 300);
                        line.setPosition(QPointF(indL, nF * kStride + overflowY));
                        overflowY += line.height();
                        B->lines << Line{-1, 0, QRectF(indL, overflowY, line.naturalTextWidth(), line.height())};
                        // Blank paragraphs past the end don't count as overflow
                        // (as in .pub layouts): a box doesn't shrink or warn for them.
                        if (!B->disp.trimmed().isEmpty()) m_overflow = true;
                        placed = true;
                        break;
                    }
                    const QRectF col = colRect(f, c);
                    if (!rowActive) {
                        const double x0 = col.left() + indL, x1 = col.right() - indR;
                        QVector<Iv> ivs = freeIntervals(frames[f].obstacles, x0, x1, col.top() + y, col.top() + y + estH);
                        QVector<Iv> ok;
                        const double minW = std::max(18.0, base.pointSizeF() / fontPointFactor() * 2.5);
                        for (const Iv &iv : ivs)
                            if (iv.x1 - iv.x0 >= std::min(minW, x1 - x0)) ok << iv;
                        if (ok.isEmpty()) {
                            y += 2;
                            if (y + estH > col.height() + 0.01) advance();
                            continue;
                        }
                        row = ok; rowIdx = 0; rowH = 0; rowActive = true;
                    }
                    const Iv iv = row[rowIdx];
                    line.setLineWidth(std::max(1.0, iv.x1 - iv.x0));
                    // Hyphenation zone: a word is broken only if moving it whole to
                    // the next line would leave more than the zone empty here.
                    if (hyphenate && zone > 0) {
                        const int s0 = line.textStart(), n = line.textLength();
                        if (n > 1 && B->disp[s0 + n - 1] == QChar(0x00AD)) {
                            int k = s0 + n - 1;
                            while (k > s0 && !B->disp[k - 1].isSpace()) --k;
                            // The width is given again: without it Qt treats the line
                            // as endless, and justified text spreads off the page.
                            if (k > s0 && (iv.x1 - iv.x0) - (line.cursorToX(k) - line.cursorToX(s0)) < zone) line.setNumColumns(k - s0, std::max(1.0, iv.x1 - iv.x0));
                        }
                    }
                    double single = singleSpacing(ranges, line.textStart(), std::max(1, line.textLength()), base);
                    if (line.textStart() == 0) single = std::max(single, markerSingle);
                    double h = lineHeightFor(bf, scale, single);
                    // .pub layouts add extra spacing between lines, never above the
                    // first line of a column: its baseline sits one ascent below the top.
                    if (columnEmpty && h > single && bf.lineHeightType() == QTextBlockFormat::ProportionalHeight) h = single;
                    const bool firstInColumn = (y <= 0.001) && rowIdx == 0;
                    if (col.top() + y + h > col.bottom() + 0.01 && !firstInColumn) {
                        advance();
                        continue;
                    }
                    // Re-check wrap for a line taller than estimated.
                    if (h > estH * 1.05 && !frames[f].obstacles.isEmpty() && rowIdx == 0) {
                        const QVector<Iv> recheck = freeIntervals(frames[f].obstacles, iv.x0, iv.x1, col.top() + y, col.top() + y + h);
                        if (recheck.size() != 1 || recheck[0].x0 > iv.x0 + 0.5 || recheck[0].x1 < iv.x1 - 0.5) {
                            y += 2;
                            rowActive = false;
                            if (y + h > col.height() + 0.01) advance();
                            continue;
                        }
                    }
                    // Baseline one descent above the line's bottom; for a substituted
                    // proprietary font, the original font's descent.
                    const double kd = knownDescent(ranges, line.textStart(), std::max(1, line.textLength()));
                    const double lead = kd > 0 ? h - kd - line.ascent() : h - line.height();
                    // Align to baseline guides: the baseline moves down onto the next guide.
                    if (bf.boolProperty(tp::AlignToBaseline) && frames[f].baselineGrid > 0.5) {
                        const double grid = frames[f].baselineGrid * scale, origin = frames[f].baselineOrigin * scale;
                        const double base = col.top() + y + lead + line.ascent();
                        const double snapped = origin + std::ceil((base - origin) / grid - 1e-6) * grid;
                        if (snapped > base) y += snapped - base;
                    }
                    if (col.top() + y + h > col.bottom() + 0.01 && !firstInColumn) {   // snapped past the bottom
                        advance();
                        continue;
                    }
                    line.setPosition(QPointF(iv.x0, f * kStride + col.top() + y + lead));
                    B->lines << Line{f, c, QRectF(iv.x0, col.top() + y, iv.x1 - iv.x0, h)};
                    m_used[f] = std::max(m_used[f], col.top() + y + h);
                    columnEmpty = false;
                    rowH = std::max(rowH, h);
                    if (++rowIdx >= row.size()) { y += rowH; rowActive = false; }
                    placed = true;
                }
                ++lineNo;
            }
            B->tl->endLayout();
            if (rowActive) { y += rowH; rowActive = false; }
            if (f < nF) {
                y += after;
            }
        };
        placeLines(false, -1);
        // Where each line landed: the first line in another column or box.
        auto splitAt = [&] {
            for (int i = 1; i < B->lines.size(); ++i)
                if (B->lines[i].frame != B->lines[0].frame || B->lines[i].column != B->lines[0].column) return i;
            return -1;
        };
        auto redo = [&](bool startNext, int breakAfter) {
            f = startSpot.f; c = startSpot.c; y = startSpot.y; overflowY = startSpot.overflowY; rowH = startSpot.rowH; row = startSpot.row;
            rowIdx = startSpot.rowIdx; rowActive = startSpot.rowActive; columnEmpty = startSpot.columnEmpty; m_overflow = startSpot.overflow; m_used = startSpot.used;
            B->lines.clear();
            placeLines(startNext, breakAfter);
        };
        // Only when there is somewhere to go: a later column or text box.
        const bool roomAhead = startSpot.f < nF && (startSpot.c + 1 < std::max(1, frames[startSpot.f].columns) || startSpot.f + 1 < nF);
        if (roomAhead && !startSpot.columnEmpty && B->lines.size() > 1 && B->lines.first().frame >= 0) {
            const int at = splitAt();
            const bool split = at > 0 && B->lines[at].frame >= 0;
            if (split && bf.boolProperty(tp::KeepTogether)) redo(true, -1);                 // keep lines together
            else if (split && at == 1 && bf.boolProperty(tp::WidowControl)) redo(true, -1);  // an orphan: one line left behind
            else if (split && int(B->lines.size()) - at == 1 && at >= 2 && bf.boolProperty(tp::WidowControl))
                redo(false, at - 1);                                                          // a widow: one line carried over
        }
        // Distribute: the last line is spread across the line too (Qt
        // justifies every line but the last), by spacing its letters out.
        if (bf.boolProperty(tp::Distribute) && !B->lines.isEmpty() && B->lines.last().frame >= 0) {
            const int li = int(B->lines.size()) - 1;
            const QTextLine last = B->tl->lineAt(li);
            int n = last.textLength();
            while (n > 0 && B->disp.at(last.textStart() + n - 1).isSpace()) --n;
            const double room = B->lines[li].rect.width() - last.naturalTextWidth();
            if (n > 1 && room > 0.5) {
                QList<QTextLayout::FormatRange> fmts = B->tl->formats();
                const double extra = room * 0.995 / (n - 1);
                QList<QTextLayout::FormatRange> spread;
                for (const auto &r : fmts) {
                    // Split each range at the last line's start; spacing on its part.
                    const int s0 = r.start, e0 = r.start + r.length, ls = last.textStart(), le = last.textStart() + n - 1;
                    if (e0 <= ls || s0 >= le) { spread << r; continue; }
                    if (s0 < ls) { auto a = r; a.length = ls - s0; spread << a; }
                    auto mid = r;
                    mid.start = std::max(s0, ls);
                    mid.length = std::min(e0, le) - mid.start;
                    QFont mf = mid.format.font();
                    mid.format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                    mid.format.setFontLetterSpacing((mf.letterSpacingType() == QFont::AbsoluteSpacing ? mf.letterSpacing() : 0) + extra);
                    spread << mid;
                    if (e0 > le) { auto z = r; z.start = le; z.length = e0 - le; spread << z; }
                }
                B->tl->setFormats(spread);
                redo(false, -1);
            }
        }
        // Keep with next: when the next paragraph's first line won't fit
        // after this one, this one moves on with it.
        if (roomAhead && bf.boolProperty(tp::KeepWithNext) && !startSpot.columnEmpty && b.next().isValid() && f < nF && !B->lines.isEmpty() && B->lines.first().frame >= 0) {
            const QTextBlock nb = b.next();
            const QTextBlockFormat nbf = nb.blockFormat();
            const double nextH = (nbf.topMargin() * scale) + std::max(1.0, lineHeightFor(nbf, scale, singleSpacing({}, 0, 0, baseFontFor(nb, env))));
            const QRectF col = colRect(f, c);
            if (y + nextH > col.height() + 0.01 && B->lines.first().frame == f && B->lines.first().column == c) redo(true, -1);
        }
        m_blocks.push_back(std::move(B));
    }

    // Marker x for hanging lists: the line's left edge minus the hang.
    for (auto &B : m_blocks) {
        if (B->marker.isEmpty() || B->lines.isEmpty()) continue;
        const QTextLine l0 = B->tl->lineAt(0);
        const double lineX = l0.position().x();
        QTextBlock tb = doc->findBlock(B->docStart);
        const QTextBlockFormat bf = tb.blockFormat();
        const double ti = bf.textIndent() * scale, lm = bf.leftMargin() * scale;
        const double markerW = QFontMetricsF(B->markerFont).horizontalAdvance(B->marker) + 4 * scale;
        B->markerX = ti < 0 ? lineX + ti : lineX - markerW;
        Q_UNUSED(lm);
    }

    // Vertical alignment for single-column frames.
    for (int fi = 0; fi < nF; ++fi) {
        const FrameSpec &fs = frames[fi];
        if (fs.valign == VAlign::Top || fs.columns > 1) continue;
        const QRectF col = colRect(fi, 0);
        double top = 1e18, bottom = -1e18;
        for (const auto &B : m_blocks)
            for (const Line &l : B->lines)
                if (l.frame == fi) { top = std::min(top, l.rect.top()); bottom = std::max(bottom, l.rect.bottom()); }
        if (bottom < top) continue;
        const double usedH = bottom - col.top();
        const double shift = (col.height() - usedH) * (fs.valign == VAlign::Middle ? 0.5 : 1.0);
        if (shift <= 0) continue;
        for (auto &B : m_blocks)
            for (int i = 0; i < B->lines.size(); ++i)
                if (B->lines[i].frame == fi) {
                    QTextLine tl = B->tl->lineAt(i);
                    tl.setPosition(tl.position() + QPointF(0, shift));
                    B->lines[i].rect.translate(0, shift);
                }
        m_used[fi] += shift;
    }
    for (int fi = 0; fi < nF; ++fi) m_used[fi] += frames[fi].insets.bottom();
}

QVector<QRectF> StoryLayout::lineRects(int frame) const
{
    QVector<QRectF> v;
    for (const auto &B : m_blocks)
        for (const Line &l : B->lines)
            if (l.frame == frame) v << l.rect;
    return v;
}

QVector<StoryLayout::LineInfo> StoryLayout::lineInfo(int frame) const
{
    QVector<LineInfo> v;
    for (const auto &B : m_blocks)
        for (int i = 0; i < B->lines.size(); ++i) {
            if (B->lines[i].frame != frame) continue;
            const QTextLine l = B->tl->lineAt(i);
            LineInfo li;
            li.rect = B->lines[i].rect;
            li.text = B->disp.mid(l.textStart(), l.textLength());
            li.docStart = B->docStart;
            for (const auto &r : B->tl->formats())
                if (r.start <= l.textStart() && r.start + r.length > l.textStart()) {
                    li.family = r.format.font().family() + QStringLiteral(" w%1%2").arg(r.format.font().weight()).arg(r.format.font().italic() ? QStringLiteral(" italic") : QString());
                    li.pointSize = r.format.fontPointSize() / fontPointFactor();
                }
            v << li;
        }
    return v;
}

double StoryLayout::usedHeight(int frame) const { return frame >= 0 && frame < m_used.size() ? m_used[frame] : 0; }

int StoryLayout::firstPosition(int frame) const
{
    for (const auto &B : m_blocks)
        for (int i = 0; i < B->lines.size(); ++i)
            if (B->lines[i].frame == frame) return B->docStart + B->docFromDisp(B->tl->lineAt(i).textStart());
    return -1;
}

int StoryLayout::lastPosition(int frame) const
{
    int out = -1;
    for (const auto &B : m_blocks)
        for (int i = 0; i < B->lines.size(); ++i)
            if (B->lines[i].frame == frame) {
                const QTextLine l = B->tl->lineAt(i);
                out = B->docStart + B->docFromDisp(l.textStart() + l.textLength());
            }
    return out;
}

const StoryLayout::Block *StoryLayout::blockAt(int pos, int *rel) const
{
    for (const auto &B : m_blocks)
        if (pos >= B->docStart && pos <= B->docStart + B->docLen) {
            if (rel) *rel = pos - B->docStart;
            return B.get();
        }
    if (!m_blocks.empty()) {
        const auto &B = m_blocks.back();
        if (rel) *rel = B->docLen;
        return B.get();
    }
    return nullptr;
}

int StoryLayout::frameOf(int pos) const
{
    int f = -1;
    QRectF r;
    caretRect(pos, &f, &r);
    return f;
}

bool StoryLayout::caretRect(int pos, int *frame, QRectF *rect) const
{
    int rel = 0;
    const Block *B = blockAt(pos, &rel);
    if (!B || B->lines.isEmpty()) return false;
    const int d = B->dispFromDoc(rel);
    int li = 0;
    for (int i = 0; i < B->lines.size(); ++i) {
        const QTextLine l = B->tl->lineAt(i);
        if (d >= l.textStart() && (d < l.textStart() + l.textLength() || i == B->lines.size() - 1)) { li = i; break; }
        if (d >= l.textStart()) li = i;
    }
    const QTextLine l = B->tl->lineAt(li);
    const Line &info = B->lines[li];
    if (frame) *frame = info.frame;
    const double x = l.cursorToX(d);
    if (rect) *rect = QRectF(x, info.rect.top(), 0, info.rect.height());
    return info.frame >= 0;
}

int StoryLayout::hitTest(int frame, const QPointF &pt) const
{
    const Block *bestB = nullptr;
    int bestLine = -1;
    double bestScore = 1e18;
    for (const auto &B : m_blocks)
        for (int i = 0; i < B->lines.size(); ++i) {
            const Line &L = B->lines[i];
            if (L.frame != frame) continue;
            double dy = 0;
            if (pt.y() < L.rect.top()) dy = L.rect.top() - pt.y();
            else if (pt.y() > L.rect.bottom()) dy = pt.y() - L.rect.bottom();
            double dx = 0;
            if (pt.x() < L.rect.left()) dx = L.rect.left() - pt.x();
            else if (pt.x() > L.rect.right()) dx = pt.x() - L.rect.right();
            const double score = dy * 1000 + dx;
            if (score < bestScore) { bestScore = score; bestB = B.get(); bestLine = i; }
        }
    if (!bestB) return -1;
    const QTextLine l = bestB->tl->lineAt(bestLine);
    int d = l.xToCursor(pt.x(), QTextLine::CursorBetweenCharacters);
    // Stay before a trailing line break or space-wrapped end.
    if (bestLine < bestB->lines.size() - 1 && d >= l.textStart() + l.textLength()) d = l.textStart() + l.textLength() - 1;
    return bestB->docStart + bestB->docFromDisp(d);
}

int StoryLayout::moveVertical(int pos, int dir, double x) const
{
    QVector<QPair<const Block *, int>> flat;
    int cur = -1;
    int rel = 0;
    const Block *cb = blockAt(pos, &rel);
    const int d = cb ? cb->dispFromDoc(rel) : 0;
    for (const auto &B : m_blocks)
        for (int i = 0; i < B->lines.size(); ++i) {
            if (B->lines[i].frame < 0) continue;
            if (B.get() == cb) {
                const QTextLine l = B->tl->lineAt(i);
                if (d >= l.textStart() && (d < l.textStart() + l.textLength() || i == B->lines.size() - 1)) cur = flat.size();
            }
            flat << qMakePair(B.get(), i);
        }
    if (cur < 0) return pos;
    const int t = cur + dir;
    if (t < 0) return 0;
    if (t >= flat.size()) {
        const Block *B = m_blocks.back().get();
        return B->docStart + B->docLen;
    }
    const Block *B = flat[t].first;
    const QTextLine l = B->tl->lineAt(flat[t].second);
    int nd = l.xToCursor(x, QTextLine::CursorBetweenCharacters);
    if (flat[t].second < B->lines.size() - 1 && nd >= l.textStart() + l.textLength()) nd = l.textStart() + l.textLength() - 1;
    return B->docStart + B->docFromDisp(nd);
}

int StoryLayout::lineStart(int pos) const
{
    int rel = 0;
    const Block *B = blockAt(pos, &rel);
    if (!B) return pos;
    const int d = B->dispFromDoc(rel);
    for (int i = B->lines.size() - 1; i >= 0; --i)
        if (B->tl->lineAt(i).textStart() <= d) return B->docStart + B->docFromDisp(B->tl->lineAt(i).textStart());
    return B->docStart;
}

int StoryLayout::lineEnd(int pos) const
{
    int rel = 0;
    const Block *B = blockAt(pos, &rel);
    if (!B) return pos;
    const int d = B->dispFromDoc(rel);
    for (int i = 0; i < B->lines.size(); ++i) {
        const QTextLine l = B->tl->lineAt(i);
        const int end = l.textStart() + l.textLength();
        if (d < end || i == B->lines.size() - 1) {
            int e = end;
            if (i < B->lines.size() - 1 && e > l.textStart() && (B->disp[e - 1].isSpace())) --e;
            return B->docStart + B->docFromDisp(e);
        }
    }
    return B->docStart + B->docLen;
}

QVector<QRectF> StoryLayout::rangeRects(int frame, int from, int to) const
{
    QVector<QRectF> out;
    if (from > to) std::swap(from, to);
    for (const auto &B : m_blocks) {
        const int bs = B->docStart, be = B->docStart + B->docLen;
        if (to < bs || from > be) continue;
        const int df = B->dispFromDoc(std::max(from, bs) - bs);
        const int dt = B->dispFromDoc(std::min(to, be) - bs);
        for (int i = 0; i < B->lines.size(); ++i) {
            if (B->lines[i].frame != frame) continue;
            const QTextLine l = B->tl->lineAt(i);
            const int ls = l.textStart(), le = l.textStart() + l.textLength();
            const int s = std::max(df, ls), e = std::min(dt, le);
            const bool paraEnd = (to > be) && i == B->lines.size() - 1;
            if (s > e || (s == e && !paraEnd)) continue;
            double x1 = l.cursorToX(s), x2 = l.cursorToX(e);
            if (x1 > x2) std::swap(x1, x2);
            if (paraEnd) x2 += 4;   // show the paragraph mark as selected
            out << QRectF(x1, B->lines[i].rect.top(), std::max(1.0, x2 - x1), B->lines[i].rect.height());
        }
    }
    return out;
}

void StoryLayout::paint(QPainter *p, int frame, const PaintOptions &o) const
{
    if (frame < 0 || frame >= m_frames.size()) return;
    const FrameSpec &fs = m_frames[frame];
    const QPointF off(0, -frame * kStride);
    const QRectF clip(-1e5, -1e4, 2e5, fs.size.height() + 2e4);
    p->save();
    for (const auto &B : m_blocks) {
        bool any = false;
        for (const Line &l : B->lines) any |= (l.frame == frame);
        if (!any) continue;

        if (o.shadeFields)
            for (const auto &fr : B->fieldRanges)
                for (int i = 0; i < B->lines.size(); ++i) {
                    if (B->lines[i].frame != frame) continue;
                    const QTextLine l = B->tl->lineAt(i);
                    const int s = std::max(fr.first, l.textStart()), e = std::min(fr.first + fr.second, l.textStart() + l.textLength());
                    if (s >= e) continue;
                    const double x1 = l.cursorToX(s), x2 = l.cursorToX(e);
                    p->fillRect(QRectF(std::min(x1, x2), B->lines[i].rect.top(), std::abs(x2 - x1), B->lines[i].rect.height()), QColor(0, 0, 0, 28));
                }

        // Text effects drawn beneath the text from the same glyph runs.
        for (const auto &er : B->effects) {
            const double sz = er.format.fontPointSize() / fontPointFactor();
            const bool shadow = er.format.boolProperty(tp::Shadow), emboss = er.format.boolProperty(tp::Emboss),
                       engrave = er.format.boolProperty(tp::Engrave);
            const QString glow = er.format.stringProperty(tp::GlowRef);
            for (int i = 0; i < B->lines.size(); ++i) {
                if (B->lines[i].frame != frame) continue;
                const QTextLine l = B->tl->lineAt(i);
                const int s = std::max(er.start, l.textStart()), e = std::min(er.start + er.length, l.textStart() + l.textLength());
                if (s >= e) continue;
                const auto runs = l.glyphRuns(s, e - s);
                auto drawRuns = [&](const QPointF &d, const QColor &c) {
                    p->setPen(c);
                    for (const QGlyphRun &r : runs) p->drawGlyphRun(off + d, r);
                };
                const double k = std::max(0.6, sz / 18.0);
                if (!glow.isEmpty()) {
                    QColor gc = ColorRef::fromString(glow).resolve(m_env.colors);
                    gc.setAlphaF(0.35f);
                    for (int a = 0; a < 8; ++a) {
                        const double ang = a * M_PI / 4;
                        drawRuns(QPointF(std::cos(ang), std::sin(ang)) * 1.6 * k, gc);
                    }
                }
                if (shadow) drawRuns(QPointF(1.2, 1.2) * k, QColor(0, 0, 0, 110));
                if (emboss) { drawRuns(QPointF(-0.7, -0.7) * k, QColor(255, 255, 255, 220)); drawRuns(QPointF(0.7, 0.7) * k, QColor(0, 0, 0, 140)); }
                if (engrave) { drawRuns(QPointF(-0.7, -0.7) * k, QColor(0, 0, 0, 140)); drawRuns(QPointF(0.7, 0.7) * k, QColor(255, 255, 255, 220)); }
            }
        }

        B->tl->draw(p, off, {}, clip);

        // Tab leaders: the stop each tab goes to is the first one past where
        // it starts; its leader fills the gap, on a grid so rows line up.
        if (!B->leaders.trimmed().isEmpty()) {
            const QList<QTextOption::Tab> stops = B->tl->textOption().tabs();
            const QList<QTextLayout::FormatRange> fmts = B->tl->formats();
            for (int i = 0; i < B->lines.size(); ++i) {
                if (B->lines[i].frame != frame) continue;
                const QTextLine l = B->tl->lineAt(i);
                for (int k = l.textStart(); k < l.textStart() + l.textLength(); ++k) {
                    if (B->disp[k] != QLatin1Char('\t')) continue;
                    const double x0 = l.cursorToX(k), x1 = l.cursorToX(k + 1);
                    int idx = -1;
                    for (int s = 0; s < stops.size() && idx < 0; ++s)
                        if (stops[s].position > x0 + 0.01) idx = s;
                    if (idx < 0 || idx >= B->leaders.size() || B->leaders[idx].isSpace() || B->leaders[idx].isNull()) continue;
                    QTextCharFormat cf;
                    for (const auto &r : fmts)
                        if (k >= r.start && k < r.start + r.length) cf = r.format;
                    const QFont f = cf.hasProperty(QTextFormat::FontFamilies) ? cf.font() : B->tl->font();
                    const QString ch(B->leaders[idx]);
                    const double w = QFontMetricsF(f).horizontalAdvance(ch);
                    if (w <= 0.01) continue;
                    const double gap = w * 0.4;
                    double x = std::ceil((x0 + gap) / w) * w;
                    QString run;
                    const double startX = x;
                    for (; x + w <= x1 - gap; x += w) run += ch;
                    if (run.isEmpty()) continue;
                    p->setPen(cf.foreground().style() != Qt::NoBrush ? cf.foreground().color() : m_env.colors.slot(Main));
                    drawPlainText(p, off + QPointF(startX, l.position().y() + l.ascent()), f, run);
                }
            }
        }

        // Misspelling squiggles.
        if (o.showSpelling)
            for (const auto &m : o.misspelled) {
                if (m.second < B->docStart || m.first > B->docStart + B->docLen) continue;
                const int ds = B->dispFromDoc(m.first - B->docStart), de = B->dispFromDoc(m.second - B->docStart);
                for (int i = 0; i < B->lines.size(); ++i) {
                    if (B->lines[i].frame != frame) continue;
                    const QTextLine l = B->tl->lineAt(i);
                    const int s = std::max(ds, l.textStart()), e = std::min(de, l.textStart() + l.textLength());
                    if (s >= e) continue;
                    const double x1 = l.cursorToX(s), x2 = l.cursorToX(e);
                    const double yb = l.position().y() - frame * kStride + l.ascent() + 2;
                    QPainterPath wave;
                    wave.moveTo(std::min(x1, x2), yb);
                    for (double x = std::min(x1, x2); x < std::max(x1, x2); x += 2) wave.lineTo(x + 1, yb + ((int(x / 2) % 2) ? -1 : 1));
                    p->setPen(QPen(QColor(220, 30, 30), 0.6));
                    p->setBrush(Qt::NoBrush);
                    p->drawPath(wave);
                }
            }

        if (B->lines.first().frame == frame) {
            const QTextLine l0 = B->tl->lineAt(0);
            if (!B->marker.isEmpty()) {
                p->setPen(B->markerColor);
                drawPlainText(p, QPointF(B->markerX, l0.position().y() - frame * kStride + l0.ascent()), B->markerFont, B->marker);
            }
            if (!B->dropText.isEmpty()) {
                // A raised cap stands on the first line; a dropped one on the last line it spans.
                const int n = B->dropUp ? 1 : std::min<int>(B->dropLines, B->lines.size());
                const QTextLine ln = B->tl->lineAt(n - 1);
                const double baseline = ln.position().y() - frame * kStride + ln.ascent();
                p->setPen(B->dropColor);
                drawPlainText(p, QPointF(l0.position().x() - B->dropWidth, baseline), B->dropFont, B->dropText);
            }
        }

        if (o.showSpecial) {
            const QFont sf = B->tl->font();
            p->setPen(QColor(90, 120, 200));
            for (int i = 0; i < B->lines.size(); ++i) {
                if (B->lines[i].frame != frame) continue;
                const QTextLine l = B->tl->lineAt(i);
                const double base = l.position().y() - frame * kStride + l.ascent();
                for (int k = l.textStart(); k < l.textStart() + l.textLength(); ++k) {
                    const QChar ch = B->disp[k];
                    if (ch == ' ' || ch == '\t' || ch == QChar::LineSeparator) {
                        const double x1 = l.cursorToX(k), x2 = l.cursorToX(k + 1);
                        const QString mark = ch == ' ' ? QStringLiteral("·") : ch == '\t' ? QStringLiteral("→") : QStringLiteral("↵");
                        const double w = QFontMetricsF(sf).horizontalAdvance(mark);
                        drawPlainText(p, QPointF((x1 + x2 - w) / 2, base), sf, mark);
                    }
                }
                if (i == B->lines.size() - 1) drawPlainText(p, QPointF(l.cursorToX(l.textStart() + l.textLength()) + 1, base), sf, QStringLiteral("¶"));
            }
        }
    }
    if (o.selFrom >= 0 && o.selTo > o.selFrom)
        for (const QRectF &r : rangeRects(frame, o.selFrom, o.selTo)) p->fillRect(r, o.selColor);
    p->restore();
}

} // namespace jp
