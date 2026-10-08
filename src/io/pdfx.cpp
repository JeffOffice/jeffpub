#include "io/pdfx.h"

#include "io/overprint.h"
#include "io/qtpdf.h"
#include "io/zip.h"

#include <QCryptographicHash>
#include <QMap>
#include <QImage>
#include <QRegularExpression>
#include <QVector>

namespace jp {

const QVector<PdfXCondition> &pdfXConditions()
{
    static const QVector<PdfXCondition> list{
        {QStringLiteral("U.S. web offset, coated paper (SWOP)"), QStringLiteral("CGATS TR 001"),
         QStringLiteral("SWOP (Publication) Grade 5 Paper"), QStringLiteral("U.S. Web Coated (SWOP) v2")},
        {QStringLiteral("European offset, coated paper (FOGRA39)"), QStringLiteral("FOGRA39"),
         QStringLiteral("Offset commercial and specialty printing according to ISO 12647-2:2004 / Amd 1, paper type 1 or 2"),
         QStringLiteral("Coated FOGRA39 (ISO 12647-2:2004)")},
        [] {
            PdfXCondition c{QStringLiteral("Offset, premium coated paper (FOGRA51, PSO Coated v3)"), QStringLiteral("FOGRA51"),
                            QStringLiteral("Commercial and specialty offset printing according to ISO 12647-2:2013, premium coated paper"),
                            QStringLiteral("PSO Coated v3")};
            c.x4 = true;
            c.profileUrl = QStringLiteral("https://www.eci.org/lib/exe/pso-coated_v3.zip");
            c.zipSha256 = QByteArrayLiteral("5c8ed32d40949c2e8b84a03642ca7aadc4e3f153237cf439d3cdddffffd4ed95");
            c.profileFile = QStringLiteral("PSOcoated_v3.icc");
            c.profileSha256 = QByteArrayLiteral("c30ad2c01e8f93135ec7682c535e0a81bc2d177c301e196376c5f5838b5c8e86");
            c.profileSize = QStringLiteral("1.8 MB");
            return c;
        }(),
        [] {
            PdfXCondition c{QStringLiteral("Your printer's color profile (an .icc file)"), QStringLiteral("Custom"), QString(), QString()};
            c.x4 = true;
            c.ownProfile = true;
            return c;
        }(),
    };
    return list;
}

QByteArray pdfXProfileFromZip(const PdfXCondition &c, const QByteArray &zip, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error) *error = why;
        return QByteArray();
    };
    if (QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex() != c.zipSha256)
        return fail(QStringLiteral("the download isn't the file expected"));
    QMap<QString, QByteArray> entries;
    if (!readZip(zip, entries)) return fail(QStringLiteral("the download couldn't be unpacked"));
    const QByteArray icc = entries.value(c.profileFile);
    if (QCryptographicHash::hash(icc, QCryptographicHash::Sha256).toHex() != c.profileSha256)
        return fail(QStringLiteral("the profile in the download isn't the one expected"));
    return icc;
}

namespace {

QByteArray box(const QRectF &r)
{
    auto n = [](double v) { return QByteArray::number(v, 'f', 3); };
    return "[" + n(r.left()) + ' ' + n(r.top()) + ' ' + n(r.right()) + ' ' + n(r.bottom()) + "]";
}

// A PDF text string: plain ASCII in parentheses, escaped.
QByteArray pdfString(const QString &s)
{
    QByteArray out("(");
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u == '(' || u == ')' || u == '\\') out += '\\';
        out += u < 0x20 || u > 0x7e ? '?' : char(u);
    }
    return out + ')';
}

QString dictString(const QByteArray &dict) { return QString::fromLatin1(dict); }

int intIn(const QByteArray &dict, const char *key)
{
    const QRegularExpression re(QStringLiteral("/%1\\s+(\\d+)").arg(QLatin1String(key)));
    const auto m = re.match(dictString(dict));
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

// An RGB picture as CMYK, Flate-compressed, with Qt's own conversion (the
// one the vector colors went through).
bool cmykImage(QtPdf &pdf, QtPdf::Obj &o, bool keepMask, QString *why)
{
    const QByteArray dict = QtPdf::dictOf(o.body);
    if (dict.contains("/SMask") && !keepMask) {
        *why = QStringLiteral("a picture still has see-through parts");
        return false;
    }
    const int w = intIn(dict, "Width"), h = intIn(dict, "Height");
    if (w <= 0 || h <= 0 || intIn(dict, "BitsPerComponent") != 8) {
        *why = QStringLiteral("a picture in an unexpected form");
        return false;
    }
    bool ok = false;
    const QByteArray data = dict.contains("/DCTDecode") ? o.body.mid(o.body.indexOf(">>\nstream\n") + 10) : pdf.streamData(o, &ok);
    QImage img;
    if (dict.contains("/DCTDecode")) {
        img = QImage::fromData(data.left(data.lastIndexOf("\nendstream")), "JPG");
    } else if (ok && data.size() >= qsizetype(w) * h * 3) {
        img = QImage(reinterpret_cast<const uchar *>(data.constData()), w, h, w * 3, QImage::Format_RGB888).copy();
    }
    if (img.isNull() || img.size() != QSize(w, h)) {
        *why = QStringLiteral("a picture JeffPub couldn't read back");
        return false;
    }
    const QImage cmyk = img.convertToFormat(QImage::Format_CMYK8888);
    QByteArray bytes;
    bytes.reserve(qsizetype(w) * h * 4);
    for (int y = 0; y < h; ++y) bytes.append(reinterpret_cast<const char *>(cmyk.constScanLine(y)), qsizetype(w) * 4);
    QString d = dictString(dict);
    d.replace(QLatin1String("/ColorSpace /DeviceRGB"), QLatin1String("/ColorSpace /DeviceCMYK"));
    d.replace(QLatin1String("/Filter /DCTDecode"), QLatin1String("/Filter /FlateDecode"));
    d.remove(QRegularExpression(QStringLiteral("\\s*/DecodeParms\\s*<<[^>]*>>")));
    pdf.setStream(o, d.toLatin1(), QtPdf::deflate(bytes));
    return true;
}

// Both versions: X-4 keeps transparency, and Qt has already written its
// output intent, document information and metadata.
bool finishPdfX(const QString &path, const PdfXOptions &opt, bool x4, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error) *error = why;
        return false;
    };
    QtPdf pdf;
    QString err;
    if (!pdf.load(path, &err)) return fail(err);
    const PdfXCondition &cond = pdfXConditions().value(opt.condition, pdfXConditions().first());

    static const QRegularExpression rgbOp(QStringLiteral("(^|\\s)[-\\d.]+\\s+[-\\d.]+\\s+[-\\d.]+\\s+(rg|RG)(?=\\s)"));
    static const QRegularExpression rgbSc(QStringLiteral("/P?CSp\\s+(cs|CS)\\s+[-\\d.]+\\s+[-\\d.]+\\s+[-\\d.]+\\s+(/\\w+\\s+)?(sc|scn|SC|SCN)(?=\\s)"));
    static const QRegularExpression alpha(QStringLiteral("/(ca|CA)\\s+([-\\d.]+)"));
    static const QRegularExpression blend(QStringLiteral("/BM\\s*/(\\w+)"));
    const int infoId = QtPdf::ref(pdf.trailer, "/Info");

    for (QtPdf::Obj &o : pdf.objects) {
        const QByteArray dict = QtPdf::dictOf(o.body);
        // Pictures.
        if (dict.contains("/Subtype /Image")) {
            if (dict.contains("/DeviceRGB") || dict.contains("/Indexed")) {
                QString why;
                if (!cmykImage(pdf, o, x4, &why)) return fail(why);
            }
            continue;
        }
        if (QtPdf::isStream(o.body)) {
            // Page content (and forms and patterns): no RGB colors left.
            // Not fonts' own programs or the metadata.
            if (dict.contains("/Length1") || dict.contains("/Length2") || dict.contains("/Type1C") || dict.contains("/CIDFontType0C") ||
                dict.contains("/OpenType") || dict.contains("/Type /Metadata"))
                continue;
            bool ok = false;
            const QByteArray data = pdf.streamData(o, &ok);
            if (!ok || data.contains("/CIDInit")) continue;
            const QString text = QString::fromLatin1(data);
            if (text.contains(rgbOp) || text.contains(rgbSc)) return fail(QStringLiteral("a color JeffPub couldn't turn into ink amounts"));
            continue;
        }
        QString body = dictString(o.body);
        // Graphics states: transparency must be gone; PDF 1.4 keys go.
        if (!x4 && (body.contains(QLatin1String("/ExtGState")) || body.contains(alpha))) {
            auto it = alpha.globalMatch(body);
            while (it.hasNext())
                if (it.next().captured(2).toDouble() < 0.999) return fail(QStringLiteral("see-through objects were left unflattened"));
            const auto bm = blend.match(body);
            if (bm.hasMatch() && bm.captured(1) != QLatin1String("Normal") && bm.captured(1) != QLatin1String("Compatible"))
                return fail(QStringLiteral("a blend mode other than Normal"));
            if (body.contains(QRegularExpression(QStringLiteral("/SMask\\s*(?!/None)[^\\s/]"))))
                return fail(QStringLiteral("a soft mask (transparency)"));
            body.remove(QRegularExpression(QStringLiteral("\\s*/(ca|CA)\\s+[-\\d.]+")));
            body.remove(QRegularExpression(QStringLiteral("\\s*/AIS\\s+(true|false)")));
            body.remove(QRegularExpression(QStringLiteral("\\s*/SMask\\s*/None")));
        }
        if (!x4 && body.contains(QLatin1String("/S /Transparency"))) return fail(QStringLiteral("a transparency group"));
        // Qt declares RGB spaces in every resource dictionary; its CMYK
        // output never colors with them but starts each page in one.
        body.replace(QLatin1String("/CSp /DeviceRGB"), QLatin1String("/CSp /DeviceCMYK"));
        if (body.trimmed() == QLatin1String("[/Pattern /DeviceRGB]")) body = QStringLiteral("[/Pattern /DeviceCMYK]\n");
        // Fonts are embedded.
        if (body.contains(QLatin1String("/Type /FontDescriptor")) && !body.contains(QLatin1String("/FontFile")))
            return fail(QStringLiteral("a font that isn't embedded"));
        // Pages: trim and bleed boxes; no links over the page.
        if (body.contains(QRegularExpression(QStringLiteral("/Type\\s*/Page(?!s)")))) {
            body.remove(QRegularExpression(QStringLiteral("\\s*/(TrimBox|BleedBox|ArtBox)\\s*\\[[^\\]]*\\]")));
            body.replace(QRegularExpression(QStringLiteral("(/MediaBox\\s*\\[[^\\]]*\\])")),
                         QStringLiteral("\\1 /TrimBox %1 /BleedBox %2").arg(QString::fromLatin1(box(opt.trim)), QString::fromLatin1(box(opt.bleed))));
            if (const int annots = QtPdf::ref(body.toLatin1(), "/Annots"))
                if (QtPdf::Obj *a = pdf.object(annots)) a->body = "[ ]\n";
            body.remove(QRegularExpression(QStringLiteral("\\s*/Annots\\s*\\[[^\\]]*\\]")));
        }
        // The catalog names the printing condition.
        if (body.contains(QLatin1String("/Type /Catalog")) && !body.contains(QLatin1String("/OutputIntents"))) {
            if (x4) return fail(QStringLiteral("no output intent (the printing condition)"));
            const QByteArray intent = "<< /Type /OutputIntent /S /GTS_PDFX /OutputConditionIdentifier " + pdfString(cond.identifier) +
                                      " /OutputCondition " + pdfString(cond.condition) + " /RegistryName (http://www.color.org) /Info " +
                                      pdfString(cond.info) + " >>\n";
            const int id = pdf.addObject(intent);
            body.replace(QLatin1String("/Type /Catalog"), QStringLiteral("/Type /Catalog /OutputIntents [%1 0 R]").arg(id));
        }
        // The document information: the PDF/X version, a title, not trapped.
        if (o.id == infoId && !x4) {
            body.remove(QRegularExpression(QStringLiteral("\\s*/GTS_PDFX\\w+\\s*\\([^)]*\\)")));
            body.remove(QRegularExpression(QStringLiteral("\\s*/Trapped\\s*/\\w+")));
            body.replace(QRegularExpression(QStringLiteral("/Title\\s*\\(\\)")), QStringLiteral("/Title (Untitled)"));
            if (!body.contains(QLatin1String("/Title"))) body.replace(QLatin1String("<<"), QLatin1String("<< /Title (Untitled)"));
            const qsizetype close = body.lastIndexOf(QLatin1String(">>"));
            if (close < 0) return fail(QStringLiteral("unexpected PDF layout"));
            body.insert(close, QLatin1String("/GTS_PDFXVersion (PDF/X-1:2001) /GTS_PDFXConformance (PDF/X-1a:2001) /Trapped /False "));
        }
        o.body = body.toLatin1();
    }
    // Black prints over the inks under it, as the publication says.
    applyOverprint(pdf, opt.overprint);
    // Everything left is PDF 1.3 (PDF/X-4 stays 1.6).
    if (!x4) pdf.header.replace("%PDF-1.4", "%PDF-1.3");
    for (const QtPdf::Obj &o : pdf.objects)
        if (QtPdf::dictOf(o.body).contains("/DeviceRGB")) return fail(QStringLiteral("an RGB color space JeffPub couldn't convert"));
    return pdf.save(path, error);
}

} // namespace

bool makePdfX1a(const QString &path, const PdfXOptions &opt, QString *error) { return finishPdfX(path, opt, false, error); }
bool makePdfX4(const QString &path, const PdfXOptions &opt, QString *error) { return finishPdfX(path, opt, true, error); }

} // namespace jp
