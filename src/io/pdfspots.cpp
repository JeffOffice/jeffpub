#include "io/pdfspots.h"

#include "io/qtpdf.h"

#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace jp {

namespace {

QByteArray num(double v) { return QByteArray::number(std::round(v * 10000) / 10000, 'g', 6); }

// A PDF name: anything but letters, digits and a few marks as #xx.
QByteArray pdfName(const QString &s)
{
    QByteArray out;
    for (uchar c : s.toUtf8()) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out += char(c);
        else out += '#' + QByteArray::number(c, 16).rightJustified(2, '0').toUpper();
    }
    return out.isEmpty() ? QByteArray("Spot") : out;
}

struct Spot {
    QByteArray name;
    double c, m, y, k;   // ink amounts for full strength
    QColor rgb;
    bool inks;           // given as CMYK: tints scale the inks
};

// How much of the spot a CMYK color is (1 the spot, less its tints), or -1.
double amountOf(const Spot &s, double c, double m, double y, double k)
{
    if (s.inks) {
        const double sv[4] = {s.c, s.m, s.y, s.k}, cv[4] = {c, m, y, k};
        double a = -1;
        for (int i = 0; i < 4; ++i) {
            if (sv[i] < 0.01) {
                if (cv[i] > 0.01) return -1;
                continue;
            }
            const double ai = cv[i] / sv[i];
            if (a < 0) a = ai;
            else if (std::abs(ai - a) > 0.03) return -1;
        }
        return a < 0.02 || a > 1.02 ? -1 : std::min(1.0, a);
    }
    // Given as RGB: Qt wrote its plain CMYK conversion; turn it back and
    // compare as a tint (the spot mixed with white).
    const QColor got = QColor::fromRgbF(float((1 - c) * (1 - k)), float((1 - m) * (1 - k)), float((1 - y) * (1 - k)));
    const int sc[3] = {s.rgb.red(), s.rgb.green(), s.rgb.blue()}, gc[3] = {got.red(), got.green(), got.blue()};
    double t = -1;
    for (int i = 0; i < 3; ++i) {
        if (255 - sc[i] < 8) {
            if (std::abs(gc[i] - 255) > 8) return -1;
            continue;
        }
        const double ti = double(gc[i] - sc[i]) / (255 - sc[i]);
        if (t < 0) t = ti;
        else if (std::abs(ti - t) > 0.04) return -1;
    }
    if (t < 0) t = 0;
    if (t < -0.02 || t > 0.98) return -1;
    return 1 - std::clamp(t, 0.0, 1.0);
}

} // namespace

bool addPdfSpotColors(const QString &path, const QVector<QColor> &colors, const QStringList &names, QString *error)
{
    if (colors.isEmpty()) return true;
    QtPdf pdf;
    if (!pdf.load(path, error)) return false;

    QVector<Spot> spots;
    for (int i = 0; i < colors.size(); ++i) {
        const QColor k = colors[i].toCmyk();
        spots << Spot{pdfName(names.value(i)), k.cyanF(), k.magentaF(), k.yellowF(), k.blackF(), colors[i].toRgb(), colors[i].spec() == QColor::Cmyk};
    }

    // Content streams: a process color that is a spot color (or one of its
    // tints) becomes that spot's ink at that strength.
    static const QRegularExpression op(QStringLiteral("/CSpcmyk (cs|CS)\\s+([-\\d.]+) ([-\\d.]+) ([-\\d.]+) ([-\\d.]+) (scn|SCN)"));
    int changed = 0;
    for (QtPdf::Obj &o : pdf.objects) {
        const QByteArray dict = QtPdf::dictOf(o.body);
        if (!QtPdf::isStream(o.body) || !dict.contains("/FlateDecode") || dict.contains("/Image")) continue;
        bool ok = false;
        const QByteArray raw = pdf.streamData(o, &ok);
        if (!ok || !raw.contains("/CSpcmyk")) continue;
        QString text = QString::fromLatin1(raw);
        QString out;
        qsizetype last = 0;
        bool any = false;
        auto it = op.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const double c = m.captured(2).toDouble(), mg = m.captured(3).toDouble(), y = m.captured(4).toDouble(), k = m.captured(5).toDouble();
            int which = -1;
            double amount = -1;
            for (int i = 0; i < spots.size() && which < 0; ++i) {
                const double a = amountOf(spots[i], c, mg, y, k);
                if (a > 0) { which = i; amount = a; }
            }
            if (which < 0) continue;
            out += text.mid(last, m.capturedStart() - last);
            out += QStringLiteral("/CSspot%1 %2 %3 %4").arg(which).arg(m.captured(1), QString::fromLatin1(num(amount)), m.captured(6));
            last = m.capturedEnd();
            any = true;
        }
        if (!any) continue;
        out += text.mid(last);
        const QByteArray packed = QtPdf::deflate(out.toLatin1());
        if (packed.isEmpty()) continue;
        pdf.setStream(o, dict, packed);
        ++changed;
    }
    if (!changed) return true;

    // Each resource dictionary with color spaces gets the spot inks:
    // Separation spaces with their process equivalents for proofing.
    QByteArray spaces;
    for (int i = 0; i < spots.size(); ++i)
        spaces += " /CSspot" + QByteArray::number(i) + " [/Separation /" + spots[i].name + " /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [" +
                  num(spots[i].c) + ' ' + num(spots[i].m) + ' ' + num(spots[i].y) + ' ' + num(spots[i].k) + "] /N 1 >>]";
    for (QtPdf::Obj &o : pdf.objects) {
        const qsizetype cs = o.body.indexOf("/ColorSpace <<");
        if (cs < 0 || QtPdf::isStream(o.body)) continue;
        o.body.insert(cs + 14, spaces);
    }
    return pdf.save(path, error);
}

} // namespace jp
