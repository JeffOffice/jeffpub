#include "io/importers.h"

#include "app/editor.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "io/zip.h"
#include "render/renderer.h"
#include "text/textprops.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QFontMetricsF>
#include <QLocale>
#include <QDateTime>
#include <QPrinter>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QXmlStreamReader>

namespace jp {

// ---------------- inflate (for .docx and .xlsx, which use deflate) ----------------
// A compact raw-DEFLATE decoder (RFC 1951) so .docx and .xlsx files can be read without zlib headers.
namespace {
struct Inflater {
    // Text and table parts of an office document stay far below this; more
    // is a decompression bomb (a few MB that inflate to gigabytes).
    static constexpr qsizetype kMaxOut = 256 * 1024 * 1024;
    const uchar *in;
    qsizetype n, pos = 0;
    quint32 bitbuf = 0;
    int bitcnt = 0;
    QByteArray out;
    bool ok = true;
    int bits(int need)
    {
        quint32 v = bitbuf;
        while (bitcnt < need) {
            if (pos >= n) { ok = false; return 0; }
            v |= quint32(in[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = v >> need;
        bitcnt -= need;
        return int(v & ((1u << need) - 1));
    }
    struct Huff { QVector<short> count, symbol; };
    int decode(const Huff &h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (!ok) return -1;
        }
        ok = false;
        return -1;
    }
    static Huff build(const short *lengths, int n)
    {
        Huff h;
        h.count = QVector<short>(16, 0);
        h.symbol = QVector<short>(n, 0);
        for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
        QVector<short> offs(16, 0);
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; ++s)
            if (lengths[s]) h.symbol[offs[lengths[s]]++] = short(s);
        h.count[0] = 0;
        return h;
    }
    bool codes(const Huff &lencode, const Huff &distcode)
    {
        static const short lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        while (ok) {
            int sym = decode(lencode);
            if (sym < 0) return false;
            if (out.size() > kMaxOut) return false;
            if (sym < 256) out.append(char(sym));
            else if (sym == 256) return true;
            else {
                sym -= 257;
                if (sym >= 29) return false;
                const int len = lbase[sym] + bits(lext[sym]);
                const int ds = decode(distcode);
                if (ds < 0 || ds >= 30) return false;
                const int dist = dbase[ds] + bits(dext[ds]);
                if (dist > out.size()) return false;
                for (int i = 0; i < len; ++i) out.append(out[out.size() - dist]);
            }
        }
        return false;
    }
    bool run()
    {
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (type == 0) {
                bitbuf = 0; bitcnt = 0;
                if (pos + 4 > n) return false;
                const int len = in[pos] | (in[pos + 1] << 8);
                pos += 4;
                if (pos + len > n || out.size() + len > kMaxOut) return false;
                out.append(reinterpret_cast<const char *>(in + pos), len);
                pos += len;
            } else if (type == 1) {
                static Huff fl, fd;
                static bool init = false;
                if (!init) {
                    short l[288];
                    for (int s = 0; s < 144; ++s) l[s] = 8;
                    for (int s = 144; s < 256; ++s) l[s] = 9;
                    for (int s = 256; s < 280; ++s) l[s] = 7;
                    for (int s = 280; s < 288; ++s) l[s] = 8;
                    fl = build(l, 288);
                    short d[30];
                    for (short &x : d) x = 5;
                    fd = build(d, 30);
                    init = true;
                }
                if (!codes(fl, fd)) return false;
            } else if (type == 2) {
                const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                short lengths[320] = {0};
                for (int i = 0; i < ncode; ++i) lengths[order[i]] = short(bits(3));
                const Huff lencode = build(lengths, 19);
                int idx = 0;
                while (idx < nlen + ndist && ok) {
                    int sym = decode(lencode);
                    if (sym < 0) return false;
                    if (sym < 16) lengths[idx++] = short(sym);
                    else {
                        int len = 0, rep;
                        if (sym == 16) { if (idx == 0) return false; len = lengths[idx - 1]; rep = 3 + bits(2); }
                        else if (sym == 17) rep = 3 + bits(3);
                        else rep = 11 + bits(7);
                        while (rep-- && idx < nlen + ndist) lengths[idx++] = short(len);
                    }
                }
                const Huff lc = build(lengths, nlen), dc = build(lengths + nlen, ndist);
                if (!codes(lc, dc)) return false;
            } else {
                return false;
            }
        } while (!last && ok);
        return ok;
    }
};

// Reads zip entries, inflating deflated ones (.docx and .xlsx documents).
QMap<QString, QByteArray> readOfficeZip(const QByteArray &zip)
{
    QMap<QString, QByteArray> out;
    auto u16 = [&](qsizetype at) { return at >= 0 && at + 2 <= zip.size() ? quint16(uchar(zip[at]) | (uchar(zip[at + 1]) << 8)) : quint16(0); };
    auto u32 = [&](qsizetype at) { return quint32(u16(at)) | (quint32(u16(at + 2)) << 16); };
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - 22; i >= std::max<qsizetype>(0, zip.size() - 65557); --i)
        if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return out;
    const int count = u16(eocd + 10);
    qsizetype p = u32(eocd + 16);
    for (int i = 0; i < count && p + 46 <= zip.size(); ++i) {
        const int method = u16(p + 10);
        const quint32 csize = u32(p + 20);
        const int nlen = u16(p + 28), xlen = u16(p + 30), clen = u16(p + 32);
        const quint32 local = u32(p + 42);
        if (p + 46 + nlen + xlen + clen > zip.size()) break;   // the name and extras must fit
        const QString name = QString::fromUtf8(zip.constData() + p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (qsizetype(local) + 30 > zip.size()) continue;
        const qsizetype data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + qsizetype(csize) > zip.size()) continue;
        if (method == 0) out.insert(name, zip.mid(data, csize));
        else if (method == 8) {
            Inflater inf{reinterpret_cast<const uchar *>(zip.constData() + data), qsizetype(csize)};
            if (inf.run() || !inf.out.isEmpty()) out.insert(name, inf.out);
        }
    }
    return out;
}
} // namespace

QString docxToHtml(const QByteArray &docx)
{
    const auto files = readOfficeZip(docx);
    const QByteArray xml = files.value("word/document.xml");
    if (xml.isEmpty()) return {};
    QXmlStreamReader r(xml);
    QString html = "<html><body>";
    bool inPara = false, bold = false, italic = false, underline = false;
    QString align;
    while (!r.atEnd()) {
        r.readNext();
        const QStringView n = r.name();
        if (r.isStartElement()) {
            if (n == u"p") { inPara = true; align.clear(); html += "<p>"; }
            else if (n == u"jc") align = r.attributes().value("w:val").toString();
            else if (n == u"r") { bold = italic = underline = false; }
            else if (n == u"b") bold = r.attributes().value("w:val") != u"0";
            else if (n == u"i") italic = r.attributes().value("w:val") != u"0";
            else if (n == u"u") underline = r.attributes().value("w:val") != u"none";
            else if (n == u"t") {
                QString t = r.readElementText().toHtmlEscaped();
                if (bold) t = "<b>" + t + "</b>";
                if (italic) t = "<i>" + t + "</i>";
                if (underline) t = "<u>" + t + "</u>";
                html += t;
            } else if (n == u"tab") html += "\t";
            else if (n == u"br") html += "<br>";
        } else if (r.isEndElement() && n == u"p" && inPara) {
            if (!align.isEmpty()) {
                const QString a = align == u"center" ? "center" : align == u"right" || align == u"end" ? "right" : align == u"both" ? "justify" : "left";
                const int at = html.lastIndexOf("<p>");
                if (at >= 0) html.replace(at, 3, QStringLiteral("<p align=\"%1\">").arg(a));
            }
            html += "</p>";
            inPara = false;
        }
    }
    return html + "</body></html>";
}

static QString rtfToText(const QByteArray &rtf)
{
    // Plain-text extraction: drop groups like fonttbl/colortbl and control words.
    QString out;
    int depth = 0, skipDepth = -1;
    for (int i = 0; i < rtf.size(); ++i) {
        const char c = rtf[i];
        if (c == '{') { ++depth; continue; }
        if (c == '}') { if (depth == skipDepth) skipDepth = -1; --depth; continue; }
        if (skipDepth >= 0) continue;
        if (c == '\\') {
            QByteArray word;
            int j = i + 1;
            if (j < rtf.size() && rtf[j] == '\'') {
                const int v = rtf.mid(j + 1, 2).toInt(nullptr, 16);
                out += QString::fromLatin1(QByteArray(1, char(v)));
                i = j + 2;
                continue;
            }
            if (j < rtf.size() && !isalpha(uchar(rtf[j]))) { if (rtf[j] == '\\' || rtf[j] == '{' || rtf[j] == '}') out += QChar(rtf[j]); i = j; continue; }
            while (j < rtf.size() && isalpha(uchar(rtf[j]))) word += rtf[j++];
            while (j < rtf.size() && (isdigit(uchar(rtf[j])) || rtf[j] == '-')) ++j;
            if (j < rtf.size() && rtf[j] == ' ') ++j;
            i = j - 1;
            if (word == "par" || word == "line") out += '\n';
            else if (word == "tab") out += '\t';
            else if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info" || word == "pict" || word == "header" || word == "footer") skipDepth = depth;
            continue;
        }
        if (c == '\r' || c == '\n') continue;
        out += QChar(uchar(c));
    }
    return out.trimmed();
}

bool loadTextFileInto(QTextDocument *doc, const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    if (suffix == "docx") {
        c.insertFragment(QTextDocumentFragment::fromHtml(docxToHtml(data)));
    } else if (suffix == "html" || suffix == "htm") {
        c.insertFragment(QTextDocumentFragment::fromHtml(QString::fromUtf8(data)));
    } else if (suffix == "md") {
        QTextDocument tmp;
        tmp.setMarkdown(QString::fromUtf8(data));
        c.insertFragment(QTextDocumentFragment(&tmp));
    } else if (suffix == "rtf") {
        c.insertText(rtfToText(data));
    } else {
        QStringDecoder dec(QStringConverter::Utf8);
        QString text = dec.decode(data);
        if (dec.hasError()) text = QString::fromLatin1(data);
        c.insertText(text);
    }
    return true;
}

// ---------------- mail merge ----------------
QVector<QStringList> parseDelimited(const QString &text, QChar sep)
{
    QVector<QStringList> rows;
    QStringList row;
    QString cell;
    bool quoted = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else quoted = false;
            } else cell += ch;
            continue;
        }
        if (ch == '"') quoted = true;
        else if (ch == sep) { row << cell; cell.clear(); }
        else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            row << cell;
            cell.clear();
            if (!(row.size() == 1 && row[0].isEmpty())) rows << row;
            row.clear();
        } else cell += ch;
    }
    if (!cell.isEmpty() || !row.isEmpty()) { row << cell; rows << row; }
    return rows;
}

static QVector<QStringList> readXlsx(const QByteArray &data)
{
    const auto files = readOfficeZip(data);
    QStringList shared;
    {
        QXmlStreamReader r(files.value("xl/sharedStrings.xml"));
        QString cur;
        bool inSi = false;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == u"si") { inSi = true; cur.clear(); }
            else if (r.isStartElement() && r.name() == u"t" && inSi) cur += r.readElementText();
            else if (r.isEndElement() && r.name() == u"si") { shared << cur; inSi = false; }
        }
    }
    QVector<QStringList> rows;
    QXmlStreamReader r(files.value("xl/worksheets/sheet1.xml"));
    QStringList row;
    int rowIndex = -1;
    QString cellRef, cellType;
    // A column from its letters ("C" is 2), or -1 past the 16,384 columns a
    // spreadsheet has (the row is padded up to the column).
    auto colOf = [](const QString &ref) {
        int c = 0;
        for (QChar ch : ref) {
            const QChar u = ch.toUpper();
            if (u < QLatin1Char('A') || u > QLatin1Char('Z')) break;
            c = c * 26 + (u.unicode() - 'A' + 1);
            if (c > 16384) return -1;
        }
        return c - 1;
    };
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == u"row") { row.clear(); ++rowIndex; }
        else if (r.isStartElement() && r.name() == u"c") { cellRef = r.attributes().value("r").toString(); cellType = r.attributes().value("t").toString(); }
        else if (r.isStartElement() && (r.name() == u"v" || r.name() == u"t")) {
            QString v = r.readElementText();
            if (cellType == "s") v = shared.value(v.toInt());
            const int col = colOf(cellRef);
            while (row.size() < col) row << QString();
            if (col >= 0 && col == row.size()) row << v; else if (col >= 0 && col < row.size()) row[col] = v;
        } else if (r.isEndElement() && r.name() == u"row") rows << row;
    }
    return rows;
}

static QVector<QStringList> readVcf(const QString &text, QStringList *fields)
{
    *fields = {"First Name", "Last Name", "Company", "Address Line 1", "City", "State", "ZIP Code", "Country", "Email", "Phone"};
    QVector<QStringList> rows;
    QStringList cur;   // the card being read; empty outside one
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("BEGIN:VCARD", Qt::CaseInsensitive)) cur = QStringList(fields->size());
        else if (cur.isEmpty()) continue;   // anything outside BEGIN/END belongs to no card
        else if (line.startsWith("END:VCARD", Qt::CaseInsensitive)) { rows << cur; cur.clear(); }
        else if (line.startsWith("N:") || line.startsWith("N;")) {
            const QStringList p = line.section(':', 1).split(';');
            cur[1] = p.value(0); cur[0] = p.value(1);
        } else if (line.startsWith("ORG")) cur[2] = line.section(':', 1).section(';', 0, 0);
        else if (line.startsWith("ADR")) {
            const QStringList p = line.section(':', 1).split(';');
            cur[3] = p.value(2); cur[4] = p.value(3); cur[5] = p.value(4); cur[6] = p.value(5); cur[7] = p.value(6);
        } else if (line.startsWith("EMAIL")) cur[8] = line.section(':', 1);
        else if (line.startsWith("TEL") && cur[9].isEmpty()) cur[9] = line.section(':', 1);
    }
    return rows;
}

bool loadMergeSource(const QString &path, MergeSource *out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const QByteArray data = f.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();
    QVector<QStringList> rows;
    MergeSource m;
    m.path = path;
    if (suffix == "xlsx") rows = readXlsx(data);
    else if (suffix == "vcf") rows = readVcf(QString::fromUtf8(data), &m.fields);
    else {
        QString text = QString::fromUtf8(data);
        if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
        const QString first = text.section('\n', 0, 0);
        const QChar sep = suffix == "tsv" || first.count('\t') > first.count(',') ? QChar('\t') : first.count(';') > first.count(',') ? QChar(';') : QChar(',');
        rows = parseDelimited(text, sep);
    }
    if (m.fields.isEmpty()) {
        if (rows.isEmpty()) {
            if (error) *error = QStringLiteral("The file has no rows.");
            return false;
        }
        m.fields = rows.takeFirst();
        for (QString &fl : m.fields) fl = fl.trimmed();
    }
    for (QStringList &r : rows) {
        while (r.size() < m.fields.size()) r << QString();
        m.rows << r;
        m.include << true;
    }
    *out = m;
    return true;
}

bool saveMergeCsv(const MergeSource &m, const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    auto esc = [](QString s) {
        if (s.contains(',') || s.contains('"') || s.contains('\n')) s = '"' + s.replace("\"", "\"\"") + '"';
        return s;
    };
    QStringList lines;
    QStringList head;
    for (const auto &h : m.fields) head << esc(h);
    lines << head.join(',');
    for (const auto &r : m.rows) {
        QStringList cells;
        for (const auto &c : r) cells << esc(c);
        lines << cells.join(',');
    }
    f.write(lines.join("\r\n").toUtf8());
    return true;
}

// Freeze merge fields in these items to one record's values: text fields
// become plain text, picture fields load the record's picture.
static void freezeMergeFields(Document &out, const MergeSource &src, const ItemList &items, int rec)
{
    FieldContext ctx;
    ctx.doc = &out;
    ctx.mergeRecord = rec;
    walkItems(items, [&](const ItemPtr &it) {
        QStringList stories;
        if (it->type() == ItemType::Text) stories << static_cast<TextItem *>(it.get())->storyId;
        if (it->type() == ItemType::Shape) stories << static_cast<ShapeItem *>(it.get())->storyId;
        if (it->type() == ItemType::Table) for (const auto &c : static_cast<TableItem *>(it.get())->cells) stories << c.storyId;
        if (it->type() == ItemType::Picture && it->name.startsWith("merge:")) {
            const QString file = src.value(rec, it->name.mid(6));
            QFile f(QDir(QFileInfo(src.path).absolutePath()).absoluteFilePath(file));
            if (!file.isEmpty() && f.open(QIODevice::ReadOnly)) {
                auto *pic = static_cast<PictureItem *>(it.get());
                pic->imageId = out.addImage(f.readAll(), QFileInfo(file).suffix().toLower(), f.fileName());
                pic->fitImage(out.imageSize(pic->imageId), true);
            }
            it->name.clear();
        }
        for (const QString &sid : stories) {
            QTextDocument *d = out.storyDoc(sid);
            if (!d) continue;
            for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) {
                QVector<QPair<int, QString>> fields;
                for (auto fi = b.begin(); !fi.atEnd(); ++fi) {
                    const QString code = fi.fragment().charFormat().stringProperty(tp::Field);
                    if (code.startsWith("merge")) fields << qMakePair(fi.fragment().position(), code);
                }
                for (int k = fields.size() - 1; k >= 0; --k) {
                    QTextCursor c(d);
                    c.setPosition(fields[k].first);
                    c.setPosition(fields[k].first + 1, QTextCursor::KeepAnchor);
                    QTextCharFormat cf = c.charFormat();
                    cf.clearProperty(tp::Field);
                    c.insertText(ctx.resolve(fields[k].second), cf);
                }
            }
        }
    });
}

// Replace merge fields with each record's values in a copy of the
// publication: every page once per record, or, with a catalog area, the
// catalog page once per pageful of records with a record in each cell.
std::unique_ptr<Document> mergeToNewPublication(const Document &src)
{
    QString err;
    auto out = publicationFromBytes(publicationBytes(src, QImage()), &err);
    if (!out) return Document::blank(src.pageSize());
    out->pages.clear();
    out->catalog = CatalogArea();
    auto tmpl = publicationFromBytes(publicationBytes(src, QImage()), &err);
    const QVector<int> records = src.merge.includedRows();
    auto copyPage = [&](const Page &pg) {
        auto np = out->addPage(-1, pg.masterId);
        np->background = pg.background;
        np->guides = pg.guides;
        np->title = pg.title;
        return np;
    };
    if (src.catalog.isActive()) {
        const CatalogArea &cat = src.catalog;
        const int n = cat.perPage();
        for (const auto &pg : tmpl->pages) {
            const bool catalogPage = pg->id == cat.pageId;
            if (!catalogPage) {
                auto np = copyPage(*pg);
                for (const auto &it : out->cloneItems(pg->items)) np->items.push_back(it);
                continue;
            }
            for (int first = 0; first < std::max<qsizetype>(1, records.size()); first += n) {
                auto np = copyPage(*pg);
                ItemList fixed;
                for (const auto &it : pg->items)
                    if (!cat.inTemplate(it->bounds())) fixed.push_back(it);
                for (const auto &it : out->cloneItems(fixed)) np->items.push_back(it);
                for (int k = 0; k < n && first + k < records.size(); ++k) {
                    const QPointF d = cat.cell(k).topLeft() - cat.cell(0).topLeft();
                    ItemList inCell;
                    for (const auto &it : pg->items)
                        if (cat.inTemplate(it->bounds())) inCell.push_back(it);
                    const ItemList cell = out->cloneItems(inCell);
                    for (const auto &c : cell) c->moveBy(d.x(), d.y());
                    freezeMergeFields(*out, src.merge, cell, records[first + k]);
                    for (const auto &c : cell) np->items.push_back(c);
                }
            }
        }
    } else {
        for (int rec : records) {
            const int before = out->pages.size();
            for (const auto &pg : tmpl->pages) {
                auto np = copyPage(*pg);
                for (const auto &it : out->cloneItems(pg->items)) np->items.push_back(it);
            }
            for (int p = before; p < out->pages.size(); ++p) freezeMergeFields(*out, src.merge, out->pages[p]->items, rec);
        }
    }
    out->merge = MergeSource();
    if (out->pages.isEmpty()) out->addPage();
    out->props.title = src.props.title + QStringLiteral(" (merged)");
    return out;
}

void mergeToEmailFiles(QWidget *parent, Editor *ed)
{
    const Document &d = *ed->doc();
    if (d.merge.isEmpty()) {
        QMessageBox::information(parent, QStringLiteral("Merge to Email"), QStringLiteral("Select a recipient list first."));
        return;
    }
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Merge to Email"));
    auto *form = new QFormLayout(&dlg);
    auto *to = new QComboBox(&dlg);
    to->addItems(d.merge.fields);
    for (int i = 0; i < d.merge.fields.size(); ++i)
        if (d.merge.fields[i].contains("mail", Qt::CaseInsensitive)) to->setCurrentIndex(i);
    auto *subject = new QLineEdit(d.props.title.isEmpty() ? ed->displayName() : d.props.title, &dlg);
    form->addRow(QStringLiteral("To:"), to);
    form->addRow(QStringLiteral("Subject:"), subject);
    form->addRow(new QLabel(QStringLiteral("JeffPub 79 creates one ready-to-send email file (.eml) per recipient, with the publication as an inline picture.\n"
                                           "Open them with your email program to send."), &dlg));
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString dir = QFileDialog::getExistingDirectory(parent, QStringLiteral("Save Email Files To"));
    if (dir.isEmpty()) return;
    PaintContext ctx;
    ctx.doc = &d;
    ctx.cache = &ed->cache();
    ctx.opt.output = true;
    int n = 0;
    for (int rec : d.merge.includedRows()) {
        ctx.opt.mergeRecord = rec;
        const QImage img = Renderer::renderToImage(ctx, 0, 1.5);
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        const QString addr = d.merge.value(rec, to->currentText());
        QString eml;
        eml += "To: " + addr + "\r\n";
        eml += "Subject: " + subject->text() + "\r\n";
        eml += "X-Unsent: 1\r\nMIME-Version: 1.0\r\n";
        eml += "Content-Type: multipart/related; boundary=\"jp79\"\r\n\r\n";
        eml += "--jp79\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<html><body><img src=\"cid:page1\" alt=\"\"></body></html>\r\n";
        eml += "--jp79\r\nContent-Type: image/png\r\nContent-Transfer-Encoding: base64\r\nContent-ID: <page1>\r\n\r\n";
        const QByteArray b64 = png.toBase64();
        for (int i = 0; i < b64.size(); i += 76) eml += QString::fromLatin1(b64.mid(i, 76)) + "\r\n";
        eml += "--jp79--\r\n";
        QString safe = addr.isEmpty() ? QStringLiteral("recipient-%1").arg(rec + 1) : addr;
        safe.replace(QRegularExpression("[^A-Za-z0-9@._-]"), "_");
        QFile f(QDir(dir).filePath(safe + ".eml"));
        if (f.open(QIODevice::WriteOnly)) { f.write(eml.toUtf8()); ++n; }
    }
    QMessageBox::information(parent, QStringLiteral("Merge to Email"), QStringLiteral("Created %1 email file(s) in %2.").arg(n).arg(dir));
}

std::unique_ptr<Document> loadAnyPublication(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return nullptr;
    }
    const QByteArray bytes = f.readAll();
    if (isPublisherFile(bytes)) return importPublisher(bytes, error);
    return publicationFromBytes(bytes, error);
}

// ---------------- printing ----------------
QVector<QVector<int>> bookletOrder(int pages)
{
    const int n = ((pages + 3) / 4) * 4;
    QVector<QVector<int>> sides;
    for (int s = 0; s < n / 2; s += 2) {
        auto pg = [&](int i) { return i < pages ? i : -1; };
        sides << QVector<int>{pg(n - 1 - s), pg(s)};
        sides << QVector<int>{pg(s + 1), pg(n - 2 - s)};
    }
    return sides;
}

PrinterMarks PrinterMarks::fromJson(const QJsonObject &o)
{
    PrinterMarks m;
    m.crop = o.value("cropMarks").toBool();
    m.bleed = o.value("bleedMarks").toBool();
    m.registration = o.value("registration").toBool();
    m.density = o.value("densityBars").toBool();
    m.colorBars = o.value("colorBars").toBool();
    m.jobInfo = o.value("jobInfo").toBool();
    return m;
}

QString plateName(int plate, const Document *doc)
{
    static const char *names[] = {"Cyan", "Magenta", "Yellow", "Black"};
    if (plate >= 4 && doc) return doc->print.spotName(plate - 4);
    return QString::fromLatin1(names[std::clamp(plate, 0, 3)]);
}

double spotAmount(const QColor &c, const QColor &spot)
{
    // A tint mixes the spot color with white: every channel the same share
    // of the way to white.
    const int sc[3] = {spot.red(), spot.green(), spot.blue()}, cc[3] = {c.red(), c.green(), c.blue()};
    double t = -1;
    for (int k = 0; k < 3; ++k) {
        if (255 - sc[k] < 8) {
            if (std::abs(cc[k] - 255) > 8) return -1;
            continue;
        }
        const double tk = double(cc[k] - sc[k]) / (255 - sc[k]);
        if (t < 0) t = tk;
        else if (std::abs(tk - t) > 0.04) return -1;
    }
    if (t < 0) t = 0;   // white spot color
    if (t < -0.02 || t > 0.98) return -1;
    return 1 - std::clamp(t, 0.0, 1.0);
}

QImage separationPlate(const QImage &rgb, int plate)
{
    const QImage src = rgb.convertToFormat(QImage::Format_RGB32);
    QImage out(src.size(), QImage::Format_Grayscale8);
    out.setDotsPerMeterX(src.dotsPerMeterX());
    out.setDotsPerMeterY(src.dotsPerMeterY());
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *o = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            // Naive RGB to CMYK with full black generation.
            const double r = qRed(in[x]) / 255.0, g = qGreen(in[x]) / 255.0, b = qBlue(in[x]) / 255.0;
            const double k = 1 - std::max({r, g, b});
            double ink = k;
            if (plate < 3) {
                const double ch = plate == 0 ? r : plate == 1 ? g : b;
                ink = k >= 0.999 ? 0 : (1 - ch - k) / (1 - k);
            }
            o[x] = uchar(std::lround(255 * (1 - std::clamp(ink, 0.0, 1.0))));
        }
    }
    return out;
}

void drawPrinterMarks(QPainter *p, const QRectF &page, const PrinterMarks &m, const QString &jobInfo)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QPen hair(Qt::black, 0.3);
    // On a separation plate every swatch shows that plate's ink.
    auto ink = [&](const QColor &c) {
        if (m.plate < 0) return c;
        QImage px(1, 1, QImage::Format_RGB32);
        px.setPixel(0, 0, c.rgb());
        const int g = qGray(separationPlate(px, std::min(m.plate, 3)).pixel(0, 0));
        return QColor(g, g, g);
    };
    const QRectF bleed = page.adjusted(-m.bleedSize, -m.bleedSize, m.bleedSize, m.bleedSize);
    auto corners = [&](const QRectF &r, double off, double len) {
        for (const QPointF &c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) {
            const double sx = c.x() == r.left() ? -1 : 1, sy = c.y() == r.top() ? -1 : 1;
            p->drawLine(QPointF(c.x() + sx * off, c.y()), QPointF(c.x() + sx * (off + len), c.y()));
            p->drawLine(QPointF(c.x(), c.y() + sy * off), QPointF(c.x(), c.y() + sy * (off + len)));
        }
    };
    if (m.crop) {
        // Trim lines start beyond the bleed so they never print on the page.
        p->setPen(QPen(Qt::black, 0.5));
        corners(page, m.bleedSize + 3, 18);
    }
    if (m.bleed) {
        // Short lines at the bleed edge, offset from the crop marks.
        p->setPen(QPen(Qt::black, 0.3, Qt::DashLine));
        corners(bleed, 3, 9);
    }
    if (m.registration) {
        // A target with crosshairs centered on each side.
        p->setPen(hair);
        const double r = 5, d = m.bleedSize + 3 + 18 + 4;
        for (const QPointF &c : {QPointF(page.center().x(), page.top() - d), QPointF(page.center().x(), page.bottom() + d),
                                 QPointF(page.left() - d, page.center().y()), QPointF(page.right() + d, page.center().y())}) {
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(c, r, r);
            p->drawEllipse(c, r * 0.5, r * 0.5);
            p->drawLine(c - QPointF(r * 1.6, 0), c + QPointF(r * 1.6, 0));
            p->drawLine(c - QPointF(0, r * 1.6), c + QPointF(0, r * 1.6));
        }
    }
    const double barY = page.top() - m.bleedSize - 3 - 18;   // swatches sit level with the crop marks
    const double sw = 11;
    if (m.density) {
        // Gray steps from 0 to 100 percent, along the top left.
        p->setPen(hair);
        for (int i = 0; i <= 10; ++i) {
            const int g = 255 - i * 255 / 10;
            p->setBrush(ink(QColor(g, g, g)));
            p->drawRect(QRectF(page.left() + 24 + i * sw, barY, sw, sw));
        }
    }
    if (m.colorBars) {
        // Process and overprint colors, then 50 percent tints, along the top right.
        const QList<QColor> cols{QColor(0, 174, 239), QColor(236, 0, 140), QColor(255, 242, 0), Qt::black,
                                 QColor(237, 28, 36), QColor(0, 166, 81), QColor(46, 49, 146),
                                 QColor(128, 215, 247), QColor(246, 128, 198), QColor(255, 249, 128), QColor(128, 128, 128)};
        p->setPen(hair);
        const double x0 = page.right() - 24 - cols.size() * sw;
        for (int i = 0; i < cols.size(); ++i) {
            p->setBrush(ink(cols[i]));
            p->drawRect(QRectF(x0 + i * sw, barY, sw, sw));
        }
    }
    if (m.jobInfo && !jobInfo.isEmpty()) {
        QFont f(QStringLiteral("Arimo"));
        f.setPointSizeF(6.5);
        p->setFont(f);
        p->setPen(Qt::black);
        // Left of the bottom registration mark.
        const QRectF box(page.left() + 24, page.bottom() + m.bleedSize + 6, page.width() / 2 - 24 - 14, 12);
        p->drawText(box, Qt::AlignLeft | Qt::AlignTop, QFontMetricsF(f).elidedText(jobInfo, Qt::ElideRight, box.width()));
    }
    p->restore();
}

void renderPlateInto(QPainter *p, const PaintContext &ctx, int page, int plate)
{
    // Spot colors and their tints print on their own plates and are knocked
    // out of the process plates. Text is laid out afresh so its colors go
    // through the filter too.
    const Document *d = ctx.doc;
    const QVector<QColor> spots = d->print.usesSpots() ? d->print.spotColors : QVector<QColor>{};
    PaintContext pc = ctx;
    LayoutCache plateCache;
    pc.cache = &plateCache;
    if (plate >= 4) {
        const QColor spot = spots.value(plate - 4);
        setColorFilter([spot](const QColor &c) {
            const double a = spotAmount(c, spot);
            return a < 0 ? QColor(255, 255, 255, c.alpha()) : QColor::fromRgbF(float(1 - a), float(1 - a), float(1 - a), c.alphaF());
        });
        pc.opt.skipPictures = true;
    } else if (!spots.isEmpty()) {
        setColorFilter([spots](const QColor &c) {
            for (const QColor &s : spots)
                if (spotAmount(c, s) > 0) return QColor(255, 255, 255, c.alpha());
            return c;
        });
    }
    Renderer::paintPage(p, pc, page);
    setColorFilter({});
}

QImage renderPlate(const Document &doc, int page, int plate, double dpi)
{
    const QSizeF ps = doc.pageSize();
    QImage img(QSize(int(std::ceil(ps.width() * dpi / 72)), int(std::ceil(ps.height() * dpi / 72))), QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter ip(&img);
    ip.setRenderHint(QPainter::Antialiasing);
    ip.scale(dpi / 72, dpi / 72);
    PaintContext ctx;
    ctx.doc = &doc;
    LayoutCache cache;
    ctx.cache = &cache;
    ctx.opt.output = true;
    renderPlateInto(&ip, ctx, page, plate);
    ip.end();
    return separationPlate(img, std::min(plate, 3));
}

void printDocument(Editor *ed, QPrinter *printer, const QJsonObject &opts)
{
    Document *d = ed->doc();
    // A catalog prints its merged pages: a record in each cell.
    std::unique_ptr<Document> catalogPages;
    if (opts.value("merged").toBool() && d->catalog.isActive() && !d->merge.isEmpty()) {
        catalogPages = mergeToNewPublication(*d);
        d = catalogPages.get();
    }
    const bool sheetLayout = d->setup.layout == PageSetup::MultiplePerSheet || d->setup.layout == PageSetup::Labels;
    const QString layout = opts.value("layout").toString(d->setup.layout == PageSetup::Booklet ? "booklet" : sheetLayout ? "multiple" : "one");
    const PrinterMarks marks = PrinterMarks::fromJson(opts);
    const bool allowBleeds = opts.value("allowBleeds").toBool();
    const bool merged = opts.value("merged").toBool() && !d->merge.isEmpty();
    // Separations: one sheet per process plate, in plate order for each pass.
    QVector<int> plates{-1};
    if (opts.value("separations").toBool()) {
        plates.clear();
        const QString want = opts.value("plates").toString(QStringLiteral("CMYK"));
        for (int i = 0; i < 4; ++i)
            if (want.contains(QLatin1Char("CMYK"[i]))) plates << i;
        // Spot plates: "0"-"9" for the publication's spot colors.
        for (int i = 0; i < std::min<qsizetype>(10, d->print.spotColors.size()); ++i)
            if (d->print.usesSpots() && want.contains(QChar('0' + i))) plates << 4 + i;
        if (plates.isEmpty()) plates << 3;
    }
    int from = printer->fromPage() > 0 ? printer->fromPage() - 1 : 0;
    int to = printer->toPage() > 0 ? printer->toPage() - 1 : d->pages.size() - 1;
    if (printer->printRange() == QPrinter::CurrentPage) from = to = ed->currentPage();
    to = std::min(to, int(d->pages.size()) - 1);
    QPainter p;
    if (!p.begin(printer)) return;
    const QRectF sheet = printer->pageRect(QPrinter::Point);
    const double dev = printer->resolution() / 72.0;
    PaintContext ctx;
    ctx.doc = d;
    ctx.cache = &ed->cache();
    ctx.opt.output = true;
    ctx.opt.grayscale = opts.value("grayscale").toBool();
    const QSizeF ps = d->pageSize();
    const QString title = ed->displayName();
    const QString stamp = QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat);
    int plate = -1;
    bool first = true;
    auto newSheet = [&] {
        if (!first) printer->newPage();
        first = false;
    };
    auto drawPage = [&](int page, const QRectF &target) {
        if (page < 0) return;
        const double s = std::min(target.width() / ps.width(), target.height() / ps.height());
        const double bl = allowBleeds ? marks.bleedSize : 0;
        p.save();
        p.scale(dev, dev);
        p.translate(target.topLeft());
        p.scale(s, s);
        const QRectF area(QPointF(-bl, -bl), ps + QSizeF(2 * bl, 2 * bl));
        p.setClipRect(area);
        if (plate < 0) {
            Renderer::paintPage(&p, ctx, page);
        } else {
            // Plates print as halftone-ready grayscale at up to 300 dpi.
            const double res = std::min(300.0, printer->resolution() * 1.0) / 72.0 * s;
            QImage img(QSize(int(std::ceil(area.width() * res)), int(std::ceil(area.height() * res))), QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter ip(&img);
            ip.setRenderHint(QPainter::Antialiasing);
            ip.scale(res, res);
            ip.translate(bl, bl);
            renderPlateInto(&ip, ctx, page, plate);
            ip.end();
            p.drawImage(area, separationPlate(img, std::min(plate, 3)));
        }
        p.restore();
        if (marks.any()) {
            p.save();
            p.scale(dev, dev);
            PrinterMarks m = marks;
            m.bleedSize *= s;
            m.plate = plate;
            QString info = QStringLiteral("%1  ·  Page %2 of %3  ·  %4").arg(title).arg(page + 1).arg(d->pages.size()).arg(stamp);
            if (plate >= 0) info += QStringLiteral("  ·  ") + plateName(plate, d);
            drawPrinterMarks(&p, QRectF(target.topLeft(), ps * s), m, info);
            p.restore();
        }
    };
    QVector<int> records{catalogPages ? -1 : ed->mergeRecord()};
    if (merged) records = d->merge.includedRows();
    for (int rec : records) {
        ctx.opt.mergeRecord = rec;
        if (layout == "booklet" || layout == "bookletTop") {
            const bool top = layout == "bookletTop";
            for (const auto &side : bookletOrder(d->pages.size()))
                for (int pl : plates) {
                    plate = pl;
                    newSheet();
                    if (!top) {
                        const double half = sheet.width() / 2;
                        const double s = std::min(half / ps.width(), sheet.height() / ps.height());
                        const QSizeF sz = ps * s;
                        drawPage(side[0], QRectF(QPointF(half - sz.width(), (sheet.height() - sz.height()) / 2), sz));
                        drawPage(side[1], QRectF(QPointF(half, (sheet.height() - sz.height()) / 2), sz));
                    } else {
                        // Top fold: the pages sit above and below a horizontal fold.
                        const double half = sheet.height() / 2;
                        const double s = std::min(sheet.width() / ps.width(), half / ps.height());
                        const QSizeF sz = ps * s;
                        drawPage(side[0], QRectF(QPointF((sheet.width() - sz.width()) / 2, half - sz.height()), sz));
                        drawPage(side[1], QRectF(QPointF((sheet.width() - sz.width()) / 2, half), sz));
                    }
                }
        } else if (layout == "multiple") {
            // A publication laid out for several pages per sheet (business
            // cards, labels) places them by its own side and top margins and
            // gaps; any other is spaced evenly and centered.
            const bool ownSheet = d->setup.layout == PageSetup::MultiplePerSheet || d->setup.layout == PageSetup::Labels;
            const double gap = marks.any() ? 2 * kMarksMargin : 9;
            const double gapH = ownSheet ? d->setup.gapH : gap, gapV = ownSheet ? d->setup.gapV : gap;
            const double sideM = ownSheet ? d->setup.sideMargin : 0, topM = ownSheet ? d->setup.topMargin : 0;
            const int cols = std::max(1, int((sheet.width() - 2 * sideM + gapH + 0.001) / (ps.width() + gapH)));
            const int rows = std::max(1, int((sheet.height() - 2 * topM + gapV + 0.001) / (ps.height() + gapV)));
            const bool copies = opts.value("copiesPerSheet").toBool(true);
            const double ox = ownSheet ? sideM : (sheet.width() - (cols * ps.width() + (cols - 1) * gapH)) / 2;
            const double oy = ownSheet ? topM : (sheet.height() - (rows * ps.height() + (rows - 1) * gapV)) / 2;
            int page = from;
            while (page <= to) {
                for (int pl : plates) {
                    plate = pl;
                    newSheet();
                    for (int rr = 0; rr < rows; ++rr)
                        for (int cc = 0; cc < cols; ++cc) {
                            const int pg = copies ? page : page + rr * cols + cc;
                            if (pg > to) continue;
                            drawPage(pg, QRectF(QPointF(ox + cc * (ps.width() + gapH), oy + rr * (ps.height() + gapV)), ps));
                        }
                }
                page += copies ? 1 : rows * cols;
            }
        } else if (layout == "tiled" || ps.width() > sheet.width() * 1.05 || ps.height() > sheet.height() * 1.05) {
            // Poster and banner printing: tile across sheets with a small overlap.
            const double overlap = opts.value("overlap").toDouble(18);
            for (int page = from; page <= to; ++page) {
                for (double y = 0; y < ps.height() - 0.5; y += sheet.height() - overlap)
                    for (double x = 0; x < ps.width() - 0.5; x += sheet.width() - overlap) {
                        newSheet();
                        p.save();
                        p.scale(dev, dev);
                        p.translate(-x, -y);
                        p.setClipRect(QRectF(x, y, sheet.width(), sheet.height()));
                        Renderer::paintPage(&p, ctx, page);
                        p.restore();
                    }
            }
        } else {
            for (int page = from; page <= to; ++page)
                for (int pl : plates) {
                    plate = pl;
                    newSheet();
                    const QSizeF fit = ps.scaled(sheet.size(), Qt::KeepAspectRatio);
                    const QSizeF sz = (ps.width() <= sheet.width() && ps.height() <= sheet.height()) ? ps : fit;
                    drawPage(page, QRectF(QPointF((sheet.width() - sz.width()) / 2, (sheet.height() - sz.height()) / 2), sz));
                }
        }
    }
    p.end();
}

void compressPicturesDialog(QWidget *parent, Editor *ed)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Compress Pictures"));
    auto *form = new QFormLayout(&dlg);
    auto *target = new QComboBox(&dlg);
    target->addItems({"Commercial printing (300 ppi)", "Desktop printing (220 ppi)", "Web (96 ppi)"});
    auto *all = new QCheckBox(QStringLiteral("Apply to all pictures in the publication"), &dlg);
    all->setChecked(true);
    auto *crop = new QCheckBox(QStringLiteral("Delete cropped areas of pictures"), &dlg);
    crop->setChecked(true);
    form->addRow(QStringLiteral("Target output:"), target);
    form->addRow(all);
    form->addRow(crop);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const double ppi = target->currentIndex() == 0 ? 300 : target->currentIndex() == 1 ? 220 : 96;
    QVector<PictureItem *> pics;
    if (all->isChecked()) ed->doc()->forEachItem([&](Item *it, int, const QString &) { if (auto *p = dynamic_cast<PictureItem *>(it)) pics << p; });
    else for (Item *it : ed->selectedItems()) if (auto *p = dynamic_cast<PictureItem *>(it)) pics << p;
    qint64 saved = 0;
    ed->change(QStringLiteral("Compress Pictures"), [&] {
        for (PictureItem *p : pics) {
            ImageData &data = ed->doc()->images[p->imageId];
            if (data.format == "svg" || data.format == "wmf" || data.format == "emf" || data.format == "pdf") continue;
            QImage img = data.image();
            if (img.isNull()) continue;
            const double wantW = p->imgRect.width() / 72.0 * ppi;
            QImage out = img;
            if (img.width() > wantW * 1.05) out = img.scaledToWidth(int(wantW), Qt::SmoothTransformation);
            if (crop->isChecked() && out.width() > 0) {
                // Keep only the visible part of the picture.
                const double sx = out.width() / p->imgRect.width(), sy = out.height() / p->imgRect.height();
                const QRectF vis = QRectF(QPointF(0, 0), p->rect.size()).intersected(p->imgRect).translated(-p->imgRect.topLeft());
                const QRect px(int(vis.x() * sx), int(vis.y() * sy), int(vis.width() * sx), int(vis.height() * sy));
                if (px.isValid() && px != out.rect()) {
                    out = out.copy(px);
                    p->imgRect = QRectF(p->imgRect.topLeft() + vis.topLeft(), vis.size());
                }
            }
            QByteArray bytes;
            QBuffer b(&bytes);
            b.open(QIODevice::WriteOnly);
            const bool alpha = out.hasAlphaChannel();
            out.save(&b, alpha ? "PNG" : "JPG", alpha ? -1 : 88);
            if (bytes.size() < data.bytes.size()) {
                saved += data.bytes.size() - bytes.size();
                const QString id = ed->doc()->addImage(bytes, alpha ? "png" : "jpg", data.sourcePath);
                p->imageId = id;
            }
        }
    });
    QMessageBox::information(parent, QStringLiteral("Compress Pictures"), QStringLiteral("Saved %1 KB.").arg(saved / 1024));
}

} // namespace jp
