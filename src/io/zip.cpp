#include "io/zip.h"

#include <QFile>
#include <zlib.h>

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

// Raw deflate (no zlib header), as ZIP entries hold it.
static QByteArray deflateRaw(const QByteArray &in)
{
    z_stream zs{};
    if (deflateInit2(&zs, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    QByteArray out(qsizetype(deflateBound(&zs, uLong(in.size()))), Qt::Uninitialized);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    const int r = ::deflate(&zs, Z_FINISH);
    out.resize(qsizetype(zs.total_out));
    deflateEnd(&zs);
    return r == Z_STREAM_END ? out : QByteArray();
}

static QByteArray inflateRaw(const QByteArray &in, qsizetype expected, bool *ok)
{
    *ok = false;
    // Nothing this program saves is near this; more is a decompression bomb.
    if (expected < 0 || expected > (qsizetype(1) << 30)) return {};
    if (expected == 0) {
        *ok = true;
        return {};
    }
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK) return {};
    QByteArray out(expected, Qt::Uninitialized);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    const int r = ::inflate(&zs, Z_FINISH);
    *ok = r == Z_STREAM_END && qsizetype(zs.total_out) == expected;
    inflateEnd(&zs);
    return out;
}

void ZipWriter::add(const QString &name, const QByteArray &data, bool compress)
{
    const QByteArray n = name.toUtf8();
    QByteArray packed = compress ? deflateRaw(data) : QByteArray();
    const bool deflated = !packed.isEmpty() && packed.size() < data.size();
    if (!deflated) packed = data;
    Entry e{name, crc32(data), quint32(data.size()), quint32(packed.size()), quint16(deflated ? 8 : 0), quint32(m_out.size())};
    put32(m_out, 0x04034b50);
    put16(m_out, 20);        // version needed
    put16(m_out, 0x0800);    // UTF-8 names
    put16(m_out, e.method);  // stored or deflated
    put16(m_out, 0); put16(m_out, 0x21); // time, date (1980-01-01)
    put32(m_out, e.crc);
    put32(m_out, e.csize);
    put32(m_out, e.size);
    put16(m_out, quint16(n.size()));
    put16(m_out, 0);
    m_out.append(n);
    m_out.append(packed);
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
        put16(m_out, e.method);
        put16(m_out, 0); put16(m_out, 0x21);
        put32(m_out, e.crc);
        put32(m_out, e.csize);
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

// Values past the end read as 0: a damaged file can't make these read
// outside the data, whatever the caller checked.
static quint16 get16(const QByteArray &a, qsizetype at) { return at >= 0 && at + 2 <= a.size() ? qFromLittleEndian<quint16>(a.constData() + at) : 0; }
static quint32 get32(const QByteArray &a, qsizetype at) { return at >= 0 && at + 4 <= a.size() ? qFromLittleEndian<quint32>(a.constData() + at) : 0; }

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
        const quint32 crc = get32(zip, p + 16), csize = get32(zip, p + 20), usize = get32(zip, p + 24);
        const quint16 nlen = get16(zip, p + 28), xlen = get16(zip, p + 30), clen = get16(zip, p + 32);
        const quint32 local = get32(zip, p + 42);
        // The entry's name, extra field and comment must fit in the file too.
        if (p + 46 + qsizetype(nlen) + xlen + clen > zip.size()) return fail("The publication's file directory is damaged.");
        const QString name = QString::fromUtf8(zip.constData() + p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (qsizetype(local) + 30 > zip.size()) return fail("The publication is truncated.");
        const qsizetype data = local + 30 + get16(zip, local + 26) + get16(zip, local + 28);
        if (method != 0 && method != 8) return fail("The publication uses a compression method JeffPub cannot read.");
        if (data + qsizetype(csize) > zip.size()) return fail("The publication is truncated.");
        if (method == 8) {
            bool ok = false;
            const QByteArray inflated = inflateRaw(zip.mid(data, csize), qsizetype(usize), &ok);
            if (!ok || crc32(inflated) != crc) return fail("The publication is damaged.");
            out.insert(name, inflated);
        } else {
            out.insert(name, zip.mid(data, csize));
        }
    }
    return true;
}

QByteArray readZipEntry(const QString &path, const QString &name)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 22) return {};
    // The end-of-directory record sits in the last 65,557 bytes.
    const qint64 tailAt = std::max<qint64>(0, f.size() - 65557);
    f.seek(tailAt);
    const QByteArray tail = f.read(f.size() - tailAt);
    qsizetype eocd = -1;
    for (qsizetype i = tail.size() - 22; i >= 0; --i)
        if (get32(tail, i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return {};
    const quint32 dirSize = get32(tail, eocd + 12), dirAt = get32(tail, eocd + 16);
    if (qint64(dirAt) + dirSize > f.size() || dirSize > 64 * 1024 * 1024) return {};
    f.seek(dirAt);
    const QByteArray dir = f.read(dirSize);
    for (qsizetype p = 0; p + 46 <= dir.size() && get32(dir, p) == 0x02014b50;) {
        const quint16 method = get16(dir, p + 10), nlen = get16(dir, p + 28), xlen = get16(dir, p + 30), clen = get16(dir, p + 32);
        const quint32 csize = get32(dir, p + 20), local = get32(dir, p + 42);
        if (p + 46 + qsizetype(nlen) + xlen + clen > dir.size()) return {};
        const QString entry = QString::fromUtf8(dir.constData() + p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (entry != name) continue;
        if (method != 0 || !f.seek(local)) return {};
        const QByteArray head = f.read(30);
        if (head.size() < 30) return {};
        const qint64 data = qint64(local) + 30 + get16(head, 26) + get16(head, 28);
        if (data + csize > f.size() || !f.seek(data)) return {};
        return f.read(csize);
    }
    return {};
}

} // namespace jp
