// Saving .pub files (version 2003 and later). docs/pub-format.md describes
// the structures; the values here match files written by other programs,
// checked with tools/pubdump.py. The first version writes pages, text boxes
// with formatted text, and rectangles and ovals; other objects are reported
// as not saved yet.

#include "io/importers.h"

#include "core/document.h"
#include "core/items.h"
#include "core/fonts.h"
#include "io/cfb.h"
#include "io/pubshapes.h"
#include "render/renderer.h"
#include "render/shapes.h"
#include "text/textprops.h"

#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QHash>
#include <QSet>
#include <QMap>
#include <QRandomGenerator>
#include <QRawFont>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QtEndian>

#include <cmath>
#include <functional>

namespace jp {
namespace {

constexpr double kEmuPerPt = 12700.0;
inline qint64 emu(double pt) { return qint64(std::llround(pt * kEmuPerPt)); }

// ---------------------------------------------------------------- blocks
// A block is id, type, then data whose size the type decides; container
// types carry a u32 length and nested blocks (or raw bytes).
struct B {
    quint8 id = 0, type = 0;
    QByteArray data;     // fixed payload; raw container payload; string bytes
    QVector<B> kids;
};

int fixedSize(quint8 type)
{
    switch (type) {
    case 0x00: case 0x02: case 0x05: case 0x08: case 0x0a: case 0x78: return 0;
    case 0x07: case 0x10: case 0x12: case 0x18: case 0x1a: return 2;
    case 0x20: case 0x22: case 0x58: case 0x68: case 0x70: case 0xb8: return 4;
    case 0x28: return 8;
    case 0x38: return 16;
    case 0x48: return 24;
    default: return -1;   // 0x80 0x82 0x88 0x8a 0x90 0x98 0xa0 0xc0
    }
}

void putU16(QByteArray &o, quint32 v) { o.append(char(v & 0xff)); o.append(char((v >> 8) & 0xff)); }
void putU32(QByteArray &o, quint32 v) { for (int i = 0; i < 4; ++i) o.append(char((v >> (8 * i)) & 0xff)); }
void setU32(QByteArray &o, int at, quint32 v) { for (int i = 0; i < 4; ++i) o[at + i] = char((v >> (8 * i)) & 0xff); }
void setU16(QByteArray &o, int at, quint32 v) { o[at] = char(v & 0xff); o[at + 1] = char((v >> 8) & 0xff); }

B fixedB(quint8 id, quint8 type, quint64 v)
{
    B b;
    b.id = id;
    b.type = type;
    const int n = fixedSize(type);
    for (int i = 0; i < n; ++i) b.data.append(char((v >> (8 * i)) & 0xff));
    return b;
}
B flag(quint8 id, quint8 type = 0x08) { return fixedB(id, type, 0); }
B u16(quint8 id, quint32 v, quint8 type = 0x18) { return fixedB(id, type, v); }
B u32(quint8 id, quint32 v, quint8 type = 0x20) { return fixedB(id, type, v); }
B ref(quint8 id, quint32 seq, quint8 type = 0x70) { return fixedB(id, type, seq); }
B bytesB(quint8 id, quint8 type, const QByteArray &d)
{
    B b;
    b.id = id;
    b.type = type;
    b.data = d.leftJustified(fixedSize(type), '\0', true);
    return b;
}
// Text with a closing zero, as drawing properties keep it (UTF-16).
QByteArray utf16z(const QString &str)
{
    QByteArray b;
    for (QChar ch : str) putU16(b, ch.unicode());
    putU16(b, 0);
    return b;
}

B str(quint8 id, const QString &s)
{
    B b;
    b.id = id;
    b.type = 0xc0;
    for (QChar c : s) putU16(b.data, c.unicode());
    putU16(b.data, 0);
    return b;
}
B rec(quint8 id, QVector<B> kids, quint8 type = 0x88)
{
    B b;
    b.id = id;
    b.type = type;
    b.kids = std::move(kids);
    return b;
}
B rawCont(quint8 id, quint8 type, const QByteArray &raw)
{
    B b;
    b.id = id;
    b.type = type;
    b.data = raw;
    return b;
}
// A list (0xa0, 0x90, 0x98): items are blocks with id 0.
B list(quint8 id, QVector<B> items, quint8 type = 0xa0) { return rec(id, std::move(items), type); }

void encode(const B &b, QByteArray &out)
{
    out.append(char(b.id));
    out.append(char(b.type));
    if (fixedSize(b.type) >= 0) {
        out.append(b.data);
        return;
    }
    QByteArray body;
    if (!b.kids.isEmpty()) {
        for (const B &k : b.kids) encode(k, body);
    } else {
        body = b.data;
    }
    putU32(out, quint32(body.size() + 4));
    out.append(body);
}
QByteArray encodeAll(const QVector<B> &bs)
{
    QByteArray out;
    for (const B &b : bs) encode(b, out);
    return out;
}
// A length-prefixed run of blocks (chunks, property blocks).
QByteArray lengthPrefixed(const QVector<B> &bs)
{
    QByteArray body = encodeAll(bs);
    QByteArray out;
    putU32(out, quint32(body.size() + 4));
    out.append(body);
    return out;
}

// ---------------------------------------------------------------- Contents
struct Chunk {
    quint16 type = 0;
    quint32 parent = 0;   // 0: none
    QVector<B> body;
    int dirVer = -1;      // directory stamps when they differ from the type's
    int dirB = -1;
};

// Directory stamps for each chunk type, as every .pub file writes them.
struct DirInfo {
    quint16 ver = 0x0102;
    bool flag08 = true;
    int v0b = -1;
};
DirInfo dirInfo(quint16 type, bool nonEmpty)
{
    switch (type) {
    case 0x44: return {0x1002, true, 0x17};
    case 0x43: return {0x0102, true, 1};
    case 0x60: return {0x0102, false, nonEmpty ? 1 : -1};
    case 0x77: return {0x0102, false, 1};
    case 0x5b: case 0x5c: case 0x4f: case 0x4a: case 0x6c: return {0x0102, true, 1};
    case 0x61: case 0x65: return {0x0102, true, nonEmpty ? 1 : -1};
    case 0x4b: case 0x8a: return {0x0102, true, 0x14};
    case 0x4c: return {0x0102, true, 0x0f};
    case 0x01: return {0x0103, false, 0x16};
    case 0x20: case 0x10: return {0x0102, false, 0x16};
    case 0x63: return {0x0102, true, 1};
    case 0x66: return {0x0102, false, 1};
    default: return {0x0102, true, -1};
    }
}

// Record-type versions written at the start of every Contents stream:
// {id, version (field 03; 0xffffffff = none), flags (bits: 06 07 08 0a 0b),
//  field 09 (-1 none), field 0c (-1 none)}.
struct TypeEntry { int id; quint32 v03; int flags; int v09; qint64 v0c; };
const TypeEntry kTypeTable[] = {
#include "pubtypetable.inc"
};

class ContentsWriter {
public:
    void put(quint32 seq, Chunk c) { m_chunks[seq] = std::move(c); }
    // Adds fields to a chunk already put, keeping its fields in id order.
    void extend(quint32 seq, const QVector<B> &fields)
    {
        QVector<B> &body = m_chunks[seq].body;
        body << fields;
        std::stable_sort(body.begin(), body.end(), [](const B &a, const B &b) { return a.id < b.id; });
    }
    QByteArray build(const QString &path) const;

private:
    QMap<quint32, Chunk> m_chunks;
};

QByteArray ContentsWriter::build(const QString &path) const
{
    const quint32 version = 0x03e8001a, build = 0x4f66;
    // File information: lengths and offsets are patched once known.
    QVector<B> info = {u16(0x01, 0xace8), ref(0x02, 256, 0x68), u32(0x03, version), u32(0x04, version), u32(0x05, version),
                       u32(0x07, version), u32(0x08, 0), str(0x09, QDir::toNativeSeparators(path)), u32(0x0a, 0, 0xb8),
                       u32(0x0b, 0, 0xb8), u16(0x0c, 1), u32(0x0d, build), u32(0x0e, build), u32(0x0f, build), u32(0x10, build)};
    const int infoLen = encodeAll(info).size() + 4;
    const quint32 infoEnd = 0x2c + quint32(infoLen);

    // Record-type table.
    QVector<B> rows;
    for (const TypeEntry &e : kTypeTable) {
        QVector<B> f{u16(0x01, quint32(e.id))};
        if (e.v03 != 0xffffffffu) f << u32(0x03, e.v03);
        if (e.flags & 1) f << flag(0x06);
        if (e.flags & 2) f << flag(0x07);
        if (e.flags & 4) f << flag(0x08);
        if (e.v09 >= 0) f << u16(0x09, quint32(e.v09), 0x10);
        if (e.flags & 8) f << flag(0x0a);
        if (e.flags & 16) f << flag(0x0b);
        if (e.v0c >= 0) f << u32(0x0c, quint32(e.v0c));
        rows << rec(0x00, f);
    }
    const QByteArray table = lengthPrefixed({u32(0x01, quint32(std::size(kTypeTable))), u32(0x02, version), list(0x03, rows, 0x90)});

    QByteArray out(0x1e, '\0');
    // Header: magic, versions, stream length (0x08) and trailer offset (0x1a).
    setU16(out, 0x00, 0xace8);
    setU16(out, 0x02, 0x002c);
    setU16(out, 0x04, 0x03e8);
    setU16(out, 0x06, 0x000a);
    setU16(out, 0x0c, 0x001a);
    setU16(out, 0x0e, 0x0100);
    putU32(out, infoEnd);
    putU16(out, 0x10);
    putU16(out, 0x10);
    out.append(QByteArray(6, '\0'));
    const int infoAt = out.size();   // 0x2c
    out.append(lengthPrefixed(info));
    out.append(table);

    // Chunks in sequence order, then the directory.
    QMap<quint32, quint32> offsets;
    for (auto it = m_chunks.cbegin(); it != m_chunks.cend(); ++it) {
        offsets[it.key()] = quint32(out.size());
        out.append(lengthPrefixed(it.value().body));
    }
    const quint32 last = m_chunks.isEmpty() ? 256 : m_chunks.lastKey();
    QVector<B> dir;
    for (quint32 s = 0; s <= last; ++s) {
        auto it = m_chunks.constFind(s);
        if (it == m_chunks.cend()) {
            dir << flag(0x00, 0x78);
            continue;
        }
        DirInfo d = dirInfo(it->type, !it->body.isEmpty());
        if (it->dirVer >= 0) d.ver = quint16(it->dirVer);
        if (it->dirB >= 0) d.v0b = it->dirB;
        QVector<B> f{u16(0x02, it->type), u32(0x04, offsets[s], 0xb8)};
        if (it->parent) f << ref(0x05, it->parent, 0x68);
        f << u16(0x06, d.ver, 0x10);
        if (d.flag08) f << flag(0x08);
        if (d.v0b >= 0) f << u16(0x0b, quint32(d.v0b));
        dir << rec(0x00, f);
    }
    const quint32 trailerAt = quint32(out.size());
    out.append(lengthPrefixed({u32(0x01, last + 1), u32(0x02, last), list(0x03, dir, 0x90)}));

    // Patch lengths and offsets.
    setU32(out, 0x08, quint32(out.size()));
    setU32(out, 0x1a, trailerAt);
    QByteArray patched = lengthPrefixed({u16(0x01, 0xace8), ref(0x02, 256, 0x68), u32(0x03, version), u32(0x04, version),
                                         u32(0x05, version), u32(0x07, version), u32(0x08, quint32(out.size())),
                                         str(0x09, QDir::toNativeSeparators(path)), u32(0x0a, trailerAt, 0xb8), u32(0x0b, infoEnd, 0xb8),
                                         u16(0x0c, 1), u32(0x0d, build), u32(0x0e, build), u32(0x0f, build), u32(0x10, build)});
    out.replace(infoAt, patched.size(), patched);
    return out;
}

// ---------------------------------------------------------------- Escher
// A drawing property. Complex ones (a name, say) carry their bytes, which
// follow the property table; the value is then their length.
struct Prop { quint16 id; quint32 value; QByteArray complex = {}; };

QByteArray escherRecord(quint16 ver, quint16 inst, quint16 type, const QByteArray &body)
{
    QByteArray o;
    putU16(o, quint32(ver | (inst << 4)));
    putU16(o, type);
    putU32(o, quint32(body.size()));
    o.append(body);
    return o;
}
QByteArray escherContainer(quint16 type, const QByteArray &body) { return escherRecord(0xf, 0, type, body); }
QByteArray escherProps(quint16 type, QVector<Prop> props)
{
    // A property given twice keeps its last value (a shadow overriding the default).
    QVector<Prop> unique;
    for (int i = props.size() - 1; i >= 0; --i) {
        bool seen = false;
        for (const Prop &u : unique) seen = seen || (u.id & 0x3fff) == (props[i].id & 0x3fff);
        if (!seen) unique << props[i];
    }
    props = unique;
    std::sort(props.begin(), props.end(), [](const Prop &a, const Prop &b) { return (a.id & 0x3fff) < (b.id & 0x3fff); });
    QByteArray body, extra;
    for (const Prop &p : props) {
        putU16(body, p.id);
        putU32(body, p.complex.isEmpty() ? p.value : quint32(p.complex.size()));
        extra += p.complex;
    }
    return escherRecord(0x3, quint16(props.size()), type, body + extra);
}
// Client data and anchors carry blocks after a u32 length (version 0xA, as
// Publisher writes them).
QByteArray clientBlocks(quint16 type, const QVector<B> &blocks) { return escherRecord(0xa, 0x1a, type, lengthPrefixed(blocks)); }

const QVector<Prop> kSideLines = {{0x0540, 0x08000000}, {0x0542, 0x08000007}, {0x0580, 0x08000000}, {0x0582, 0x08000007},
                                  {0x05c0, 0x08000000}, {0x05c2, 0x08000007}, {0x0600, 0x08000000}, {0x0602, 0x08000007},
                                  {0x0640, 0x08000000}, {0x0642, 0x08000007}};
const QVector<Prop> kShadowFlags = {{0x0295, 0}, {0x0296, 0}, {0x0297, 0}, {0x0298, 0}, {0x0299, 0}, {0x029a, 0}};

quint32 bgr(const QColor &c) { return quint32(c.red()) | (quint32(c.green()) << 8) | (quint32(c.blue()) << 16); }

// A freeform outline: the frame's size as the coordinate space (EMU), the
// points (pairs of 32-bit numbers) and the path's segments: 0x4000 move,
// n lines, 0x2000 + n curves (three points each), 0x6001 close, 0x8000 end;
// runs stay under 256, as readers take the count from the low byte.
void freeformProps(QVector<Prop> &opt, const QPainterPath &path, const QSizeF &size)
{
    QByteArray verts, segs;
    int nv = 0, ns = 0;
    auto vert = [&](const QPointF &p) {
        putU32(verts, quint32(qint32(emu(p.x()))));
        putU32(verts, quint32(qint32(emu(p.y()))));
        ++nv;
    };
    auto seg = [&](quint32 v) {
        putU16(segs, v);
        ++ns;
    };
    QPointF start, last;
    bool open = false;
    auto closeIfShut = [&] {
        if (open && QLineF(start, last).length() < 1e-3) seg(0x6001);
        open = false;
    };
    const int n = path.elementCount();
    for (int i = 0; i < n;) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) {
            closeIfShut();
            seg(0x4000);
            vert(e);
            start = last = e;
            open = true;
            ++i;
        } else if (e.type == QPainterPath::LineToElement) {
            int k = 0;
            for (; i < n && k < 0xff && path.elementAt(i).type == QPainterPath::LineToElement; ++i, ++k) {
                vert(path.elementAt(i));
                last = path.elementAt(i);
            }
            seg(quint32(k));
        } else if (e.type == QPainterPath::CurveToElement) {
            int k = 0;
            for (; i + 2 < n && k < 0xff && path.elementAt(i).type == QPainterPath::CurveToElement; i += 3, ++k) {
                vert(path.elementAt(i));
                vert(path.elementAt(i + 1));
                vert(path.elementAt(i + 2));
                last = path.elementAt(i + 2);
            }
            seg(0x2000 | quint32(k));
        } else {
            ++i;
        }
    }
    closeIfShut();
    seg(0x8000);
    auto array = [](const QByteArray &data, int count, int size) {
        QByteArray a;
        putU16(a, quint32(count));
        putU16(a, quint32(count));
        putU16(a, quint32(size));
        return a + data;
    };
    opt << Prop{0x0142, quint32(emu(size.width()))} << Prop{0x0143, quint32(emu(size.height()))} << Prop{0x0144, 4}
        << Prop{0xc145, 0, array(verts, nv, 8)} << Prop{0xc146, 0, array(segs, ns, 2)};
}

// ---------------------------------------------------------------- Quill
struct Section { QByteArray name, kind; quint16 id = 0; QByteArray data; bool align = false; };

QByteArray quillStream(QVector<Section> secs)
{
    // The index of sections, then the sections from offset 512. The index
    // starts in the first block (19 entries, clear of the text at 512) and
    // goes on in 512-byte blocks of 20 at the end of the stream, each naming
    // the next, as Publisher writes it. Publisher refuses an index that runs
    // past its block.
    constexpr int kFirst = 19, kPerBlock = 20;
    QByteArray out(512, '\0');
    int at = 512;
    QVector<QPair<int, int>> places;
    QByteArray body;
    for (Section &s : secs) {
        if (s.align) while ((at + body.size()) % 512) body.append('\0');
        places << qMakePair(at + int(body.size()), int(s.data.size()));
        body.append(s.data);
    }
    out.append(body);
    while (out.size() % 512) out.append('\0');
    const int n = int(secs.size());
    const int extra = n > kFirst ? (n - kFirst + kPerBlock - 1) / kPerBlock : 0;
    const int firstExtra = int(out.size());
    out.append(QByteArray(512 * extra, '\0'));
    auto block = [&](int base, int from, int count, quint32 next) {
        setU16(out, base, 0x01f8);
        setU16(out, base + 2, quint32(count));
        setU32(out, base + 4, next);
        for (int k = 0; k < count; ++k) {
            const int i = from + k;
            const int e = base + 8 + 24 * k;
            setU16(out, e, 0x18);
            memcpy(out.data() + e + 2, secs[i].name.constData(), 4);
            setU16(out, e + 6, secs[i].id);
            setU16(out, e + 8, 1);
            memcpy(out.data() + e + 12, secs[i].kind.constData(), 4);
            setU32(out, e + 16, quint32(places[i].first));
            setU32(out, e + 20, quint32(places[i].second));
        }
    };
    memcpy(out.data(), "CHNKINK ", 8);
    setU16(out, 0x08, 4);
    setU16(out, 0x0a, 7);
    setU16(out, 0x0c, quint32(n));
    setU16(out, 0x0e, 0x0300);
    setU32(out, 0x10, quint32(512 * (1 + extra)));   // the index's blocks, all told
    setU32(out, 0x14, quint32(out.size()));
    block(0x18, 0, std::min(n, kFirst), extra ? quint32(firstExtra) : 0xffffffffu);
    for (int b = 0; b < extra; ++b) {
        const int from = kFirst + b * kPerBlock;
        block(firstExtra + 512 * b, from, std::min(kPerBlock, n - from), b + 1 < extra ? quint32(firstExtra + 512 * (b + 1)) : 0xffffffffu);
    }
    return out;
}

// A table of entries addressed by offsets (style sheets, fonts): header of
// five u32 (body length, count, a, b, c), the offsets relative to byte 20,
// then the entries.
QByteArray offsetTable(const QVector<QByteArray> &entries, quint32 a, quint32 b, quint32 c)
{
    QByteArray body;
    QVector<quint32> offs;
    const quint32 base = quint32(entries.size()) * 4;
    for (const QByteArray &e : entries) {
        offs << base + quint32(body.size());
        body.append(e);
    }
    QByteArray out;
    putU32(out, quint32(base + body.size()));
    putU32(out, quint32(entries.size()));
    putU32(out, a);
    putU32(out, b);
    putU32(out, c);
    for (quint32 o : offs) putU32(out, o);
    out.append(body);
    return out;
}

// A style sheet entry: a u16 word count (counting itself and the block),
// the property block, then two zero bytes.
QByteArray styleEntry(const QVector<B> &props)
{
    const QByteArray block = lengthPrefixed(props);
    QByteArray out;
    putU16(out, quint32((block.size() + 2) / 2));
    out.append(block);
    if (out.size() % 2) out.append('\0');
    out.append(QByteArray(2, '\0'));   // every entry is followed by two zero bytes
    return out;
}

// Formatting pages (FDPC/FDPP): 512 bytes each; a header (the run count,
// 1, and where the page's text starts: the last page's end, 0 on the
// first, as Publisher writes it), the text end offsets, the property
// offsets, and property blocks packed from the end.
struct Run { quint32 end; QByteArray props; };
QVector<QByteArray> formattingPages(const QVector<Run> &runs, QVector<quint32> *pageEnds)
{
    QVector<QByteArray> pages;
    int i = 0;
    while (i < runs.size()) {
        QByteArray pg(512, '\0');
        QVector<quint32> ends;
        QVector<quint16> offs;
        int bottom = 512;
        QHash<QByteArray, int> placed;
        while (i < runs.size()) {
            const QByteArray &p = runs[i].props;
            const int need = placed.contains(p) ? 0 : int(p.size());
            const int header = 8 + (ends.size() + 1) * 6;
            if (header + 2 > bottom - need) break;
            int at = placed.value(p, -1);
            if (at < 0) {
                bottom -= int(p.size());
                at = bottom;
                memcpy(pg.data() + at, p.constData(), size_t(p.size()));
                placed[p] = at;
            }
            ends << runs[i].end;
            offs << quint16(at);
            ++i;
        }
        setU16(pg, 0, quint32(ends.size()));
        setU16(pg, 2, 1);
        // Publisher won't open a file whose later pages start at 0.
        setU32(pg, 4, pageEnds->isEmpty() ? 0 : pageEnds->last());
        for (int k = 0; k < ends.size(); ++k) setU32(pg, 8 + 4 * k, ends[k]);
        for (int k = 0; k < offs.size(); ++k) setU16(pg, 8 + 4 * ends.size() + 2 * k, offs[k]);
        pages << pg;
        *pageEnds << ends.last();
    }
    return pages;
}

// Index of formatting pages (BTEC/BTEP).
QByteArray pageIndex(const QVector<quint32> &pageEnds, quint32 firstPageOffset)
{
    QByteArray o;
    // Page count, element size, then the text boundaries (0, then each
    // page's end) and each page's offset in the stream.
    putU32(o, quint32(pageEnds.size()));
    putU32(o, 4);
    putU32(o, 0);
    putU32(o, 0);
    for (quint32 e : pageEnds) putU32(o, e);
    for (int i = 0; i < pageEnds.size(); ++i) putU32(o, firstPageOffset + quint32(i) * 512);
    return o;
}

// ---------------------------------------------------------------- property sets
QByteArray propertySet(const QByteArray &fmtid, const QVector<QPair<quint32, QByteArray>> &props)
{
    // props: id -> typed value (VT tag + data, already padded).
    QByteArray section;
    putU32(section, 0);   // size, patched
    putU32(section, quint32(props.size()));
    int at = 8 + props.size() * 8;
    QByteArray values;
    for (const auto &p : props) {
        putU32(section, p.first);
        putU32(section, quint32(at + values.size()));
        values.append(p.second);
    }
    section.append(values);
    setU32(section, 0, quint32(section.size()));
    QByteArray out;
    putU16(out, 0xfffe);
    putU16(out, 0);
    putU32(out, 0x00020006);
    out.append(QByteArray(16, '\0'));
    putU32(out, 1);
    out.append(fmtid);
    putU32(out, 48);
    out.append(section);
    return out;
}
QByteArray vtI2(qint16 v)
{
    QByteArray o;
    putU32(o, 2);
    putU16(o, quint16(v));
    putU16(o, 0);
    return o;
}
QByteArray vtString(const QString &s)
{
    QByteArray b = s.toLatin1();
    b.append('\0');
    QByteArray o;
    putU32(o, 30);
    putU32(o, quint32(b.size()));
    o.append(b);
    while (o.size() % 4) o.append('\0');
    return o;
}
// The preview picture (summary property 17) as Publisher writes it: a
// clipboard metafile picture (format 3, mapping mode 8, its size in 0.01 mm
// at 96 dpi) whose metafile fills the window white and copies in a 24-bit
// bitmap of the first page. Checked against 282 files Publisher saved.
QByteArray vtThumbnail(const QImage &thumb)
{
    // At most 160 pixels across, as Publisher's are (and the size fields
    // are 16-bit).
    const QImage fitted = std::max(thumb.width(), thumb.height()) > 160 ? thumb.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation) : thumb;
    QImage img(fitted.size(), QImage::Format_RGB888);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.drawImage(0, 0, fitted);
    }
    const int w = img.width(), h = img.height();
    const int stride = (w * 3 + 3) & ~3;
    QByteArray dib;   // BITMAPINFOHEADER, then the rows bottom up in BGR
    putU32(dib, 40);
    putU32(dib, quint32(w));
    putU32(dib, quint32(h));
    putU16(dib, 1);
    putU16(dib, 24);
    putU32(dib, 0);
    putU32(dib, quint32(stride * h));
    for (int i = 0; i < 4; ++i) putU32(dib, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uchar *row = img.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            dib.append(char(row[x * 3 + 2]));
            dib.append(char(row[x * 3 + 1]));
            dib.append(char(row[x * 3]));
        }
        dib.append(QByteArray(stride - w * 3, '\0'));
    }
    auto record = [](quint16 function, const QByteArray &params) {
        QByteArray r;
        putU32(r, quint32((6 + params.size()) / 2));
        putU16(r, function);
        return r + params;
    };
    auto words = [](std::initializer_list<int> v) {
        QByteArray o;
        for (int x : v) putU16(o, quint16(x));
        return o;
    };
    QByteArray brush;   // solid white
    putU16(brush, 0);
    putU32(brush, 0x00ffffff);
    putU16(brush, 0);
    QByteArray patBlt, bitBlt;
    putU32(patBlt, 0x00f00021);   // PATCOPY
    patBlt += words({h, w, 0, 0});
    putU32(bitBlt, 0x00cc0020);   // SRCCOPY
    bitBlt += words({0, 0, h, w, 0, 0}) + dib;
    const QByteArray copy = record(0x0940, bitBlt);   // META_DIBBITBLT
    const QByteArray records = record(0x020b, words({0, 0})) + record(0x020c, words({h, w})) + record(0x02fc, brush) + record(0x012d, words({0})) +
                               record(0x061d, patBlt) + record(0x012d, words({0})) + copy + record(0x0000, {});
    QByteArray wmf;
    putU16(wmf, 1);        // in memory
    putU16(wmf, 9);        // header words
    putU16(wmf, 0x0300);
    putU32(wmf, quint32((18 + records.size()) / 2));
    putU16(wmf, 1);        // objects: the brush
    putU32(wmf, quint32(copy.size() / 2));
    putU16(wmf, 0);
    wmf += records;
    QByteArray cf;
    putU32(cf, 0xffffffffu);   // a Windows clipboard format
    putU32(cf, 3);             // CF_METAFILEPICT
    putU16(cf, 8);             // MM_ANISOTROPIC
    putU16(cf, quint16(std::lround(w * 2540.0 / 96)));
    putU16(cf, quint16(std::lround(h * 2540.0 / 96)));
    putU16(cf, 0);
    cf += wmf;
    QByteArray o;
    putU32(o, 0x47);   // VT_CF
    putU32(o, quint32(cf.size()));
    o += cf;
    while (o.size() % 4) o.append('\0');
    return o;
}
QByteArray guidBytes(quint32 a, quint16 b, quint16 c, const QByteArray &d8)
{
    QByteArray o;
    putU32(o, a);
    putU16(o, b);
    putU16(o, c);
    o.append(d8);
    return o;
}

// ---------------------------------------------------------------- the writer
class PubWriter {
public:
    PubWriter(const Document &doc, const QString &path, const QImage &thumbnail) : m_doc(doc), m_path(path), m_thumbnail(thumbnail) {}
    QByteArray write(QStringList *skipped);

private:
    // Fonts and text colors referenced from text.
    int fontIndex(const QString &family)
    {
        // A font the .pub this came from named keeps its name; JeffPub's own
        // look-alikes go under the standard font's name.
        const QString chosen = family.isEmpty() ? m_doc.fonts.body : family;
        const QString f = m_doc.pubFonts.contains(chosen) ? chosen : interchangeFontName(chosen);
        int i = m_fonts.indexOf(f);
        if (i < 0) {
            m_fonts << f;
            i = m_fonts.size() - 1;
        }
        return i;
    }
    int colorIndex(const QColor &c)
    {
        const quint32 v = bgr(c);
        int i = m_colors.indexOf(v);
        if (i < 0) {
            m_colors << v;
            i = m_colors.size() - 1;
        }
        return i + 2;   // entries 0 and 1 are the scheme's main color and black
    }
    QByteArray charProps(const QTextCharFormat &f, const QTextCharFormat &style = QTextCharFormat()) { return lengthPrefixed(charBlocks(f, style)); }
    QByteArray paraProps(const QTextBlock &b) { return lengthPrefixed(paraBlocks(b)); }
    // `style` is the paragraph style's character settings: bold and italic
    // in a run switch the style's own on or off, as Publisher reads them.
    QVector<B> charBlocks(const QTextCharFormat &f, const QTextCharFormat &style = QTextCharFormat());
    QVector<B> paraBlocks(const QTextBlock &b);
    // Named paragraph styles after Normal that the publication's text uses,
    // in the style sheet's order (from 1).
    const QStringList &styleNames()
    {
        if (m_styleNamesDone) return m_styleNames;
        m_styleNamesDone = true;
        QSet<QString> used;
        for (const auto &story : m_doc.stories)
            if (const QTextDocument *d = story->doc.get())
                for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) used.insert(b.blockFormat().stringProperty(tp::StyleName));
        for (const TextStyle &st : m_doc.styles)
            if (!st.charOnly && used.contains(st.name) && !st.name.isEmpty() && st.name != QLatin1String("Normal") && !m_styleNames.contains(st.name))
                m_styleNames << st.name;
        return m_styleNames;
    }
    QStringList m_styleNames;
    bool m_styleNamesDone = false;
    void addStory(int textId, const QTextDocument *doc) { addStory(textId, QVector<const QTextDocument *>{doc}); }
    // A story from several documents in turn (a table's cells); cellEnds gets
    // where each one's text ends.
    void addStory(int textId, const QVector<const QTextDocument *> &docs, QVector<quint32> *cellEnds = nullptr);
    int blipIndex(const QString &imageId);
    int patternBlip(const Fill &f);
    QString extraImage(const QByteArray &bytes, quint16 kind);
    QString shapedPictureImage(const PictureItem &pic);
    QString recoloredPictureImage(const PictureItem &pic);
    QString madeImage(const QImage &img, const QString &sourceId, bool seeThrough);

    // Tables: each table story's cell ends (by story index), the table
    // stories' text ids, and the cell fill and border records, which live in
    // the table-format drawing.
    QVector<QPair<int, QVector<quint32>>> m_cellEnds;
    // Each story's text frames (a text box, or a table's cells in list
    // order) for the frame layout section, MCLD.
    // A vertical box's text runs along its height: its frame is recorded
    // turned a quarter turn, as Publisher records a spine.
    struct Frame { QRectF r; QMarginsF m; bool cell = false; bool vertical = false; };
    QVector<QVector<Frame>> m_frames;
    QVector<int> m_tableTextIds;
    QSet<int> m_notHyphenated;   // stories without automatic hyphenation
    QHash<int, int> m_autofit;    // story -> AutoFit Text (TextItem::Autofit) of a lone box
    QVector<QByteArray> m_cellFormats;

    // Pictures: one drawing-store entry per image (numbered from 1), its
    // bytes kept in the delay stream.
    struct Blip { QString imageId, fileName; QByteArray uid, record; quint16 kind = 6; quint32 refs = 0; };
    QVector<Blip> m_blips;
    QHash<QString, QPair<QByteArray, quint16>> m_extraImages;   // made while saving (pattern tiles, cut-out pictures): bytes, kind

    const Document &m_doc;
    QString m_path;
    QImage m_thumbnail;   // page 1, for the preview picture
    QStringList m_fonts;
    QVector<quint32> m_colors;
    // Text: stream bytes from offset 512, runs, story lengths and ids.
    QByteArray m_text;
    QVector<Run> m_charRuns, m_paraRuns;
    QVector<quint32> m_storyLengths;
    QVector<int> m_textIds;
    // Text boxes: text id, place in its chain of linked boxes, sequence number.
    struct TextShape { int tid; int index; quint32 seq; };
    QVector<TextShape> m_textShapes;
    QHash<int, int> m_chainLength;   // text id -> boxes in its chain
};

// The Windows language code (LCID) for a run's language, as Publisher keeps
// it on every run (properties 0x12 and 0x3E, US English 1033 in every file
// it saved here); the same table libmspub reads back.
quint32 languageCode(const QString &tag)
{
    static const QHash<QString, quint32> codes{
        {"en-US", 0x0409}, {"en-GB", 0x0809}, {"en-AU", 0x0c09}, {"en-CA", 0x1009}, {"en-NZ", 0x1409}, {"en-IE", 0x1809},
        {"en-ZA", 0x1c09}, {"en-IN", 0x4009}, {"fr-FR", 0x040c}, {"fr-CA", 0x0c0c}, {"de-DE", 0x0407}, {"de-CH", 0x0807},
        {"de-AT", 0x0c07}, {"es-ES", 0x0c0a}, {"es-MX", 0x080a}, {"es-US", 0x540a}, {"it-IT", 0x0410}, {"nl-NL", 0x0413},
        {"nl-BE", 0x0813}, {"pt-BR", 0x0416}, {"pt-PT", 0x0816}, {"ru-RU", 0x0419}, {"pl-PL", 0x0415}, {"cs-CZ", 0x0405},
        {"hu-HU", 0x040e}, {"sv-SE", 0x041d}, {"da-DK", 0x0406}, {"nb-NO", 0x0414}, {"fi-FI", 0x040b}, {"el-GR", 0x0408},
        {"tr-TR", 0x041f}, {"he-IL", 0x040d}, {"ar-SA", 0x0401}, {"ja-JP", 0x0411}, {"ko-KR", 0x0412}, {"zh-CN", 0x0804},
        {"zh-TW", 0x0404}, {"uk-UA", 0x0422}, {"ro-RO", 0x0418}, {"sk-SK", 0x041b}, {"sl-SI", 0x0424}, {"hr-HR", 0x041a},
        {"bg-BG", 0x0402}};
    const QStringList parts = QString(tag).replace('_', '-').split('-', Qt::SkipEmptyParts);
    const QString key = parts.value(0).toLower() + '-' + parts.value(1).toUpper();
    return codes.value(key, 0x0409);
}

QVector<B> PubWriter::charBlocks(const QTextCharFormat &f, const QTextCharFormat &style)
{
    QVector<B> p;
    const bool bold = (f.fontWeight() >= QFont::DemiBold) != (style.hasProperty(QTextFormat::FontWeight) && style.fontWeight() >= QFont::DemiBold);
    const bool italic = f.fontItalic() != style.fontItalic();
    if (bold) p << flag(0x02, 0x0a);
    if (italic) p << flag(0x03, 0x0a);
    double size = f.hasProperty(QTextFormat::FontPointSize) ? f.fontPointSize() : 0;
    if (size > 0) p << u32(0x0c, quint32(emu(size)), 0x22);
    const quint32 language = languageCode(f.hasProperty(tp::Language) ? f.stringProperty(tp::Language) : style.stringProperty(tp::Language));
    p << u32(0x12, language, 0x22);
    QString family;
    const QStringList fams = f.fontFamilies().toStringList();
    if (!fams.isEmpty()) family = fams.first();
    else {
        const QString theme = f.stringProperty(tp::ThemeFont);
        family = theme == QLatin1String("major") ? m_doc.fonts.heading : m_doc.fonts.body;
    }
    const int fi = fontIndex(family);
    QVector<B> slots;
    for (quint8 s = 0; s < 5; ++s) slots << rec(s, {u16(0x00, quint32(fi))});
    p << rec(0x24, slots, 0x8a);
    if (bold) p << flag(0x37, 0x0a);
    if (italic) p << flag(0x38, 0x0a);
    if (f.fontUnderline()) p << u16(0x1e, 1, 0x12);   // single underline
    if (f.fontStrikeOut()) p << flag(0x10, 0x0a);
    // Letter spacing: kerning as added space in EMU (0x1B), tracking in
    // tenths of a percent (0x1F).
    if (const double kern = tp::kerningOf(f); std::abs(kern) > 0.001) p << u32(0x1b, quint32(qint32(emu(kern))), 0x22);
    if (const double track = tp::trackingOf(f); std::abs(track - 100) > 0.01) p << u16(0x1f, quint32(std::llround(track * 10)), 0x1a);
    // Character scaling, in tenths of a percent like tracking (0x20; a flyer
    // keeps 100.1% as 1001).
    if (f.hasProperty(QTextFormat::FontStretch) && f.fontStretch() > 0 && f.fontStretch() != 100) p << u16(0x20, quint32(f.fontStretch() * 10), 0x1a);
    if (f.fontCapitalization() == QFont::SmallCaps) p << flag(0x13, 0x0a);
    else if (f.fontCapitalization() == QFont::AllUppercase) p << flag(0x14, 0x0a);
    if (f.verticalAlignment() == QTextCharFormat::AlignSuperScript) p << u16(0x0f, 1, 0x12);
    else if (f.verticalAlignment() == QTextCharFormat::AlignSubScript) p << u16(0x0f, 2, 0x12);
    if (size > 0) p << u32(0x39, quint32(emu(size)), 0x22);
    p << u32(0x3e, language, 0x22);
    const QString cref = f.stringProperty(tp::ColorRefP);
    QColor color;
    if (!cref.isEmpty()) color = ColorRef::fromString(cref).resolve(m_doc.colors);
    else if (f.hasProperty(QTextFormat::ForegroundBrush)) color = f.foreground().color();
    if (color.isValid()) {
        // The color (0x44) and the text fill (0x58: solid, that color, 100%
        // opaque). Publisher draws the fill, so a run without it keeps the
        // Normal style's fill and shows in the main color.
        const quint32 ci = quint32(colorIndex(color));
        p << rec(0x44, {u32(0x00, ci, 0x22)}, 0x8a)
          << rec(0x58, {u16(0x00, 1, 0x12), u32(0x01, ci, 0x22), u32(0x02, 100000, 0x22)}, 0x8a);
    }
    std::sort(p.begin(), p.end(), [](const B &a, const B &b) { return a.id < b.id; });
    return p;
}

QVector<B> PubWriter::paraBlocks(const QTextBlock &block)
{
    const QTextBlockFormat f = block.blockFormat();
    QVector<B> p;
    const Qt::Alignment a = f.alignment() & Qt::AlignHorizontal_Mask;
    if (a & Qt::AlignRight) p << u16(0x04, 1, 0x12);
    else if (a & Qt::AlignHCenter) p << u16(0x04, 2, 0x12);
    else if (a & Qt::AlignJustify) p << u16(0x04, 3, 0x12);
    // Indents (EMU; the first line's can be negative for a hanging indent).
    if (std::abs(f.textIndent()) > 0.001) p << u32(0x0c, quint32(qint32(emu(f.textIndent()))), 0x22);
    if (f.leftMargin() > 0.001) p << u32(0x0d, quint32(emu(f.leftMargin())), 0x22);
    if (f.rightMargin() > 0.001) p << u32(0x0e, quint32(emu(f.rightMargin())), 0x22);
    // Spacing is always written, so Publisher's Normal style (6 pt after,
    // 1.19 lines) doesn't fill in where JeffPub has none.
    p << u32(0x12, quint32(emu(std::max(0.0, f.topMargin()))), 0x22) << u32(0x13, quint32(emu(std::max(0.0, f.bottomMargin()))), 0x22);
    // Line spacing: in points, eighths of an EMU plus 1; in lines, what would
    // be EMU at 96 pt (a multiple of 4) plus 2.
    const int lht = f.lineHeightType();
    quint32 spacing;
    if ((lht == QTextBlockFormat::FixedHeight || lht == QTextBlockFormat::MinimumHeight) && f.lineHeight() > 0)
        spacing = quint32(std::llround(f.lineHeight() * kEmuPerPt * 8)) | 1;
    else {
        const double lines = lht == QTextBlockFormat::ProportionalHeight && f.lineHeight() > 0 ? f.lineHeight() / 100.0 : 1.0;
        spacing = quint32(4 * std::llround(lines * 914400.0 * 96 / 72 / 4)) + 2;
    }
    p << u32(0x34, spacing, 0x22);
    // Tab stops: a count, then each one's position, alignment (left left
    // out, 1 right, 2 center, 3 decimal) and leader character ('.', '-',
    // '_' or 0xB7 for bullets; left out for none).
    const QList<QTextOption::Tab> tabs = f.tabPositions();
    if (!tabs.isEmpty()) {
        const QString leaders = f.stringProperty(tp::TabLeaders);
        QVector<B> stops;
        for (int i = 0; i < tabs.size(); ++i) {
            QVector<B> t{u32(0x00, quint32(emu(tabs[i].position)), 0x20)};
            const int align = tabs[i].type == QTextOption::RightTab ? 1 : tabs[i].type == QTextOption::CenterTab ? 2 : tabs[i].type == QTextOption::DelimiterTab ? 3 : 0;
            if (align) t << u16(0x01, quint32(align), 0x10);
            const QChar leader = i < leaders.size() ? leaders.at(i) : QChar(' ');
            const uint code = leader == QChar(0x2022) || leader == QChar(0x00B7) ? 0xB7 : leader.unicode();
            if (code > 0x20 && code < 0x100) t << u16(0x02, code);
            stops << rec(quint8(i), t);
        }
        p << rec(0x32, {u16(0x27, quint32(tabs.size()), 0x1a), rec(0x28, stops, 0x8a)}, 0x82);
    }
    // Lists: kind 23 bulleted (with the bullet's character in its font) or a
    // numbering style (0 1 2 3, 1 I, 2 i, 3 A, 4 a) with its punctuation in
    // the high half of 0x58 (2 "1.", 0 "1)", 1 "(1)"); 02 the marker's size
    // and 03 the bullet's font (its place in the font table).
    if (const QTextList *list = block.textList()) {
        const QTextListFormat lf = list->format();
        const QString custom = !f.stringProperty(tp::BulletChar).isEmpty() ? f.stringProperty(tp::BulletChar) : lf.stringProperty(tp::BulletChar);
        const bool bullet = lf.style() == QTextListFormat::ListDisc || lf.style() == QTextListFormat::ListCircle ||
                            lf.style() == QTextListFormat::ListSquare || !custom.isEmpty();
        quint32 kind = 23, ch = 0xB7, delim = 2;
        QString bulletFont = lf.stringProperty(tp::BulletFont);
        if (bullet) {
            const uint u = custom.size() == 1 ? custom[0].unicode() : 0;
            if (u && (u < 0x100 || (u >= 0xF020 && u <= 0xF0FF))) ch = u;   // a character of the bullet's font
            else if (u) {
                // A Unicode picture: the symbol font that has it.
                QString f;
                if (const uint code = unicodeToSymbol(custom[0], &f)) { ch = code; bulletFont = f; }
            } else if (lf.style() == QTextListFormat::ListSquare) { ch = 0xF0A7; bulletFont = QStringLiteral("Wingdings"); }
            if (ch == 0xB7 && !u) bulletFont = QStringLiteral("Symbol");
        } else {
            switch (lf.style()) {
            case QTextListFormat::ListUpperRoman: kind = 1; break;
            case QTextListFormat::ListLowerRoman: kind = 2; break;
            case QTextListFormat::ListUpperAlpha: kind = 3; break;
            case QTextListFormat::ListLowerAlpha: kind = 4; break;
            default: kind = 0; break;
            }
            ch = 0;
            if (lf.numberSuffix() == QLatin1String(")")) delim = lf.numberPrefix() == QLatin1String("(") ? 1 : 0;
        }
        // The marker's size: its own, or the text's.
        double size = lf.hasProperty(tp::BulletSize) ? lf.property(tp::BulletSize).toDouble() : 0;
        for (auto it = block.begin(); size <= 0 && !it.atEnd(); ++it)
            if (it.fragment().charFormat().hasProperty(QTextFormat::FontPointSize)) size = it.fragment().charFormat().fontPointSize();
        if (size <= 0) size = 10;
        if (bulletFont.isEmpty()) bulletFont = QStringLiteral("Symbol");
        p << u32(0x02, quint32(emu(size)), 0x22) << u16(0x03, quint32(fontIndex(bulletFont)), 0x1a)
          << rec(0x57, {u32(0x00, kind, 0x22), u32(0x01, ch, 0x22), u32(0x02, 0, 0x22)}, 0x8a);
        if (!bullet) p << u32(0x58, delim << 16, 0x22);
    }
    // The paragraph's style: its place in the style sheet (Normal, 0, is left out).
    if (const int si = int(styleNames().indexOf(f.stringProperty(tp::StyleName))) + 1; si > 0) p << u16(0x19, quint32(si), 0x1a);
    // Right to left, as Publisher writes it (a sample made in Publisher:
    // 0x06 = 0 and 0x3A = 0xF3FF on the right-to-left paragraph only).
    if (f.layoutDirection() == Qt::RightToLeft) p << u32(0x06, 0, 0x22) << u16(0x3a, 0xF3FF, 0x12);
    std::sort(p.begin(), p.end(), [](const B &a, const B &b) { return a.id < b.id; });
    return p;
}

void PubWriter::addStory(int textId, const QVector<const QTextDocument *> &docs, QVector<quint32> *cellEnds)
{
    const quint32 start = quint32(512 + m_text.size());
    for (const QTextDocument *doc : docs) {
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        QTextCharFormat last = b.charFormat();
        const QString styleName = b.blockFormat().stringProperty(tp::StyleName);
        const TextStyle *st = styleNames().contains(styleName) ? m_doc.style(styleName) : nullptr;
        const QTextCharFormat styleChr = st ? st->chr : QTextCharFormat();
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            if (!fr.isValid()) continue;
            QString t = fr.text();
            t.replace(QChar::LineSeparator, QChar(0x0b));
            t.replace(QChar(0x2029), QChar('\r'));
            for (QChar c : t) putU16(m_text, c.unicode());
            m_charRuns << Run{quint32(512 + m_text.size()), charProps(fr.charFormat(), styleChr)};
            last = fr.charFormat();
        }
        putU16(m_text, '\r');
        m_charRuns << Run{quint32(512 + m_text.size()), charProps(last, styleChr)};
        m_paraRuns << Run{quint32(512 + m_text.size()), paraProps(b)};
    }
        // A cell ends at its last paragraph mark (the last cell at the story's end).
        if (cellEnds) *cellEnds << quint32((512 + m_text.size() - start) / 2 - 1);
    }
    if (cellEnds && !cellEnds->isEmpty()) cellEnds->last() += 1;
    m_storyLengths << quint32((512 + m_text.size() - start) / 2);
    m_textIds << textId;
}

// A JPEG whose frame has four components is CMYK, which has its own record.
static bool jpegIsCmyk(const QByteArray &d)
{
    int i = 2;
    while (i + 9 < d.size() && quint8(d[i]) == 0xff) {
        const quint8 m = quint8(d[i + 1]);
        if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) return quint8(d[i + 9]) == 4;
        i += 2 + ((quint8(d[i + 2]) << 8) | quint8(d[i + 3]));
    }
    return false;
}

// A pattern fill as an 8 x 8 tile in its colors (the background clear
// when it has none), kept with the pictures.
int PubWriter::patternBlip(const Fill &f)
{
    const QImage tile = f.patternTile(m_doc.colors);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    tile.save(&buf, "PNG");
    return blipIndex(extraImage(png, 6));
}

// A picture made while saving, kept by its contents (kind 5 JPEG, 6 PNG).
QString PubWriter::extraImage(const QByteArray &bytes, quint16 kind)
{
    const QString key = QStringLiteral("made:") + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex());
    m_extraImages.insert(key, {bytes, kind});
    return key;
}

// A picture cut to a shape is saved as the shape filled with the picture,
// which always fills the shape's box: so the part showing in the frame is
// cut out, with its adjustments applied, unless it is the whole picture as is.
QString PubWriter::shapedPictureImage(const PictureItem &pic)
{
    const QSizeF fs = pic.rect.size();
    const QRectF ir = pic.imgRect.isEmpty() ? QRectF(QPointF(), fs) : pic.imgRect;
    const bool whole = std::abs(ir.left()) < 1e-3 && std::abs(ir.top()) < 1e-3 && std::abs(ir.width() - fs.width()) < 1e-3
                       && std::abs(ir.height() - fs.height()) < 1e-3;
    const bool adjusted = pic.brightness || pic.contrast || pic.recolor != PictureItem::NoRecolor || pic.hasTransparentColor;
    if (whole && !adjusted) return pic.imageId;
    QImage img = adjusted ? Renderer::processedImage(m_doc, pic, QSizeF()) : m_doc.image(pic.imageId);
    if (img.isNull()) return pic.imageId;
    const QRectF shown = QRectF(QPointF(), fs).intersected(ir);
    if (!whole && !shown.isEmpty()) {
        const double sx = img.width() / ir.width(), sy = img.height() / ir.height();
        const QRect px = QRectF((shown.left() - ir.left()) * sx, (shown.top() - ir.top()) * sy, shown.width() * sx, shown.height() * sy).toAlignedRect()
                             .intersected(img.rect());
        if (!px.isEmpty()) img = img.copy(px);
    }
    return madeImage(img, pic.imageId, pic.hasTransparentColor);
}

// A picture made from one of the publication's: a JPEG source stays JPEG
// (photos stay small) unless the change made parts of it see-through. The
// decision is the source's, since a converted image always has an alpha
// channel whether it uses it or not.
QString PubWriter::madeImage(const QImage &img, const QString &sourceId, bool seeThrough)
{
    const auto src = m_doc.images.constFind(sourceId);
    const bool jpeg = !seeThrough && src != m_doc.images.cend() &&
                      (src->format.toLower() == QLatin1String("jpg") || src->format.toLower() == QLatin1String("jpeg"));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (jpeg) img.convertToFormat(QImage::Format_RGB32).save(&buf, "JPEG", 92);
    else img.save(&buf, "PNG");
    return extraImage(bytes, jpeg ? 5 : 6);
}

// Grayscale and black and white: the other program shows a saved picture in
// its own colors even with these set (0x013F), so the picture is saved
// already in gray or black and white, the setting kept alongside. Applying
// either again changes nothing.
QString PubWriter::recoloredPictureImage(const PictureItem &pic)
{
    if (pic.recolor != PictureItem::Grayscale && pic.recolor != PictureItem::BlackWhite) return pic.imageId;
    QImage img = m_doc.image(pic.imageId);
    if (img.isNull()) return pic.imageId;
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = line[x];
            const double lum = 0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c);
            const int v = pic.recolor == PictureItem::BlackWhite ? (lum >= 128 ? 255 : 0) : std::clamp(int(std::lround(lum)), 0, 255);
            line[x] = qRgba(v, v, v, qAlpha(c));
        }
    }
    return madeImage(img, pic.imageId, false);
}

int PubWriter::blipIndex(const QString &imageId)
{
    if (imageId.isEmpty()) return -1;
    for (int i = 0; i < m_blips.size(); ++i) {
        if (m_blips[i].imageId == imageId) {
            ++m_blips[i].refs;
            return i + 1;
        }
    }
    Blip b;
    b.imageId = imageId;
    QByteArray data;
    if (m_extraImages.contains(imageId)) {
        data = m_extraImages.value(imageId).first;
        b.kind = m_extraImages.value(imageId).second;
        b.refs = 1;
        b.uid = QCryptographicHash::hash(data, QCryptographicHash::Md4);
        b.fileName = QStringLiteral("picture%1.%2").arg(m_blips.size() + 1).arg(b.kind == 5 ? QStringLiteral("jpg") : QStringLiteral("png"));
        QByteArray body = b.uid;
        body.append(char(0xff));
        body += data;
        if (b.kind == 6) b.record = escherRecord(0x0, 0x6e0, 0xf01e, body);
        else b.record = escherRecord(0x0, jpegIsCmyk(data) ? 0x6e2 : 0x46a, 0xf01d, body);
        m_blips << b;
        return int(m_blips.size());
    }
    const auto it = m_doc.images.constFind(imageId);
    if (imageId.isEmpty() || it == m_doc.images.cend()) return -1;
    // PNG and JPEG go in as they are; anything else is saved as PNG.
    const QString fmt = it->format.toLower();
    bool converted = false;
    if (fmt == QLatin1String("png")) data = it->bytes;
    else if (fmt == QLatin1String("jpg") || fmt == QLatin1String("jpeg")) {
        data = it->bytes;
        b.kind = 5;
    }
    if (data.isEmpty()) {
        const QImage img = it->image();
        if (img.isNull()) return -1;
        QBuffer buf(&data);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        b.kind = 6;
        converted = true;
    }
    b.refs = 1;
    b.uid = QCryptographicHash::hash(data, QCryptographicHash::Md4);
    for (int i = 0; i < m_blips.size(); ++i) {
        if (m_blips[i].uid == b.uid) {   // the same picture under another id
            ++m_blips[i].refs;
            return i + 1;
        }
    }
    QString name = QFileInfo(it->sourcePath).fileName();
    if (name.isEmpty()) name = QStringLiteral("picture%1.%2").arg(m_blips.size() + 1).arg(b.kind == 5 ? QStringLiteral("jpg") : QStringLiteral("png"));
    else if (converted) name = QFileInfo(name).completeBaseName() + QStringLiteral(".png");
    b.fileName = name;
    QByteArray body = b.uid;
    body.append(char(0xff));
    body += data;
    if (b.kind == 6) b.record = escherRecord(0x0, 0x6e0, 0xf01e, body);
    else b.record = escherRecord(0x0, jpegIsCmyk(data) ? 0x6e2 : 0x46a, 0xf01d, body);
    m_blips << b;
    return int(m_blips.size());
}

QByteArray PubWriter::write(QStringList *skipped)
{
    const QSizeF ps = m_doc.pageSize();
    const qint64 pw = emu(ps.width()), ph = emu(ps.height());
    const QMarginsF mg = m_doc.setup.margins;

    // ---- plan sequence numbers: the fixed set every publication has, then
    // extra pages and objects.
    ContentsWriter cw;
    const quint32 kMaster = 263, kFirstPage = 266, kSpecial[4] = {269, 272, 275, 279};
    quint32 next = 293;
    QVector<quint32> pageSeq{kFirstPage};
    QVector<QPair<quint32, quint32>> pageSub{{267, 268}};
    for (int i = 1; i < m_doc.pages.size(); ++i) {
        pageSeq << next;
        pageSub << qMakePair(next + 1, next + 2);
        next += 3;
    }
    // Master pages: the first is the fixed one; each other has its own
    // chunk, two sub-chunks and margin guides.
    struct MasterSeqs { quint32 seq, sub60, sub77, guides; };
    QVector<MasterSeqs> masterSeqs{{kMaster, 264, 265, 289}};
    for (int k = 1; k < m_doc.masters.size(); ++k) {
        masterSeqs << MasterSeqs{next, next + 1, next + 2, next + 3};
        next += 4;
    }
    auto masterFor = [&](const Page &pg) {
        for (int k = 0; k < m_doc.masters.size(); ++k)
            if (m_doc.masters[k]->abbr == pg.masterId) return masterSeqs[k].seq;
        return kMaster;
    };

    auto webForm = [] {
        return QVector<B>{u32(0x0c, 2), str(0x0e, QStringLiteral("someone@example.com")), str(0x0f, QStringLiteral("Web Site Form Response")),
                          str(0x10, QStringLiteral("Your information was received")), str(0x13, QStringLiteral("FORMDATA.HTM")), u32(0x17, 1),
                          str(0x18, QStringLiteral("http://example.com/~user/ispscript.cgi"))};
    };
    auto pageBody = [&](const QVector<quint32> &shapes, quint32 sub60, quint32 sub77, bool master, bool special, QSizeF scratch, QSizeF ext,
                        int index = 0, quint32 masterRef = 263, quint32 guidesRef = 289, const QString &abbr = QStringLiteral("A"),
                        const QString &desc = QStringLiteral("Master Page A")) {
        QVector<B> b;
        if (!shapes.isEmpty()) {
            QVector<B> refs;
            for (quint32 s : shapes) refs << ref(0x00, s);
            b << u32(0x01, quint32(shapes.size())) << list(0x02, refs);
        }
        if (master) b << ref(0x03, guidesRef);
        b << rec(0x05, {u32(0x01, quint32(scratch.width())), u32(0x02, quint32(scratch.height()))});
        QByteArray f06(8, '\0');
        // A normal page: 2, then its index among the pages.
        f06[0] = char(special || master ? 0 : 2);
        if (!special && !master) setU32(f06, 4, quint32(index));
        b << bytesB(0x06, 0x28, f06);
        b << ref(0x09, sub60) << ref(0x0b, sub77);
        if (!master) b << ref(0x0d, masterRef, 0x68);
        b << str(0x0e, master ? abbr : QString()) << str(0x0f, master ? desc : QString());
        b << u32(0x10, master ? 3 : 5);
        b << rec(0x11, {u32(0x01, quint32(ext.width())), u32(0x02, quint32(ext.height()))});
        return b;
    };

    // ---- objects on each page
    struct Obj { quint32 seq; int page; QByteArray escher; };
    QVector<Obj> objs;
    // Objects go on the pages and on the master pages.
    QVector<const PageBase *> surfaces;
    QVector<quint32> surfaceSeq;
    for (int i = 0; i < m_doc.pages.size(); ++i) {
        surfaces << m_doc.pages[i].get();
        surfaceSeq << pageSeq[i];
    }
    for (int k = 0; k < m_doc.masters.size(); ++k) {
        surfaces << m_doc.masters[k].get();
        surfaceSeq << masterSeqs[k].seq;
    }
    QVector<QVector<quint32>> pageShapes(surfaces.size());
    int textId = 2;
    int spid = 0x401;
    QHash<QString, quint32> spidOf;   // item -> its drawing shape id, for connector rules
    int skippedCount = 0;
    const double cx = ps.width() / 2, cy = ps.height() / 2;
    auto anchor = [&](const QRectF &r) {
        return clientBlocks(0xf010, {u32(0x01, quint32(qint32(emu(r.left() - cx)))), u32(0x02, quint32(qint32(emu(r.top() - cy)))),
                                     u32(0x03, quint32(qint32(emu(r.right() - cx)))), u32(0x04, quint32(qint32(emu(r.bottom() - cy))))});
    };
    // Turned shapes keep their unturned frame, except that between 45° and
    // 135° (and 225° and 315°) the anchor and size hold it turned a quarter.
    auto turnedBox = [](const QRectF &r, double deg) {
        const double a = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
        if ((a >= 45 && a < 135) || (a >= 225 && a < 315)) {
            QRectF b(0, 0, r.height(), r.width());
            b.moveCenter(r.center());
            return b;
        }
        return r;
    };
    // Publisher flips a shape before turning it, and turns a shape flipped
    // one way (not both) the other way round; JeffPub always turns clockwise.
    auto rotationProp = [](QVector<Prop> &opt, const Item *it) {
        const double deg = it->flipH != it->flipV ? -it->rotation : it->rotation;
        const double a = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
        if (a > 1e-6) opt << Prop{0x0004, quint32(std::llround(a * 65536.0))};
    };
    // Text wrapping: field 04's low byte (0 none, 2 tight, 3 through, 4 top
    // and bottom; square leaves the field out, as Publisher does), and the
    // distances kept clear in the drawing.
    auto wrapField = [](QVector<B> &body, const Item *it, quint32 high = 0x100) {
        if (it->wrap.mode != Wrap::Square) body << u16(0x04, high | quint32(it->wrap.mode), 0x10);
    };
    auto wrapProps = [](QVector<Prop> &opt, const Item *it) {
        opt << Prop{0x0384, quint32(emu(it->wrap.left))} << Prop{0x0385, quint32(emu(it->wrap.top))} << Prop{0x0386, quint32(emu(it->wrap.right))}
            << Prop{0x0387, quint32(emu(it->wrap.bottom))};
    };
    auto spRecord = [&](quint16 kind, quint32 flags, const Item *it) {
        QByteArray d;
        putU32(d, quint32(spid));
        putU32(d, flags | (it->flipH ? 0x40 : 0) | (it->flipV ? 0x80 : 0));
        return escherRecord(0x2, kind, 0xf00a, d);
    };
    // Outline: color, width, dashes and arrowheads.
    auto opacity = [](double transparency) { return quint32(std::llround(std::clamp(1.0 - transparency, 0.0, 1.0) * 65536)); };
    auto strokeProps = [&](QVector<Prop> &opt, const Stroke &st) {
        const bool lined = !st.isNone();
        opt << Prop{0x01c0, lined ? bgr(st.color.resolve(m_doc.colors)) : 0x08000000} << Prop{0x01c2, 0x08000007}
            << Prop{0x01cb, quint32(emu(lined ? st.width : 2))} << Prop{0x01ff, lined ? 0x00080008u : 0x00080000u};
        if (!lined) return;
        if (st.transparency > 0.001) opt << Prop{0x01c1, opacity(st.transparency)};
        static const quint32 kDash[] = {0, 2, 2, 6, 8, 7, 9, 10};
        if (st.dash != Stroke::SolidLine) opt << Prop{0x01ce, kDash[st.dash]};
        if (st.dash == Stroke::RoundDot) opt << Prop{0x01d7, 0};
        auto arrow = [](Arrow a) -> quint32 {
            switch (a) {
            case Arrow::Triangle: return 1;
            case Arrow::Stealth: return 2;
            case Arrow::Diamond: return 3;
            case Arrow::Oval: return 4;
            case Arrow::Open: return 5;
            default: return 0;
            }
        };
        if (st.startArrow != Arrow::None)
            opt << Prop{0x01d0, arrow(st.startArrow)} << Prop{0x01d2, quint32(st.startSize)} << Prop{0x01d3, quint32(st.startSize)};
        if (st.endArrow != Arrow::None)
            opt << Prop{0x01d1, arrow(st.endArrow)} << Prop{0x01d4, quint32(st.endSize)} << Prop{0x01d5, quint32(st.endSize)};
    };
    // Fill: none; solid (with transparency); a gradient (linear as type 7
    // with the angle, radial as 5 from the center, along the outline as 6)
    // with its colors, stops and transparency; or a picture (3 stretched,
    // 2 tiled). The reader turns a stored angle a into 90 + a, and treats
    // -45 and -135 specially, so angles are stored minus 90 within 0-360.
    // A solid fill given as process inks: in the tertiary properties, the
    // color as shown (0x019E) and the inks packed into 0x019F and 0x01A6 as
    // one run of bits, 31 from each: the bits per ink (8), which inks there
    // are (0x100 cyan, 0x80 magenta, 0x40 yellow, 0x20 black), then only
    // those inks' values, in that order. Checked in Publisher one ink at a
    // time and against three reference files.
    // A fill in one of the publication's spot colors also names its ink
    // (0x01A1, UTF-16 "P2,#" + the shown color as four 16-bit hex values + ","
    // + the ink's name), as ten book covers keep PANTONE 2727 C.
    auto inkProps = [&](QVector<Prop> &topt, const Fill &f) {
        if (f.type != Fill::Solid || f.color.kind() != ColorRef::Rgb || f.color.rgbValue().spec() != QColor::Cmyk) return;
        const QPair<quint32, quint32> packed = packPubInks(f.color.rgbValue());
        const QColor shown = f.color.resolve(m_doc.colors);
        topt << Prop{0x019e, bgr(shown)} << Prop{0x019f, packed.first};
        if (packed.second) topt << Prop{0x01a6, packed.second};
        if (m_doc.print.usesSpots())
            for (int i = 0; i < m_doc.print.spotColors.size(); ++i)
                if (packPubInks(m_doc.print.spotColors[i].toCmyk()) == packed) {
                    const QString text = QStringLiteral("P2,#%1%2%3%4,%5")
                                             .arg(shown.red(), 4, 16, QLatin1Char('0')).arg(shown.green(), 4, 16, QLatin1Char('0'))
                                             .arg(shown.blue(), 4, 16, QLatin1Char('0')).arg(0, 4, 16, QLatin1Char('0')).arg(m_doc.print.spotName(i));
                    topt << Prop{0xc1a1, 0, utf16z(text)};
                    break;
                }
    };
    auto fillProps = [&](QVector<Prop> &opt, const Fill &f) {
        auto rgb = [&](const ColorRef &c) { return bgr(c.resolve(m_doc.colors)); };
        if (f.type == Fill::Gradient) {
            QVector<GradientStop> stops = f.stops;
            if (stops.size() < 2) stops = {GradientStop{0, f.color, f.transparency}, GradientStop{1, f.color2, f.transparency}};
            std::stable_sort(stops.begin(), stops.end(), [](const GradientStop &a, const GradientStop &b) { return a.pos < b.pos; });
            const quint32 type = f.gradType == Fill::Linear ? 7 : f.gradType == Fill::PathGrad ? 6 : 5;
            opt << Prop{0x0180, type} << Prop{0x0181, rgb(stops.first().color)} << Prop{0x0183, rgb(stops.last().color)} << Prop{0x01bf, 0x00100010};
            if (stops.first().transparency > 0.001) opt << Prop{0x0182, opacity(stops.first().transparency)};
            if (stops.last().transparency > 0.001) opt << Prop{0x0184, opacity(stops.last().transparency)};
            if (type == 7) {
                const double a = std::fmod(std::fmod(f.angle - 90, 360.0) + 360.0, 360.0);
                opt << Prop{0x018b, quint32(std::llround(a)) << 16};
            } else if (type == 5) {
                opt << Prop{0x018d, 32768} << Prop{0x018e, 32768} << Prop{0x018f, 32768} << Prop{0x0190, 32768};
            }
            // Two end stops need only the two colors (each with its own
            // transparency); more stops add the list: count, count, 8, then
            // each color and position (16.16).
            if (stops.size() == 2 && stops.first().pos < 0.001 && stops.last().pos > 0.999) return;
            QByteArray shade;
            putU16(shade, quint32(stops.size()));
            putU16(shade, quint32(stops.size()));
            putU16(shade, 8);
            for (const GradientStop &st : stops) {
                putU32(shade, rgb(st.color));
                putU32(shade, quint32(std::llround(std::clamp(st.pos, 0.0, 1.0) * 65536)));
            }
            opt << Prop{0xc197, 0, shade};
            return;
        }
        if (f.type == Fill::Picture || f.type == Fill::Texture || f.type == Fill::Pattern) {
            const int b = f.type == Fill::Pattern ? patternBlip(f) : blipIndex(f.imageId);
            if (b > 0) {
                opt << Prop{0x0180, f.type != Fill::Picture || f.tile ? 2u : 3u} << Prop{0x4186, quint32(b)} << Prop{0x0181, 0x08000001}
                    << Prop{0x0183, 0x08000007} << Prop{0x01bf, 0x00100010};
                if (f.transparency > 0.001) opt << Prop{0x0182, opacity(f.transparency)};
                return;
            }
        }
        const bool filled = !f.isNone() && (f.type == Fill::Solid || f.type == Fill::Pattern);
        opt << Prop{0x0181, filled ? rgb(f.color) : 0x08000001} << Prop{0x0183, 0x08000007} << Prop{0x01bf, filled ? 0x00100010u : 0x00100000u};
        if (filled && f.transparency > 0.001) opt << Prop{0x0182, opacity(f.transparency)};
    };
    // Shadow: offset type, color, opacity and offset (EMU), switched on.
    auto shadowProps = [&](QVector<Prop> &opt, const ShadowFx &sh) {
        if (!sh.on || sh.inner) return;
        const QPointF o = sh.offset();
        opt << Prop{0x0200, 0} << Prop{0x0201, bgr(sh.color.resolve(m_doc.colors))} << Prop{0x0204, opacity(sh.transparency)}
            << Prop{0x0205, quint32(qint32(emu(o.x())))} << Prop{0x0206, quint32(qint32(emu(o.y())))} << Prop{0x023f, 0x00020002};
    };
    const QVector<Prop> kInsets = {{0x0081, 36576}, {0x0082, 36576}, {0x0083, 36576}, {0x0084, 36576}};
    const QVector<Prop> kTail = {{0x0201, 0x08000000}, {0x0285, 0}, {0x02cb, 0}, {0x02cc, 0}, {0x02ce, 0}, {0x02cf, 0},
                                 {0x0384, 36576}, {0x0385, 36576}, {0x0386, 36576}, {0x0387, 36576}};
    // Text boxes first: each chain of linked boxes is one story, numbered
    // in page order by its first box, with a frame per box in chain order.
    QHash<QString, QPair<int, int>> chainPos;   // box id -> text id, place in chain
    QHash<QString, QPair<int, int>> shapeText;  // shape id -> text id, story index
    for (int pi = 0; pi < surfaces.size(); ++pi) {
        std::function<void(const ItemPtr &)> find = [&](const ItemPtr &it) {
            if (it->type() == ItemType::Group) {
                for (const ItemPtr &c : static_cast<const GroupItem *>(it.get())->children) find(c);
                return;
            }
            if (it->type() == ItemType::Shape) {
                // Text in a shape is a story too; its frame is set when the shape is written.
                auto *sh = static_cast<const ShapeItem *>(it.get());
                const QTextDocument *sd = sh->storyId.isEmpty() ? nullptr : m_doc.storyDoc(sh->storyId);
                if (!sd) return;
                const int tid = textId++;
                addStory(tid, sd);
                shapeText[sh->id] = qMakePair(tid, int(m_frames.size()));
                m_frames << QVector<Frame>{Frame{sh->rect, QMarginsF(), false}};
                return;
            }
            if (it->type() != ItemType::Text || m_doc.prevFrame(it->id)) return;
            auto *t = static_cast<const TextItem *>(it.get());
            const QTextDocument *sd = m_doc.storyDoc(t->storyId);
            if (!sd) return;
            const int tid = textId++;
            addStory(tid, sd);
            if (!t->hyphenate) m_notHyphenated << tid;
            if (t->autofit != TextItem::NoAutofit && t->nextId.isEmpty()) m_autofit[tid] = t->autofit;
            QVector<Frame> frames;
            QSet<QString> seen;
            for (const TextItem *box = t; box && !seen.contains(box->id);) {
                seen.insert(box->id);
                chainPos[box->id] = qMakePair(tid, int(frames.size()));
                frames << Frame{box->rect, box->insets, false, box->vertical};
                Item *nx = box->nextId.isEmpty() ? nullptr : m_doc.item(box->nextId);
                box = nx && nx->type() == ItemType::Text ? static_cast<const TextItem *>(nx) : nullptr;
            }
            m_frames << frames;
            m_chainLength[tid] = int(frames.size());
        };
        for (const ItemPtr &it : surfaces[pi]->items) find(it);
    }
    for (int pi = 0; pi < surfaces.size(); ++pi) {
        std::function<void(const ItemPtr &)> visit = [&](const ItemPtr &it) {
            if (it->type() == ItemType::Group) {
                for (const ItemPtr &c : static_cast<const GroupItem *>(it.get())->children) visit(c);
                return;
            }
            const QRectF r = turnedBox(it->rect, it->rotation);
            auto finish = [&](quint32 seq, QByteArray sp) {
                objs << Obj{seq, pi, escherContainer(0xf004, sp)};
                pageShapes[pi] << seq;
                spidOf.insert(it->id, quint32(spid));
                ++spid;
            };
            if (it->type() == ItemType::Text) {
                auto *t = static_cast<const TextItem *>(it.get());
                // Every box of a chain names the chain's story.
                const auto pos = chainPos.constFind(t->id);
                if (pos == chainPos.cend()) {
                    ++skippedCount;
                    return;
                }
                const quint32 seq = next++;
                const int tid = pos->first;
                m_textShapes << TextShape{tid, pos->second, seq};
                QVector<B> body{flag(0x02)};
                wrapField(body, t);
                body << bytesB(0x0c, 0x28, {}) << bytesB(0x0d, 0x28, {}) << u32(0x27, quint32(tid));
                // Vertical text: 34 = 2 here and text flow 1 in the drawing (every
                // vertical box in 40 of Publisher's covers has both).
                if (t->vertical) body << u32(0x34, 2);
                if (t->valign != VAlign::Top) body << u32(0x35, t->valign == VAlign::Middle ? 1 : 2);   // vertical alignment
                body << u32(0xaa, quint32(emu(r.width()))) << u32(0xab, quint32(emu(r.height()))) << u32(0xb7, 0);
                cw.put(seq, {0x01, surfaceSeq[pi], body});
                QVector<Prop> opt = {{0x0080, quint32(tid)}, {0x0081, quint32(emu(t->insets.left()))}, {0x0082, quint32(emu(t->insets.top()))},
                                     {0x0083, quint32(emu(t->insets.right()))}, {0x0084, quint32(emu(t->insets.bottom()))},
                                     {0x0181, 0x08000001}, {0x0183, 0x08000007}, {0x01bf, 0x00100000}, {0x01c0, 0x08000000}, {0x01c2, 0x08000007},
                                     {0x01cb, 25400}, {0x01ff, 0x00080000}, {0x0201, 0x08000000}, {0x0285, 0}, {0x02cb, 0}, {0x02cc, 0}, {0x02ce, 0},
                                     {0x02cf, 0}, {0x0384, 36576}, {0x0385, 36576}, {0x0386, 36576}, {0x0387, 36576}};
                // Vertical text: text flow 1, top to bottom, after the insets (as
                // Publisher writes a spine). Without it the words stack up letter
                // by letter in the tall box.
                if (t->vertical) opt.insert(5, Prop{0x0088, 1});
                fillProps(opt, t->fill);
                strokeProps(opt, t->stroke);
                shadowProps(opt, t->fx.shadow);
                rotationProp(opt, t);
                QVector<Prop> topt = {{0x008d, 73152}, {0x017f, 0x00400040}, {0x01ff, 0x00400000}, {0x057f, 0x00080000},
                                      {0x05bf, 0x00080000}, {0x05ff, 0x00080000}, {0x063f, 0x00080000}, {0x06ff, 0x00020002}};
                topt << kSideLines << kShadowFlags;
                inkProps(topt, t->fill);
                wrapProps(opt, t);
                QByteArray sp = spRecord(202, 0x0a00, t) + escherProps(0xf00b, opt) + escherProps(0xf122, topt) + anchor(r);
                sp += clientBlocks(0xf011, {ref(0x01, seq, 0x68)});
                sp += clientBlocks(0xf00d, {u32(0x01, quint32(tid))});
                finish(seq, sp);
                return;
            }
            if (it->type() == ItemType::Shape) {
                auto *s = static_cast<const ShapeItem *>(it.get());
                const auto stx = shapeText.constFind(s->id);
                const bool hasText = stx != shapeText.cend();
                // Publisher's own shape when it has one (at its standard
                // handle settings); otherwise the exact outline as a freeform.
                const ShapeDef *def = shapeDef(s->shape);
                const QVector<double> adj = s->adj.isEmpty() && def ? def->defaults : s->adj;
                bool atDefaults = def && adj.size() == def->defaults.size();
                for (int k = 0; atDefaults && k < adj.size(); ++k) atDefaults = std::abs(adj[k] - def->defaults[k]) < 1e-6;
                int st = s->customPath.isEmpty() && atDefaults ? pubShapeType(s->shape) : -1;
                // Text sits in a rectangle's or a freeform's whole box, so other
                // shapes holding text are written as freeforms with margins
                // that put the text where JeffPub does.
                if (hasText && st != 1) st = -1;
                const bool open = s->customPath.isEmpty() && def && def->open;
                QPainterPath outline;
                if (st < 0) {
                    const QSizeF fs = s->rect.size();
                    outline = !s->customPath.isEmpty() ? s->customPath : shapePath(s->shape, fs, s->adj);
                    // Overlapping parts that merge on screen become one outline,
                    // since a freeform's parts fill alternately.
                    // Inner lines (a smiley's eyes) would vanish in the merge, so
                    // each part goes in twice more: an even number of extra
                    // layers leaves the filled area alone but draws their lines.
                    if (outline.fillRule() == Qt::WindingFill && !open) {
                        int parts = 0;
                        for (int k = 0; k < outline.elementCount(); ++k) parts += outline.elementAt(k).type == QPainterPath::MoveToElement;
                        QPainterPath merged = outline.simplified();
                        if (parts > 1 && !s->stroke.isNone()) {
                            merged.addPath(outline);
                            merged.addPath(outline);
                        }
                        outline = merged;
                    }
                    if (outline.isEmpty()) {
                        ++skippedCount;
                        return;
                    }
                    st = 0;
                }
                // A freeform reaching outside its frame (a callout's pointer)
                // gets a frame grown evenly to hold it, keeping the center.
                QRectF frame = s->rect;
                double gx = 0, gy = 0;
                if (st == 0) {
                    const QRectF pb = outline.boundingRect();
                    gx = std::max({0.0, -pb.left(), pb.right() - frame.width()});
                    gy = std::max({0.0, -pb.top(), pb.bottom() - frame.height()});
                    outline.translate(gx, gy);
                    frame.adjust(-gx, -gy, gx, gy);
                }
                const QRectF box = turnedBox(frame, s->rotation);
                const quint32 seq = next++;
                QVector<B> body{flag(0x02)};
                wrapField(body, s);
                body << bytesB(0x0c, 0x28, {}) << bytesB(0x0d, 0x28, {});
                if (hasText) body << u32(0x27, quint32(stx->first));
                body << u32(0x34, 0);
                if (hasText && s->valign != VAlign::Top) body << u32(0x35, s->valign == VAlign::Middle ? 1 : 2);
                body << u32(0xaa, quint32(emu(box.width()))) << u32(0xab, quint32(emu(box.height()))) << u32(0xb7, 0);
                cw.put(seq, {0x01, surfaceSeq[pi], body});
                QVector<Prop> opt = kInsets;
                fillProps(opt, open ? Fill::none() : s->fill);
                strokeProps(opt, s->stroke);
                opt << kTail;
                shadowProps(opt, s->fx.shadow);
                rotationProp(opt, s);
                if (st == 0) freeformProps(opt, outline, frame.size());
                if (hasText) {
                    // Margins from the frame to JeffPub's text area.
                    QRectF tr = s->customPath.isEmpty() ? shapeTextRect(s->shape, s->rect.size(), s->adj) : QRectF(QPointF(), s->rect.size());
                    tr.translate(gx, gy);
                    const QMarginsF m(tr.left() + s->insets.left(), tr.top() + s->insets.top(), frame.width() - tr.right() + s->insets.right(),
                                      frame.height() - tr.bottom() + s->insets.bottom());
                    opt[0].value = quint32(emu(m.left()));
                    opt[1].value = quint32(emu(m.top()));
                    opt[2].value = quint32(emu(m.right()));
                    opt[3].value = quint32(emu(m.bottom()));
                    opt << Prop{0x0080, quint32(stx->first)};
                    m_frames[stx->second][0] = Frame{frame, m, false};
                    m_textShapes << TextShape{stx->first, 0, seq};
                }
                QVector<Prop> topt = {{0x01ff, 0x00400000}, {0x06ff, 0x00020002}};
                topt << kSideLines << kShadowFlags;
                if (!open) inkProps(topt, s->fill);
                wrapProps(opt, s);
                QByteArray sp = spRecord(quint16(st), 0x0a00, s) + escherProps(0xf00b, opt) + escherProps(0xf122, topt) + anchor(box);
                sp += clientBlocks(0xf011, {ref(0x01, seq, 0x68)});
                if (hasText) sp += clientBlocks(0xf00d, {u32(0x01, quint32(stx->first))});
                finish(seq, sp);
                return;
            }
            if (it->type() == ItemType::Line) {
                // A connector: the anchor is the box the line spans. Its shape
                // runs from the box's top left, level first; flips and a turn
                // put its start at the line's start (as Publisher writes them
                // for connectors attached each way).
                auto *l = static_cast<const LineItem *>(it.get());
                const quint32 seq = next++;
                {
                    QVector<B> body{flag(0x02), flag(0x03)};
                    wrapField(body, l);
                    body << bytesB(0x0c, 0x28, {}) << bytesB(0x0d, 0x28, {}) << u32(0xb7, 0);
                    cw.put(seq, {0x20, surfaceSeq[pi], body});
                }
                const double dx = l->p2.x() - l->p1.x(), dy = l->p2.y() - l->p1.y();
                const bool twoBends = l->startVertical == l->endVertical;
                quint16 kind = 32;
                if (l->route == LineItem::Elbow) kind = twoBends ? 34 : 33;
                else if (l->route == LineItem::Curved) kind = twoBends ? 38 : 37;
                bool flipH = dx < 0, flipV = dy < 0;
                int turn = 0;
                if (l->route != LineItem::Straight) {
                    if (l->startVertical) {
                        turn = dy > 0 ? 90 : 270;
                        flipH = (dx > 0) == (dy > 0);
                        flipV = false;
                    } else if (flipH && flipV) {
                        turn = 180;
                        flipH = flipV = false;
                    }
                }
                QVector<Prop> opt = kInsets;
                if (turn) opt << Prop{0x0004, quint32(turn) << 16};
                // Where the middle of a two-bend route sits, in 21600ths of the way.
                if (twoBends && l->route != LineItem::Straight && (l->route == LineItem::Curved || std::abs(l->bend - 0.5) > 1e-6))
                    opt << Prop{0x0147, quint32(qint32(std::lround(l->bend * 21600)))};
                opt << Prop{0x01bf, 0x00100000};
                strokeProps(opt, l->stroke);
                opt << kTail << Prop{0x0303, quint32(l->route == LineItem::Elbow ? 1 : l->route == LineItem::Curved ? 2 : 0)};
                QVector<Prop> topt = kShadowFlags;
                topt << kSideLines;
                QByteArray d;
                putU32(d, quint32(spid));
                putU32(d, 0x0b00 | (flipH ? 0x40 : 0) | (flipV ? 0x80 : 0));
                const QRectF box = QRectF(l->p1, l->p2).normalized();
                wrapProps(opt, l);
                QByteArray sp = escherRecord(0x2, kind, 0xf00a, d) + escherProps(0xf00b, opt) + escherProps(0xf122, topt) + anchor(box);
                sp += clientBlocks(0xf011, {ref(0x01, seq, 0x68)});
                finish(seq, sp);
                return;
            }
            if (it->type() == ItemType::Table) {
                auto *tb = static_cast<const TableItem *>(it.get());
                if (tb->rows <= 0 || tb->cols <= 0 || tb->cells.size() != tb->rows * tb->cols) {
                    ++skippedCount;
                    return;
                }
                const quint32 seq = next++, cellsSeq = next++;
                const int tid = textId++;
                // Cells in reading order; a merged cell is one entry spanning
                // its rows and columns, and the cells under it are left out.
                QTextDocument blank;
                QVector<const QTextDocument *> docs;
                QVector<B> cellRecs;
                QVector<Frame> frames;
                for (int row = 0; row < tb->rows; ++row) {
                    for (int col = 0; col < tb->cols; ++col) {
                        const TableCell &c = tb->cell(row, col);
                        if (c.covered) continue;
                        const QTextDocument *d = m_doc.storyDoc(c.storyId);
                        docs << (d ? d : &blank);
                        frames << Frame{tb->cellRect(row, col).translated(tb->rect.topLeft()), c.margins, true};
                        const int r1 = std::min(tb->rows, row + std::max(1, c.rowSpan)) - 1;
                        const int c1 = std::min(tb->cols, col + std::max(1, c.colSpan)) - 1;
                        QVector<B> f;
                        if (row) f << u32(0x01, quint32(row));
                        if (r1) f << u32(0x02, quint32(r1));
                        if (col) f << u32(0x03, quint32(col));
                        if (c1) f << u32(0x04, quint32(c1));
                        f << u32(0x0a, quint32(emu(c.margins.left()))) << u32(0x0b, quint32(emu(c.margins.right())))
                          << u32(0x0c, quint32(emu(c.margins.top()))) << u32(0x0d, quint32(emu(c.margins.bottom()))) << u32(0x0e, 114300);
                        cellRecs << rec(0x00, f);
                    }
                }
                QVector<quint32> ends;
                addStory(tid, docs, &ends);
                m_frames << frames;
                m_cellEnds << qMakePair(int(m_textIds.size()) - 1, ends);
                m_tableTextIds << tid;
                // Column then row edges: where each ends, and its size.
                QVector<B> grid;
                QVector<qint64> colEdge{0}, rowEdge{0};
                double acc = 0;
                for (int col = 0; col < tb->cols; ++col) {
                    acc += tb->colW.value(col);
                    colEdge << emu(acc);
                    grid << rec(0x00, {u32(0x01, quint32(colEdge.last())), u32(0x02, quint32(colEdge.last() - colEdge[col]))});
                }
                acc = 0;
                for (int row = 0; row < tb->rows; ++row) {
                    acc += tb->rowH.value(row);
                    rowEdge << emu(acc);
                    grid << rec(0x00, {u32(0x01, quint32(rowEdge.last())), u32(0x02, quint32(rowEdge.last() - rowEdge[row]))});
                }
                QVector<B> tableHead{flag(0x02)};
                wrapField(tableHead, tb);
                cw.put(seq, {0x10, surfaceSeq[pi], tableHead + QVector<B>{bytesB(0x0c, 0x28, {}), bytesB(0x0d, 0x28, {}),
                                                 u32(0x27, quint32(tid)), flag(0x2a), u32(0x66, quint32(tb->rows)), u32(0x67, quint32(tb->cols)),
                                                 u32(0x68, quint32(colEdge.last())), u32(0x69, quint32(rowEdge.last())), ref(0x6b, cellsSeq),
                                                 list(0x6d, grid, 0x90), u32(0x70, 0xfffffffdu), u32(0xb7, 0)}});
                cw.put(cellsSeq, {0x63, seq, {u16(0x01, quint32(cellRecs.size())), list(0x02, cellRecs)}});

                // Fills: one record per grid cell, naming its column (03) and row (04).
                auto formatShape = [&](const QVector<Prop> &o, const QVector<Prop> &t, const QVector<B> &where) {
                    if (m_cellFormats.size() >= 1000) return;   // one id block's worth
                    QByteArray d;
                    putU32(d, quint32(0x0c03 + m_cellFormats.size()));
                    putU32(d, 0x0a00);
                    QVector<B> a = where;
                    a.insert(where.isEmpty() || where.first().id != 0x01 ? 0 : 1, ref(0x02, seq, 0x68));
                    m_cellFormats << escherContainer(0xf004, escherRecord(0x2, 1, 0xf00a, d) + escherProps(0xf00b, o) + escherProps(0xf122, t) +
                                                                  clientBlocks(0xf010, a));
                };
                for (int row = 0; row < tb->rows; ++row) {
                    for (int col = 0; col < tb->cols; ++col) {
                        const TableCell &c = tb->cell(row, col);
                        if (c.covered || c.fill.type != Fill::Solid) continue;
                        const quint32 color = bgr(c.fill.color.resolve(m_doc.colors));
                        for (int r = row; r < std::min(tb->rows, row + std::max(1, c.rowSpan)); ++r) {
                            for (int k = col; k < std::min(tb->cols, col + std::max(1, c.colSpan)); ++k) {
                                QVector<B> where;
                                if (k) where << u32(0x03, quint32(k));
                                if (r) where << u32(0x04, quint32(r));
                                QVector<Prop> o = {{0x0181, color}, {0x0183, 0}, {0x01bf, 0x001f001c}, {0x01ff, 0x00080000}, {0x0285, 0},
                                                   {0x02cb, 0}, {0x02cc, 0}, {0x02ce, 0}, {0x02cf, 0}};
                                QVector<Prop> t = {{0x01bf, 0x00600000}, {0x01ff, 0x00400040}};
                                t << kShadowFlags;
                                formatShape(o, t, where);
                            }
                        }
                    }
                }
                // Ruled lines: 01 = 1 across or 2 down, then the grid box it
                // runs along (04 first row, 05 first column, 06 last row, 07
                // last column, as grid lines); zero fields are left out.
                QSet<QString> drawn;
                auto rule = [&](int kind, int r0, int c0, int r1, int c1, const Stroke &st) {
                    if (st.isNone()) return;
                    const QString key = QStringLiteral("%1 %2 %3 %4 %5").arg(kind).arg(r0).arg(c0).arg(r1).arg(c1);
                    if (drawn.contains(key)) return;
                    drawn.insert(key);
                    QVector<B> where{u32(0x01, quint32(kind))};
                    if (r0) where << u32(0x04, quint32(r0));
                    if (c0) where << u32(0x05, quint32(c0));
                    if (r1) where << u32(0x06, quint32(r1));
                    if (c1) where << u32(0x07, quint32(c1));
                    QVector<Prop> o = kInsets;
                    o << Prop{0x0181, bgr(st.color.resolve(m_doc.colors))} << Prop{0x0183, 0xffffffffu} << Prop{0x01bf, 0x001f001c}
                      << Prop{0x01cb, quint32(emu(st.width))} << Prop{0x01d6, 2} << Prop{0x01ff, 0x001f0006} << kTail;
                    QVector<Prop> t = {{0x01bf, 0x00600000}, {0x01ff, 0x03e00020}, {0x057f, 0x00080000}, {0x05bf, 0x00080000},
                                       {0x05ff, 0x00080000}, {0x063f, 0x00080000}};
                    t << kShadowFlags << kSideLines;
                    formatShape(o, t, where);
                };
                for (int row = 0; row < tb->rows; ++row) {
                    for (int col = 0; col < tb->cols; ++col) {
                        const TableCell &c = tb->cell(row, col);
                        if (c.covered) continue;
                        const int r1 = std::min(tb->rows, row + std::max(1, c.rowSpan)), c1 = std::min(tb->cols, col + std::max(1, c.colSpan));
                        rule(1, row, col, row, c1, c.border.top);
                        rule(1, r1, col, r1, c1, c.border.bottom);
                        rule(2, row, col, r1, col, c.border.left);
                        rule(2, row, c1, r1, c1, c.border.right);
                    }
                }

                QVector<Prop> opt = {{0x0080, quint32(tid)}, {0x0081, 0}, {0x0082, 0}, {0x0083, 0}, {0x0084, 0}, {0x017f, 0x00300000},
                                     {0x01c0, 0x08000000}, {0x01c2, 0x08000007}, {0x01cb, 25400}, {0x01ff, 0x00080000}, {0x0201, 0x08000000},
                                     {0x0384, 36576}, {0x0385, 36576}, {0x0386, 36576}, {0x0387, 36576}};
                rotationProp(opt, tb);
                QVector<Prop> topt = {{0x017f, 0x03800000}, {0x01ff, 0x00400000}, {0x054b, 0}, {0x058b, 0}, {0x05cb, 0}, {0x060b, 0},
                                      {0x06ff, 0x00020002}};
                topt << kSideLines;
                wrapProps(opt, tb);
                QByteArray sp = spRecord(201, 0x0a00, tb) + escherProps(0xf00b, opt) + escherProps(0xf122, topt) + anchor(r);
                sp += clientBlocks(0xf011, {ref(0x01, seq, 0x68)});
                sp += clientBlocks(0xf00d, {u32(0x01, quint32(tid))});
                finish(seq, sp);
                return;
            }
            if (it->type() == ItemType::TextArt) {
                // Publisher's Text Art: a shape whose number is the warp, with
                // the words, font, size, spacing and alignment as properties.
                auto *ta = static_cast<const TextArtItem *>(it.get());
                const quint32 seq = next++;
                {
                    QVector<B> body{flag(0x02), flag(0x03)};
                    wrapField(body, ta);
                    body << bytesB(0x0c, 0x28, {}) << bytesB(0x0d, 0x28, {}) << u32(0xb7, 0);
                    cw.put(seq, {0x20, surfaceSeq[pi], body});
                }
                // Publisher draws the words only when their font is in the
                // document's font table.
                fontIndex(ta->font);
                QVector<Prop> opt = kInsets;
                opt << Prop{0xc0c0, 0, utf16z(ta->text)} << Prop{0xc0c5, 0, utf16z(m_doc.pubFonts.contains(ta->font) ? ta->font : interchangeFontName(ta->font))};
                // Alignment: Publisher counts stretch 0, center 1, left 2, right 3, letter 4, word 5.
                static const quint32 kAlign[] = {2, 1, 3, 5, 4, 0};
                if (ta->align != 1 && ta->align >= 0 && ta->align <= 5) opt << Prop{0x00c2, kAlign[ta->align]};
                if (std::abs(ta->size - 36) > 0.01) opt << Prop{0x00c3, quint32(std::llround(ta->size * 65536))};
                if (std::abs(ta->spacing - 1) > 0.001) opt << Prop{0x00c4, quint32(std::llround(ta->spacing * 65536))};
                // On/off settings (all marked as set): Text Art, kerning,
                // stretch to fit, best fit; bold, italic, even height, vertical.
                quint32 flags = 0x5700;
                if (ta->bold) flags |= 0x20;
                if (ta->italic) flags |= 0x10;
                if (ta->evenHeight) flags |= 0x80;
                if (ta->vertical) flags |= 0x2000;
                opt << Prop{0x00ff, 0xffff0000u | flags} << Prop{0x017f, 0x00100010};
                fillProps(opt, ta->fill);
                strokeProps(opt, ta->stroke);
                // Shadow and 3D settings as Publisher writes them for Text Art.
                opt << Prop{0x0201, 0x00d8d8d8} << Prop{0x0204, 0} << Prop{0x0205, 0} << Prop{0x0206, 0} << Prop{0x0209, 0} << Prop{0x020c, 0}
                    << Prop{0x023f, 0x00030000} << Prop{0x027f, 0x00010000} << Prop{0x02bf, 0x000f0001} << Prop{0x02ff, 0x001f0016}
                    << Prop{0x0384, 36576} << Prop{0x0385, 36576} << Prop{0x0386, 36576} << Prop{0x0387, 36576};
                rotationProp(opt, ta);
                QVector<Prop> topt = {{0x017f, 0x02000200}, {0x023f, 0x00040000}, {0x057f, 0x00080000}, {0x05bf, 0x00080000},
                                      {0x05ff, 0x00080000}, {0x063f, 0x00080000}, {0x06ff, 0x00020002}};
                topt << kShadowFlags << kSideLines;
                inkProps(topt, ta->fill);
                wrapProps(opt, ta);
                QByteArray sp = spRecord(quint16(pubTextArtType(ta->transform_)), 0x0a00, ta) + escherProps(0xf00b, opt) +
                                escherProps(0xf122, topt) + anchor(r);
                sp += clientBlocks(0xf011, {ref(0x01, seq, 0x68)});
                finish(seq, sp);
                return;
            }
            if (it->type() == ItemType::Picture) {
                auto *pic = static_cast<const PictureItem *>(it.get());
                // A picture cut to a shape, or see-through, is saved the way
                // .pub files store them: a shape filled with the picture.
                const bool shapedPic = !pic->maskShape.isEmpty() && pic->maskShape != QLatin1String("rect") && shapeDef(pic->maskShape);
                if ((shapedPic || pic->transparency > 0.001) && !pic->imageId.isEmpty()) {
                    auto shaped = std::make_shared<ShapeItem>();
                    static_cast<Item &>(*shaped) = static_cast<const Item &>(*pic);
                    shaped->shape = shapedPic ? pic->maskShape : QStringLiteral("rect");
                    shaped->fill.type = Fill::Picture;
                    shaped->fill.transparency = pic->transparency;
                    shaped->fill.imageId = shapedPictureImage(*pic);
                    visit(shaped);
                    return;
                }
                const int blip = blipIndex(recoloredPictureImage(*pic));
                if (blip < 0) {
                    ++skippedCount;
                    return;
                }
                const quint32 seq = next++, nameSeq = next++;
                const QString file = m_blips[blip - 1].fileName;
                QVector<B> picBody{flag(0x02), flag(0x03)};
                wrapField(picBody, pic);
                picBody << bytesB(0x0c, 0x28, {}) << bytesB(0x0d, 0x28, {}) << u32(0x34, 0) << ref(0x3a, nameSeq) << u32(0xaa, quint32(emu(r.width())))
                        << u32(0xab, quint32(emu(r.height())));
                Chunk c{0x01, surfaceSeq[pi], picBody};
                c.dirVer = 0x0102;
                c.dirB = 1;
                cw.put(seq, c);
                cw.put(nameSeq, {0x66, seq, {str(0x03, file)}});
                QByteArray name;
                const QString base = QFileInfo(file).completeBaseName();
                for (QChar ch : base) putU16(name, ch.unicode());
                putU16(name, 0);
                QVector<Prop> opt = {{0x007f, 0x00800080}};
                opt << kInsets << Prop{0x4104, quint32(blip)} << Prop{0xc105, 0, name} << Prop{0x0106, 1}
                    << Prop{0x0181, 0x08000001} << Prop{0x0183, 0x08000007} << Prop{0x01bf, 0x00100000};
                strokeProps(opt, pic->stroke);
                opt << kTail << Prop{0x033f, 0x00100010};
                // Cropping, as fractions of the picture (16.16) trimmed from each side.
                const QRectF ir = pic->imgRect;
                const QSizeF fs = pic->rect.size();
                if (!ir.isEmpty()) {
                    auto frac = [](double v) { return quint32(qint32(std::llround(v * 65536.0))); };
                    const double t = -ir.top() / ir.height(), b = (ir.bottom() - fs.height()) / ir.height();
                    const double lf = -ir.left() / ir.width(), rt = (ir.right() - fs.width()) / ir.width();
                    if (std::abs(t) > 1e-4) opt << Prop{0x0100, frac(t)};
                    if (std::abs(b) > 1e-4) opt << Prop{0x0101, frac(b)};
                    if (std::abs(lf) > 1e-4) opt << Prop{0x0102, frac(lf)};
                    if (std::abs(rt) > 1e-4) opt << Prop{0x0103, frac(rt)};
                }
                // Adjustments: brightness (0x8000 = all the way), contrast
                // (16.16 multiplier, JeffPub's is the square of 1 + c/100),
                // grayscale and black and white (0x013F), washout as the
                // usual brightness and contrast pair, a recolor in the
                // tertiary props (sepia as a brown one), the clear color.
                double bright = pic->brightness, contrast = std::pow((100.0 + pic->contrast) / 100.0, 2);
                if (pic->recolor == PictureItem::Washout) {
                    bright = 22938 / 327.68;
                    contrast = 19661 / 65536.0;
                }
                if (std::abs(bright) > 1e-3) opt << Prop{0x0109, quint32(qint32(std::llround(std::clamp(bright, -100.0, 100.0) * 327.68)))};
                if (std::abs(contrast - 1) > 1e-6) opt << Prop{0x0108, quint32(std::min<double>(0x7fffffff, std::llround(contrast * 65536)))};
                if (pic->recolor == PictureItem::Grayscale) opt << Prop{0x013f, 0x00040004};
                if (pic->recolor == PictureItem::BlackWhite) opt << Prop{0x013f, 0x00020002};
                if (pic->hasTransparentColor) opt << Prop{0x0107, bgr(pic->transparentColor)};
                shadowProps(opt, pic->fx.shadow);
                rotationProp(opt, pic);
                QVector<Prop> topt = {{0x01ff, 0x00400000}, {0x06ff, 0x00020002}};
                topt << kShadowFlags << kSideLines;
                if (pic->recolor == PictureItem::ColorTint) topt << Prop{0x011a, bgr(pic->recolorColor.resolve(m_doc.colors))};
                if (pic->recolor == PictureItem::Sepia) topt << Prop{0x011a, bgr(QColor::fromRgb(kPubSepia))};
                wrapProps(opt, pic);
                QByteArray sp = spRecord(1, 0x0a00, pic) + escherProps(0xf00b, opt) + escherProps(0xf122, topt) + anchor(r);
                sp += clientBlocks(0xf011, {ref(0x01, seq, 0x68)});
                finish(seq, sp);
                return;
            }
            ++skippedCount;
        };
        for (const ItemPtr &it : surfaces[pi]->items) visit(it);
    }
    // Linked boxes point at their neighbors: 28 = place in the chain,
    // 36 = the box before, 37 = the box after; 2d marks the last box.
    {
        QHash<int, QVector<quint32>> chains;
        for (const TextShape &ts : m_textShapes) {
            QVector<quint32> &c = chains[ts.tid];
            if (c.size() <= ts.index) c.resize(ts.index + 1);
            c[ts.index] = ts.seq;
        }
        for (auto it = chains.cbegin(); it != chains.cend(); ++it) {
            const QVector<quint32> &c = it.value();
            if (c.size() < 2) continue;
            for (int k = 0; k < c.size(); ++k) {
                if (!c[k]) continue;
                QVector<B> f;
                if (k) f << u32(0x28, quint32(k)) << ref(0x36, c[k - 1], 0x68);
                if (k + 1 < c.size()) f << ref(0x37, c[k + 1], 0x68);
                else f << flag(0x2d);
                cw.extend(c[k], f);
            }
        }
    }
    if (skipped && skippedCount) *skipped << QStringLiteral("%1 object(s) couldn't be saved to .pub").arg(skippedCount);
    const quint32 fontSeq = m_fonts.isEmpty() && m_textIds.isEmpty() ? 0 : next++;
    // Fonts the style sheet names, so the font table, written first, has them.
    for (const QString &name : styleNames())
        if (const TextStyle *st = m_doc.style(name)) {
            charBlocks(st->chr);
            QTextDocument tmp;
            QTextCursor(&tmp).setBlockFormat(st->blk);
            paraBlocks(tmp.begin());
        }
    if (m_fonts.isEmpty()) m_fonts << m_doc.fonts.body;

    // ---- the fixed chunks
    QVector<B> pageList;
    for (const MasterSeqs &m : masterSeqs) pageList << ref(0x00, m.seq);
    for (quint32 s : pageSeq) pageList << ref(0x00, s);
    for (quint32 s : kSpecial) pageList << ref(0x00, s);
    cw.put(256, {0x44, 0, {u32(0x01, quint32(pageList.size())), list(0x02, pageList), ref(0x03, 287), ref(0x04, 291), flag(0x08),
                           rec(0x12, {u32(0x01, quint32(pw)), u32(0x02, quint32(ph))}), ref(0x18, 259), ref(0x19, 261), ref(0x1a, 257),
                           ref(0x20, 282), ref(0x21, 262), ref(0x22, 285), u32(0x23, quint32(pageSeq.size())), bytesB(0x2a, 0x38, {}), u16(0x2c, 5),
                           u16(0x2d, quint32(masterSeqs.size())),
                           ref(0x31, 278, 0x68), flag(0x39, 0x00), u32(0x3c, 1), u32(0x41, 0), ref(0x44, 292), flag(0x4d)}});
    cw.put(257, {0x72, 256, {}});
    cw.put(259, {0x73, 256, {}});
    cw.put(261, {0x46, 256, {}});
    cw.put(262, {0x54, 256, {}});
    const QSizeF scratch(22860000, 22860000), ext(110185200, 110185200);
    for (int k = 0; k < masterSeqs.size(); ++k) {
        const MasterSeqs &m = masterSeqs[k];
        const MasterPage *mp = k < m_doc.masters.size() ? m_doc.masters[k].get() : nullptr;
        const QString abbr = mp && !mp->abbr.isEmpty() ? mp->abbr : QStringLiteral("A");
        const QString desc = mp && !mp->name.isEmpty() && mp->name != QLatin1String("Master Page") ? mp->name : QStringLiteral("Master Page ") + abbr;
        cw.put(m.seq, {0x43, 256, pageBody(mp ? pageShapes[m_doc.pages.size() + k] : QVector<quint32>{}, m.sub60, m.sub77, true, false, scratch, ext, 0,
                                           kMaster, m.guides, abbr, desc)});
        cw.put(m.sub60, {0x60, m.seq, {u32(0x05, 1)}});
        cw.put(m.sub77, {0x77, m.seq, webForm()});
    }
    for (int i = 0; i < pageSeq.size(); ++i) {
        cw.put(pageSeq[i], {0x43, 256, pageBody(pageShapes[i], pageSub[i].first, pageSub[i].second, false, false, scratch, ext, i,
                                                masterFor(*m_doc.pages[i]))});
        cw.put(pageSub[i].first, {0x60, pageSeq[i], {}});
        cw.put(pageSub[i].second, {0x77, pageSeq[i], webForm()});
    }
    const QSizeF specScratch[4] = {QSizeF(18973800, 17830800), scratch, scratch, scratch};
    const QSizeF specExt[4] = {QSizeF(106299000, 105156000), ext, ext, ext};
    for (int k = 0; k < 4; ++k) {
        const quint32 s = kSpecial[k];
        cw.put(s, {0x43, 256, pageBody({}, s + 1, s + 2, false, true, specScratch[k], specExt[k])});
        cw.put(s + 1, {0x60, s, {u32(0x05, 1)}});
        cw.put(s + 2, {0x77, s, webForm()});
    }
    cw.put(278, {0x7f, 256, {}});
    QVector<B> textIndex{ref(0x01, 283), ref(0x02, 284)};
    if (fontSeq) textIndex << ref(0x04, fontSeq);
    textIndex << rec(0x0a, {u32(0x01, 0x08000000)}, 0x98);
    cw.put(282, {0x5b, 256, textIndex});
    // 0x61 maps each text id to its text box; 0x65 lists the stories.
    // Tables appear only in 0x65, with 03 = 0.
    {
        QVector<B> map, stories;
        // 0x61: a record per box, in story then chain order; 02 = place in the chain.
        QVector<TextShape> boxes = m_textShapes;
        std::sort(boxes.begin(), boxes.end(), [](const TextShape &a, const TextShape &b) {
            return a.tid != b.tid ? a.tid < b.tid : a.index < b.index;
        });
        for (const TextShape &ts : boxes) {
            QVector<B> f{u32(0x01, quint32(ts.tid))};
            if (ts.index) f << u32(0x02, quint32(ts.index));
            f << ref(0x03, ts.seq, 0x68);
            map << rec(0x00, f);
        }
        for (int i = 0; i < m_textIds.size(); ++i) {
            const int id = m_textIds[i];
            QVector<B> f{u32(0x01, quint32(id))};
            if (m_chainLength.value(id, 1) > 1) f << u16(0x02, quint32(m_chainLength.value(id)));   // boxes in the chain
            if (m_tableTextIds.contains(id)) f << u16(0x03, 0, 0x10);
            if (m_notHyphenated.contains(id)) f << flag(0x04, 0x00);   // not hyphenated automatically
            // AutoFit Text: 05 = 1 for best fit, 3 for shrink on overflow.
            const int fit = m_autofit.value(id, TextItem::NoAutofit);
            if (fit == TextItem::BestFit) f << u16(0x05, 1, 0x10);
            else if (fit == TextItem::ShrinkOnOverflow) f << u16(0x05, 3, 0x10);
            // 07: the story's entry in the frame layout section.
            f << u32(0x07, quint32(i + 1)) << u32(0x08, 0xcb18967cu, 0x58) << u32(0x09, 0xcb18967cu, 0x58);
            if (fit == TextItem::GrowBox) f << flag(0x0c, 0x08);   // grow text box to fit
            stories << rec(0x00, f);
        }
        cw.put(283, {0x61, 282, map.isEmpty() ? QVector<B>{} : QVector<B>{u32(0x01, quint32(map.size())), list(0x02, map)}});
        cw.put(284, {0x65, 282, stories.isEmpty() ? QVector<B>{} : QVector<B>{u32(0x01, quint32(stories.size())), list(0x02, stories)}});
    }
    // Palette: the color scheme.
    {
        QVector<B> colors{flag(0x00, 0x78)};
        for (int i = 1; i < 8; ++i) colors << rec(0x00, {u32(0x01, bgr(m_doc.colors.slot(i)))});
        cw.put(285, {0x5c, 256, {u32(0x01, 8), list(0x02, colors), u32(0x03, 66),
                                 str(0x06, m_doc.colors.name.isEmpty() ? QStringLiteral("JeffPub") : m_doc.colors.name)}});
    }
    // Print settings (no printer named).
    {
        auto setup = [&] {
            return rec(0x00, {u16(0x01, 1), rec(0x02, {u32(0x01, quint32(pw)), u32(0x02, quint32(ph))}, 0x98), u16(0x03, 7), u16(0x05, 2, 0x10),
                              flag(0x06), str(0x08, QString()), str(0x09, QString()), str(0x0a, QString()), rawCont(0x0e, 0x80, QByteArray(2, '\0'))});
        };
        cw.put(287, {0x4b, 256, {u32(0x01, 2), list(0x02, {setup(), setup()}, 0x90), ref(0x03, 288), flag(0x0a), u32(0x0b, 1), flag(0x17),
                                 flag(0x18), flag(0x1a), flag(0x1c), flag(0x1d), u16(0x22, 0), u16(0x23, 0), u32(0x24, 0), u32(0x25, 0)}});
        cw.put(288, {0x4f, 287, {list(0x02, {rec(0x00, {rec(0x02, {u32(0x01, 0xffffffffu)}, 0x98)})}), u32(0x03, 304800), u32(0x04, 243),
                                 u32(0x0c, 3175), u32(0x0d, 3175), u32(0x0e, 70), flag(0x0f), flag(0x10), flag(0x11)}});
    }
    // Margin guides on the master page.
    {
        auto guide = [](qint64 pos, quint8 sideFlag) { return rec(0x00, {u32(0x01, quint32(pos)), flag(sideFlag), flag(0x04)}); };
        for (const MasterSeqs &m : masterSeqs)
            cw.put(m.guides, {0x4c, m.seq, {u32(0x01, 4), list(0x02, {guide(emu(mg.left()), 0x03), guide(pw - emu(mg.right()), 0x02),
                                                                     guide(emu(mg.top()), 0x03), guide(ph - emu(mg.bottom()), 0x02)}),
                                           rec(0x03, {u32(0x01, quint32(pw)), u32(0x02, quint32(ph))}), u32(0x04, 45720), u32(0x05, 45720),
                                           u16(0x06, 2), u16(0x07, 2), u32(0x08, 152400), u32(0x09, 152400), u32(0x0a, 152400), u32(0x0b, 152400)}});
    }
    // Bullet characters (Symbol font).
    {
        QVector<B> bullets;
        const int chars[6] = {183, 183, 168, 222, 224, 42};
        const int sizes[6] = {127000, 152400, 127000, 127000, 127000, 127000};
        for (int i = 0; i < 6; ++i) bullets << rec(0x00, {u16(0x01, quint32(chars[i])), u32(0x02, quint32(sizes[i])), str(0x03, QStringLiteral("Symbol"))});
        cw.put(291, {0x4a, 256, {u16(0x01, 6), list(0x02, bullets)}});
    }
    // Page setup.
    {
        QString paper = m_doc.setup.sizeName;
        if (paper.isEmpty() || paper == QLatin1String("Custom")) {
            // Name standard sizes the way other programs do.
            struct Named { const char *name; double w, h; };
            static const Named named[] = {{"Letter", 612, 792}, {"Legal", 612, 1008}, {"Tabloid", 792, 1224}, {"Executive", 522, 756},
                                          {"A3", 841.9, 1190.6}, {"A4", 595.3, 841.9}, {"A5", 419.5, 595.3}, {"B5", 498.9, 708.7}};
            paper = QStringLiteral("Custom");
            for (const Named &n : named)
                if ((std::abs(ps.width() - n.w) < 1 && std::abs(ps.height() - n.h) < 1) || (std::abs(ps.width() - n.h) < 1 && std::abs(ps.height() - n.w) < 1))
                    paper = QString::fromLatin1(n.name);
        }
        cw.put(292, {0x8a, 256, {rec(0x04, {u32(0x02, 104), u32(0x03, 109), u32(0x04, 0x80010000u), str(0x06, paper), u32(0x07, 16937216),
                                            u32(0x0a, 0), u32(0x11, 1),
                                            list(0x12, {rec(0x00, {u32(0x03, quint32(pw)), u32(0x04, quint32(ph)), u32(0x06, quint32(emu(mg.left()))),
                                                                   u32(0x07, quint32(emu(mg.top()))), u32(0x08, quint32(emu(mg.right()))),
                                                                   u32(0x09, quint32(emu(mg.bottom())))})}, 0x90),
                                            u32(0x13, 1),
                                            list(0x14, {rec(0x00, {u32(0x01, quint32(pw)), u32(0x02, quint32(ph)),
                                                                   rec(0x08, {u32(0x02, 0), u32(0x03, 0)})})}, 0x90)})}});
    }
    // Fonts used by the text.
    if (fontSeq) {
        QVector<B> fonts;
        for (const QString &f : m_fonts) {
            QByteArray ranges(24, '\0'), panose(10, '\0');
            QRawFont raw = QRawFont::fromFont(QFont(f));
            const QByteArray os2 = raw.isValid() ? raw.fontTable("OS/2") : QByteArray();
            if (os2.size() >= 86) {
                panose = os2.mid(32, 10);
                // ulUnicodeRange1-4 then ulCodePageRange1-2, little-endian.
                QByteArray r;
                for (int k = 0; k < 4; ++k) putU32(r, qFromBigEndian<quint32>(os2.constData() + 42 + 4 * k));
                for (int k = 0; k < 2; ++k) putU32(r, qFromBigEndian<quint32>(os2.constData() + 78 + 4 * k));
                ranges = r;
            }
            fonts << rec(0x00, {u16(0x03, 0, 0x10), str(0x04, f), u32(0x06, 0x001000ff), u32(0x07, 376323), bytesB(0x0b, 0x48, ranges),
                                rawCont(0x0d, 0x80, panose), u32(0x0e, 5120)});
        }
        cw.put(fontSeq, {0x6c, 282, {u32(0x01, quint32(fonts.size())), list(0x02, fonts)}});
    }

    // ---- text stream
    const quint32 magic = QRandomGenerator::global()->generate();
    QVector<Section> secs;
    if (!m_text.isEmpty()) secs << Section{"TEXT", "TEXT", 0, m_text, false};
    // The style sheet: Normal (built-in id -1, 10 pt body font, 6 pt after),
    // then the publication's named paragraph styles. Three sections: the
    // names, each style's character and paragraph properties, and what each
    // is based on.
    {
        // Normal's character properties as Publisher writes them: size (twice),
        // a font for each of 35 writing systems, text and highlight colors,
        // and record 0x58. Publisher rejects a sheet whose Normal lacks these.
        static const quint8 kScriptSlots[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
                                              0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1e, 0x1f, 0x21, 0x22, 0x23, 0x24, 0x27, 0x28, 0x2a, 0x2c};
        QVector<B> slotsB;
        for (quint8 s : kScriptSlots) slotsB << rec(s, {u16(0x00, 0)});
        const QVector<B> normalCharB{u32(0x0c, 127000, 0x22), rec(0x24, slotsB, 0x8a), u32(0x39, 127000, 0x22),
                                     rec(0x44, {u32(0x00, 0, 0x22)}, 0x8a), rec(0x48, {u32(0x00, 0, 0x22)}, 0x8a),
                                     rec(0x58, {u16(0x00, 1, 0x12), u32(0x01, 0, 0x22), u32(0x02, 100000, 0x22)}, 0x8a)};
        const QVector<B> normalParaB{u32(0x13, 76200, 0x22), u16(0x30, 5, 0x12), u32(0x34, 1450850, 0x22)};
        // A named style keeps everything Normal has, with its own settings
        // over it; its font fills the first writing systems' slots as a
        // Publisher style's does.
        auto overlay = [](QVector<B> base, const QVector<B> &own) {
            for (const B &o : own) {
                if (o.id == 0x24) {
                    for (B &b : base)
                        if (b.id == 0x24) {
                            QVector<B> slotsOut;
                            QVector<B> ownSlots = o.kids;
                            for (const B &k : ownSlots) slotsOut << k;
                            for (const B &k : b.kids)
                                if (std::none_of(ownSlots.cbegin(), ownSlots.cend(), [&](const B &x) { return x.id == k.id; })) slotsOut << k;
                            std::sort(slotsOut.begin(), slotsOut.end(), [](const B &x, const B &y) { return x.id < y.id; });
                            b.kids = slotsOut;
                        }
                    continue;
                }
                bool replaced = false;
                for (B &b : base)
                    if (b.id == o.id) { b = o; replaced = true; }
                if (!replaced) base << o;
            }
            std::sort(base.begin(), base.end(), [](const B &x, const B &y) { return x.id < y.id; });
            return base;
        };
        QVector<QByteArray> names, props, links;
        auto link = [](quint32 index) {
            QByteArray l;
            putU16(l, 7);
            putU32(l, 0xffffffffu);   // based on no other style: each holds all its settings
            putU32(l, index);
            putU32(l, 4);
            putU16(l, 0);
            return l;
        };
        QByteArray id0;
        putU16(id0, 0);
        putU32(id0, 0xffffffffu);
        names << id0;
        props << styleEntry(normalCharB) << styleEntry(normalParaB);
        links << link(0) << link(0);
        const QStringList named = styleNames();
        for (int i = 0; i < named.size(); ++i) {
            const TextStyle *st = m_doc.style(named[i]);
            // The name: its length in characters, the name in UTF-16, then a
            // zero id (built-in styles have a negative id and no name).
            QByteArray n;
            putU16(n, quint32(named[i].size()));
            for (QChar c : named[i]) putU16(n, c.unicode());
            putU32(n, 0);
            names << n;
            QTextDocument tmp;
            QTextCursor(&tmp).setBlockFormat(st->blk);
            QVector<B> para = paraBlocks(tmp.begin());
            para.erase(std::remove_if(para.begin(), para.end(), [](const B &b) { return b.id == 0x19; }), para.end());
            props << styleEntry(overlay(normalCharB, charBlocks(st->chr))) << styleEntry(overlay(normalParaB, para));
            links << link(quint32(i + 1)) << link(quint32(i + 1));
        }
        secs << Section{"STSH", "STSH", 0, offsetTable(names, magic, 4, 0), false};
        secs << Section{"STSH", "STSH", 1, offsetTable(props, magic, 0, 0), false};
        secs << Section{"STSH", "STSH", 2, offsetTable(links, magic, 0, 0), false};
    }
    QVector<quint32> paraEnds, charEnds;
    QVector<QByteArray> paraPages, charPages;
    if (!m_text.isEmpty()) {
        paraPages = formattingPages(m_paraRuns, &paraEnds);
        charPages = formattingPages(m_charRuns, &charEnds);
        for (int i = 0; i < paraPages.size(); ++i) secs << Section{"FDPP", "FDPP", quint16(i), paraPages[i], i == 0};
        for (int i = 0; i < charPages.size(); ++i) secs << Section{"FDPC", "FDPC", quint16(i), charPages[i], false};
    }
    {
        QByteArray syid;
        putU32(syid, quint32(m_textIds.isEmpty() ? 1 : m_textIds.last() + 1));
        putU32(syid, quint32(m_textIds.size()));
        for (int id : m_textIds) putU32(syid, quint32(id));
        if (m_textIds.isEmpty()) putU32(syid, 0);
        secs << Section{"SYID", "SYID", 0, syid, false};
        QByteArray sgp, ink;
        putU32(sgp, 4);
        putU16(ink, 0x74);
        putU16(ink, 0x74);
        secs << Section{"SGP ", "SGP ", 0, sgp, false} << Section{"INK ", "INK ", 0, ink, false};
    }
    int btepIndex = -1, btecIndex = -1;
    if (!m_text.isEmpty()) {
        btepIndex = secs.size();
        secs << Section{"BTEP", "PLC ", 0, {}, false};
        btecIndex = secs.size();
        secs << Section{"BTEC", "PLC ", 0, {}, false};
    }
    {
        QVector<QByteArray> entries;
        for (const QString &f : m_fonts) {
            QByteArray e;
            putU16(e, quint32(f.size()));
            for (QChar c : f) putU16(e, c.unicode());
            putU32(e, 3);
            entries << e;
        }
        secs << Section{"FONT", "FONT", 0, offsetTable(entries, 4, 4, 0), false};
    }
    // Table cell ends: count - 1, 0, 0xff00, then each cell's end.
    for (const auto &ce : m_cellEnds) {
        QByteArray tcd;
        putU32(tcd, quint32(ce.second.size() - 1));
        putU32(tcd, 0);
        putU32(tcd, 0xff00);
        for (quint32 e : ce.second) putU32(tcd, e);
        secs << Section{"TCD ", "PLC ", quint16(ce.first), tcd, false};
    }
    if (!m_text.isEmpty()) {
        QByteArray strs;
        putU32(strs, quint32(m_storyLengths.size()));
        putU32(strs, 8);
        putU32(strs, 0xff);
        for (quint32 l : m_storyLengths) putU32(strs, l);
        putU32(strs, 0);
        for (int i = 0; i < m_storyLengths.size(); ++i) strs.append(lengthPrefixed(i == 0 ? QVector<B>{u32(0x00, 5, 0x22)} : QVector<B>{}));
        secs << Section{"STRS", "PLC ", 0, strs, false};
    }
    if (!m_frames.isEmpty()) {
        // Frame layout: the last entry number, the count and the numbers,
        // then per story a column record and its frames. A frame gives its
        // box in layout units (147 per inch, absolute, the page center at
        // 110185200 EMU; top and left round up, bottom and right down), its
        // size, its margins (top, left, bottom, right) and fixed settings.
        // Publisher won't open a table without it.
        const double unit = 914400.0 / 147;
        const qint64 origin = 110185200;
        const double pcx = ps.width() / 2, pcy = ps.height() / 2;
        QByteArray mcld;
        putU32(mcld, quint32(m_frames.size()));
        putU32(mcld, quint32(m_frames.size()));
        for (int i = 0; i < m_frames.size(); ++i) putU32(mcld, quint32(i + 1));
        for (const QVector<Frame> &frames : m_frames) {
            mcld += lengthPrefixed({flag(0x00, 0x0a), u32(0x01, 228600, 0x22)});
            putU32(mcld, quint32(frames.size()));
            for (const Frame &frame : frames) {
                Frame f = frame;
                if (f.vertical) {
                    const QPointF c = f.r.center();
                    f.r = QRectF(c.x() - f.r.height() / 2, c.y() - f.r.width() / 2, f.r.height(), f.r.width());
                }
                auto at = [&](double pt, double center, bool start) {
                    const double v = double(origin + emu(pt - center)) / unit;
                    return quint32(qint64(start ? std::ceil(v - 0.01) : std::floor(v + 0.01)));
                };
                mcld += lengthPrefixed({u32(0x00, at(f.r.top(), pcy, true), 0x22), u32(0x01, at(f.r.left(), pcx, true), 0x22),
                                        u32(0x02, at(f.r.bottom(), pcy, false), 0x22), u32(0x03, at(f.r.right(), pcx, false), 0x22),
                                        u32(0x04, quint32(emu(f.r.width())), 0x22), u32(0x05, quint32(emu(f.r.height())), 0x22),
                                        u32(0x06, quint32(emu(f.m.top())), 0x22), u32(0x07, quint32(emu(f.m.left())), 0x22),
                                        u32(0x08, quint32(emu(f.m.bottom())), 0x22), u32(0x09, quint32(emu(f.m.right())), 0x22),
                                        u32(0x0a, f.cell ? 0 : 82676, 0x22), u16(0x0b, 0, 0x1a), u32(0x0d, 0, 0x22), u32(0x11, 0, 0x22),
                                        u32(0x12, f.cell ? 219456000 : 0, 0x22), u16(0x13, 255, 0x12), flag(0x14, 0x0a), u32(0x15, 1, 0x22),
                                        u32(0x16, f.cell ? 0 : 9525, 0x22), u32(0x18, 0, 0x22), flag(0x1a, 0x02),
                                        rec(0x1d, {u32(0x00, 0xfffffffcu, 0x22)}, 0x8a)});
            }
        }
        secs << Section{"MCLD", "MCLD", 0, mcld, false};
    }
    {
        // Text colors: entry 0 the scheme's main color, entry 1 black, then
        // each RGB used, as Publisher writes them.
        QByteArray pl;
        putU32(pl, quint32(2 + m_colors.size()));
        putU32(pl, 0x28);
        putU32(pl, 0);
        pl.append(lengthPrefixed({u32(0x01, 0x08000000, 0x22), u32(0x02, 0xffffffffu, 0x22), u32(0x03, 0x20000000, 0x22),
                                  u32(0x04, 0xffffffffu, 0x22), u32(0x05, 0xffffffffu, 0x22), rawCont(0x06, 0x82, QByteArray(2, '\0'))}));
        pl.append(lengthPrefixed({u32(0x01, 0, 0x22), rawCont(0x06, 0x82, QByteArray(2, '\0'))}));
        for (quint32 c : m_colors)
            pl.append(lengthPrefixed({u32(0x01, c, 0x22), u32(0x02, 0xffffffffu, 0x22), u32(0x03, 0x20000000, 0x22),
                                      rawCont(0x06, 0x82, QByteArray(2, '\0'))}));
        secs << Section{"PL  ", "PL  ", 0, pl, false};
    }
    // Formatting page indexes need the pages' final offsets: lay out once,
    // read the offsets back, then fill them in (sizes don't change).
    QByteArray quill = quillStream(secs);
    if (!m_text.isEmpty()) {
        auto sectionOffset = [&](const char *name) {
            for (int i = 0; i < secs.size(); ++i) {
                const int e = 0x20 + 24 * i;
                if (memcmp(quill.constData() + e + 2, name, 4) == 0) return qFromLittleEndian<quint32>(quill.constData() + e + 16);
            }
            return quint32(0);
        };
        secs[btepIndex].data = pageIndex(paraEnds, sectionOffset("FDPP"));
        secs[btecIndex].data = pageIndex(charEnds, sectionOffset("FDPC"));
        quill = quillStream(secs);
    }

    // ---- drawing stream
    QByteArray escher, delay;
    {
        const int pageShapesCount = int(objs.size());
        // Default shape properties.
        const QVector<Prop> dggOpt = {{0x0081, 36576}, {0x0082, 36576}, {0x0083, 36576}, {0x0084, 36576}, {0x0180, 0}, {0x0181, 0x08000001},
                                      {0x0183, 0x08000007}, {0x01bf, 0x00100010}, {0x01c0, 0x08000000}, {0x01c2, 0x08000007}, {0x01cb, 25400},
                                      {0x01d7, 2}, {0x01ff, 0x00080008}, {0x0201, 0x08000000}, {0x0285, 0}, {0x02cb, 0}, {0x02cc, 0}, {0x02ce, 0},
                                      {0x02cf, 0}, {0x0384, 36576}, {0x0385, 36576}, {0x0386, 36576}, {0x0387, 36576}};
        QVector<Prop> dggTopt = {{0x01ff, 0x00400000}};
        dggTopt << kShadowFlags << kSideLines;
        QByteArray dgg;
        const quint32 formats = quint32(m_cellFormats.size());
        putU32(dgg, 0x0c03 + formats);    // largest shape id + 1
        putU32(dgg, 4);                   // clusters + 1
        putU32(dgg, quint32(5 + pageShapesCount) + formats);
        putU32(dgg, 3);                   // drawings
        putU32(dgg, 1);
        putU32(dgg, quint32(1 + pageShapesCount));
        putU32(dgg, 2);
        putU32(dgg, 2);
        putU32(dgg, 3);
        putU32(dgg, 3 + formats);
        QByteArray menu;
        for (quint32 v : {0x08000001u, 0x08000000u, 0x08000004u, 0x100000f7u}) putU32(menu, v);
        // The picture store: an entry per image pointing into the delay stream.
        QByteArray bstore;
        if (!m_blips.isEmpty()) {
            QByteArray entries;
            for (const Blip &b : m_blips) {
                QByteArray e;
                e.append(char(b.kind));
                e.append(char(b.kind));
                e += b.uid;
                putU16(e, 0x00ff);
                putU32(e, quint32(b.record.size()));
                putU32(e, b.refs);
                putU32(e, quint32(delay.size()));
                putU32(e, 0);
                entries += escherRecord(0x2, b.kind, 0xf007, e);
                delay += b.record;
            }
            bstore = escherRecord(0xf, quint16(m_blips.size()), 0xf001, entries);
        }
        escher += escherContainer(0xf000, escherRecord(0x0, 0, 0xf006, dgg) + bstore + escherProps(0xf00b, dggOpt) + escherProps(0xf122, dggTopt) +
                                              escherRecord(0x0, 4, 0xf11e, menu));
        auto spgrHead = [](quint32 groupId) {
            QByteArray sp;
            putU32(sp, groupId);
            putU32(sp, 0x05);
            return escherContainer(0xf004, escherRecord(0x1, 0, 0xf009, QByteArray(16, '\0')) + escherRecord(0x2, 0, 0xf00a, sp));
        };
        auto dgRecord = [](quint16 id, quint32 count, quint32 last) {
            QByteArray d;
            putU32(d, count);
            putU32(d, last);
            return escherRecord(0x0, id, 0xf008, d);
        };
        // Drawing 3: table format defaults.
        {
            QByteArray sp1;
            putU32(sp1, 0x0c01);
            putU32(sp1, 0x0a00);
            QVector<Prop> o1 = {{0x0081, 36576}, {0x0082, 36576}, {0x0083, 36576}, {0x0084, 36576}, {0x0181, 0xffffffffu}, {0x01bf, 0x001f000c},
                                {0x01c0, 0x08000000}, {0x01c2, 0x08000007}, {0x01cb, 25400}, {0x01ff, 0x00080008}, {0x0201, 0x08000000},
                                {0x0285, 0}, {0x02cb, 0}, {0x02cc, 0}, {0x02ce, 0}, {0x02cf, 0}, {0x0384, 36576}, {0x0385, 36576}, {0x0386, 36576},
                                {0x0387, 36576}};
            QVector<Prop> t1 = {{0x01bf, 0x00600000}, {0x01ff, 0x00400000}};
            t1 << kShadowFlags << kSideLines;
            QByteArray s1 = escherRecord(0x2, 1, 0xf00a, sp1) + escherProps(0xf00b, o1) + escherProps(0xf122, t1) +
                            escherRecord(0xa, 0x1a, 0xf010, lengthPrefixed({}));
            QByteArray sp2;
            putU32(sp2, 0x0c02);
            putU32(sp2, 0x0a00);
            QVector<Prop> o2 = {{0x0081, 36576}, {0x0082, 36576}, {0x0083, 36576}, {0x0084, 36576}, {0x01bf, 0x00100000}, {0x01c0, 0xffffffffu},
                                {0x01c2, 0x08000007}, {0x01cb, 0}, {0x01ff, 0x00080000}, {0x0201, 0x08000000}, {0x0285, 0}, {0x02cb, 0}, {0x02cc, 0},
                                {0x02ce, 0}, {0x02cf, 0}, {0x0384, 36576}, {0x0385, 36576}, {0x0386, 36576}, {0x0387, 36576}};
            QVector<Prop> t2 = kShadowFlags;
            t2 << kSideLines;
            QByteArray s2 = escherRecord(0x2, 20, 0xf00a, sp2) + escherProps(0xf00b, o2) + escherProps(0xf122, t2) + clientBlocks(0xf010, {u32(0x01, 1)});
            QByteArray cellFormats;
            for (const QByteArray &f : m_cellFormats) cellFormats += f;
            QByteArray dg = dgRecord(3, 3 + formats, 0x0c02 + formats) +
                            escherContainer(0xf003, spgrHead(0x0c00) + escherContainer(0xf004, s1) + escherContainer(0xf004, s2) + cellFormats);
            putU32(escher, 2);
            escher += escherContainer(0xf002, dg);
        }
        // Drawing 2: the page background.
        {
            QByteArray sp;
            putU32(sp, 0x0801);
            putU32(sp, 0x0c00);
            QByteArray bg = escherRecord(0x2, 1, 0xf00a, sp) + escherProps(0xf00b, {{0x0304, 9}, {0x033f, 0x00010001}}) +
                            escherProps(0xf122, {{0x01ff, 0x00400040}});
            QByteArray dg = dgRecord(2, 1, 0x0801) + escherContainer(0xf003, spgrHead(0x0800)) + escherContainer(0xf004, bg);
            putU32(escher, 1);
            escher += escherContainer(0xf002, dg);
        }
        // Drawing 1: the pages' objects.
        {
            QByteArray shapes;
            for (const Obj &o : objs) shapes += o.escher;
            // Connector rules, one per line as Publisher writes them: its
            // start attaches to a site of shape A, its end to one of shape B
            // (shape 0 and site -1 for a loose end).
            QByteArray rules;
            quint32 ruleCount = 0;
            for (const auto *list : surfaces)
                walkItems(list->items, [&](const ItemPtr &it) {
                    if (it->type() != ItemType::Line || !spidOf.contains(it->id)) return;
                    const auto *l = static_cast<const LineItem *>(it.get());
                    const quint32 a = l->start.id.isEmpty() ? 0 : spidOf.value(l->start.id), b = l->end.id.isEmpty() ? 0 : spidOf.value(l->end.id);
                    QByteArray r;
                    for (quint32 v : {++ruleCount, a, b, spidOf.value(it->id), a ? quint32(l->start.site) : 0xffffffffu, b ? quint32(l->end.site) : 0xffffffffu})
                        putU32(r, v);
                    rules += escherRecord(0x1, 0, 0xf012, r);
                });
            QByteArray dg = dgRecord(1, quint32(1 + pageShapesCount), quint32(0x400 + pageShapesCount)) +
                            escherContainer(0xf003, spgrHead(0x0400) + shapes);
            if (ruleCount) dg += escherRecord(0xf, quint16(ruleCount), 0xf005, rules);
            putU32(escher, 0);
            escher += escherContainer(0xf002, dg);
        }
    }

    // ---- property sets and the compound file
    QVector<QPair<quint32, QByteArray>> summaryProps{{1, vtI2(1252)}, {2, vtString(m_doc.props.title)}, {4, vtString(m_doc.props.author)}};
    if (!m_thumbnail.isNull()) summaryProps.append({17, vtThumbnail(m_thumbnail)});
    const QByteArray summary = propertySet(guidBytes(0xf29f85e0, 0x4ff9, 0x1068, QByteArray::fromHex("ab9108002b27b3d9")), summaryProps);
    const QByteArray docSummary = propertySet(guidBytes(0xd5cdd502, 0x2e9c, 0x101b, QByteArray::fromHex("939708002b2cf9ae")), {{1, vtI2(1252)}});
    const QByteArray pubClsid = guidBytes(0x00021201, 0x0000, 0x0000, QByteArray::fromHex("c000000000000046"));
    const QByteArray quillClsid = guidBytes(0x08c8f6da, 0x969d, 0x11d1, QByteArray::fromHex("8e0200c04fb6fece"));
    auto compObj = [](const QByteArray &clsid, const QString &userType, const QString &clipName, const QString &progId) {
        QByteArray o = QByteArray::fromHex("0100feff030a0000ffffffff");
        o.append(clsid);
        auto ansi = [&](const QString &s) {
            if (s.isEmpty()) {
                putU32(o, 0);
                return;
            }
            QByteArray b = s.toLatin1();
            b.append('\0');
            putU32(o, quint32(b.size()));
            o.append(b);
        };
        ansi(userType);
        ansi(clipName);
        ansi(progId);
        putU32(o, 0x71b239f4);
        o.append(QByteArray(12, '\0'));
        return o;
    };

    cfb::File f;
    auto add = [&](int parent, cfb::Entry::Type type, const QString &name, const QByteArray &data = {}, const QByteArray &clsid = {}) {
        cfb::Entry e;
        e.type = type;
        e.name = name;
        e.clsid = clsid.isEmpty() ? QByteArray(16, '\0') : clsid;
        e.data = data;
        f.entries << e;
        const int idx = f.entries.size() - 1;
        if (parent >= 0) f.entries[parent].children << idx;
        return idx;
    };
    const int root = add(-1, cfb::Entry::Root, QStringLiteral("Root Entry"), {}, pubClsid);
    add(root, cfb::Entry::Storage, QStringLiteral("Objects"));
    const int quillSt = add(root, cfb::Entry::Storage, QStringLiteral("Quill"));
    const int quillSub = add(quillSt, cfb::Entry::Storage, QStringLiteral("QuillSub"), {}, quillClsid);
    add(quillSub, cfb::Entry::Stream, QStringLiteral("\x01") + QStringLiteral("CompObj"),
        compObj(quillClsid, QStringLiteral("Quill96 Story Group Class"), QString(), QString()));
    add(quillSub, cfb::Entry::Stream, QStringLiteral("CONTENTS"), quill);
    const int escherSt = add(root, cfb::Entry::Storage, QStringLiteral("Escher"));
    add(escherSt, cfb::Entry::Stream, QStringLiteral("EscherStm"), escher);
    add(escherSt, cfb::Entry::Stream, QStringLiteral("EscherDelayStm"), delay);
    add(root, cfb::Entry::Stream, QStringLiteral("\x01") + QStringLiteral("CompObj"),
        compObj(pubClsid, QStringLiteral("Publication"), QString(), QString()));
    add(root, cfb::Entry::Storage, QStringLiteral("VBA"));
    add(root, cfb::Entry::Stream, QStringLiteral("Envelope"), {});
    add(root, cfb::Entry::Stream, QStringLiteral("\x03") + QStringLiteral("Internal"), QByteArray::fromHex("000001000000000000000000"));
    add(root, cfb::Entry::Stream, QStringLiteral("Contents"), cw.build(m_path));
    add(root, cfb::Entry::Stream, QStringLiteral("\x05") + QStringLiteral("SummaryInformation"), summary);
    add(root, cfb::Entry::Stream, QStringLiteral("\x05") + QStringLiteral("DocumentSummaryInformation"), docSummary);
    return cfb::write(f);
}

} // namespace

bool exportPublisher(const Document &doc, const QString &path, QString *error, const QImage &thumbnail)
{
    QStringList skipped;
    PubWriter w(doc, path, thumbnail);
    const QByteArray bytes = w.write(&skipped);
    // Written beside the file and swapped in only when complete: a full disk
    // or a crash leaves the old file as it was (often the only copy of a
    // publication made in the other program).
    QSaveFile f(path);
    f.setDirectWriteFallback(true);   // some sync and security tools refuse the temporary file
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    if (error) *error = skipped.join(QStringLiteral("; "));
    return true;
}

QPair<quint32, quint32> packPubInks(const QColor &cmyk)
{
    const QColor k = cmyk.toCmyk();
    const quint64 v[4] = {quint64(std::lround(k.cyanF() * 255)), quint64(std::lround(k.magentaF() * 255)), quint64(std::lround(k.yellowF() * 255)),
                          quint64(std::lround(k.blackF() * 255))};
    quint64 bits = 8;   // bits per ink
    int at = 9;
    for (int i = 0; i < 4; ++i)
        if (v[i]) {
            bits |= quint64(0x100) >> i;   // this ink is there
            bits |= v[i] << at;
            at += 8;
        }
    return {quint32(bits & 0x7fffffff), quint32((bits >> 31) & 0x7fffffff)};
}

} // namespace jp
