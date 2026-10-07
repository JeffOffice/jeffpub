#include "io/qtpdf.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <zlib.h>

namespace jp {

// PDF streams are zlib data, of a size the dictionary doesn't give.
QByteArray QtPdf::inflate(const QByteArray &in, bool *ok)
{
    *ok = false;
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray out;
    char buf[65536];
    int r = Z_OK;
    while (r == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof buf;
        r = ::inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) break;
        out.append(buf, qsizetype(sizeof buf - zs.avail_out));
        // No picture or page this program writes is this big: a bomb.
        if (out.size() > (qsizetype(1) << 30)) break;
        if (r == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) break;   // truncated
    }
    inflateEnd(&zs);
    *ok = r == Z_STREAM_END;
    return out;
}

QByteArray QtPdf::deflate(const QByteArray &in) { return qCompress(in, 9).mid(4); }

bool QtPdf::load(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const QByteArray pdf = f.readAll();
    objects.clear();
    qsizetype at = pdf.indexOf("\n1 0 obj\n");
    if (!pdf.startsWith("%PDF-") || at < 0) {
        if (error) *error = QStringLiteral("not a PDF this program wrote");
        return false;
    }
    header = pdf.left(at + 1);
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
        objects << Obj{id, pdf.mid(nl + 1, end - nl - 1)};
        at = end + 7;
    }
    const qsizetype t = pdf.indexOf("trailer", pdf.lastIndexOf("\nxref\n"));
    const qsizetype startxref = pdf.lastIndexOf("startxref");
    if (objects.isEmpty() || t < 0 || startxref < t) {
        if (error) *error = QStringLiteral("unexpected PDF layout");
        return false;
    }
    trailer = pdf.mid(t, startxref - t);
    return true;
}

bool QtPdf::save(const QString &path, QString *error) const
{
    QByteArray out = header;
    int maxId = 0;
    QHash<int, qsizetype> offsets;
    for (const Obj &o : objects) {
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
    // The trailer's /Size follows the objects added.
    static const QRegularExpression size(QStringLiteral("/Size \\d+"));
    out += QString::fromLatin1(trailer).replace(size, QStringLiteral("/Size %1").arg(maxId + 1)).toLatin1();
    out += "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QSaveFile f(path);
    f.setDirectWriteFallback(true);
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size() || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

QtPdf::Obj *QtPdf::object(int id)
{
    for (Obj &o : objects)
        if (o.id == id) return &o;
    return nullptr;
}

int QtPdf::addObject(const QByteArray &body)
{
    int maxId = 0;
    for (const Obj &o : objects) maxId = std::max(maxId, o.id);
    objects << Obj{maxId + 1, body.endsWith('\n') ? body : body + '\n'};
    return maxId + 1;
}

bool QtPdf::isStream(const QByteArray &body) { return body.contains(">>\nstream\n"); }

QByteArray QtPdf::dictOf(const QByteArray &body)
{
    const qsizetype s = body.indexOf(">>\nstream\n");
    return s < 0 ? body : body.left(s + 2);
}

QByteArray QtPdf::streamData(const Obj &o, bool *ok) const
{
    *ok = false;
    const qsizetype s = o.body.indexOf(">>\nstream\n");
    const qsizetype e = o.body.lastIndexOf("\nendstream");
    if (s < 0 || e < s + 10) return {};
    const QByteArray raw = o.body.mid(s + 10, e - s - 10);
    if (!dictOf(o.body).contains("/FlateDecode")) {
        *ok = true;
        return raw;
    }
    return inflate(raw, ok);
}

void QtPdf::setStream(Obj &o, const QByteArray &dictIn, const QByteArray &data)
{
    // A length kept in an object of its own becomes direct.
    static const QRegularExpression len(QStringLiteral("\\s*/Length \\d+( 0 R)?"));
    QByteArray dict = QString::fromLatin1(dictIn).remove(len).toLatin1().trimmed();
    if (dict.startsWith("<<")) dict.insert(2, " /Length " + QByteArray::number(data.size()));
    o.body = dict + "\nstream\n" + data + "\nendstream\n";
}

int QtPdf::ref(const QByteArray &dict, const QByteArray &key)
{
    const QRegularExpression re(QRegularExpression::escape(QString::fromLatin1(key)) + QStringLiteral("\\s+(\\d+) 0 R"));
    const auto m = re.match(QString::fromLatin1(dict));
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

} // namespace jp
