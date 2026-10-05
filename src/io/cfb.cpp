#include "io/cfb.h"

#include <QStringList>
#include <QtEndian>
#include <algorithm>
#include <functional>

namespace jp::cfb {

namespace {

constexpr quint32 FREESECT = 0xFFFFFFFF, ENDOFCHAIN = 0xFFFFFFFE, FATSECT = 0xFFFFFFFD, DIFSECT = 0xFFFFFFFC, NOSTREAM = 0xFFFFFFFF;
const QByteArray kSignature = QByteArray::fromHex("D0CF11E0A1B11AE1");

quint16 u16(const QByteArray &b, qsizetype o) { return o + 2 <= b.size() ? qFromLittleEndian<quint16>(b.constData() + o) : 0; }
quint32 u32(const QByteArray &b, qsizetype o) { return o + 4 <= b.size() ? qFromLittleEndian<quint32>(b.constData() + o) : 0; }
quint64 u64(const QByteArray &b, qsizetype o) { return o + 8 <= b.size() ? qFromLittleEndian<quint64>(b.constData() + o) : 0; }

void put16(QByteArray &b, qsizetype o, quint16 v) { qToLittleEndian(v, b.data() + o); }
void put32(QByteArray &b, qsizetype o, quint32 v) { qToLittleEndian(v, b.data() + o); }
void put64(QByteArray &b, qsizetype o, quint64 v) { qToLittleEndian(v, b.data() + o); }

// Directory order inside a storage: shorter names first, then by upper case.
bool nameLess(const QString &a, const QString &b)
{
    if (a.size() != b.size()) return a.size() < b.size();
    return a.toUpper() < b.toUpper();
}

} // namespace

// ---------------- File ----------------
int File::find(const QString &path) const
{
    if (entries.isEmpty()) return -1;
    int cur = 0;
    for (const QString &part : path.split('/', Qt::SkipEmptyParts)) {
        int next = -1;
        for (int c : entries[cur].children)
            if (entries[c].name.compare(part, Qt::CaseInsensitive) == 0) { next = c; break; }
        if (next < 0) return -1;
        cur = next;
    }
    return cur;
}

QByteArray File::stream(const QString &path) const
{
    const int i = find(path);
    return i >= 0 && entries[i].type == Entry::Stream ? entries[i].data : QByteArray();
}

bool File::setStream(const QString &path, const QByteArray &data)
{
    const int i = find(path);
    if (i < 0 || entries[i].type != Entry::Stream) return false;
    entries[i].data = data;
    return true;
}

QStringList File::streamPaths() const
{
    QStringList out;
    std::function<void(int, const QString &)> walk = [&](int i, const QString &prefix) {
        for (int c : entries[i].children) {
            const QString p = prefix.isEmpty() ? entries[c].name : prefix + '/' + entries[c].name;
            if (entries[c].type == Entry::Stream) out << p;
            else walk(c, p);
        }
    };
    if (!entries.isEmpty()) walk(0, QString());
    return out;
}

// ---------------- read ----------------
bool read(const QByteArray &bytes, File *out, QString *error)
{
    auto fail = [&](const char *m) { if (error) *error = QString::fromLatin1(m); return false; };
    if (bytes.size() < 512 || !bytes.startsWith(kSignature)) return fail("not a compound file");
    const int shift = u16(bytes, 30);
    if (shift != 9 && shift != 12) return fail("unsupported sector size");
    const qsizetype ss = qsizetype(1) << shift;
    const int miniShift = u16(bytes, 32);
    const qsizetype mss = qsizetype(1) << miniShift;
    const quint32 numFat = u32(bytes, 44), firstDir = u32(bytes, 48), cutoff = u32(bytes, 56);
    const quint32 firstMiniFat = u32(bytes, 60), firstDifat = u32(bytes, 68), numDifat = u32(bytes, 72);
    const qsizetype sectorCount = (bytes.size() - ss + ss - 1) / ss;
    auto sector = [&](quint32 n) { return bytes.mid((qsizetype(n) + 1) * ss, ss); };

    // DIFAT: 109 entries in the header, the rest in a chain of DIFAT sectors.
    QVector<quint32> difat;
    for (int i = 0; i < 109; ++i) difat << u32(bytes, 76 + i * 4);
    quint32 d = firstDifat;
    for (quint32 k = 0; k < numDifat && d < quint32(sectorCount); ++k) {
        const QByteArray s = sector(d);
        for (qsizetype i = 0; i < ss / 4 - 1; ++i) difat << u32(s, i * 4);
        d = u32(s, ss - 4);
    }
    QVector<quint32> fat;
    for (quint32 i = 0, n = 0; i < quint32(difat.size()) && n < numFat; ++i) {
        if (difat[i] == FREESECT || difat[i] >= quint32(sectorCount)) continue;
        const QByteArray s = sector(difat[i]);
        for (qsizetype k = 0; k < ss / 4; ++k) fat << u32(s, k * 4);
        ++n;
    }
    auto chain = [&](quint32 start, const QVector<quint32> &table) {
        QVector<quint32> c;
        quint32 cur = start;
        while (cur < quint32(table.size()) && c.size() <= table.size()) {
            c << cur;
            cur = table[cur];
        }
        return c;
    };
    auto readChain = [&](quint32 start) {
        QByteArray r;
        for (quint32 n : chain(start, fat)) r += sector(n);
        return r;
    };
    const QByteArray dir = readChain(firstDir);
    if (dir.size() < 128) return fail("no directory");
    QVector<quint32> miniFat;
    if (firstMiniFat != ENDOFCHAIN) {
        const QByteArray mf = readChain(firstMiniFat);
        for (qsizetype k = 0; k + 4 <= mf.size(); k += 4) miniFat << u32(mf, k);
    }
    const quint32 rootStart = u32(dir, 116);
    const QByteArray ministream = readChain(rootStart).left(qsizetype(u64(dir, 120)));

    struct Raw { int type; QString name; quint32 left, right, child; QByteArray clsid; quint32 state; quint64 c, m; quint32 start; quint64 size; };
    QVector<Raw> raw;
    for (qsizetype o = 0; o + 128 <= dir.size(); o += 128) {
        Raw r;
        const int nameLen = std::clamp(int(u16(dir, o + 64)), 0, 64);
        r.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(dir.constData() + o), std::max(0, nameLen / 2 - 1));
        r.type = quint8(dir[o + 66]);
        r.left = u32(dir, o + 68);
        r.right = u32(dir, o + 72);
        r.child = u32(dir, o + 76);
        r.clsid = dir.mid(o + 80, 16);
        r.state = u32(dir, o + 96);
        r.c = u64(dir, o + 100);
        r.m = u64(dir, o + 108);
        r.start = u32(dir, o + 116);
        r.size = ss == 512 ? u32(dir, o + 120) : u64(dir, o + 120);
        raw << r;
    }
    if (raw.isEmpty() || raw[0].type != Entry::Root) return fail("no root entry");

    File f;
    QVector<int> map(raw.size(), -1);
    std::function<void(quint32, QVector<quint32> &)> siblings = [&](quint32 n, QVector<quint32> &acc) {
        if (n == NOSTREAM || n >= quint32(raw.size()) || acc.size() > raw.size()) return;
        siblings(raw[n].left, acc);
        acc << n;
        siblings(raw[n].right, acc);
    };
    std::function<int(quint32)> add = [&](quint32 n) -> int {
        const Raw &r = raw[n];
        Entry e;
        e.type = Entry::Type(r.type);
        e.name = r.name;
        e.clsid = r.clsid;
        e.stateBits = r.state;
        e.created = r.c;
        e.modified = r.m;
        if (r.type == Entry::Stream) {
            if (r.size < cutoff) {
                QByteArray data;
                for (quint32 m : chain(r.start, miniFat)) data += ministream.mid(qsizetype(m) * mss, mss);
                e.data = data.left(qsizetype(r.size));
            } else {
                e.data = readChain(r.start).left(qsizetype(r.size));
            }
        }
        const int idx = int(f.entries.size());
        map[n] = idx;
        f.entries << e;
        if (r.type == Entry::Storage || r.type == Entry::Root) {
            QVector<quint32> kids;
            siblings(r.child, kids);
            for (quint32 k : kids) {
                if (map[k] >= 0 || (raw[k].type != Entry::Storage && raw[k].type != Entry::Stream)) continue;
                const int ci = add(k);
                f.entries[idx].children << ci;
            }
        }
        return idx;
    };
    add(0);
    *out = f;
    return true;
}

// ---------------- write ----------------
QByteArray write(const File &f)
{
    const qsizetype ss = 512, mss = 64;
    const quint32 cutoff = 4096;
    const int n = int(f.entries.size());

    // Small streams go in the mini stream, large ones in regular sectors.
    QByteArray mini;
    QVector<quint32> miniFat;
    QVector<quint32> start(n, ENDOFCHAIN);
    QVector<int> large;
    for (int i = 0; i < n; ++i) {
        const Entry &e = f.entries[i];
        if (e.type != Entry::Stream || e.data.isEmpty()) continue;
        if (quint32(e.data.size()) >= cutoff) { large << i; continue; }
        const quint32 first = quint32(mini.size() / mss);
        const quint32 count = quint32((e.data.size() + mss - 1) / mss);
        start[i] = first;
        for (quint32 k = 0; k < count; ++k) miniFat << (k + 1 < count ? first + k + 1 : ENDOFCHAIN);
        QByteArray d = e.data;
        d.append(QByteArray(count * mss - d.size(), '\0'));
        mini += d;
    }

    const quint32 nDir = quint32((qsizetype(n) * 128 + ss - 1) / ss);
    const quint32 nMiniFat = quint32((qsizetype(miniFat.size()) * 4 + ss - 1) / ss);
    const quint32 nMini = quint32((mini.size() + ss - 1) / ss);
    quint32 nLarge = 0;
    for (int i : large) nLarge += quint32((f.entries[i].data.size() + ss - 1) / ss);
    const quint32 dataSectors = nDir + nMiniFat + nMini + nLarge;
    quint32 nFat = 1, nDifat = 0;
    for (;;) {
        nDifat = nFat > 109 ? (nFat - 109 + 126) / 127 : 0;
        if (quint64(nFat) * (ss / 4) >= quint64(dataSectors) + nFat + nDifat) break;
        ++nFat;
    }

    QVector<quint32> fat(nFat * (ss / 4), FREESECT);
    quint32 next = 0;
    auto alloc = [&](quint32 count) {
        const quint32 first = next;
        for (quint32 k = 0; k < count; ++k) fat[first + k] = k + 1 < count ? first + k + 1 : ENDOFCHAIN;
        next += count;
        return count ? first : ENDOFCHAIN;
    };
    const quint32 dirStart = alloc(nDir);
    const quint32 miniFatStart = alloc(nMiniFat);
    const quint32 miniStart = alloc(nMini);
    for (int i : large) start[i] = alloc(quint32((f.entries[i].data.size() + ss - 1) / ss));
    const quint32 fatStart = next;
    for (quint32 k = 0; k < nFat; ++k) fat[next++] = FATSECT;
    const quint32 difatStart = nDifat ? next : ENDOFCHAIN;
    for (quint32 k = 0; k < nDifat; ++k) fat[next++] = DIFSECT;

    // Each storage's children form a red-black tree: balanced by median split,
    // with the bottom level red when the tree isn't perfect, so every path has
    // the same number of black nodes.
    QVector<quint32> left(n, NOSTREAM), right(n, NOSTREAM), child(n, NOSTREAM);
    QVector<quint8> color(n, 1);   // 1 black, 0 red
    for (int s = 0; s < n; ++s) {
        QVector<int> kids = f.entries[s].children;
        if (kids.isEmpty()) continue;
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return nameLess(f.entries[a].name, f.entries[b].name); });
        int h = 0;
        while ((1 << h) - 1 < kids.size()) ++h;
        const bool perfect = (1 << h) - 1 == kids.size();
        std::function<int(int, int, int)> build = [&](int lo, int hi, int depth) -> int {
            if (lo > hi) return -1;
            const int mid = (lo + hi) / 2;
            const int node = kids[mid];
            const int l = build(lo, mid - 1, depth + 1), r = build(mid + 1, hi, depth + 1);
            left[node] = l < 0 ? NOSTREAM : quint32(l);
            right[node] = r < 0 ? NOSTREAM : quint32(r);
            color[node] = (!perfect && depth == h - 1) ? 0 : 1;
            return node;
        };
        child[s] = quint32(build(0, int(kids.size()) - 1, 0));
    }

    QByteArray out((qsizetype(next) + 1) * ss, '\0');
    // Header.
    out.replace(0, 8, kSignature);
    put16(out, 24, 0x003E);
    put16(out, 26, 0x0003);
    put16(out, 28, 0xFFFE);
    put16(out, 30, 9);
    put16(out, 32, 6);
    put32(out, 44, nFat);
    put32(out, 48, dirStart);
    put32(out, 56, cutoff);
    put32(out, 60, nMiniFat ? miniFatStart : ENDOFCHAIN);
    put32(out, 64, nMiniFat);
    put32(out, 68, difatStart);
    put32(out, 72, nDifat);
    for (int i = 0; i < 109; ++i) put32(out, 76 + i * 4, quint32(i) < nFat ? fatStart + quint32(i) : FREESECT);
    auto sectorOffset = [&](quint32 s) { return (qsizetype(s) + 1) * ss; };

    // Directory.
    QByteArray dir(qsizetype(nDir) * ss, '\0');
    for (qsizetype o = 0; o < dir.size(); o += 128) {
        put32(dir, o + 68, NOSTREAM);
        put32(dir, o + 72, NOSTREAM);
        put32(dir, o + 76, NOSTREAM);
    }
    for (int i = 0; i < n; ++i) {
        const Entry &e = f.entries[i];
        const qsizetype o = qsizetype(i) * 128;
        const QString name = e.name.left(31);
        for (int k = 0; k < name.size(); ++k) put16(dir, o + k * 2, name[k].unicode());
        put16(dir, o + 64, quint16((name.size() + 1) * 2));
        dir[o + 66] = char(e.type);
        dir[o + 67] = char(color[i]);
        put32(dir, o + 68, left[i]);
        put32(dir, o + 72, right[i]);
        put32(dir, o + 76, child[i]);
        if (e.clsid.size() == 16) dir.replace(o + 80, 16, e.clsid);
        put32(dir, o + 96, e.stateBits);
        put64(dir, o + 100, e.type == Entry::Root ? 0 : e.created);
        put64(dir, o + 108, e.modified);
        if (e.type == Entry::Root) {
            put32(dir, o + 116, nMini ? miniStart : ENDOFCHAIN);
            put64(dir, o + 120, quint64(mini.size()));
        } else if (e.type == Entry::Stream) {
            put32(dir, o + 116, start[i]);
            put64(dir, o + 120, quint64(e.data.size()));
        } else {
            put32(dir, o + 116, 0);
            put64(dir, o + 120, 0);
        }
    }
    out.replace(sectorOffset(dirStart), dir.size(), dir);
    // Mini FAT and mini stream.
    if (nMiniFat) {
        QByteArray mf(qsizetype(nMiniFat) * ss, '\xFF');
        for (int k = 0; k < miniFat.size(); ++k) put32(mf, k * 4, miniFat[k]);
        out.replace(sectorOffset(miniFatStart), mf.size(), mf);
    }
    if (nMini) out.replace(sectorOffset(miniStart), mini.size(), mini);
    // Large streams.
    for (int i : large) out.replace(sectorOffset(start[i]), f.entries[i].data.size(), f.entries[i].data);
    // FAT.
    for (quint32 k = 0; k < quint32(fat.size()); ++k) put32(out, sectorOffset(fatStart) + qsizetype(k) * 4, fat[k]);
    // DIFAT sectors for FAT sectors beyond the header's 109.
    for (quint32 k = 0; k < nDifat; ++k) {
        const qsizetype o = sectorOffset(difatStart + k);
        for (int j = 0; j < 127; ++j) {
            const quint32 idx = 109 + k * 127 + quint32(j);
            put32(out, o + j * 4, idx < nFat ? fatStart + idx : FREESECT);
        }
        put32(out, o + 508, k + 1 < nDifat ? difatStart + k + 1 : ENDOFCHAIN);
    }
    return out;
}

} // namespace jp::cfb
