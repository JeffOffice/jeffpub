#include "io/zip.h"

#include <QtEndian>

namespace jp {

quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put16(QByteArray &a, quint16 v) { char b[2]; qToLittleEndian(v, b); a.append(b, 2); }
static void put32(QByteArray &a, quint32 v) { char b[4]; qToLittleEndian(v, b); a.append(b, 4); }

void ZipWriter::add(const QString &name, const QByteArray &data)
{
    const QByteArray n = name.toUtf8();
    Entry e{name, crc32(data), quint32(data.size()), quint32(m_out.size())};
    put32(m_out, 0x04034b50);
    put16(m_out, 20);        // version needed
    put16(m_out, 0x0800);    // UTF-8 names
    put16(m_out, 0);         // stored
    put16(m_out, 0); put16(m_out, 0x21); // time, date (1980-01-01)
    put32(m_out, e.crc);
    put32(m_out, e.size);
    put32(m_out, e.size);
    put16(m_out, quint16(n.size()));
    put16(m_out, 0);
    m_out.append(n);
    m_out.append(data);
    m_entries << e;
}

QByteArray ZipWriter::finish()
{
    const quint32 cdStart = quint32(m_out.size());
    for (const auto &e : m_entries) {
        const QByteArray n = e.name.toUtf8();
        put32(m_out, 0x02014b50);
        put16(m_out, 20); put16(m_out, 20);
        put16(m_out, 0x0800);
        put16(m_out, 0);
        put16(m_out, 0); put16(m_out, 0x21);
        put32(m_out, e.crc);
        put32(m_out, e.size);
        put32(m_out, e.size);
        put16(m_out, quint16(n.size()));
        put16(m_out, 0); put16(m_out, 0); put16(m_out, 0); put16(m_out, 0);
        put32(m_out, 0);
        put32(m_out, e.offset);
        m_out.append(n);
    }
    const quint32 cdSize = quint32(m_out.size()) - cdStart;
    put32(m_out, 0x06054b50);
    put16(m_out, 0); put16(m_out, 0);
    put16(m_out, quint16(m_entries.size()));
    put16(m_out, quint16(m_entries.size()));
    put32(m_out, cdSize);
    put32(m_out, cdStart);
    put16(m_out, 0);
    return m_out;
}

static quint16 get16(const QByteArray &a, qsizetype at) { return qFromLittleEndian<quint16>(a.constData() + at); }
static quint32 get32(const QByteArray &a, qsizetype at) { return qFromLittleEndian<quint32>(a.constData() + at); }

bool readZip(const QByteArray &zip, QMap<QString, QByteArray> &out, QString *error)
{
    auto fail = [&](const char *m) { if (error) *error = QString::fromLatin1(m); return false; };
    if (zip.size() < 22) return fail("The file is too small to be a publication.");
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - 22; i >= std::max<qsizetype>(0, zip.size() - 65557); --i)
        if (get32(zip, i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return fail("The file is not a JeffPub publication (no ZIP directory).");
    const int count = get16(zip, eocd + 10);
    qsizetype p = get32(zip, eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (p + 46 > zip.size() || get32(zip, p) != 0x02014b50) return fail("The publication's file directory is damaged.");
        const quint16 method = get16(zip, p + 10);
        const quint32 csize = get32(zip, p + 20);
        const quint16 nlen = get16(zip, p + 28), xlen = get16(zip, p + 30), clen = get16(zip, p + 32);
        const quint32 local = get32(zip, p + 42);
        const QString name = QString::fromUtf8(zip.constData() + p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (local + 30 > quint32(zip.size())) return fail("The publication is truncated.");
        const qsizetype data = local + 30 + get16(zip, local + 26) + get16(zip, local + 28);
        if (method != 0) return fail("The publication uses a compression method JeffPub cannot read.");
        if (data + csize > quint64(zip.size())) return fail("The publication is truncated.");
        out.insert(name, zip.mid(data, csize));
    }
    return true;
}

} // namespace jp
