#include "io/pdfspots.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace jp {

namespace {

// PDF streams are zlib data; Qt's (un)compress add a 4-byte length.
QByteArray inflate(const QByteArray &in, bool *ok)
{
    QByteArray sized(4, '\0');
    qToBigEndian<quint32>(quint32(std::min<qsizetype>(qsizetype(in.size()) * 4 + 4096, 64 * 1024 * 1024)), sized.data());   // grows if short
    const QByteArray out = qUncompress(sized + in);
    *ok = !out.isEmpty();
    return out;
}

QByteArray deflate(const QByteArray &in) { return qCompress(in, 9).mid(4); }

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
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const QByteArray pdf = f.readAll();
    f.close();

    QVector<Spot> spots;
    for (int i = 0; i < colors.size(); ++i) {
        const QColor k = colors[i].toCmyk();
        spots << Spot{pdfName(names.value(i)), k.cyanF(), k.magentaF(), k.yellowF(), k.blackF(), colors[i].toRgb(), colors[i].spec() == QColor::Cmyk};
    }

    // The objects, in file order.
    struct Obj { int id; QByteArray body; };
    QVector<Obj> objs;
    qsizetype at = pdf.indexOf("\n1 0 obj\n");
    if (at < 0) return true;
    ++at;
    while (true) {
        const qsizetype nl = pdf.indexOf('\n', at);
        if (nl < 0) break;
        const QByteArray first = pdf.mid(at, nl - at);
        if (!first.endsWith(" 0 obj")) break;
        const int id = first.left(first.indexOf(' ')).toInt();
        // A stream's data can hold any bytes, so its end is found by the
        // marker after the data rather than the first "endobj".
        qsizetype end = pdf.indexOf("endobj\n", nl + 1);
        const qsizetype stream = pdf.indexOf(">>\nstream\n", nl + 1);
        if (stream >= 0 && (end < 0 || stream < end)) {
            const qsizetype close = pdf.indexOf("\nendstream\nendobj\n", stream);
            end = close < 0 ? -1 : close + 11;
        }
        if (end < 0) break;
        objs << Obj{id, pdf.mid(nl + 1, end - nl - 1)};
        at = end + 7;
    }
    if (objs.isEmpty()) return true;
    QHash<int, int> index;
    for (int i = 0; i < objs.size(); ++i) index.insert(objs[i].id, i);

    // Content streams: a process color that is a spot color (or one of its
    // tints) becomes that spot's ink at that strength.
    static const QRegularExpression op(QStringLiteral("/CSpcmyk (cs|CS)\\s+([-\\d.]+) ([-\\d.]+) ([-\\d.]+) ([-\\d.]+) (scn|SCN)"));
    int changed = 0;
    for (Obj &o : objs) {
        const qsizetype s = o.body.indexOf("stream\n");
        if (s < 0 || !o.body.left(s).contains("/FlateDecode") || o.body.left(s).contains("/Image")) continue;
        const qsizetype e = o.body.lastIndexOf("endstream");
        if (e < s) continue;
        bool ok = false;
        const QByteArray raw = inflate(o.body.mid(s + 7, e - s - 7), &ok);
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
        const QByteArray packed = deflate(out.toLatin1());
        if (packed.isEmpty()) continue;
        QByteArray dict = o.body.left(s);
        // The length: written in place, or in an object of its own.
        static const QRegularExpression lenRef(QStringLiteral("/Length (\\d+) 0 R"));
        static const QRegularExpression lenDirect(QStringLiteral("/Length (\\d+)"));
        const QString d = QString::fromLatin1(dict);
        const auto ref = lenRef.match(d);
        if (ref.hasMatch()) {
            const int lid = ref.captured(1).toInt();
            if (index.contains(lid)) objs[index[lid]].body = QByteArray::number(packed.size()) + '\n';
        } else {
            dict = QString(d).replace(lenDirect, QStringLiteral("/Length %1").arg(packed.size())).toLatin1();
        }
        o.body = dict + "stream\n" + packed + "\nendstream\n";
        ++changed;
    }
    if (!changed) return true;

    // Each resource dictionary with color spaces gets the spot inks:
    // Separation spaces with their process equivalents for proofing.
    QByteArray spaces;
    for (int i = 0; i < spots.size(); ++i)
        spaces += " /CSspot" + QByteArray::number(i) + " [/Separation /" + spots[i].name + " /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [" +
                  num(spots[i].c) + ' ' + num(spots[i].m) + ' ' + num(spots[i].y) + ' ' + num(spots[i].k) + "] /N 1 >>]";
    for (Obj &o : objs) {
        const qsizetype cs = o.body.indexOf("/ColorSpace <<");
        if (cs < 0 || o.body.indexOf("stream\n") >= 0) continue;
        o.body.insert(cs + 14, spaces);
    }

    // Write it back with a new cross-reference table.
    QByteArray out = pdf.left(pdf.indexOf("\n1 0 obj\n") + 1);
    int maxId = 0;
    QHash<int, qsizetype> offsets;
    for (const Obj &o : objs) {
        offsets.insert(o.id, out.size());
        out += QByteArray::number(o.id) + " 0 obj\n" + o.body + "endobj\n";
        maxId = std::max(maxId, o.id);
    }
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(maxId + 1) + "\n0000000000 65535 f \n";
    for (int i = 1; i <= maxId; ++i) {
        if (offsets.contains(i)) out += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
        else out += "0000000000 65535 f \n";
    }
    const qsizetype trailer = pdf.indexOf("trailer", pdf.lastIndexOf("\nxref\n"));
    const qsizetype startxref = pdf.lastIndexOf("startxref");
    if (trailer < 0 || startxref < trailer) {
        if (error) *error = QStringLiteral("unexpected PDF layout");
        return false;
    }
    out += pdf.mid(trailer, startxref - trailer);
    out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(out);
    return true;
}

} // namespace jp
