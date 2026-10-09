#include "core/document.h"
#include "core/barcode.h"
#include "core/svg.h"
#include "render/svgexport.h"
#include "io/epub.h"
#include <QXmlStreamReader>
#include <QSvgRenderer>
#include <QSvgGenerator>
#include "io/cfb.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "io/pubshapes.h"
#include "core/fonts.h"
#include "render/metafile.h"
#include "text/storyio.h"
#include "text/textengine.h"
#include "text/textprops.h"
#include "text/hyphenation.h"
#include "text/dictionaries.h"
#include "render/renderer.h"
#include "render/shapes.h"
#include "render/textart.h"
#include "templates/templates.h"
#include "app/appfuncs.h"
#include "io/importers.h"
#include "io/zip.h"
#include "io/qtpdf.h"
#include "render/pdfpage.h"
#include "io/pdfx.h"
#include <QColorSpace>
#include <QCryptographicHash>
#include <QPdfWriter>
#include <QPrinter>
#include "app/icons.h"
#include <QTabBar>
#include <QTabWidget>
#include "app/dialogs.h"
#include "app/editor.h"
#include "app/mainwindow.h"
#include "app/recovery.h"
#include "app/ribbon.h"
#include "app/telemetry.h"
#include "app/updater.h"
#include "app/toc.h"
#include "app/notes.h"
#include "app/iconpicker.h"
#include "app/widgets.h"
#include "app/settings.h"
#include "canvas/canvas.h"
#include <cstdio>
#include <QTemporaryDir>
#include <QFileOpenEvent>
#include <QSettings>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <QElapsedTimer>
#include <QBuffer>
#include <QAbstractItemView>
#include <QPainter>

#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextBrowser>
#include <QDirIterator>
#include <QtTest>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QtEndian>
#include <QLockFile>
#include <clocale>

using namespace jp;

// A small CMYK printer profile (ICC version 2, 2-point lookup tables), as
// a printer's own .icc file would be, made here so no real one is needed.
static QByteArray testCmykProfile()
{
    QByteArray tags;
    struct Tag { const char *sig; QByteArray data; };
    auto u16 = [](QByteArray &b, int v) { b.append(char((v >> 8) & 0xff)).append(char(v & 0xff)); };
    auto u32 = [](QByteArray &b, quint32 v) { for (int k = 3; k >= 0; --k) b.append(char((v >> (8 * k)) & 0xff)); };
    auto s15 = [&](QByteArray &b, double v) { u32(b, quint32(qint32(std::lround(v * 65536)))); };
    auto lut = [&](int in, int out, const QVector<int> &clut) {
        QByteArray b("mft2\0\0\0\0", 8);
        b.append(char(in)).append(char(out)).append(char(2)).append(char(0));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s15(b, r == c ? 1 : 0);
        u16(b, 2);
        u16(b, 2);
        for (int i = 0; i < in; ++i) { u16(b, 0); u16(b, 65535); }
        for (int v : clut) u16(b, std::clamp(v, 0, 65535));
        for (int i = 0; i < out; ++i) { u16(b, 0); u16(b, 65535); }
        return b;
    };
    QVector<int> a2b, b2a;   // corners: CMYK to Lab, Lab to CMYK
    for (int i = 0; i < 16; ++i) {
        const int c = i >> 3 & 1, m = i >> 2 & 1, y = i >> 1 & 1, k = i & 1;
        const double L = 100 * (1 - k) * (1 - 0.25 * c) * (1 - 0.35 * m) * (1 - 0.1 * y);
        a2b << int(L / 100 * 65280) << int((128 + (60 * m - 40 * c) * (1 - k)) * 256) << int((128 + (70 * y - 50 * c) * (1 - k)) * 256);
    }
    for (int i = 0; i < 8; ++i) {
        const double L = (i >> 2 & 1) * 100.0, a = -128 + (i >> 1 & 1) * 255.0, b = -128 + (i & 1) * 255.0;
        b2a << int(65535 * std::max(0.0, -a / 256)) << int(65535 * std::max(0.0, a / 256)) << int(65535 * std::max(0.0, b / 256)) << int(65535 * (1 - L / 100));
    }
    QByteArray desc("desc\0\0\0\0", 8);
    u32(desc, 10);
    desc.append("Test CMYK", 10);
    desc.append(QByteArray(8 + 3 + 67, '\0'));
    QByteArray cprt("text\0\0\0\0", 8);
    cprt.append("No copyright, use freely", 25);
    QByteArray wtpt("XYZ \0\0\0\0", 8);
    s15(wtpt, 0.9642);
    s15(wtpt, 1.0);
    s15(wtpt, 0.8249);
    const QVector<Tag> list{{"desc", desc}, {"cprt", cprt}, {"wtpt", wtpt}, {"A2B0", lut(4, 3, a2b)}, {"B2A0", lut(3, 4, b2a)}};
    QByteArray table, body;
    const int start = 128 + 4 + 12 * int(list.size());
    for (const Tag &t : list) {
        table.append(t.sig, 4);
        u32(table, quint32(start + body.size()));
        u32(table, quint32(t.data.size()));
        body += t.data;
        while (body.size() % 4) body.append('\0');
    }
    QByteArray head;
    u32(head, quint32(start + body.size()));
    head.append("none", 4);
    head.append("\x02\x10\0\0", 4);
    head.append("prtrCMYKLab ", 12);
    for (int v : {2026, 1, 1, 0, 0, 0}) u16(head, v);
    head.append("acsp", 4);
    head.append(QByteArray(24, '\0'));   // platform, flags, maker, model, attributes
    u32(head, 0);                        // perceptual
    s15(head, 0.9642);
    s15(head, 1.0);
    s15(head, 0.8249);
    head.append("none", 4);
    head.append(QByteArray(44, '\0'));
    QByteArray count;
    u32(count, quint32(list.size()));
    return head + count + table + body;
}

// Rebuilds a .pub's drawing records (Escher/EscherStm) with the first
// shape properties table (OPT) that `edit` changes: `edit` gets its body
// and header and returns true once it has changed them. Container lengths
// are recomputed; padding between drawings is kept.
static QByteArray editFirstOpt(const QByteArray &escher, const std::function<bool(QByteArray &body, quint16 &head)> &edit)
{
    bool done = false;
    std::function<QByteArray(int, int)> rebuild = [&](int from, int to) {
        QByteArray out;
        int off = from;
        while (off + 8 <= to) {
            const quint16 vi = qFromLittleEndian<quint16>(escher.constData() + off);
            const quint16 type = qFromLittleEndian<quint16>(escher.constData() + off + 2);
            const quint32 len = qFromLittleEndian<quint32>(escher.constData() + off + 4);
            if (type < 0xF000 || type > 0xF200 || off + 8 + qint64(len) > to) {   // padding between drawings
                out += escher.at(off++);
                continue;
            }
            QByteArray body = escher.mid(off + 8, len);
            quint16 head = vi;
            if ((vi & 0xF) == 0xF) body = rebuild(off + 8, off + 8 + int(len));
            else if (type == 0xF00B && !done) done = edit(body, head);
            QByteArray h(8, 0);
            qToLittleEndian<quint16>(head, h.data());
            qToLittleEndian<quint16>(type, h.data() + 2);
            qToLittleEndian<quint32>(quint32(body.size()), h.data() + 4);
            out += h + body;
            off += 8 + int(len);
        }
        out += escher.mid(off, to - off);
        return out;
    };
    const QByteArray out = rebuild(0, int(escher.size()));
    return done ? out : QByteArray();
}

// Adds a property to an OPT body (after the others, its complex data last).
static void addOptProp(QByteArray &body, quint16 &head, quint16 id, quint32 value, const QByteArray &complex = {})
{
    const int count = head >> 4;
    QByteArray entry(6, 0);
    qToLittleEndian<quint16>(id, entry.data());
    qToLittleEndian<quint32>(complex.isEmpty() ? value : quint32(complex.size()), entry.data() + 2);
    body = body.left(count * 6) + entry + body.mid(count * 6) + complex;
    head = quint16((head & 0xF) | ((count + 1) << 4));
}

// Where a complex property's data starts in an OPT body, or -1.
static int optComplexAt(const QByteArray &body, quint16 head, quint16 want)
{
    const int count = head >> 4;
    int at = count * 6;
    for (int i = 0; i < count; ++i) {
        const quint16 id = qFromLittleEndian<quint16>(body.constData() + i * 6);
        const quint32 v = qFromLittleEndian<quint32>(body.constData() + i * 6 + 2);
        if ((id & 0x3FFF) == want) return at;
        if (id & 0x8000) at += int(v);
    }
    return -1;
}

class Tests : public QObject {
    Q_OBJECT
    QTemporaryDir m_settingsDir;   // tests never touch the user's own settings

private Q_SLOTS:
    void initTestCase()
    {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
        initCore();
    }
    void publisherSamplesImport_data()
    {
        QTest::addColumn<QString>("path");
        QDir dir(QStringLiteral(JP_TEST_DATA "/pub"));
        for (const QString &f : dir.entryList({"*.pub"}, QDir::Files)) QTest::newRow(qPrintable(f)) << dir.filePath(f);
    }
    void publisherSamplesImport()
    {
        QFETCH(QString, path);
        QString err;
        PubImportReport rep;
        auto doc = importPublisherFile(path, &err, &rep);
        QVERIFY2(doc, qPrintable(err));
        QVERIFY(!doc->pages.isEmpty());
        // Round trip through .jpub keeps everything.
        auto back = publicationFromBytes(publicationBytes(*doc, QImage()), &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->pages.size(), doc->pages.size());
        LayoutCache cache;
        PaintContext ctx;
        ctx.doc = doc.get();
        ctx.cache = &cache;
        for (int i = 0; i < doc->pages.size(); ++i) QVERIFY(!Renderer::renderToImage(ctx, i, 0.25).isNull());
    }
    // Photos stay JPEG when saved with a gray setting or cut to a shape
    // (a converted picture always has an alpha channel, which once made
    // every one of them a much larger PNG).
    void pubWriterKeepsJpegPhotos()
    {
        QImage photo(320, 200, QImage::Format_RGB32);
        for (int y = 0; y < photo.height(); ++y)
            for (int x = 0; x < photo.width(); ++x) photo.setPixel(x, y, qRgb(x * 255 / 320, y * 255 / 200, 128));
        QByteArray jpg;
        QBuffer buf(&jpg);
        buf.open(QIODevice::WriteOnly);
        photo.save(&buf, "JPEG", 90);
        auto doc = jp::Document::blank(QSizeF(612, 792));
        const QString id = doc->addImage(jpg, QStringLiteral("jpg"));
        auto gray = std::make_shared<jp::PictureItem>();
        gray->imageId = id;
        gray->rect = QRectF(72, 72, 160, 100);
        gray->recolor = jp::PictureItem::Grayscale;
        auto oval = std::make_shared<jp::PictureItem>();
        oval->imageId = id;
        oval->rect = QRectF(72, 300, 160, 100);
        oval->maskShape = QStringLiteral("ellipse");
        oval->brightness = 20;
        doc->pages[0]->items.push_back(gray);
        doc->pages[0]->items.push_back(oval);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("photos.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->images.size(), 2);
        for (auto it = back->images.cbegin(); it != back->images.cend(); ++it) QCOMPARE(it->format, QStringLiteral("jpg"));
    }

    // A file whose style names point outside their section (made from
    // JeffPub's own styles sample): it still opens with all its text,
    // losing only the damaged names.
    void damagedStyleNamesStillOpen()
    {
        QString err;
        auto doc = importPublisherFile(QStringLiteral(JP_TEST_DATA "/pub/jp-damaged-style-names.pub"), &err);
        QVERIFY2(doc, qPrintable(err));
        QString text;
        for (const auto &story : doc->stories) text += story->doc->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("A pull quote")) && text.contains(QStringLiteral("Plain text")), qPrintable(text));
    }
    // The Open page's thumbnails read only the thumbnail out of each file.
    void thumbnailWithoutLoadingFile()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        QImage thumb(40, 52, QImage::Format_RGB32);
        thumb.fill(QColor(10, 120, 200));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("t.jpub"));
        QString err;
        QVERIFY2(jp::savePublication(*doc, path, thumb, &err), qPrintable(err));
        const QImage got = jp::publicationThumbnail(path);
        QCOMPARE(got.size(), thumb.size());
        QCOMPARE(got.pixelColor(5, 5), QColor(10, 120, 200));
        QFile junk(dir.filePath(QStringLiteral("junk.jpub")));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write(QByteArray(100, 'x'));
        junk.close();
        QVERIFY(jp::publicationThumbnail(junk.fileName()).isNull());
    }

    // A publication's name goes into an email's headers: a line break must
    // not start a header of its own, and accented names are encoded.
    void emailHeaderNames()
    {
        QCOMPARE(jp::emlHeaderText(QStringLiteral("Spring Flyer")), QStringLiteral("Spring Flyer"));
        const QString forged = jp::emlHeaderText(QStringLiteral("Flyer\r\nBcc: someone@example.com"));
        QVERIFY(!forged.contains(QLatin1Char('\n')) && !forged.contains(QLatin1Char('\r')));
        QVERIFY(!jp::emlHeaderText(QStringLiteral("say \"hi\"")).contains(QLatin1Char('"')));
        const QString accented = jp::emlHeaderText(QStringLiteral("Café menu"));
        QVERIFY(accented.startsWith(QLatin1String("=?UTF-8?B?")));
        QCOMPARE(QString::fromUtf8(QByteArray::fromBase64(accented.mid(10).chopped(2).toLatin1())), QStringLiteral("Café menu"));
    }

    // Mail-merge sources from elsewhere: a vCard with fields outside any card
    // (once written into an empty list), and a spreadsheet cell at column
    // "ZZZZZZZZZZ" (once padded with billions of empty cells).
    void mergeSourcesHostile()
    {
        QTemporaryDir dir;
        {
            QFile f(dir.filePath(QStringLiteral("contacts.vcf")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("N:Stray;Field\nTEL:555\nBEGIN:VCARD\nN:Doe;Jane\nEND:VCARD\nEMAIL:after@example.com\n");
        }
        jp::MergeSource m;
        QString err;
        QVERIFY2(jp::loadMergeSource(dir.filePath(QStringLiteral("contacts.vcf")), &m, &err), qPrintable(err));
        QCOMPARE(m.rows.size(), 1);
        QCOMPARE(m.rows[0].value(1), QStringLiteral("Doe"));

        jp::ZipWriter z;
        z.add(QStringLiteral("xl/worksheets/sheet1.xml"),
              "<worksheet><sheetData><row><c r=\"A1\" t=\"inlineStr\"><is><t>Name</t></is></c></row>"
              "<row><c r=\"ZZZZZZZZZZ2\" t=\"inlineStr\"><is><t>far</t></is></c><c r=\"A2\" t=\"inlineStr\"><is><t>Jane</t></is></c></row></sheetData></worksheet>");
        {
            QFile f(dir.filePath(QStringLiteral("list.xlsx")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(z.finish());
        }
        jp::MergeSource x;
        QVERIFY2(jp::loadMergeSource(dir.filePath(QStringLiteral("list.xlsx")), &x, &err), qPrintable(err));
        for (const QStringList &r : x.rows) QVERIFY(r.size() <= 16384);
    }

    // A .jpub from someone else may claim anything: a huge table, a page a
    // million miles wide, NaN geometry, or a zip directory whose names run
    // past the end. It opens with sane values, or is refused, never crashes.
    void jpubHostileContent()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TableItem>();
        t->rows = 2;
        t->cols = 2;
        t->colW = {100, 100};
        t->rowH = {30, 30};
        t->rect = QRectF(72, 72, 0, 0);
        t->syncRect();
        t->cells.resize(4);
        doc->pages[0]->items.push_back(t);
        QMap<QString, QByteArray> entries;
        QVERIFY(jp::readZip(jp::publicationBytes(*doc, QImage()), entries));
        QJsonObject o = QJsonDocument::fromJson(entries["document.json"]).object();
        QJsonObject setup = o["setup"].toObject();
        setup["size"] = QJsonArray{1e12, -5};
        o["setup"] = setup;
        QJsonArray pages = o["pages"].toArray();
        QJsonObject page = pages[0].toObject();
        QJsonArray items = page["items"].toArray();
        QJsonObject table = items[0].toObject();
        table["rows"] = 100000;
        table["cols"] = 100000;
        table["rect"] = QJsonArray{1e300, -1e300, 5, 5};
        items[0] = table;
        page["items"] = items;
        pages[0] = page;
        o["pages"] = pages;
        jp::ZipWriter z;
        z.add(QStringLiteral("mimetype"), entries["mimetype"]);
        z.add(QStringLiteral("document.json"), QJsonDocument(o).toJson());
        QString err;
        auto back = jp::publicationFromBytes(z.finish(), &err);
        QVERIFY2(back, qPrintable(err));
        QVERIFY(back->pageSize().width() <= 20000 && back->pageSize().height() >= 1);
        const auto *bt = static_cast<const jp::TableItem *>(back->pages[0]->items[0].get());
        QVERIFY(bt->rows <= 256 && bt->cols <= 256);
        QCOMPARE(bt->cells.size(), qsizetype(bt->rows) * bt->cols);
        QVERIFY(std::abs(bt->rect.x()) <= 1e6);

        // A directory entry whose name length runs past the end of the file.
        QByteArray zip = jp::publicationBytes(*doc, QImage());
        const qsizetype dir = zip.lastIndexOf(QByteArray::fromHex("504b0102"));
        QVERIFY(dir > 0);
        zip[dir + 28] = char(0xFF);
        zip[dir + 29] = char(0xFF);
        QMap<QString, QByteArray> bad;
        QVERIFY(!jp::readZip(zip, bad));
    }

    // Vector pictures inside .pub files come from the file, so their
    // records may lie: a header shorter than it claims, a text length that is
    // negative, a character count that wraps a 32-bit size check. Each must
    // play without reading outside the picture (run under AddressSanitizer
    // to see a stray read; a wrapped count crashed outright).
    void metafileHostileRecords()
    {
        auto le16 = [](QByteArray &b, int v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); };
        auto le32 = [](QByteArray &b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF)); };
        auto play = [](const QByteArray &data) {
            jp::Metafile m;
            if (!m.load(data)) return;
            QImage img(64, 64, QImage::Format_ARGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            m.play(&p, QRectF(0, 0, 64, 64));
        };
        // 1. An EMF cut off after its signature: 44 bytes, the header needs 88.
        {
            QByteArray emf;
            le32(emf, 1);
            le32(emf, 88);
            while (emf.size() < 40) le32(emf, 0);
            le32(emf, 0x464D4520u);
            QCOMPARE(emf.size(), 44);
            play(emf);
        }
        // 2. An EMF text record whose character count wraps "count * 2".
        {
            QByteArray emf;
            le32(emf, 1);
            le32(emf, 88);
            for (int v : {0, 0, 100, 100, 0, 0, 2540, 2540}) le32(emf, quint32(v));   // bounds, frame
            le32(emf, 0x464D4520u);
            le32(emf, 0x10000);
            le32(emf, 88 + 84);
            le32(emf, 2);
            le16(emf, 1);
            le16(emf, 0);
            for (int i = 0; i < 3; ++i) le32(emf, 0);
            for (int v : {1024, 768, 320, 240}) le32(emf, quint32(v));   // device, millimeters
            QCOMPARE(emf.size(), 88);
            le32(emf, 84);   // EMR_EXTTEXTOUTW
            le32(emf, 84);
            for (int i = 0; i < 4; ++i) le32(emf, 0);   // bounds
            le32(emf, 1);
            le32(emf, 0);
            le32(emf, 0);   // mode, scales
            le32(emf, 10);
            le32(emf, 10);   // reference point
            le32(emf, 0x80000001u);   // characters: twice this wraps to 2
            le32(emf, 76);            // the string, inside the record
            le32(emf, 0);
            for (int i = 0; i < 4; ++i) le32(emf, 0);   // clip
            le32(emf, 0);                                // no spacing array
            le32(emf, 0x00410041u);
            le32(emf, 0x00410041u);
            QCOMPARE(emf.size(), 88 + 84);
            play(emf);
        }
        // 3. A WMF text record with a negative length (which once moved the
        // reading position before the start of the picture).
        {
            QByteArray wmf;
            le32(wmf, 0x9AC6CDD7u);
            le16(wmf, 0);
            for (int v : {0, 0, 1000, 1000}) le16(wmf, v);
            le16(wmf, 1440);
            le32(wmf, 0);
            le16(wmf, 0);
            le16(wmf, 1);
            le16(wmf, 9);
            le16(wmf, 0x300);
            le32(wmf, 0);
            le16(wmf, 0);
            le32(wmf, 0);
            le16(wmf, 0);
            le32(wmf, 8);       // record of 8 words
            le16(wmf, 0x0521);  // TEXTOUT
            le16(wmf, -32768);  // length
            for (int i = 0; i < 9; ++i) wmf.append(char('A'));
            wmf.append(char(0));
            le32(wmf, 3);
            le16(wmf, 0);       // end of file
            play(wmf);
        }
    }

    void publisherFuzzFilesDoNotCrash()
    {
        QDir dir(QStringLiteral(JP_TEST_DATA "/pub/fuzz"));
        for (const QString &f : dir.entryList({"*.pub"}, QDir::Files)) {
            QString err;
            auto doc = importPublisherFile(dir.filePath(f), &err);
            if (doc) {
                LayoutCache cache;
                PaintContext ctx;
                ctx.doc = doc.get();
                ctx.cache = &cache;
                for (int i = 0; i < doc->pages.size(); ++i) Renderer::renderToImage(ctx, i, 0.1);
            }
        }
    }
    void privateCorpus()
    {
        // Set JP_PUB_CORPUS to a folder of real .pub files to check them all (not part of CI).
        const QString root = qEnvironmentVariable("JP_PUB_CORPUS");
        if (root.isEmpty()) QSKIP("JP_PUB_CORPUS not set");
        QDirIterator it(root, {"*.pub", "*.PUB"}, QDir::Files, QDirIterator::Subdirectories);
        int ok = 0, total = 0;
        QStringList failed;
        while (it.hasNext()) {
            const QString f = it.next();
            QFile file(f);
            if (!file.open(QIODevice::ReadOnly) || !isPublisherFile(file.read(8))) continue;
            ++total;
            QString err;
            if (importPublisherFile(f, &err)) ++ok; else failed << f + ": " + err;
            // Compound-file round trip on real files too.
            file.seek(0);
            const QByteArray bytes = file.readAll();
            jp::cfb::File a, b;
            if (!jp::cfb::read(bytes, &a, &err) || !jp::cfb::read(jp::cfb::write(a), &b, &err)) { failed << f + ": cfb " + err; continue; }
            for (const QString &sp : a.streamPaths())
                if (b.stream(sp) != a.stream(sp)) { failed << f + ": cfb stream " + sp; break; }
        }
        qInfo("%d of %d .pub files imported", ok, total);
        for (const auto &f : failed) qWarning("%s", qPrintable(f));
        QCOMPARE(ok, total);
    }
    // Every built-in template must open with all of its text visible: no text box
    // (except a linked chain's last box, which may continue) and no shape text overflows.
    // Template options: "Include logo" places the business logo (or an empty
    // frame for it) and makes room; "Include mailing address" adds the
    // address side of a postcard. Templates show only their own options.
    void templateOptions()
    {
        QImage logo(40, 40, QImage::Format_RGB32);
        logo.fill(QColor(200, 30, 30));
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        logo.save(&buf, "PNG");
        auto logos = [](const jp::Document &d) {
            QVector<const jp::PictureItem *> out;
            for (const auto &pg : d.pages)
                jp::walkItems(pg->items, [&](const jp::ItemPtr &it) {
                    if (it->type() == jp::ItemType::Picture && it->name == QLatin1String("Logo")) out << static_cast<const jp::PictureItem *>(it.get());
                });
            return out;
        };
        int withLogo = 0;
        for (const jp::TemplateInfo &t : jp::templates()) {
            jp::TemplateOptions on;
            on.options["logo"] = true;
            on.logoBytes = png;
            on.logoFormat = QStringLiteral("png");
            auto a = t.build(on);
            auto b = t.build(jp::TemplateOptions());
            QVERIFY2(logos(*b).isEmpty(), qPrintable(t.id));
            if (!t.optionKeys.contains(QStringLiteral("logo"))) continue;
            ++withLogo;
            const auto got = logos(*a);
            QCOMPARE(got.size(), 1);
            QCOMPARE(a->image(got.first()->imageId).pixelColor(20, 20), QColor(200, 30, 30));
            // No text box on its page sits on top of the logo.
            const QRectF lr = got.first()->rect.adjusted(2, 2, -2, -2);
            for (const auto &pg : a->pages) {
                bool here = false;
                for (const auto &it : pg->items) here = here || it.get() == got.first();
                if (!here) continue;
                for (const auto &it : pg->items)
                    if (it->type() == jp::ItemType::Text) QVERIFY2(!it->rect.intersects(lr), qPrintable(t.id + QStringLiteral(" ") + a->storyDoc(static_cast<const jp::TextItem *>(it.get())->storyId)->toPlainText().left(30)));
            }
        }
        QVERIFY(withLogo >= 10);
        auto mergeBlocks = [](const jp::Document &d) {
            int n = 0;
            for (const auto &pg : d.pages)
                jp::walkItems(pg->items, [&](const jp::ItemPtr &it) {
                    if (it->type() != jp::ItemType::Text) return;
                    QTextDocument *sd = d.storyDoc(static_cast<const jp::TextItem *>(it.get())->storyId);
                    for (QTextBlock bl = sd->begin(); bl.isValid(); bl = bl.next())
                        for (auto f = bl.begin(); !f.atEnd(); ++f)
                            n += f.fragment().charFormat().stringProperty(jp::tp::Field) == QLatin1String("mergeblock:address");
                });
            return n;
        };
        const jp::TemplateInfo *pc = jp::findTemplate(QStringLiteral("postcard-greetings"));
        QVERIFY(pc && pc->optionKeys.contains(QStringLiteral("address")));
        jp::TemplateOptions off;
        off.options["address"] = false;
        QCOMPARE(mergeBlocks(*pc->build(off)), 0);
        QCOMPARE(mergeBlocks(*pc->build(jp::TemplateOptions())), 1);
    }

    // Typing: AutoCorrect fixes a word when it ends, capitalizes sentences,
    // and smart quotes curl; each can be turned off in Options.
    void typingAutoCorrect()
    {
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 400, 200)));
        ed->addItem(box);
        ed->beginTextEdit(box->id);
        auto type = [&](const QString &s) { for (QChar c : s) ed->typeText(QString(c)); };
        type(QStringLiteral("teh cat said \"hi\" and i didnt mind. it's THursday (c) "));
        const QString got = ed->doc()->storyDoc(box->storyId)->toPlainText();
        QCOMPARE(got, QStringLiteral("The cat said “hi” and I didn’t mind. It’s Thursday © "));
        ed->endTextEdit();
        jp::Settings::get().setValue("proof/autocorrect", false);
        jp::Settings::get().setValue("proof/smartQuotes", false);
        auto plain = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 300, 400, 200)));
        ed->addItem(plain);
        ed->beginTextEdit(plain->id);
        type(QStringLiteral("teh \"x\" "));
        QCOMPARE(ed->doc()->storyDoc(plain->storyId)->toPlainText(), QStringLiteral("teh \"x\" "));
        ed->endTextEdit();
        jp::Settings::get().setValue("proof/autocorrect", true);
        jp::Settings::get().setValue("proof/smartQuotes", true);
        // New text boxes follow "hyphenate automatically in new text boxes".
        jp::Settings::get().setValue("edit/hyphenate", false);
        QVERIFY(!std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(0, 0, 50, 50)))->hyphenate);
        jp::Settings::get().setValue("edit/hyphenate", true);
        QVERIFY(std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(0, 0, 50, 50)))->hyphenate);
    }

    // Dragging across words selects whole words; dragging selected text
    // moves it (Options > Advanced).
    void textMouseSelectAndDrag()
    {
        jp::MainWindow w;
        w.resize(1200, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 400, 100), QStringLiteral("alpha beta gamma delta")));
        box->hyphenate = false;
        ed->addItem(box);
        ed->beginTextEdit(box->id);
        jp::Canvas *cv = w.canvas();
        QTest::qWait(50);
        auto at = [&](int pos) {
            QTextCursor c = ed->cursor();
            c.setPosition(pos);
            ed->setCursor(c);
            return cv->caretViewRect().center().toPoint();
        };
        auto drag = [&](const QPoint &a, const QPoint &b) {
            QTest::mousePress(cv->viewport(), Qt::LeftButton, Qt::NoModifier, a);
            QMouseEvent mv(QEvent::MouseMove, QPointF(b), cv->viewport()->mapToGlobal(QPointF(b)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(cv->viewport(), &mv);
            QTest::mouseRelease(cv->viewport(), Qt::LeftButton, Qt::NoModifier, b);
        };
        // Press in "beta", drag into "gamma": both words, whole.
        const QPoint from = at(8), to = at(13);
        drag(from, to);
        QCOMPARE(ed->cursor().selectedText(), QStringLiteral("beta gamma"));
        // Drag the selection to the end: it moves there.
        QTest::qWait(QApplication::doubleClickInterval() + 100);   // not a double click
        const QPoint grab = at(9), drop = at(22);
        QTextCursor c = ed->cursor();
        c.setPosition(6);
        c.setPosition(16, QTextCursor::KeepAnchor);
        ed->setCursor(c);
        drag(grab, drop);
        QCOMPARE(ed->doc()->storyDoc(box->storyId)->toPlainText(), QStringLiteral("alpha  deltabeta gamma"));
        QVERIFY(ed->undoStack()->canUndo());
    }

    // The hyphenation zone: a word is broken only if moving it whole would
    // leave more than the zone empty at the end of the line.
    void hyphenationZone()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 110, 200);
        t->storyId = doc->createStory(QStringLiteral("We met at the international conference today."));
        doc->pages[0]->items.push_back(t);
        auto firstLine = [&](double zone) {
            t->hyphenZone = zone;
            jp::LayoutCache cache;
            jp::RenderOptions opt;
            const auto fl = cache.textFrame(*doc, *t, 1, opt);
            return fl.layout ? fl.layout->lineInfo(0).value(0).text.remove(QChar(0x00AD)) : QString();
        };
        const QString tight = firstLine(0), loose = firstLine(200);
        QVERIFY2(tight.contains(QStringLiteral("inter")), qPrintable(tight));
        QVERIFY2(!loose.contains(QStringLiteral("inter")), qPrintable(loose));
        // A justified line the zone shortened still fits the box (Qt would
        // otherwise spread its words over an endless line).
        {
            QTextCursor jc(doc->storyDoc(t->storyId));
            jc.movePosition(QTextCursor::End);
            jc.insertText(QStringLiteral(" We talked about everything we learned there."));
            QTextBlockFormat jf;
            jf.setAlignment(Qt::AlignJustify);
            jc.mergeBlockFormat(jf);
            t->hyphenZone = 200;
            // Ink of the black plate inside the box: every word is drawn
            // there, justified or not.
            auto ink = [&] {
                const QImage k = jp::renderPlate(*doc, 0, 3, 72);
                int n = 0;
                for (int y = 72; y < 272; ++y)
                    for (int x = 72; x < 182; ++x) n += qGray(k.pixel(x, y)) < 128;
                return n;
            };
            const int justified = ink();
            QTextBlockFormat lf;
            lf.setAlignment(Qt::AlignLeft);
            jc.mergeBlockFormat(lf);
            const int left = ink();
            QVERIFY2(justified > left * 0.9, qPrintable(QStringLiteral("%1 vs %2").arg(justified).arg(left)));
        }
        // Saved with the text box.
        t->hyphenZone = 30;
        QString err;
        auto again = jp::publicationFromBytes(jp::publicationBytes(*doc, QImage()), &err);
        QCOMPARE(static_cast<jp::TextItem *>(again->pages[0]->items[0].get())->hyphenZone, 30.0);
    }

    // Print preview: rulers and page numbers on a booklet sheet.
    void printPreviewRulersAndNumbers()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        auto doc = jp::Document::blank(QSizeF(396, 612));
        for (int i = 0; i < 3; ++i) doc->pages.push_back(std::make_shared<jp::Page>());
        w.editor()->setDocument(std::move(doc));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.showBackstage(QStringLiteral("print"));
        QTest::qWait(100);
        QComboBox *layout = nullptr;
        for (QComboBox *c : w.findChildren<QComboBox *>())
            for (int i = 0; i < c->count(); ++i)
                if (c->itemText(i).contains(QStringLiteral("Booklet"), Qt::CaseInsensitive)) { layout = c; c->setCurrentIndex(i); break; }
        QVERIFY(layout);
        QCheckBox *rulers = nullptr, *numbers = nullptr;
        for (QCheckBox *c : w.findChildren<QCheckBox *>()) {
            if (c->text() == QStringLiteral("Show rulers")) rulers = c;
            if (c->text() == QStringLiteral("Show page numbers")) numbers = c;
        }
        QVERIFY(rulers && numbers);
        rulers->setChecked(true);
        numbers->setChecked(true);
        QTest::qWait(100);
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/print-preview.png");
    }

    // Autoflow: a long story continues on new pages in linked boxes.
    void autoflowNewPages()
    {
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        jp::Editor *ed = w.editor();
        QString text;
        for (int i = 0; i < 120; ++i) text += QStringLiteral("This is paragraph %1 of a long story that will not fit on one page at all.\n").arg(i + 1);
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 468, 300), text));
        ed->addItem(box);
        const int added = jp::autoflowText(ed, box->id);
        QVERIFY(added >= 2);
        QCOMPARE(int(ed->doc()->pages.size()), 1 + added);
        const auto chain = ed->doc()->chainOf(box->id);
        QCOMPARE(int(chain.size()), 1 + added);
        for (int i = 1; i < chain.size(); ++i) {
            QCOMPARE(ed->doc()->find(chain[i]->id).page, i);
            QCOMPARE(chain[i]->rect, box->rect);
        }
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        QVERIFY(!cache.textFrame(*ed->doc(), *chain.last(), int(chain.size()), opt).layout->overflow());
        QCOMPARE(jp::autoflowText(ed, box->id), 0);   // nothing more to do
    }

    // Extra line spacing goes below each line, as .pub layouts place it: the
    // first line sits one ascent below the top, a paragraph after a more
    // widely spaced one starts lower by that paragraph's extra, and a box
    // holds as many lines as before.
    // A line of small capitals only (lowercase letters drawn at 0.8 x) is
    // spaced from the next as a line at the full size, as in Publisher's PDF
    // of the Age of Reason cover (100 pt small caps at 0.75 lines: baselines
    // 71.6 and 86.7 pt apart); a soft hyphen in it doesn't make it deeper.
    void smallCapsLineSpacing()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 400, 600);
        t->insets = QMarginsF(0, 0, 0, 0);
        t->storyId = doc->createStory(QString());
        doc->pages[0]->items.push_back(t);
        QTextCursor c(doc->storyDoc(t->storyId));
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{QStringLiteral("Times New Roman")});
        cf.setFontPointSize(100);
        cf.setFontCapitalization(QFont::SmallCaps);
        QTextBlockFormat bf;
        bf.setLineHeight(75, QTextBlockFormat::ProportionalHeight);
        c.setBlockFormat(bf);
        c.insertText(QStringLiteral("age"), cf);
        c.insertBlock(bf, cf);
        c.insertText(QString::fromUtf8("rea\u00ADson"), cf);
        c.insertBlock(bf, cf);
        c.insertText(QStringLiteral("AGE"), cf);
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        const auto lines = cache.textFrame(*doc, *t, 1, opt).layout->lineInfo(0);
        QCOMPARE(lines.size(), 3);
        const double gap1 = lines[1].baseline - lines[0].baseline, gap2 = lines[2].baseline - lines[1].baseline;
        const double line = 0.75 * 100 * (1420.0 + 442 + 307) / 2048;   // a full-size line (Times New Roman)
        QVERIFY2(std::abs(gap1 - line) < 0.5, qPrintable(QString::number(gap1)));
        // The full-size capitals' line sits the same way: set closer than
        // single, a line's descent counts at the full size too (Age of Reason
        // and North Carolina covers: baselines within 0.1 pt of Publisher's).
        QVERIFY2(std::abs(gap2 - line) < 0.5, qPrintable(QString::number(gap2)));
        // The first baseline: 0.75 x (single - descent) below the top.
        const double single = 100 * (1420.0 + 442 + 307) / 2048, descent = 100 * 442.0 / 2048;
        QVERIFY2(std::abs(lines[0].baseline - 0.75 * (single - descent)) < 0.5, qPrintable(QString::number(lines[0].baseline)));
    }

    // Text at a fractional size lays out at that size: Qt sizes fonts in
    // whole pixels (13.25 pt measured as 13, 13.5 as 14), which pushed a
    // flyer's "Civilian homeland security" onto two lines.
    void fractionalFontSizes()
    {
        auto endX = [](double size) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{QStringLiteral("Times New Roman")});
            cf.setFontPointSize(size);
            c.insertText(QStringLiteral("Civilian homeland security"), cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(1000, 200);
            fs.insets = QMarginsF(0, 0, 0, 0);
            fs.hyphenate = false;
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            int frame = -1;
            QRectF r;
            lay.caretRect(doc.characterCount() - 1, &frame, &r);
            return r.x();
        };
        const double w13 = endX(13), w1325 = endX(13.25), w135 = endX(13.5);
        QVERIFY2(std::abs(w1325 / w13 - 13.25 / 13) < 0.003, qPrintable(QStringLiteral("%1 %2").arg(w13).arg(w1325)));
        QVERIFY2(std::abs(w135 / w13 - 13.5 / 13) < 0.003, qPrintable(QStringLiteral("%1 %2").arg(w13).arg(w135)));
    }

    // A story through many linked boxes keeps its lines where they belong in
    // the last boxes too (line positions once overflowed Qt's fixed-point
    // range past the 33rd box).
    void manyLinkedFrames()
    {
        QTextDocument doc;
        QTextCursor c(&doc);
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{QStringLiteral("Arial")});
        cf.setFontPointSize(11);
        for (int i = 0; i < 40; ++i) {
            if (i) c.insertBlock();
            c.insertText(QStringLiteral("Box %1").arg(i + 1), cf);
        }
        QVector<jp::FrameSpec> frames;
        for (int i = 0; i < 40; ++i) {
            jp::FrameSpec fs;
            fs.size = QSizeF(200, 20);
            fs.insets = QMarginsF(0, 0, 0, 0);
            frames << fs;
        }
        jp::StoryLayout lay;
        lay.build(&doc, frames, jp::LayoutEnv());
        QVERIFY(!lay.overflow());
        for (int f : {0, 33, 39}) {
            const auto lines = lay.lineInfo(f);
            QCOMPARE(lines.size(), 1);
            QVERIFY2(lines[0].baseline > 5 && lines[0].baseline < 20, qPrintable(QStringLiteral("box %1: %2").arg(f).arg(lines[0].baseline)));
            QCOMPARE(lines[0].text, QStringLiteral("Box %1").arg(f + 1));
        }
    }

    void lineSpacingBelowLines()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 400);
        t->insets = QMarginsF(0, 0, 0, 0);
        t->storyId = doc->createStory(QString());
        doc->pages[0]->items.push_back(t);
        QTextCursor c(doc->storyDoc(t->storyId));
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{QStringLiteral("Times New Roman")});
        cf.setFontPointSize(10);
        QTextBlockFormat wide, single;
        wide.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
        single.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
        c.setBlockFormat(wide);
        c.insertText(QStringLiteral("Widely spaced"), cf);
        c.insertBlock(single, cf);
        c.insertText(QStringLiteral("Single spaced"), cf);
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        const auto lines = cache.textFrame(*doc, *t, 1, opt).layout->lineInfo(0);
        QCOMPARE(lines.size(), 2);
        const double s = 10 * (1420.0 + 442 + 307) / 2048, kd = 10 * 442.0 / 2048;
        QVERIFY2(std::abs(lines[0].baseline - (s - kd)) < 0.05, qPrintable(QString::number(lines[0].baseline)));
        // The wide paragraph's extra half line comes before the next paragraph.
        QVERIFY2(std::abs(lines[1].baseline - lines[0].baseline - 1.5 * s) < 0.05, qPrintable(QString::number(lines[1].baseline - lines[0].baseline)));
        // A box exactly three wide lines tall still holds three lines.
        doc->storyDoc(t->storyId)->setPlainText(QString());
        QTextCursor c2(doc->storyDoc(t->storyId));
        c2.setBlockFormat(wide);
        c2.insertText(QStringLiteral("one\ntwo\nthree"), cf);
        QTextCursor all(doc->storyDoc(t->storyId));
        all.select(QTextCursor::Document);
        all.mergeBlockFormat(wide);
        t->rect.setHeight(s + 2 * 1.5 * s + 0.01);
        jp::LayoutCache cache2;
        const auto fl = cache2.textFrame(*doc, *t, 1, opt);
        QCOMPARE(fl.layout->lineInfo(0).size(), 3);
        QVERIFY(!fl.layout->overflow());
    }

    // Bullets in symbol fonts: a .pub file keeps the bullet's font with its
    // character, and a missing symbol font's bullet shows as Unicode.
    void symbolFontBullets()
    {
        QCOMPARE(jp::symbolToUnicode(QStringLiteral("Wingdings"), 0xF0E8), QStringLiteral("\u2794"));   // the arrow
        QCOMPARE(jp::symbolToUnicode(QStringLiteral("Symbol"), 0xB7), QStringLiteral("\u2022"));
        QVERIFY(jp::symbolToUnicode(QStringLiteral("Arial"), 0x41).isEmpty());
        QString font;
        QCOMPARE(jp::unicodeToSymbol(QChar(0x2714), &font), 0xF0FCu);
        QCOMPARE(font, QStringLiteral("Wingdings"));

        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 200);
        t->storyId = doc->createStory(QString());
        doc->pages[0]->items.push_back(t);
        QTextCursor c(doc->storyDoc(t->storyId));
        auto addList = [&](const QString &bullet, const QString &bulletFont, const QString &text, bool first) {
            if (!first) c.insertBlock();
            c.insertText(text);
            QTextListFormat lf;
            lf.setStyle(QTextListFormat::ListDisc);
            lf.setProperty(jp::tp::BulletChar, bullet);
            if (!bulletFont.isEmpty()) lf.setProperty(jp::tp::BulletFont, bulletFont);
            c.createList(lf);
        };
        addList(QString(QChar(0xF0E8)), QStringLiteral("Wingdings"), QStringLiteral("An arrow"), true);
        addList(QString(QChar(0x2714)), QString(), QStringLiteral("A check mark"), false);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("bullets.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        const jp::TextItem *bt = nullptr;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Text) bt = static_cast<const jp::TextItem *>(it.get());
        });
        QVERIFY(bt);
        QTextBlock b = back->storyDoc(bt->storyId)->begin();
        QVERIFY(b.textList() && b.next().textList());
        QCOMPARE(b.textList()->format().stringProperty(jp::tp::BulletFont), QStringLiteral("Wingdings"));
        QCOMPARE(b.textList()->format().stringProperty(jp::tp::BulletChar), QString(QChar(0xF0E8)));
        // A Unicode check mark is saved as Wingdings' own.
        QCOMPARE(b.next().textList()->format().stringProperty(jp::tp::BulletFont), QStringLiteral("Wingdings"));
        QCOMPARE(b.next().textList()->format().stringProperty(jp::tp::BulletChar), QString(QChar(0xF0FC)));
    }

    // A Symbol bullet (as .pub lists have) sets the height of its item's
    // first line when it is taller than the text's, at the bullet's own size.
    void listBulletLineHeight()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 120, 300);
        t->storyId = doc->createStory(QString());
        doc->pages[0]->items.push_back(t);
        QTextDocument *d = doc->storyDoc(t->storyId);
        QTextCursor c(d);
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{QStringLiteral("Times New Roman")});
        cf.setFontPointSize(10);
        QTextBlockFormat bf;
        bf.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
        c.setBlockFormat(bf);
        c.insertText(QStringLiteral("First item, long enough to wrap onto a second line"), cf);
        QTextListFormat lf;
        lf.setStyle(QTextListFormat::ListDisc);
        lf.setProperty(jp::tp::BulletFont, QStringLiteral("Symbol"));
        QTextList *list = c.createList(lf);
        c.insertBlock(bf, cf);
        c.insertText(QStringLiteral("Second item"), cf);
        list->add(c.block());
        auto heights = [&] {
            jp::LayoutCache cache;
            jp::RenderOptions opt;
            QVector<double> h;
            for (const auto &li : cache.textFrame(*doc, *t, 1, opt).layout->lineInfo(0)) h << li.rect.height();
            return h;
        };
        QVector<double> h = heights();
        QVERIFY(h.size() >= 3);
        const double symbolLine = 10 * (2059.0 + 450) / 2048, timesLine = 10 * (1420.0 + 442 + 307) / 2048;
        QVERIFY2(std::abs(h[0] - symbolLine) < 0.05, qPrintable(QString::number(h[0])));
        QVERIFY2(std::abs(h[1] - timesLine) < 0.05, qPrintable(QString::number(h[1])));   // the item's second line
        QVERIFY2(std::abs(h.last() - symbolLine) < 0.05, qPrintable(QString::number(h.last())));
        // A smaller bullet no longer sets the height.
        lf.setProperty(jp::tp::BulletSize, 8.0);
        list->setFormat(lf);
        h = heights();
        QVERIFY2(std::abs(h[0] - timesLine) < 0.05, qPrintable(QString::number(h[0])));
    }

    // All capitals show as capitals (Qt's layout ignores the setting), and
    // the text itself keeps its case.
    void allCapsShown()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 400, 100);
        t->storyId = doc->createStory(QStringLiteral("Report on the CIA"));
        doc->pages[0]->items.push_back(t);
        QTextDocument *d = doc->storyDoc(t->storyId);
        QTextCursor c(d);
        c.select(QTextCursor::Document);
        QTextCharFormat caps;
        caps.setFontCapitalization(QFont::AllUppercase);
        c.mergeCharFormat(caps);
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        const auto fl = cache.textFrame(*doc, *t, 1, opt);
        QVERIFY(fl.layout);
        QCOMPARE(fl.layout->lineInfo(0).value(0).text.remove(QChar(0x00AD)).trimmed(), QStringLiteral("REPORT ON THE CIA"));
        QCOMPARE(d->toPlainText(), QStringLiteral("Report on the CIA"));
    }

    // Tracking adds the font's average character width times the percentage
    // beyond 100 after each letter (as .pub layouts do); kerning adds points.
    void trackingAndKerning()
    {
        jp::LayoutEnv env;
        QTextCharFormat f;
        f.setFontFamilies(QStringList{QStringLiteral("Arial")});
        f.setFontPointSize(10);
        auto spacing = [&](const QTextCharFormat &g) {
            const QTextCharFormat r = jp::resolveCharFormat(g, env);
            return r.fontLetterSpacingType() == QFont::AbsoluteSpacing ? r.fontLetterSpacing() : 0.0;
        };
        QVERIFY(std::abs(spacing(f)) < 0.001);
        QTextCharFormat loose = f;
        loose.setProperty(jp::tp::Tracking, 125);
        QVERIFY2(std::abs(spacing(loose) - 0.25 * 904 / 2048 * 10) < 0.02, qPrintable(QString::number(spacing(loose))));
        QTextCharFormat both = f;
        both.setProperty(jp::tp::Tracking, 87.5);
        both.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        both.setFontLetterSpacing(1);
        QVERIFY(std::abs(spacing(both) - (1 - 0.125 * 904 / 2048 * 10)) < 0.02);
        // Files saved before tracking had its own property kept it as Qt
        // percentage spacing; it is read as tracking, not Qt's own stretching.
        QTextCharFormat old = f;
        old.setFontLetterSpacingType(QFont::PercentageSpacing);
        old.setFontLetterSpacing(125);
        QVERIFY(std::abs(spacing(old) - spacing(loose)) < 0.001);
    }

    // Comic Sans MS, Segoe UI and Arial Narrow, when missing, are drawn by
    // fonts made to their character widths (Comic Relief, Selawik, Liberation
    // Sans Narrow): a phrase is as wide as in the originals (their widths read
    // from the fonts on Windows). Comic Neue had run 8.6% narrower, Open Sans
    // 5.2% wider.
    void exactWidthStandIns()
    {
        const QString text = QStringLiteral("Defense Force volunteers serve their state");
        struct Case { const char *family; bool bold; double ems; };
        for (const Case &c : {Case{"Comic Sans MS", false, 20.312}, Case{"Comic Sans MS", true, 21.142}, Case{"Segoe UI", false, 18.393},
                              Case{"Arial Narrow", false, 15.454}, Case{"Arial Narrow", true, 16.544}}) {
            if (QFontDatabase::hasFamily(QString::fromLatin1(c.family))) continue;   // the original itself
            jp::LayoutEnv env;
            QTextCharFormat f;
            f.setFontFamilies(QStringList{QString::fromLatin1(c.family)});
            // At 100 pt, as text is laid out (at 8 times its size), so Qt's
            // rounding of each advance to the pixel doesn't add up.
            f.setFontPointSize(100);
            if (c.bold) f.setFontWeight(QFont::Bold);
            const QTextCharFormat r = jp::resolveCharFormat(f, env);
            QTextLayout tl(text, r.font());
            tl.beginLayout();
            QTextLine line = tl.createLine();
            line.setLineWidth(100000);
            tl.endLayout();
            const double width = line.naturalTextWidth();
            QVERIFY2(std::abs(width - c.ems * 100) < c.ems * 100 * 0.002,
                     qPrintable(QStringLiteral("%1%2: %3 pt, not %4").arg(QLatin1String(c.family), c.bold ? QStringLiteral(" bold") : QString()).arg(width).arg(c.ems * 100)));
        }
    }

    // A missing Gill Sans MT is drawn by Cabin to Gill Sans MT's widths, its
    // bold too: Cabin's letters run 5% wider than the regular's and 10%
    // narrower than the bold's, and its spaces 24% narrower (widths of the
    // fonts in Publisher's PDFs). Lines broke at other words than Publisher's.
    void standInWidthsMatchOriginals()
    {
        if (QFontDatabase::hasFamily(QStringLiteral("Gill Sans MT"))) QSKIP("Gill Sans MT is installed: its own widths are used");
#ifdef Q_OS_MACOS
        QSKIP("the Mac keeps the stand-ins' own widths for now");
#endif
        {
            QFont drawn(QStringLiteral("Gill Sans MT"));
            drawn.setFamilies({QStringLiteral("Gill Sans MT")});
            if (QFontInfo(drawn).family() != QLatin1String("Cabin")) QSKIP("Gill Sans MT is drawn by this system's own Gill Sans, at its own widths");
        }
        const QString text = QStringLiteral("Defense Force volunteers serve their state");
        // The phrase's width in Gill Sans MT (ems), regular and bold.
        for (const auto &[bold, ems] : {std::pair{false, 17.388}, std::pair{true, 19.855}}) {
            jp::LayoutEnv env;
            QTextCharFormat f;
            f.setFontFamilies(QStringList{QStringLiteral("Gill Sans MT")});
            f.setFontPointSize(10);
            if (bold) f.setFontWeight(QFont::Bold);
            const QTextCharFormat r = jp::resolveCharFormat(f, env);
            QTextLayout tl(text, r.font());
            tl.beginLayout();
            QTextLine line = tl.createLine();
            line.setLineWidth(10000);
            tl.endLayout();
            const double width = line.naturalTextWidth();
            QVERIFY2(std::abs(width - ems * 10) < ems * 10 * 0.015, qPrintable(QStringLiteral("%1: %2 pt, not %3").arg(bold ? "bold" : "regular").arg(width).arg(ems * 10)));
            const double space = line.cursorToX(8) - line.cursorToX(7);
            QVERIFY2(std::abs(space - 2.78) < 0.1, qPrintable(QString::number(space)));
        }
    }

    // Franklin Gothic Heavy, the missing font Publisher's own designs use
    // most, is drawn in Libre Franklin ExtraBold, narrowed to its width
    // (98.30 ems for this sample in the Windows font).
    void franklinGothicHeavyStandIn()
    {
        if (QFontDatabase::hasFamily(QStringLiteral("Franklin Gothic Heavy"))) QSKIP("Franklin Gothic Heavy is installed");
#ifdef Q_OS_MACOS
        QSKIP("the Mac keeps the stand-ins' own widths for now");
#endif
        const QString sample = QStringLiteral("The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs! Sphinx of black quartz, "
                                              "judge my vow. How vexingly quick daft zebras jump; 2026 Annual Report: Newsletter, Spring Edition.");
        jp::LayoutEnv env;
        QTextCharFormat f;
        f.setFontFamilies(QStringList{QStringLiteral("Franklin Gothic Heavy")});
        f.setFontPointSize(10);
        const QTextCharFormat r = jp::resolveCharFormat(f, env);
        const QFontInfo info(r.font());
        QCOMPARE(info.family(), QStringLiteral("Libre Franklin"));
        QVERIFY2(info.weight() >= 750, qPrintable(info.styleName()));
        QTextLayout tl(sample, r.font());
        tl.beginLayout();
        QTextLine line = tl.createLine();
        line.setLineWidth(100000);
        tl.endLayout();
        const double width = line.naturalTextWidth(), want = 98.3008 * 10;
        QVERIFY2(std::abs(width - want) < want * 0.01, qPrintable(QStringLiteral("%1 pt, not %2").arg(width).arg(want)));
    }

    // A missing font's stand-in is drawn as high as the real font's letters
    // (Libre Franklin's capitals stand 11% taller than Franklin Gothic
    // Demi's), with its widths and line breaks unchanged.
    void standInDrawnAtRealHeight()
    {
        if (QFontDatabase::hasFamily(QStringLiteral("Franklin Gothic Demi"))) QSKIP("Franklin Gothic Demi is installed");
        jp::LayoutEnv env;
        QTextCharFormat f;
        f.setFontFamilies(QStringList{QStringLiteral("Franklin Gothic Demi")});
        f.setFontPointSize(100);
        const QTextCharFormat r = jp::resolveCharFormat(f, env);
        if (QFontInfo(r.font()).family() != QLatin1String("Libre Franklin")) QSKIP("the stand-in isn't drawn here");
        QVERIFY(r.hasProperty(jp::tp::GlyphScaleY));
        const double scale = r.property(jp::tp::GlyphScaleY).toDouble();
        QVERIFY2(scale > 0.85 && scale < 0.95, qPrintable(QString::number(scale)));

        // Drawn: the H's ink is the stand-in's capital height times the scale.
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->id = QStringLiteral("t");
        t->rect = QRectF(36, 36, 500, 200);
        t->insets = QMarginsF(0, 0, 0, 0);
        t->storyId = doc->createStory(QStringLiteral("H"));
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            c.mergeCharFormat(f);
        }
        doc->pages[0]->items.push_back(t);
        LayoutCache cache;
        PaintContext ctx;
        ctx.doc = doc.get();
        ctx.cache = &cache;
        QImage img(612 * 2, 792 * 2, QImage::Format_ARGB32);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(2, 2);
            jp::Renderer::paintPage(&p, ctx, 0);
        }
        int top = img.height(), bottom = -1;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (qGray(img.pixel(x, y)) < 128) { top = std::min(top, y); bottom = std::max(bottom, y); }
        QVERIFY(bottom > top);
        const double ink = (bottom - top + 1) / 2.0;
        QFont sf(QStringLiteral("Libre Franklin"));
        sf.setWeight(QFont::Weight(r.fontWeight()));
        sf.setPixelSize(1000);
        const double cap = QFontMetricsF(sf).capHeight() / 1000 * 100;   // the stand-in's H at 100 pt
        QVERIFY2(std::abs(ink - cap * scale) < cap * 0.03, qPrintable(QStringLiteral("ink %1, stand-in cap %2, scale %3").arg(ink).arg(cap).arg(scale)));
    }

    // Text drawn run by run (a stand-in drawn taller or shorter) keeps its
    // underline and strikethrough.
    void scaledRunsKeepLines()
    {
        if (QFontDatabase::hasFamily(QStringLiteral("Franklin Gothic Demi"))) QSKIP("Franklin Gothic Demi is installed");
        jp::LayoutEnv env;
        QTextCharFormat f;
        f.setFontFamilies(QStringList{QStringLiteral("Franklin Gothic Demi")});
        f.setFontPointSize(60);
        if (!jp::resolveCharFormat(f, env).hasProperty(jp::tp::GlyphScaleY)) QSKIP("the stand-in isn't drawn here");
        auto ink = [&](bool underline, bool strike) {
            auto doc = Document::blank(QSizeF(612, 792));
            auto t = std::make_shared<TextItem>();
            t->id = QStringLiteral("t");
            t->rect = QRectF(36, 36, 500, 200);
            t->insets = QMarginsF(0, 0, 0, 0);
            t->storyId = doc->createStory(QStringLiteral("mmmm"));
            QTextCharFormat g = f;
            g.setFontUnderline(underline);
            g.setFontStrikeOut(strike);
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            c.mergeCharFormat(g);
            doc->pages[0]->items.push_back(t);
            LayoutCache cache;
            PaintContext ctx;
            ctx.doc = doc.get();
            ctx.cache = &cache;
            QImage img(612, 300, QImage::Format_ARGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            jp::Renderer::paintPage(&p, ctx, 0);
            p.end();
            QVector<int> rows;
            for (int y = 0; y < img.height(); ++y) {
                int n = 0;
                for (int x = 0; x < img.width(); ++x) n += qGray(img.pixel(x, y)) < 128;
                rows << n;
            }
            return rows;
        };
        const QVector<int> plain = ink(false, false), under = ink(true, false), strike = ink(false, true);
        int belowPlain = 0, belowUnder = 0, extraStrike = 0;
        int last = 0;
        for (int y = 0; y < plain.size(); ++y) if (plain[y]) last = y;
        for (int y = last + 1; y < plain.size(); ++y) { belowPlain += plain[y]; belowUnder += under[y]; }
        for (int y = 0; y < plain.size(); ++y) extraStrike += std::max(0, strike[y] - plain[y]);
        QVERIFY2(belowUnder > 100, qPrintable(QString::number(belowUnder)));   // a line under the letters
        QCOMPARE(belowPlain, 0);
        QVERIFY2(extraStrike > 100, qPrintable(QString::number(extraStrike)));   // a line through them
    }

    // The ribbon as built, one control per line (JP_RIBBON_DUMP=file writes
    // it, to compare a rebuilt ribbon with the one before).
    void ribbonDescription()
    {
        jp::MainWindow w;
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r);
        const QString d = r->describe();
        for (const char *tab : {"Home", "Insert", "Page Design", "Mailings", "Review", "View", "Text Box", "Table Layout"})
            QVERIFY2(d.contains(QStringLiteral("\ntab %1").arg(QLatin1String(tab))), tab);
        QVERIFY(d.contains(QStringLiteral("QToolButton edit.paste")));
        if (const QByteArray out = qgetenv("JP_RIBBON_DUMP"); !out.isEmpty()) {
            QFile f(QString::fromLocal8Bit(out));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(d.toUtf8());
        }
    }

    // AutoRecover keeps one copy per document and run: two "Cover.pub"
    // files in different folders overwrote each other's copy, copies were
    // never offered after a crash, and never removed.
    void autoRecoverCopies()
    {
        QTemporaryDir dir;
        jp::recovery::setRoot(dir.filePath(QStringLiteral("AutoRecover")));
        // Named for the document and its full path.
        const QString a = jp::recovery::copyPath(QStringLiteral("/one/Cover.pub"), QStringLiteral("Cover"), 1);
        const QString b = jp::recovery::copyPath(QStringLiteral("/two/Cover.pub"), QStringLiteral("Cover"), 2);
        QVERIFY(!a.isEmpty() && a != b);
        QCOMPARE(jp::recovery::copyPath(QStringLiteral("/one/Cover.pub"), QStringLiteral("Cover"), 7), a);
        QVERIFY(QFileInfo(a).fileName().startsWith(QLatin1String("Cover ")));
        QVERIFY(jp::recovery::copyPath(QString(), QStringLiteral("Publication1"), 1) != jp::recovery::copyPath(QString(), QStringLiteral("Publication1"), 2));

        // A run that crashed left a folder whose lock nobody holds; one
        // that's still going holds its lock; an older version left a copy
        // loose in the folder. This run's own copies aren't offered.
        const auto doc = jp::Document::blank(QSizeF(612, 792));
        QVERIFY(jp::recovery::write(*doc, a, QStringLiteral("/one/Cover.pub"), QStringLiteral("Cover")));
        const QString crashed = dir.filePath(QStringLiteral("AutoRecover/crashed"));
        QDir().mkpath(crashed);
        QVERIFY(jp::recovery::write(*doc, crashed + QStringLiteral("/Flyer 0123abcd.jpub"), QStringLiteral("/docs/Flyer.jpub"), QStringLiteral("Flyer")));
        const QString live = dir.filePath(QStringLiteral("AutoRecover/live"));
        QDir().mkpath(live);
        QLockFile liveLock(live + QStringLiteral("/session.lock"));
        QVERIFY(liveLock.tryLock(0));
        QVERIFY(jp::recovery::write(*doc, live + QStringLiteral("/Card 0123abcd.jpub"), QString(), QStringLiteral("Card")));
        QVERIFY(QFile::copy(a, dir.filePath(QStringLiteral("AutoRecover/Publication1.autorecover.jpub"))));
        const QVector<jp::recovery::Recovered> found = jp::recovery::orphans();
        QStringList titles;
        for (const auto &r : found) titles << r.title;
        titles.sort();
        QCOMPARE(titles, (QStringList{QStringLiteral("Flyer"), QStringLiteral("Publication1")}));
        for (const auto &r : found)
            if (r.title == QLatin1String("Flyer")) {
                QCOMPARE(r.source, QStringLiteral("/docs/Flyer.jpub"));
                jp::recovery::discard(r);
            }
        QVERIFY(!QDir(crashed).exists());   // the crashed run's folder goes with its last copy

        // A normal exit removes this run's folder.
        const QString session = jp::recovery::sessionDir();
        QVERIFY(QDir(session).exists());
        jp::recovery::endSession();
        QVERIFY(!QDir(session).exists());

        // A window keeps a copy while its work is unsaved, and drops it
        // once the work is saved or the window closes.
        {
            jp::MainWindow w;
            w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
            jp::Editor *ed = w.editor();
            ed->addItem(ed->newTextBox(QRectF(72, 72, 200, 100)));
            QVERIFY(ed->isModified());
            w.autoRecover();
            const QString copy = w.recoveryCopy();
            QVERIFY(QFile::exists(copy));
            QVERIFY(w.saveTo(dir.filePath(QStringLiteral("Saved.jpub"))));
            QVERIFY(!QFile::exists(copy));
            ed->addItem(ed->newTextBox(QRectF(72, 300, 200, 100)));
            w.autoRecover();
            QVERIFY(QFile::exists(w.recoveryCopy()));
            QVERIFY(w.recoveryCopy() != copy);   // named for Saved.jpub now
            const QString second = w.recoveryCopy();
            ed->setDocument(jp::Document::blank(QSizeF(612, 792)));   // File > New in this window
            QVERIFY(!QFile::exists(second));
        }
        jp::recovery::endSession();
        jp::recovery::setRoot(QString());
    }

    // A run that states its character scaling, even 100%, still gets its
    // missing font's stand-in weight and narrowing (times its own scaling):
    // sign designs' phone numbers in Franklin Gothic Heavy came out thin.
    void standInWithOwnScaling()
    {
        if (QFontDatabase::hasFamily(QStringLiteral("Franklin Gothic Heavy"))) QSKIP("Franklin Gothic Heavy is installed");
        jp::LayoutEnv env;
        QTextCharFormat plain;
        plain.setFontFamilies(QStringList{QStringLiteral("Franklin Gothic Heavy")});
        plain.setFontPointSize(10);
        const QTextCharFormat base = jp::resolveCharFormat(plain, env);
        for (int scale : {100, 80}) {
            QTextCharFormat f = plain;
            f.setFontStretch(scale);
            const QTextCharFormat r = jp::resolveCharFormat(f, env);
            QCOMPARE(r.fontWeight(), base.fontWeight());
            const int want = int(std::lround((base.hasProperty(QTextFormat::FontStretch) ? base.fontStretch() : 100) * scale / 100.0));
            QCOMPARE(r.hasProperty(QTextFormat::FontStretch) ? r.fontStretch() : 100, want);
#ifndef Q_OS_MACOS
            // The face drawn (the Mac's font matching picks Bold for a
            // narrowed ExtraBold; it keeps the stand-ins' own widths for now).
            QVERIFY2(QFontInfo(r.font()).weight() >= 750, qPrintable(QFontInfo(r.font()).styleName()));
#endif
        }
    }

    // A missing AG_Futura keeps its own half-em spaces with its stand-in
    // Jost, whose spaces are 0.3 em (word gaps measured in Publisher's PDFs).
    void substituteSpaceWidth()
    {
        if (QFontDatabase::hasFamily(QStringLiteral("AG_Futura"))) QSKIP("AG_Futura is installed: its own spaces are used");
        jp::LayoutEnv env;
        QTextCharFormat f;
        f.setFontFamilies(QStringList{QStringLiteral("AG_Futura")});
        f.setFontPointSize(20);
        const QTextCharFormat r = jp::resolveCharFormat(f, env);
        QTextLayout tl(QStringLiteral("A B"), r.font());
        tl.beginLayout();
        QTextLine line = tl.createLine();
        line.setLineWidth(1000);
        tl.endLayout();
        const double space = line.cursorToX(2) - line.cursorToX(1);
        QVERIFY2(std::abs(space - 10) < 0.3, qPrintable(QString::number(space)));
        // Fonts whose stand-ins match keep their spaces as drawn.
        QTextCharFormat arial = f;
        arial.setFontFamilies(QStringList{QStringLiteral("Arial")});
        QVERIFY(!jp::resolveCharFormat(arial, env).hasProperty(QTextFormat::FontWordSpacing));
    }

    // Circle Text Art follows Publisher's path: clockwise from just above
    // 9 o'clock, the whole text (trailing spaces too) filling it, letters
    // straddling the frame's ellipse.
    void textArtCircle()
    {
        jp::TextArtItem w;
        w.transform_ = QStringLiteral("circle");
        w.font = QStringLiteral("Arimo");
        w.text = QStringLiteral("A") + QString(30, QLatin1Char(' '));
        const QPainterPath p = jp::textArtPath(w, QSizeF(200, 200));
        QVERIFY(!p.isEmpty());
        const QPointF c = p.boundingRect().center() - QPointF(100, 100);
        const double ang = std::fmod(qRadiansToDegrees(std::atan2(c.y(), c.x())) + 360, 360);   // clockwise from 3 o'clock
        QVERIFY2(ang > 181 && ang < 215, qPrintable(QString::number(ang)));
        const double r = std::hypot(c.x(), c.y());
        QVERIFY2(r > 96 && r < 112, qPrintable(QString::number(r)));
        // A long text goes all the way round.
        w.text = QStringLiteral("UNITED STATES SENTENCING COMMISSION");
        const QRectF all = jp::textArtPath(w, QSizeF(200, 200)).boundingRect();
        QVERIFY2(all.left() < 0 && all.top() < 0 && all.right() > 200, qPrintable(QStringLiteral("%1 %2 %3").arg(all.left()).arg(all.top()).arg(all.right())));
        // A missing AG_Futura's wide spaces leave its letters less of the circle.
        if (!QFontDatabase::hasFamily(QStringLiteral("AG_Futura"))) {
            auto span = [&](const QString &font) {
                w.font = font;
                w.text = QStringLiteral("AB") + QString(10, QLatin1Char(' '));
                const QPainterPath q = jp::textArtPath(w, QSizeF(200, 200));
                double lo = 360, hi = 0;
                for (const QPolygonF &poly : q.toSubpathPolygons())
                    for (const QPointF &pt : poly) {
                        const double a = std::fmod(qRadiansToDegrees(std::atan2(pt.y() - 100, pt.x() - 100)) + 360 - 170, 360);
                        lo = std::min(lo, a);
                        hi = std::max(hi, a);
                    }
                return hi - lo;
            };
            QVERIFY(span(QStringLiteral("AG_Futura")) < span(QStringLiteral("Jost")) * 0.85);
        }
    }

    // AutoFormat as you type: dashes, fractions, ordinals and lists.
    void typingAutoFormat()
    {
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 400, 300)));
        ed->addItem(box);
        ed->beginTextEdit(box->id);
        auto type = [&](const QString &s) { for (QChar c : s) ed->typeText(QString(c)); };
        type(QStringLiteral("It was 1/2 done--almost the 1st try - really "));
        QTextDocument *doc = ed->doc()->storyDoc(box->storyId);
        QCOMPARE(doc->toPlainText(), QString::fromUtf8("It was \u00BD done\u2014almost the 1st try \u2013 really "));
        // The ordinal's letters are raised.
        const int st = int(doc->toPlainText().indexOf(QStringLiteral("1st"))) + 1;
        QTextCursor c(doc);
        c.setPosition(st + 1);
        QCOMPARE(c.charFormat().verticalAlignment(), QTextCharFormat::AlignSuperScript);
        // Lists.
        auto list = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 400, 400, 200)));
        ed->endTextEdit();
        ed->addItem(list);
        ed->beginTextEdit(list->id);
        type(QStringLiteral("* apples "));
        QTextDocument *ld = ed->doc()->storyDoc(list->storyId);
        QVERIFY(ld->begin().textList());
        QVERIFY(jp::isBulletList(ld->begin().textList()->format().style()));
        QCOMPARE(ld->begin().text(), QStringLiteral("Apples "));   // and AutoCorrect capitalizes the item
        ed->endTextEdit();
        auto nums = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 600, 400, 100)));
        ed->addItem(nums);
        ed->beginTextEdit(nums->id);
        type(QStringLiteral("3) third"));
        QTextBlock nb = ed->doc()->storyDoc(nums->storyId)->begin();
        QVERIFY(nb.textList());
        QVERIFY(!jp::isBulletList(nb.textList()->format().style()));
        QCOMPARE(nb.textList()->format().numberSuffix(), QStringLiteral(")"));
        QCOMPARE(nb.textList()->format().start(), 3);
    }

    // Widow and orphan control, keep lines together and keep with next, on a
    // story running from one text box into the next.
    void paragraphBreakRules()
    {
        // paras: (line count, flags) ; returns each paragraph's lines in box 1 and box 2.
        enum { Widow = 1, Together = 2, WithNext = 4 };
        auto run = [](const QVector<QPair<int, int>> &paras) {
            auto doc = jp::Document::blank(QSizeF(612, 792));
            auto a = std::make_shared<jp::TextItem>(), b = std::make_shared<jp::TextItem>();
            a->rect = QRectF(72, 72, 300, 150);
            b->rect = QRectF(72, 400, 300, 300);
            a->storyId = doc->createStory();
            b->storyId = a->storyId;
            a->nextId = b->id;
            doc->pages[0]->items.push_back(a);
            doc->pages[0]->items.push_back(b);
            QTextCursor c(doc->storyDoc(a->storyId));
            for (int i = 0; i < paras.size(); ++i) {
                if (i) c.insertBlock();
                QTextBlockFormat bf;
                bf.setProperty(jp::tp::WidowControl, bool(paras[i].second & Widow));
                bf.setProperty(jp::tp::KeepTogether, bool(paras[i].second & Together));
                bf.setProperty(jp::tp::KeepWithNext, bool(paras[i].second & WithNext));
                c.setBlockFormat(bf);
                QStringList lines;
                for (int k = 0; k < paras[i].first; ++k) lines << QStringLiteral("P%1 line %2").arg(i).arg(k);
                c.insertText(lines.join(QChar(QChar::LineSeparator)));
            }
            jp::LayoutCache cache;
            jp::RenderOptions opt;
            const auto fl = cache.textFrame(*doc, *a, 1, opt);
            QVector<QPair<int, int>> out(paras.size());
            for (int f = 0; f < 2; ++f)
                for (const auto &li : fl.layout->lineInfo(f)) {
                    const int pi = li.text.mid(1, li.text.indexOf(' ') - 1).toInt();
                    if (f == 0) ++out[pi].first; else ++out[pi].second;
                }
            return out;
        };
        // How many one-line paragraphs fit in the first box.
        QVector<QPair<int, int>> ones(30, qMakePair(1, 0));
        int fit = 0;
        for (const auto &r : run(ones)) fit += r.first;
        QVERIFY(fit > 6);
        auto fill = [&](int n) { return QVector<QPair<int, int>>(n, qMakePair(1, 0)); };
        // Orphan: one line left at the bottom moves on.
        auto paras = fill(fit - 1) << qMakePair(6, 0);
        QCOMPARE(run(paras).last(), qMakePair(1, 5));
        paras.last().second = Widow;
        QCOMPARE(run(paras).last(), qMakePair(0, 6));
        // Widow: one line carried over takes another with it.
        paras = fill(fit - 5) << qMakePair(6, 0);
        QCOMPARE(run(paras).last(), qMakePair(5, 1));
        paras.last().second = Widow;
        QCOMPARE(run(paras).last(), qMakePair(4, 2));
        // Keep lines together.
        paras = fill(fit - 3) << qMakePair(6, Together);
        QCOMPARE(run(paras).last(), qMakePair(0, 6));
        // Keep with next: a heading on the last line goes with its paragraph.
        paras = fill(fit - 1) << qMakePair(1, 0) << qMakePair(3, 0);
        auto got = run(paras);
        QCOMPARE(got[fit - 1], qMakePair(1, 0));
        paras[fit - 1].second = WithNext;
        got = run(paras);
        QCOMPARE(got[fit - 1], qMakePair(0, 1));
        QCOMPARE(got.last(), qMakePair(0, 3));
    }

    // Distribute spreads the last line too; baseline alignment puts lines on
    // the baseline guides; a raised cap stands on the first line.
    void paragraphDistributeBaselineRaisedCap()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto box = [&](const QRectF &r, const QString &text, const std::function<void(QTextBlockFormat &)> &fmt) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = r;
            t->storyId = doc->createStory(text);
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextBlockFormat bf;
            fmt(bf);
            c.mergeBlockFormat(bf);
            doc->pages[0]->items.push_back(t);
            return t;
        };
        // Distribute: "Hi there" reaches both sides of the box.
        auto d = box(QRectF(100, 100, 300, 40), QStringLiteral("Hi there"), [](QTextBlockFormat &bf) {
            bf.setAlignment(Qt::AlignJustify);
            bf.setProperty(jp::tp::Distribute, true);
        });
        jp::PaintContext ctx;
        ctx.doc = doc.get();
        jp::LayoutCache cache;
        ctx.cache = &cache;
        ctx.opt.output = true;
        QImage img(612, 792, QImage::Format_RGB32);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            jp::Renderer::paintPage(&p, ctx, 0);
        }
        int minX = 9999, maxX = -1;
        for (int y = 100; y < 140; ++y)
            for (int x = 100; x < 400; ++x)
                if (qGray(img.pixel(x, y)) < 128) { minX = std::min(minX, x); maxX = std::max(maxX, x); }
        QVERIFY2(minX < 112 && maxX > 386, qPrintable(QStringLiteral("%1..%2").arg(minX).arg(maxX)));
        Q_UNUSED(d);
        // Baseline guides every 18 pt: lines land 18 pt apart.
        doc->masters.first()->grid.baseline = 18;
        auto g = box(QRectF(100, 200, 300, 200), QStringLiteral("one\ntwo\nthree"), [](QTextBlockFormat &bf) { bf.setProperty(jp::tp::AlignToBaseline, true); });
        jp::RenderOptions opt;
        const auto lines = cache.textFrame(*doc, *g, 1, opt).layout->lineInfo(0);
        QCOMPARE(lines.size(), 3);
        // (Paragraph spacing can carry a line past one guide to the next.)
        auto onGrid = [](double d) { const double r = std::fmod(d, 18.0); return d > 1 && (r < 0.01 || r > 17.99); };
        QVERIFY2(onGrid(lines[1].rect.top() - lines[0].rect.top()) && onGrid(lines[2].rect.top() - lines[1].rect.top()),
                 qPrintable(QStringLiteral("%1 %2 %3").arg(lines[0].rect.top()).arg(lines[1].rect.top()).arg(lines[2].rect.top())));
        // A raised cap: room above the first line, and only it is indented.
        const QString words = QStringLiteral("Once upon a time there was a long paragraph with enough words to fill several lines of this box, "
                                             "and then it went on a good while longer so that it runs well past the drop cap's lines.");
        auto drop = box(QRectF(100, 450, 200, 200), words, [](QTextBlockFormat &bf) { bf.setProperty(jp::tp::DropCapLines, 3); bf.setProperty(jp::tp::DropCapChars, 1); });
        auto up = box(QRectF(320, 450, 200, 200), words, [](QTextBlockFormat &bf) {
            bf.setProperty(jp::tp::DropCapLines, 3);
            bf.setProperty(jp::tp::DropCapChars, 1);
            bf.setProperty(jp::tp::DropCapUpper, true);
        });
        const auto dl = cache.textFrame(*doc, *drop, 1, opt).layout->lineInfo(0), ul = cache.textFrame(*doc, *up, 1, opt).layout->lineInfo(0);
        QVERIFY(ul[0].rect.top() > dl[0].rect.top() + 5);      // room for the raised letter
        QVERIFY2(ul.size() >= 4 && dl.size() >= 5, qPrintable(QStringLiteral("%1 %2").arg(ul.size()).arg(dl.size())));
        QVERIFY(std::abs(ul[1].rect.left() - ul[2].rect.left()) < 0.5);   // a raised one only the first
        QVERIFY(ul[0].rect.left() > ul[1].rect.left() + 5);
        QVERIFY(dl[1].rect.left() > dl[3].rect.left() + 5);      // a dropped cap indents its lines

        // A gradient text fill: red on the left of the word, blue on the right.
        auto gt = std::make_shared<jp::TextItem>();
        gt->rect = QRectF(50, 700, 500, 80);
        gt->storyId = doc->createStory(QStringLiteral("MMMMMMMMMM"));
        {
            QTextCursor c(doc->storyDoc(gt->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setFontPointSize(48);
            const jp::Fill g = jp::Fill::gradient(jp::ColorRef::rgb(QColor(220, 0, 0)), jp::ColorRef::rgb(QColor(0, 0, 220)), 0);
            f.setProperty(jp::tp::TextFill, QString::fromUtf8(QJsonDocument(g.toJson()).toJson(QJsonDocument::Compact)));
            c.mergeCharFormat(f);
        }
        doc->pages[0]->items.push_back(gt);
        QImage gimg(612, 792, QImage::Format_RGB32);
        gimg.fill(Qt::white);
        {
            jp::LayoutCache gc;
            ctx.cache = &gc;
            QPainter p(&gimg);
            jp::Renderer::paintPage(&p, ctx, 0);
        }
        auto inkColor = [&](int x0, int x1) {
            long r = 0, b = 0;
            for (int y = 700; y < 780; ++y)
                for (int x = x0; x < x1; ++x) {
                    const QRgb px = gimg.pixel(x, y);
                    if (qGray(px) < 200) { r += qRed(px); b += qBlue(px); }
                }
            return qMakePair(r, b);
        };
        const auto leftInk = inkColor(55, 120), rightInk = inkColor(380, 545);
        QVERIFY2(leftInk.first > leftInk.second && rightInk.second > rightInk.first,
                 qPrintable(QStringLiteral("%1/%2 %3/%4").arg(leftInk.first).arg(leftInk.second).arg(rightInk.first).arg(rightInk.second)));
    }

    // Web page export: links are clickable areas over the page picture.
    void webPageLinks()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto shape = std::make_shared<jp::ShapeItem>();
        shape->rect = QRectF(100, 100, 50, 40);
        shape->fill = jp::Fill::solid(jp::ColorRef::rgb(Qt::red));
        shape->hyperlink = QStringLiteral("https://example.org/shape");
        doc->pages[0]->items.push_back(shape);
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 300, 300, 60);
        t->storyId = doc->createStory(QStringLiteral("Visit "));
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.movePosition(QTextCursor::End);
            QTextCharFormat f;
            f.setAnchor(true);
            f.setAnchorHref(QStringLiteral("https://example.org/text"));
            c.insertText(QStringLiteral("our site"), f);
        }
        doc->pages[0]->items.push_back(t);
        w.editor()->setDocument(std::move(doc));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("page.html"));
        QVERIFY(w.exportHtmlTo(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(f.readAll());
        QVERIFY(html.contains(QStringLiteral("usemap=\"#p1\"")));
        QVERIFY(html.contains(QStringLiteral("coords=\"200,200,300,280\" href=\"https://example.org/shape\"")));
        QVERIFY(html.contains(QStringLiteral("href=\"https://example.org/text\"")));
    }

    // "Always create backup copy" keeps the file as it was before saving.
    void saveBackupCopy()
    {
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("letter.jpub"));
        QVERIFY(w.saveTo(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray first = f.readAll();
        f.close();
        jp::Settings::get().setValue("save/backup", true);
        w.editor()->doc()->props.title = QStringLiteral("changed");
        QVERIFY(w.saveTo(path));
        jp::Settings::get().setValue("save/backup", false);
        QFile b(dir.filePath(QStringLiteral("Backup of letter.jpub")));
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), first);
    }

    // Inserting several pictures at once puts them in the picture tray: the
    // scratch area beside the page, as thumbnails in a column.
    void pictureTray()
    {
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QTemporaryDir dir;
        QStringList paths;
        for (int i = 0; i < 3; ++i) {
            QImage img(600, 400 + i * 100, QImage::Format_RGB32);
            img.fill(QColor::fromHsv(i * 100, 200, 220));
            const QString f = dir.filePath(QStringLiteral("p%1.png").arg(i));
            QVERIFY(img.save(f));
            paths << f;
        }
        w.insertFiles(paths, QPointF(-1, -1));
        const jp::Document *d = w.editor()->doc();
        QCOMPARE(int(d->pages[0]->items.size()), 0);
        QCOMPARE(int(d->scratch.size()), 3);
        double y = -1;
        for (const auto &it : d->scratch) {
            QVERIFY(it->rect.left() >= 612 + 30);
            QVERIFY(std::max(it->rect.width(), it->rect.height()) <= 108.01);
            QVERIFY(it->rect.top() > y);
            y = it->rect.top();
        }
        // One picture still goes on the page.
        w.insertFiles({paths.first()}, QPointF(-1, -1));
        QCOMPARE(int(d->pages[0]->items.size()), 1);
    }

    void templatesFitTheirText()
    {
        QStringList problems;
        for (int withLogo = 0; withLogo < 2; ++withLogo)
        for (const jp::TemplateInfo &t : jp::templates()) {
            if (withLogo && !t.optionKeys.contains(QStringLiteral("logo"))) continue;
            jp::TemplateOptions opts;
            if (withLogo) opts.options["logo"] = true;
            std::unique_ptr<jp::Document> doc = t.build(opts);
            QVERIFY2(doc && !doc->pages.isEmpty(), qPrintable(t.id));
            jp::LayoutCache cache;
            jp::PaintContext ctx;
            ctx.doc = doc.get();
            ctx.cache = &cache;
            ctx.opt.output = true;
            for (int pi = 0; pi < doc->pages.size(); ++pi) {
                ctx.pageNumber = pi + 1;
                ctx.pageCount = int(doc->pages.size());
                jp::walkItems(doc->pages[pi]->items, [&](const jp::ItemPtr &it) {
                    if (auto *tx = dynamic_cast<jp::TextItem *>(it.get())) {
                        if (!tx->nextId.isEmpty()) return;
                        auto fl = cache.textFrame(*doc, *tx, pi + 1, ctx.opt);
                        if (fl.layout && fl.layout->overflow())
                            problems << QStringLiteral("%1 p%2 text \"%3\" used=%4 box=%5x%6").arg(t.id).arg(pi + 1).arg(doc->storyDoc(tx->storyId)->toPlainText().left(30)).arg(fl.layout->usedHeight(0)).arg(tx->rect.width()).arg(tx->rect.height());
                    } else if (auto *sh = dynamic_cast<jp::ShapeItem *>(it.get())) {
                        if (sh->storyId.isEmpty()) return;
                        QPointF o;
                        const jp::StoryLayout *l = jp::Renderer::shapeTextLayout(ctx, *sh, &o);
                        if (l && l->overflow())
                            problems << QStringLiteral("%1 p%2 shape %3 \"%4\"").arg(t.id).arg(pi + 1).arg(sh->shape, doc->storyDoc(sh->storyId)->toPlainText().left(30));
                    }
                });
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    // File > Export > PDF/A writes a file that identifies itself as PDF/A-1b
    // and carries the sRGB output intent the standard requires.
    void pdfaExport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        jp::MainWindow w;
        w.editor()->setDocument(jp::findTemplate(QStringLiteral("newsletter-classic"))->build(jp::TemplateOptions()));
        const QString path = dir.filePath(QStringLiteral("archive.pdf"));
        w.exportPdf(path, false, true);
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray pdf = f.readAll();
        QVERIFY(pdf.size() > 10000);
        QVERIFY2(pdf.contains("pdfaid:part"), "no PDF/A identification in the XMP metadata");
        QVERIFY2(pdf.contains("GTS_PDFA1"), "no PDF/A output intent");
    }

    // PDF/A has no transparency, so see-through picture pixels are flattened
    // onto what lies beneath them instead of turning white.
    void flattenKeepsSeeThroughPictures()
    {
        auto doc = jp::Document::blank(QSizeF(300, 300));
        doc->pages[0]->background = jp::Fill::solid(jp::ColorRef::rgb(QColor(40, 50, 70)));
        QImage pic(100, 100, QImage::Format_ARGB32);
        pic.fill(Qt::transparent);
        QPainter pp(&pic);
        pp.fillRect(QRect(0, 0, 50, 100), Qt::white);   // left half opaque white, right half clear
        pp.end();
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        pic.save(&buf, "PNG");
        auto item = std::make_shared<jp::PictureItem>();
        item->imageId = doc->addImage(png, "png");
        item->rect = QRectF(100, 100, 100, 100);
        item->imgRect = QRectF(0, 0, 100, 100);
        doc->pages[0]->items.push_back(item);
        QVERIFY(jp::Renderer::usesTransparency(*doc, *item));
        jp::LayoutCache cache;
        jp::PaintContext ctx;
        ctx.doc = doc.get();
        ctx.cache = &cache;
        ctx.opt.output = true;
        ctx.opt.flattenTransparency = true;
        const QImage out = jp::Renderer::renderToImage(ctx, 0, 1.0);
        QCOMPARE(QColor(out.pixel(125, 150)), QColor(Qt::white));          // opaque half
        QCOMPARE(QColor(out.pixel(175, 150)), QColor(40, 50, 70));         // clear half shows the page
    }

    // Every command in the window runs without crashing or Qt warnings, with a
    // fitting selection for its kind. Dialogs and menus it opens are closed.
    void allCommandsRun()
    {
        static QStringList warnings;
        static QString current;
        warnings.clear();
        QtMessageHandler prev = qInstallMessageHandler([](QtMsgType t, const QMessageLogContext &, const QString &m) {
            // The offscreen test display can't size native windows; that note is not a fault.
            if ((t == QtWarningMsg || t == QtCriticalMsg) && !m.contains(QLatin1String("This plugin does not support"))) warnings << current + ": " + m;
        });
        QTimer closer;
        closer.setInterval(20);
        QObject::connect(&closer, &QTimer::timeout, [] {
            if (QWidget *pop = QApplication::activePopupWidget()) pop->close();
            if (QWidget *mod = QApplication::activeModalWidget()) {
                if (auto *d = qobject_cast<QDialog *>(mod)) d->reject();
                else mod->close();
            }
        });
        closer.start();
        // Commands that end the session or open more windows are run elsewhere.
        QSet<QString> skip = {"file.exit", "win.new", "win.arrange", "win.cascade"};
#ifdef Q_OS_MACOS
        // macOS always shows its own print panel, which a test can't close.
        skip << QStringLiteral("file.printNow");
#endif
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QStringList ran;
        for (const QString &id : w.actionIds()) {
            if (skip.contains(id)) continue;
            QAction *a = w.act(id);
            if (!a) continue;
            current = id;
            // Which command a hang or crash is in (CI asks for this).
            if (qEnvironmentVariableIsSet("JP_TEST_TRACE")) {
                std::fprintf(stderr, "command %s\n", qPrintable(id));
                std::fflush(stderr);
            }
            const QString cat = id.section('.', 0, 0);
            // Fresh publication: the flyer has a picture, shapes, TextArt and text;
            // the newsletter's page 2 has a table.
            const bool table = cat == "tbl";
            w.editor()->setDocument(jp::findTemplate(table ? QStringLiteral("newsletter-classic") : QStringLiteral("flyer-event"))->build(jp::TemplateOptions()));
            jp::Editor *ed = w.editor();
            if (table) ed->setCurrentPage(1);
            auto firstOf = [&](jp::ItemType t) -> QString {
                for (const auto &it : ed->doc()->pages[ed->currentPage()]->items)
                    if (it->type() == t) return it->id;
                return {};
            };
            const QSet<QString> textCats = {"fmt", "para", "case", "dropcap", "style", "fit", "cols", "edit", "rev", "tb"};
            if (table) ed->select(firstOf(jp::ItemType::Table));
            else if (cat == "pic") ed->select(firstOf(jp::ItemType::Picture));
            else if (cat == "wa") ed->select(firstOf(jp::ItemType::TextArt));
            else if (cat == "shape" || cat == "fill" || cat == "line" || cat == "shadow" || cat == "obj" || cat == "arr" || cat == "wrap")
                ed->select(firstOf(jp::ItemType::Shape));
            else if (textCats.contains(cat)) {
                const QString t = firstOf(jp::ItemType::Text);
                ed->select(t);
                ed->beginTextEdit(t, 0);
                QTextCursor c = ed->cursor();
                c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                ed->setCursor(c);
            }
            QTest::qWait(45);   // the window updates command states 30 ms after a change
            if (a->isEnabled()) {
                a->trigger();
                QCoreApplication::processEvents();
                ran << id;
            } else if (qEnvironmentVariableIsSet("JP_LIST_DISABLED")) {
                fprintf(stderr, "disabled: %s\n", qPrintable(id));
            }
            ed->endTextEdit();
        }
        current.clear();
        closer.stop();
        qInstallMessageHandler(prev);
        qInfo("ran %d of %d commands", int(ran.size()), int(w.actionIds().size()));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.mid(0, 40).join('\n')));
    }

    // The year calendar's twelve pictures: made in parallel, compressed (they
    // were stored unpacked, 27 MB a calendar), and the same each time.
    void calendarPictures()
    {
        const jp::TemplateInfo *info = jp::findTemplate(QStringLiteral("calendar-year"));
        QVERIFY(info);
        jp::TemplateOptions o;
        o.colorScheme = QStringLiteral("Harbor");   // not the calendar's own colors, so likely made fresh here
        auto first = info->build(o);
        auto again = info->build(o);   // from the session's cache
        qint64 total = 0;
        QStringList a, b;
        for (const auto &img : first->images) {
            total += img.bytes.size();
            a << QString::fromLatin1(QCryptographicHash::hash(img.bytes, QCryptographicHash::Sha1).toHex());
            QVERIFY(!img.cache.isNull() && img.cache.size() == img.pixelSize);
            QCOMPARE(img.cache, img.image());   // as decoded from the file
        }
        for (const auto &img : again->images) b << QString::fromLatin1(QCryptographicHash::hash(img.bytes, QCryptographicHash::Sha1).toHex());
        QCOMPARE(int(first->images.size()), 12);
        QVERIFY2(total < 4 * 1024 * 1024, qPrintable(QString::number(total)));
        a.sort();
        b.sort();
        QCOMPARE(a, b);
        QImage decoded;
        {
            jp::ImageData copy = first->images.first();
            copy.cache = QImage();
            decoded = copy.image();
        }
        QCOMPARE(decoded, first->images.first().cache);
    }

    // A file the system hands over (macOS: double-clicked in the Finder)
    // opens in the empty window, and in a new one once that has a file.
    void fileOpenEvent()
    {
        jp::MainWindow w;
        w.show();
        jp::installFileOpenHandler(qApp);
        const QString first = QStringLiteral(JP_TEST_DATA "/pub/poi-SampleBrochure.pub");
        QFileOpenEvent open1(first);
        QCoreApplication::sendEvent(qApp, &open1);
        QCOMPARE(QFileInfo(w.editor()->filePath()).fileName(), QFileInfo(first).fileName());
        const QString second = QStringLiteral(JP_TEST_DATA "/pub/poi-SampleNewsletter.pub");
        QFileOpenEvent open2(second);
        QCoreApplication::sendEvent(qApp, &open2);
        QCOMPARE(QFileInfo(w.editor()->filePath()).fileName(), QFileInfo(first).fileName());
        jp::MainWindow *other = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (auto *mw = qobject_cast<jp::MainWindow *>(top); mw && mw != &w && mw->editor()->filePath().endsWith(QLatin1String("poi-SampleNewsletter.pub"))) other = mw;
        QVERIFY(other);
        other->close();
    }

    // PDF/X-1a:2001 for a commercial printer: everything in ink (the RGB
    // photo and the flattened see-through shape become CMYK), no
    // transparency, PDF 1.3, trim and bleed boxes, the printing condition,
    // and the version keys a printer's preflight checks.
    void pdfxExport()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->props.title = QStringLiteral("Cover");
        auto pic = std::make_shared<jp::PictureItem>();
        QImage img(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) img.setPixel(x, y, qRgb(x * 4, y * 4, 128));
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        pic->imageId = doc->addImage(png, "png");
        pic->rect = QRectF(72, 72, 200, 200);
        pic->imgRect = QRectF(0, 0, 200, 200);
        doc->pages[0]->items.push_back(pic);
        auto see = std::make_shared<jp::ShapeItem>();
        see->shape = QStringLiteral("rect");
        see->rect = QRectF(150, 150, 200, 200);
        see->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(200, 30, 40)));
        see->fill.transparency = 0.5;
        doc->pages[0]->items.push_back(see);
        auto grad = std::make_shared<jp::ShapeItem>();
        grad->shape = QStringLiteral("ellipse");
        grad->rect = QRectF(350, 500, 150, 100);
        grad->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(20, 90, 200)));
        doc->pages[0]->items.push_back(grad);
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 400, 400, 100);
        t->storyId = doc->createStory(QStringLiteral("Ready for press"));
        doc->pages[0]->items.push_back(t);
        w.editor()->setDocument(std::move(doc));
        QTemporaryDir dir;
        for (bool press : {false, true}) {
            const QString path = dir.filePath(press ? QStringLiteral("press.pdf") : QStringLiteral("plain.pdf"));
            jp::MainWindow::PdfSettings s;
            s.preset = press ? jp::MainWindow::PdfSettings::CommercialPress : jp::MainWindow::PdfSettings::HighQuality;
            s.pdfx = true;
            QVERIFY(w.exportPdfTo(path, s));
            if (press && !qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
                QFile::remove(qEnvironmentVariable("JP_SHOT_DIR") + "/pdfx-press.pdf");
                QFile::copy(path, qEnvironmentVariable("JP_SHOT_DIR") + "/pdfx-press.pdf");
            }
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray pdf = f.readAll();
            QVERIFY(pdf.startsWith("%PDF-1.3"));
            QVERIFY(pdf.contains("/GTS_PDFXVersion (PDF/X-1:2001)") && pdf.contains("/GTS_PDFXConformance (PDF/X-1a:2001)"));
            QVERIFY(pdf.contains("/Trapped /False") && pdf.contains("/OutputIntents [") && pdf.contains("/S /GTS_PDFX"));
            QVERIFY(pdf.contains("/OutputConditionIdentifier (CGATS TR 001)") && pdf.contains("/RegistryName (http://www.color.org)"));
            QVERIFY(!pdf.contains("DeviceRGB"));
            QVERIFY(!pdf.contains("/SMask") && !pdf.contains("/ca ") && !pdf.contains("/CA "));
            // Every picture in ink; the see-through shape was flattened into one.
            jp::QtPdf parsed;
            QVERIFY(parsed.load(path));
            int pictures = 0;
            for (const auto &o : parsed.objects)
                if (jp::QtPdf::dictOf(o.body).contains("/Subtype /Image")) {
                    ++pictures;
                    QVERIFY(jp::QtPdf::dictOf(o.body).contains("/DeviceCMYK") || jp::QtPdf::dictOf(o.body).contains("/DeviceGray"));
                }
            QVERIFY(pictures >= 2);
            // Trim is the page; with marks, the bleed reaches past it.
            const double m = press ? (pdf.contains("/MediaBox [0 0 708") ? 48 : -1) : 0;
            QVERIFY(m >= 0);
            QVERIFY2(pdf.contains(QStringLiteral("/TrimBox [%1 %1 %2 %3]").arg(m, 0, 'f', 3).arg(m + 612, 0, 'f', 3).arg(m + 792, 0, 'f', 3).toLatin1()),
                     pdf.mid(pdf.indexOf("/TrimBox"), 80).constData());
            QVERIFY(pdf.contains("/BleedBox ["));
            if (!press) QVERIFY(pdf.contains("/BleedBox [0.000 0.000 612.000 792.000]"));
        }
        // The check before a file for a printer: the 64-pixel picture over
        // 200 points is 23 ppi; a font this computer lacks.
        QStringList problems = jp::pressProblems(*w.editor()->doc());
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems.first().contains(QLatin1String("23 ppi")));
        QTextCursor c(w.editor()->doc()->storyDoc(t->storyId));
        c.select(QTextCursor::Document);
        QTextCharFormat f;
        f.setFontFamilies({QStringLiteral("No Such Font Anywhere")});
        c.mergeCharFormat(f);
        problems = jp::pressProblems(*w.editor()->doc());
        QCOMPARE(problems.size(), 2);
        QVERIFY(problems.last().contains(QLatin1String("No Such Font Anywhere")));
    }

    // Insert > Table of Contents: the Heading 1-3 paragraphs in reading
    // order with the page each starts on (one pushed to page 2 by the text
    // before it), entries indented by level with a dot leader to the page
    // number; Update follows renamed headings; one undo takes it all back.
    void tableOfContents()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->addPage(1, doc->pages[0]->masterId);
        auto head = [&](QTextDocument *sd, const QString &styleName, const QString &text) {
            QTextCursor c(sd);
            c.movePosition(QTextCursor::End);
            if (!sd->toPlainText().isEmpty()) c.insertBlock();
            QTextBlockFormat bf;
            bf.setProperty(jp::tp::StyleName, styleName);
            c.setBlockFormat(bf);
            c.insertText(text);
        };
        // Page 1: a box linked to one on page 2; enough text to push the
        // last heading over.
        auto a = std::make_shared<jp::TextItem>();
        a->rect = QRectF(72, 360, 468, 200);
        a->storyId = doc->createStory();
        auto b2 = std::make_shared<jp::TextItem>();
        b2->rect = QRectF(72, 72, 468, 600);
        b2->storyId = a->storyId;
        a->nextId = b2->id;
        doc->pages[0]->items.push_back(a);
        doc->pages[1]->items.push_back(b2);
        QTextDocument *sd = doc->storyDoc(a->storyId);
        head(sd, QStringLiteral("Heading 1"), QStringLiteral("Introduction"));
        for (int i = 0; i < 14; ++i) head(sd, QStringLiteral("Normal"), QStringLiteral("Body text that fills the first box. ").repeated(3));
        head(sd, QStringLiteral("Heading 2"), QStringLiteral("Background"));
        head(sd, QStringLiteral("Heading 3"), QStringLiteral("Earlier work"));
        head(sd, QStringLiteral("Heading 1"), QStringLiteral("Results"));
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        const QVector<jp::TocEntry> entries = jp::tableOfContentsEntries(*ed->doc());
        QCOMPARE(entries.size(), 4);
        QCOMPARE(entries[0].text, QStringLiteral("Introduction"));
        QCOMPARE(entries[0].page, 1);
        QCOMPARE(entries[1].level, 2);
        QCOMPARE(entries[3].text, QStringLiteral("Results"));
        QCOMPARE(entries[3].page, 2);
        // Insert from page 1, which has text: on a new page 2, so the
        // later headings move to page 3.
        ed->setCurrentPage(0);
        jp::insertTableOfContents(ed);
        QCOMPARE(int(ed->doc()->pages.size()), 3);
        QCOMPARE(ed->currentPage(), 1);
        QCOMPARE(int(ed->doc()->pages[1]->items.size()), 1);
        auto *toc = dynamic_cast<jp::TextItem *>(ed->doc()->pages[1]->items.front().get());
        QVERIFY(toc);
        QTextDocument *td = ed->doc()->storyDoc(toc->storyId);
        const QStringList lines = td->toPlainText().split(QChar::ParagraphSeparator).join('\n').split('\n');
        QCOMPARE(lines.value(0), QStringLiteral("Contents"));
        QCOMPARE(lines.value(1), QStringLiteral("Introduction\t1"));
        QCOMPARE(lines.value(4), QStringLiteral("Results\t3"));
        const QTextBlock second = td->begin().next().next();   // Background, level 2
        QCOMPARE(second.blockFormat().intProperty(jp::tp::TocLevel), 2);
        QCOMPARE(second.blockFormat().leftMargin(), 18.0);
        QCOMPARE(second.blockFormat().stringProperty(jp::tp::TabLeaders), QStringLiteral("."));
        QCOMPARE(second.blockFormat().tabPositions().value(0).type, QTextOption::RightTab);
        // Renamed, then updated.
        {
            QTextCursor c(sd->findBlockByNumber(17));
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.insertText(QStringLiteral("Findings"));
        }
        QCOMPARE(jp::updateTablesOfContents(ed), 1);
        QVERIFY(td->toPlainText().contains(QStringLiteral("Findings\t3")));
        QVERIFY(!td->toPlainText().contains(QStringLiteral("Results")));
        // One undo per command: the update, then the whole insert.
        ed->undo();
        ed->undo();
        QCOMPARE(int(ed->doc()->pages.size()), 2);
        // At the cursor, before the text there: the table on its own
        // paragraphs, the text after it.
        ed->beginTextEdit(a->id, 0);
        jp::insertTableOfContents(ed);
        QTextDocument *story = ed->doc()->storyDoc(a->storyId);
        QCOMPARE(story->begin().text(), QStringLiteral("Contents"));
        bool introAfter = false;
        for (QTextBlock b = story->begin(); b.isValid(); b = b.next())
            if (!b.blockFormat().hasProperty(jp::tp::TocLevel)) {
                introAfter = b.text() == QStringLiteral("Introduction");
                break;
            }
        QVERIFY(introAfter);
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            ed->endTextEdit();
            w.resize(1200, 900);
            w.show();
            QTest::qWait(100);
            w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/toc.png");
        }
    }

    // A story through three linked boxes of the same height is drawn in
    // all three (paint clipped each paragraph in frame-local units while its
    // lines sit below the earlier boxes: the third box drew nothing).
    void linkedBoxesAllDrawn()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        QVector<std::shared_ptr<jp::TextItem>> boxes;
        const QString story = doc->createStory(QStringLiteral("A line of text to fill the boxes.\n").repeated(24).trimmed());
        for (int i = 0; i < 3; ++i) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(72, 72 + i * 220, 300, 160);
            t->storyId = story;
            if (i) boxes.last()->nextId = t->id;
            boxes << t;
            doc->pages[0]->items.push_back(t);
        }
        w.editor()->setDocument(std::move(doc));
        const QImage img = w.pageThumbnail(0, 792);
        jp::LayoutCache cache;
        for (int i = 0; i < 3; ++i) {
            const auto fl = cache.textFrame(*w.editor()->doc(), *boxes[i], 1, jp::RenderOptions());
            const auto lines = fl.layout->lineRects(i);
            QVERIFY(lines.size() >= 5);
            for (const QRectF &r : lines) {
                int ink = 0;
                for (int y = int(boxes[i]->rect.top() + r.top()); y < int(boxes[i]->rect.top() + r.bottom()); ++y)
                    for (int x = 75; x < 372; ++x) ink += qGray(img.pixel(x, y)) < 128;
                QVERIFY2(ink > 50, qPrintable(QStringLiteral("box %1, line at %2: no text drawn").arg(i + 1).arg(r.top())));
            }
        }
    }

    // The SVG reader: path data (implicit lines, relative moves, reflected
    // controls, run-together arc flags), shapes, transforms, <use>, style
    // sheets, gradients and currentColor; and every bundled icon draws the
    // same as Qt's own SVG renderer draws it.
    void svgReader()
    {
        using namespace jp;
        QPainterPath p = svg::pathData(QStringLiteral("M10 10h5v5z m10 0 5 0 0 5"));
        QCOMPARE(p.elementCount(), 7);
        QCOMPARE(QPointF(p.elementAt(4)), QPointF(20, 10));      // a relative move after a close starts at the subpath's start
        QCOMPARE(QPointF(p.elementAt(6)), QPointF(25, 15));      // pairs after a move are lines
        p = svg::pathData(QStringLiteral("M0 0a10 10 0 1010 0")); // flags "1" "0" run into "10"
        QCOMPARE(p.currentPosition(), QPointF(10, 0));
        QVERIFY(p.boundingRect().height() > 18);                  // the large arc goes the long way round
        p = svg::pathData(QStringLiteral("M0 0C0 10 10 10 10 0S20 -10 20 0"));
        QCOMPARE(QPointF(p.elementAt(4)), QPointF(10, -10));      // S reflects the last control point
        p = svg::pathData(QStringLiteral("M1.5.5L-2-3e1"));
        QCOMPARE(QPointF(p.elementAt(0)), QPointF(1.5, 0.5));
        QCOMPARE(QPointF(p.elementAt(1)), QPointF(-2, -30));

        const QByteArray doc =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"2in\" height=\"1in\" viewBox=\"0 0 200 100\">"
            "<style>.a { fill: #ff0000; stroke: none } #b { fill: rgb(0, 0, 255) }</style>"
            "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#00ff00\"/><stop offset=\"1\" stop-color=\"#000\"/></linearGradient>"
            "<rect id=\"sq\" width=\"10\" height=\"10\"/></defs>"
            "<rect class=\"a\" x=\"10\" y=\"10\" width=\"20\" height=\"20\"/>"
            "<g transform=\"translate(100 0) scale(2)\" stroke=\"currentColor\" stroke-width=\"3\" fill=\"none\">"
            "<circle id=\"b\" cx=\"5\" cy=\"5\" r=\"5\"/>"
            "<line x1=\"0\" y1=\"20\" x2=\"10\" y2=\"20\" stroke-linecap=\"round\"/>"
            "</g>"
            "<use xlink:href=\"#sq\" x=\"50\" y=\"50\" fill=\"url(#g)\"/>"
            "<text x=\"0\" y=\"90\">words</text>"
            "<rect width=\"5\" height=\"5\" display=\"none\"/>"
            "</svg>";
        const svg::Drawing d = svg::read(doc, QColor(10, 20, 30));
        QVERIFY(d.isValid());
        QCOMPARE(d.viewBox, QRectF(0, 0, 200, 100));
        QCOMPARE(d.size, QSizeF(144, 72));
        QVERIFY(d.skipped);                                       // the text
        QCOMPARE(d.elements.size(), 4);
        QCOMPARE(d.elements[0].fill, QColor(255, 0, 0));
        QVERIFY(!d.elements[0].stroke.isValid());
        QCOMPARE(d.elements[0].path.boundingRect(), QRectF(10, 10, 20, 20));
        QCOMPARE(d.elements[1].fill, QColor(0, 0, 255));          // the id rule beats the group's fill="none"
        QCOMPARE(d.elements[1].stroke, QColor(10, 20, 30));
        QCOMPARE(d.elements[1].strokeWidth, 6.0);                 // scaled with the group
        QCOMPARE(d.elements[1].path.boundingRect(), QRectF(100, 0, 20, 20));
        QVERIFY(!d.elements[2].fill.isValid());                   // a line has no inside
        QCOMPARE(d.elements[2].cap, Qt::RoundCap);
        QCOMPARE(d.elements[3].fill, QColor(0, 255, 0));          // a gradient's first color
        QCOMPARE(d.elements[3].path.boundingRect(), QRectF(50, 50, 10, 10));

        // A drawing that repeats itself through <use>, ten times over at
        // each of nine levels, stops at a bounded number of parts.
        QByteArray bomb = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" viewBox=\"0 0 10 10\">"
                          "<defs><g id=\"g0\"><rect width=\"1\" height=\"1\"/></g>";
        for (int level = 1; level < 10; ++level) {
            bomb += "<g id=\"g" + QByteArray::number(level) + "\">";
            for (int i = 0; i < 10; ++i) bomb += "<use xlink:href=\"#g" + QByteArray::number(level - 1) + "\"/>";
            bomb += "</g>";
        }
        bomb += "</defs><use xlink:href=\"#g9\"/></svg>";
        QElapsedTimer bombTime;
        bombTime.start();
        const svg::Drawing many = svg::read(bomb);
        QVERIFY(bombTime.elapsed() < 5000);
        QVERIFY(many.elements.size() <= 100000);
        QVERIFY(many.skipped);
        // A box too small to scale from gives no shapes, not endless ones.
        svg::Drawing tiny;
        tiny.viewBox = QRectF(0, 0, 1e-300, 1e-300);
        tiny.elements << svg::Element{svg::pathData(QStringLiteral("M0 0L1e-300 1e-300")), QColor(Qt::black)};
        QVERIFY(!svg::shapes(tiny, QRectF(0, 0, 72, 72), true));

        // Every icon, drawn by this reader and by Qt's renderer at 48 pixels.
        int icons = 0, worst = 0;
        QString worstName;
        QDirIterator it(QStringLiteral(":/icons"), {QStringLiteral("*.svg")});
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray bytes = f.readAll();
            if (!bytes.contains("lucide")) continue;
            QImage qt(48, 48, QImage::Format_ARGB32_Premultiplied), ours(qt.size(), qt.format());
            qt.fill(Qt::transparent);
            ours.fill(Qt::transparent);
            {
                QPainter pq(&qt);
                QSvgRenderer(bytes).render(&pq);
            }
            const svg::Drawing icon = svg::read(bytes);
            QVERIFY2(icon.isValid() && !icon.elements.isEmpty(), qPrintable(path));
            {
                QPainter po(&ours);
                po.setRenderHint(QPainter::Antialiasing);
                po.scale(48 / icon.viewBox.width(), 48 / icon.viewBox.height());
                po.translate(-icon.viewBox.topLeft());
                for (const svg::Element &e : icon.elements) {
                    QPen pen(e.stroke.isValid() ? QBrush(e.stroke) : QBrush(Qt::NoBrush), e.strokeWidth, Qt::SolidLine, e.cap, e.join);
                    if (!e.dashes.isEmpty()) {
                        QVector<qreal> pattern;
                        for (double x : e.dashes) pattern << x / e.strokeWidth;
                        pen.setDashPattern(pattern);
                        pen.setDashOffset(e.dashOffset / e.strokeWidth);
                    }
                    po.setPen(e.stroke.isValid() ? pen : QPen(Qt::NoPen));
                    po.setBrush(e.fill.isValid() ? QBrush(e.fill) : QBrush(Qt::NoBrush));
                    po.drawPath(e.path);
                }
            }
            int differ = 0;
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 48; ++x)
                    if (std::abs(qAlpha(qt.pixel(x, y)) - qAlpha(ours.pixel(x, y))) > 64) ++differ;
            if (differ > worst) { worst = differ; worstName = path; }
            ++icons;
        }
        QVERIFY(icons > 1500);
        QVERIFY2(worst <= 4, qPrintable(QStringLiteral("%1: %2 pixels differ").arg(worstName).arg(worst)));
    }

    // Insert > Icons: the picker searches names and tags; an icon becomes
    // one editable artwork shape in the scheme's main color whose line
    // weight scales with it; an icon with filled parts is a group; inserting
    // several is one undo step; edited points follow any shape's resize; and
    // icons survive saving as .jpub and as .pub.
    void iconsInsert()
    {
        using namespace jp;
        QVERIFY(iconCatalog().size() > 1500);

        ItemPtr it = iconItem(QStringLiteral("heart"), QPointF(100, 100), 72, ColorRef::scheme(Main));
        QVERIFY(it && it->type() == ItemType::Shape);
        auto *heart = static_cast<ShapeItem *>(it.get());
        QVERIFY(heart->isArt());
        QCOMPARE(heart->rect, QRectF(64, 64, 72, 72));
        QCOMPARE(heart->stroke.width, 6.0);                       // 2 of 24 units, at an inch
        QCOMPARE(heart->stroke.color, ColorRef::scheme(Main));
        QCOMPARE(heart->stroke.cap, Qt::RoundCap);
        QCOMPARE(heart->stroke.join, Qt::RoundJoin);
        QVERIFY(heart->fill.type == Fill::NoFill);
        QCOMPARE(heart->altText, QStringLiteral("heart"));
        const QRectF pathBox = heart->customPath.boundingRect();
        QVERIFY(QRectF(0, 0, 72, 72).contains(pathBox) && pathBox.width() > 55);
        heart->scaleInto(heart->rect, QRectF(64, 64, 144, 144));
        QCOMPARE(heart->stroke.width, 12.0);
        QVERIFY(std::abs(heart->customPath.boundingRect().width() - 2 * pathBox.width()) < 1e-6);

        // A shape with edited points stretches with its frame, its line kept.
        ShapeItem edited;
        edited.rect = QRectF(0, 0, 100, 50);
        edited.customPath.addRect(QRectF(0, 0, 100, 50));
        edited.scaleInto(edited.rect, QRectF(0, 0, 200, 100));
        QCOMPARE(edited.customPath.boundingRect(), QRectF(0, 0, 200, 100));
        QCOMPARE(edited.stroke.width, 1.0);

        ItemPtr tag = iconItem(QStringLiteral("tag"), QPointF(300, 300), 72, ColorRef::scheme(Main));
        QVERIFY(tag && tag->type() == ItemType::Group);
        const auto &parts = static_cast<GroupItem *>(tag.get())->children;
        QCOMPARE(parts.size(), size_t(2));
        QVERIFY(static_cast<ShapeItem *>(parts[1].get())->fill.type == Fill::Solid);   // the dot is filled
        QCOMPARE(static_cast<ShapeItem *>(parts[1].get())->fill.color, ColorRef::scheme(Main));

        // The picker: words match the starts of names and tags.
        MainWindow w;
        w.editor()->setDocument(Document::blank(QSizeF(612, 792)));
        Editor *ed = w.editor();
        QStringList picked, love, arrows;
        QTimer::singleShot(0, &w, [&] {
            // Checked after the dialog closes, so a failure can't leave it open.
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dlg) return;
            auto *search = dlg->findChild<QLineEdit *>();
            auto *list = dlg->findChild<QListWidget *>(QStringLiteral("iconList"));
            auto shown = [&] {
                QStringList v;
                for (int i = 0; i < list->count(); ++i)
                    if (!list->item(i)->isHidden()) v << list->item(i)->data(Qt::UserRole).toString();
                return v;
            };
            if (search && list) {
                search->setText(QStringLiteral("love"));             // a tag of "heart"
                love = shown();
                search->setText(QStringLiteral("arrow-big-down"));   // hyphens count as spaces
                arrows = shown();
                for (int i = 0, n = 0; i < list->count() && n < 2; ++i)
                    if (!list->item(i)->isHidden()) { list->item(i)->setSelected(true); ++n; }
            }
            dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        picked = pickIcons(&w);
        QVERIFY(love.contains(QStringLiteral("heart")));
        QVERIFY(love.size() < 60);
        QVERIFY(arrows.contains(QStringLiteral("arrow-big-down")) && arrows.contains(QStringLiteral("arrow-big-down-dash")));
        QVERIFY(!arrows.contains(QStringLiteral("arrow-big-up")));
        QCOMPARE(picked, arrows.mid(0, 2));
        insertIcons(ed, picked);
        QCOMPARE(ed->doc()->pages[0]->items.size(), size_t(2));
        QCOMPARE(ed->selectedItems().size(), 2);
        const QRectF r0 = ed->doc()->pages[0]->items[0]->rect, r1 = ed->doc()->pages[0]->items[1]->rect;
        QCOMPARE(r0.size(), QSizeF(72, 72));
        QCOMPARE(r0.center().y(), 396.0);
        QCOMPARE(r0.center().x() + r1.center().x(), 612.0);         // side by side across the middle
        ed->undo();
        QCOMPARE(ed->doc()->pages[0]->items.size(), size_t(0));
        ed->redo();
        QCOMPARE(ed->doc()->pages[0]->items.size(), size_t(2));

        // Saving: .jpub keeps the artwork exactly; .pub keeps a drawn shape.
        ed->doc()->pages[0]->items.push_back(it);
        QTemporaryDir dir;
        QString err;
        const QString jpub = dir.filePath(QStringLiteral("icons.jpub"));
        QVERIFY2(savePublication(*ed->doc(), jpub, QImage(), &err), qPrintable(err));
        auto back = loadPublication(jpub, &err);
        QVERIFY2(back, qPrintable(err));
        auto *again = static_cast<ShapeItem *>(back->pages[0]->items[2].get());
        QVERIFY(again->isArt());
        QCOMPARE(again->stroke.width, 12.0);
        QCOMPARE(again->customPath.elementCount(), heart->customPath.elementCount());
        QCOMPARE(again->customPath.fillRule(), Qt::WindingFill);
        const QString pub = dir.filePath(QStringLiteral("icons.pub"));
        QVERIFY2(exportPublisher(*ed->doc(), pub, &err), qPrintable(err));
        auto fromPub = importPublisherFile(pub, &err);
        QVERIFY2(fromPub, qPrintable(err));
        QCOMPARE(fromPub->pages[0]->items.size(), size_t(3));
        auto *pubHeart = dynamic_cast<ShapeItem *>(fromPub->pages[0]->items[2].get());
        QVERIFY(pubHeart && !pubHeart->customPath.isEmpty());
        QVERIFY(std::abs(pubHeart->stroke.width - 12) < 0.1);
        QVERIFY(std::abs(pubHeart->customPath.boundingRect().width() - heart->customPath.boundingRect().width()) < 0.5);
    }

    // An SVG picture prints and exports to PDF as vectors, and Convert to
    // Shapes turns it into artwork shapes in its place: flipped, turned and
    // faded as the picture was, text left out (and said so), one undo step.
    void svgPictureToShapes()
    {
        using namespace jp;
        const QByteArray art =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 10\">"
            "<rect width=\"10\" height=\"10\" fill=\"#ff0000\"/>"
            "<circle cx=\"15\" cy=\"5\" r=\"4\" fill=\"none\" stroke=\"#0000ff\" stroke-width=\"1\"/></svg>";
        auto doc = Document::blank(QSizeF(612, 792));
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = doc->addImage(art, QStringLiteral("svg"));
        pic->rect = QRectF(100, 100, 200, 100);
        pic->imgRect = QRectF(0, 0, 200, 100);
        pic->flipH = true;
        pic->transparency = 0.5;

        // Straight conversion: the red half lands on the right when flipped.
        bool partial = true;
        ItemPtr made = svg::pictureShapes(art, *pic, &partial);
        QVERIFY(made && made->type() == ItemType::Group);
        QVERIFY(!partial);
        auto *g = static_cast<GroupItem *>(made.get());
        QCOMPARE(g->children.size(), size_t(2));
        auto *red = static_cast<ShapeItem *>(g->children[0].get());
        QVERIFY(red->isArt());
        QCOMPARE(red->rect, QRectF(200, 100, 100, 100));
        QCOMPARE(red->fill.color, ColorRef::rgb(QColor(255, 0, 0)));
        QCOMPARE(red->fill.transparency, 0.5);
        auto *ring = static_cast<ShapeItem *>(g->children[1].get());
        QCOMPARE(ring->stroke.width, 10.0);                       // 1 unit at 10 points a unit
        QVERIFY(ring->rect.right() < 200);
        QVERIFY(svg::pictureShapes(QByteArray(art).replace("</svg>", "<text>hi</text></svg>"), *pic, &partial));
        QVERIFY(partial);
        pic->rotation = 90;
        made = svg::pictureShapes(art, *pic, &partial);
        // A quarter turn about the picture's center takes the red square
        // (flipped to 50 points right of it) to 50 points below it.
        const Item *turned = static_cast<GroupItem *>(made.get())->children[0].get();
        QCOMPARE(turned->rotation, 90.0);
        QVERIFY(QLineF(turned->rect.center(), QPointF(200, 200)).length() < 1e-9);
        pic->rotation = 0;
        pic->flipH = false;
        pic->transparency = 0;
        doc->pages[0]->items.push_back(pic);

        MainWindow w;
        w.editor()->setDocument(std::move(doc));
        Editor *ed = w.editor();
        // Print and PDF draw it as lines and areas, not as a picture.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("svg.pdf"));
        MainWindow::PdfSettings ps;
        ps.preset = MainWindow::PdfSettings::HighQuality;
        QVERIFY(w.exportPdfTo(path, ps));
        QtPdf parsed;
        QVERIFY(parsed.load(path));
        for (const auto &o : parsed.objects) QVERIFY(!QtPdf::dictOf(o.body).contains("/Subtype /Image"));

        ed->select(pic->id);
        QTRY_VERIFY(w.act(QStringLiteral("pic.toShapes"))->isEnabled());
        w.act(QStringLiteral("pic.toShapes"))->trigger();
        QCOMPARE(ed->doc()->pages[0]->items.size(), size_t(1));
        QCOMPARE(ed->doc()->pages[0]->items[0]->type(), ItemType::Group);
        QCOMPARE(ed->selectedItems().size(), 1);
        ed->undo();
        QCOMPARE(ed->doc()->pages[0]->items[0]->type(), ItemType::Picture);
        ed->clearSelection();
        QTRY_VERIFY(!w.act(QStringLiteral("pic.toShapes"))->isEnabled());
    }

    // Save as Picture > SVG: each page as a vector drawing with letters as
    // outlines (no <text> to be respaced by another font), pictures cropped
    // to their shape, and the whole looking as JeffPub draws it.
    void svgExport()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("Hello vector world, in letters drawn as outlines."));
        doc->pages[0]->items.push_back(t);
        auto sh = std::make_shared<ShapeItem>();
        sh->rect = QRectF(72, 300, 100, 60);
        doc->pages[0]->items.push_back(sh);
        doc->pages[0]->items.push_back(iconItem(QStringLiteral("heart"), QPointF(150, 500), 72, ColorRef::scheme(Accent1)));
        QImage photo(200, 100, QImage::Format_RGB32);
        photo.fill(Qt::darkGreen);
        QByteArray png;
        QBuffer pb(&png);
        pb.open(QIODevice::WriteOnly);
        photo.save(&pb, "PNG");
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = doc->addImage(png, QStringLiteral("png"));
        pic->rect = QRectF(300, 300, 100, 100);
        pic->imgRect = QRectF(-50, 0, 200, 100);
        pic->maskShape = QStringLiteral("ellipse");
        doc->pages[0]->items.push_back(pic);
        doc->pages.push_back(std::make_shared<Page>());

        MainWindow w;
        w.editor()->setDocument(std::move(doc));
        QTemporaryDir dir;
        QString err;
        QVERIFY2(w.exportSvgTo(dir.filePath(QStringLiteral("pages.svg")), &err), qPrintable(err));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("pages-1.svg"))) && QFile::exists(dir.filePath(QStringLiteral("pages-2.svg"))));
        QFile f(dir.filePath(QStringLiteral("pages-1.svg")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray svg = f.readAll();
        QVERIFY(!svg.contains("<text"));
        QVERIFY(svg.contains("<image") && svg.contains("clip-path"));
        QVERIFY(svg.contains("viewBox=\"0 0 612 792\""));

        // The oval crop is a clip path around the picture's frame.
        const qsizetype clipAt = svg.indexOf("<clipPath");
        QVERIFY(clipAt > 0 && svg.indexOf("d=\"M400,350 C", clipAt) > clipAt);

        // Drawn by Qt's SVG renderer, it matches JeffPub's own drawing. (Qt's
        // renderer ignores clip paths, which browsers follow, so the cropped
        // picture's square is left out of the comparison.)
        PaintContext ctx;
        ctx.doc = w.editor()->doc();
        ctx.cache = &w.editor()->cache();
        ctx.opt.output = true;
        const QImage ours = Renderer::renderToImage(ctx, 0, 1.0).convertToFormat(QImage::Format_RGB32);
        QImage drawn(ours.size(), QImage::Format_RGB32);
        drawn.fill(Qt::white);
        {
            QPainter p(&drawn);
            QSvgRenderer(svg).render(&p, QRectF(0, 0, drawn.width(), drawn.height()));
        }
        int differ = 0, inked = 0;
        for (int y = 0; y < ours.height(); ++y)
            for (int x = 0; x < ours.width(); ++x) {
                if (QRectF(250, 300, 200, 100).contains(x, y)) continue;
                const QRgb a = ours.pixel(x, y), b = drawn.pixel(x, y);
                inked += a != qRgb(255, 255, 255);
                differ += std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))}) > 96;
            }
        QVERIFY(inked > 3000);
        QVERIFY2(differ < inked / 50, qPrintable(QStringLiteral("%1 of %2 inked pixels differ").arg(differ).arg(inked)));

        QVERIFY(!w.exportSvgTo(dir.filePath(QStringLiteral("missing/folder/page.svg")), &err));
        QVERIFY(!err.isEmpty());
    }

    // File > Export > Save as E-book: a valid EPUB 3 (the type first and
    // uncompressed, every file it lists present, well-formed XHTML), the
    // stories in reading order split into chapters at Heading 1, formatting,
    // links, lists, pictures, tables, footnotes beside the text, endnotes at
    // the back, contents from the headings, and no page numbers.
    void epubExport()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        doc->props.author = QStringLiteral("Pat Writer");
        const QString footId = doc->createStory(QStringLiteral("A footnote's words."));
        const QString endId = doc->createStory(QStringLiteral("An endnote's words."));
        auto field = [](QTextCursor &c, const QString &code) {
            QTextCharFormat f;
            f.setProperty(tp::Field, code);
            c.insertText(QString(QChar(0xFFFC)), f);
            c.setCharFormat(QTextCharFormat());
        };
        auto heading = [](QTextCursor &c, const QString &style, const QString &text, bool first) {
            QTextBlockFormat bf;
            bf.setProperty(tp::StyleName, style);
            if (first) c.setBlockFormat(bf);
            else c.insertBlock(bf, QTextCharFormat());
            c.insertText(text);
        };
        auto t1 = std::make_shared<TextItem>();
        t1->rect = QRectF(72, 72, 468, 300);
        t1->storyId = doc->createStory();
        {
            QTextCursor c(doc->storyDoc(t1->storyId));
            heading(c, QStringLiteral("Heading 1"), QStringLiteral("Chapter One"), true);
            c.insertBlock(QTextBlockFormat(), QTextCharFormat());
            c.insertText(QStringLiteral("Plain, "));
            QTextCharFormat b;
            b.setFontWeight(QFont::Bold);
            c.insertText(QStringLiteral("bold"), b);
            c.insertText(QStringLiteral(", "), QTextCharFormat());
            QTextCharFormat i;
            i.setFontItalic(true);
            c.insertText(QStringLiteral("italic"), i);
            c.insertText(QStringLiteral(" & a "), QTextCharFormat());
            QTextCharFormat a;
            a.setAnchor(true);
            a.setAnchorHref(QStringLiteral("https://example.com/"));
            c.insertText(QStringLiteral("link"), a);
            c.insertText(QStringLiteral("."), QTextCharFormat());
            field(c, QStringLiteral("footnote:") + footId);
            c.insertBlock(QTextBlockFormat(), QTextCharFormat());
            QTextListFormat lf;
            lf.setStyle(QTextListFormat::ListDecimal);
            c.createList(lf);
            c.insertText(QStringLiteral("First"));
            c.insertBlock();
            c.insertText(QStringLiteral("Second"));
            c.insertBlock(QTextBlockFormat(), QTextCharFormat());
            c.currentList() ? c.currentList()->remove(c.block()) : void();
        }
        doc->pages[0]->items.push_back(t1);
        QImage green(80, 40, QImage::Format_RGB32);
        green.fill(Qt::darkGreen);
        QByteArray png;
        QBuffer pb(&png);
        pb.open(QIODevice::WriteOnly);
        green.save(&pb, "PNG");
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = doc->addImage(png, QStringLiteral("png"));
        pic->rect = QRectF(72, 400, 200, 100);
        pic->imgRect = QRectF(0, 0, 200, 100);
        pic->altText = QStringLiteral("A green box");
        doc->pages[0]->items.push_back(pic);

        doc->pages.push_back(std::make_shared<Page>());
        auto t2 = std::make_shared<TextItem>();
        t2->rect = QRectF(72, 72, 468, 400);
        t2->storyId = doc->createStory();
        {
            QTextCursor c(doc->storyDoc(t2->storyId));
            QTextBlockFormat toc;
            toc.setProperty(tp::TocLevel, 1);
            c.setBlockFormat(toc);
            c.insertText(QStringLiteral("Contents line\t1"));
            heading(c, QStringLiteral("Heading 1"), QStringLiteral("Chapter Two"), false);
            heading(c, QStringLiteral("Heading 2"), QStringLiteral("Part A"), false);
            c.insertBlock(QTextBlockFormat(), QTextCharFormat());
            c.insertText(QStringLiteral("Text with an endnote"));
            field(c, QStringLiteral("endnote:") + endId);
            c.insertText(QStringLiteral(" on page "));
            field(c, QStringLiteral("page"));
            c.insertText(QStringLiteral("."));
        }
        doc->pages[1]->items.push_back(t2);

        MainWindow w;
        w.editor()->setDocument(std::move(doc));
        Editor *ed = w.editor();
        auto table = std::static_pointer_cast<TableItem>(ed->newTable(QRectF(72, 600, 300, 60), 2, 2));
        const QStringList cells = {QStringLiteral("A1"), QStringLiteral("B1"), QStringLiteral("A2"), QStringLiteral("B2")};
        for (int k = 0; k < 4; ++k) setStoryText(ed->doc()->storyDoc(table->cells[k].storyId), cells[k]);
        ed->doc()->pages[1]->items.push_back(table);

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("book.epub"));
        QString err;
        QVERIFY2(w.exportEpubTo(path, QStringLiteral("My Book"), QString(), true, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray epub = f.readAll();
        QCOMPARE(epub.mid(30, 28), QByteArray("mimetypeapplication/epub+zip"));   // first, stored
        QCOMPARE(int(epub[8]), 0);
        QMap<QString, QByteArray> z;
        QVERIFY2(readZip(epub, z, &err), qPrintable(err));
        QVERIFY(z.value(QStringLiteral("META-INF/container.xml")).contains("full-path=\"OEBPS/content.opf\""));

        // Every XML file is well formed.
        for (auto it = z.cbegin(); it != z.cend(); ++it) {
            if (!it.key().endsWith(QLatin1String(".xhtml")) && !it.key().endsWith(QLatin1String(".opf")) && !it.key().endsWith(QLatin1String(".xml"))) continue;
            QXmlStreamReader r(it.value());
            while (!r.atEnd()) r.readNext();
            QVERIFY2(!r.hasError(), qPrintable(it.key() + QStringLiteral(": ") + r.errorString()));
        }
        // The package lists only files that are there, and reads them in order.
        const QByteArray opf = z.value(QStringLiteral("OEBPS/content.opf"));
        QXmlStreamReader r(opf);
        QStringList ids, spine;
        while (!r.atEnd()) {
            r.readNext();
            if (!r.isStartElement()) continue;
            if (r.name() == QLatin1String("item")) {
                ids << r.attributes().value(QStringLiteral("id")).toString();
                QVERIFY2(z.contains(QStringLiteral("OEBPS/") + r.attributes().value(QStringLiteral("href")).toString()), qPrintable(r.attributes().value(QStringLiteral("href")).toString()));
            } else if (r.name() == QLatin1String("itemref")) {
                spine << r.attributes().value(QStringLiteral("idref")).toString();
            }
        }
        for (const QString &s : std::as_const(spine)) QVERIFY(ids.contains(s));
        QCOMPARE(spine, QStringList({QStringLiteral("cover"), QStringLiteral("c1"), QStringLiteral("c2"), QStringLiteral("notes")}));
        QVERIFY(opf.contains("properties=\"nav\"") && opf.contains("properties=\"cover-image\""));
        QVERIFY(opf.contains("<dc:title>My Book</dc:title>") && opf.contains("<dc:creator>Pat Writer</dc:creator>") && opf.contains("<dc:language>en-US</dc:language>"));

        const QString c1 = QString::fromUtf8(z.value(QStringLiteral("OEBPS/chapter1.xhtml")));
        QVERIFY(c1.contains(QStringLiteral("<h1 id=\"h1\">Chapter One</h1>")));
        QVERIFY(c1.contains(QStringLiteral("<strong>bold</strong>")) && c1.contains(QStringLiteral("<em>italic</em>")) && c1.contains(QStringLiteral(" &amp; a ")));
        QVERIFY(c1.contains(QStringLiteral("<a href=\"https://example.com/\">link</a>")));
        QVERIFY(c1.contains(QStringLiteral("<ol type=\"1\">\n<li>First</li>\n<li>Second</li>\n</ol>")));
        QVERIFY(c1.contains(QStringLiteral("href=\"#fn1\">1</a>")) && c1.contains(QStringLiteral("<aside epub:type=\"footnote\" id=\"fn1\">")));
        QVERIFY(c1.contains(QStringLiteral("A footnote's words.")));
        QVERIFY(c1.contains(QStringLiteral("<img src=\"images/pic1.png\" alt=\"A green box\"")));
        QVERIFY(c1.indexOf(QStringLiteral("Plain")) < c1.indexOf(QStringLiteral("<img")));   // the text above comes first
        const QString c2 = QString::fromUtf8(z.value(QStringLiteral("OEBPS/chapter2.xhtml")));
        QVERIFY(c2.contains(QStringLiteral(">Chapter Two</h1>")) && c2.contains(QStringLiteral(">Part A</h2>")));
        QVERIFY(!c2.contains(QStringLiteral("Contents line")));                                 // the book has its own contents
        QVERIFY(c2.contains(QStringLiteral("href=\"notes.xhtml#en1\"")));
        QVERIFY(c2.contains(QStringLiteral(" on page .</p>")));                                  // no page numbers
        QCOMPARE(c2.count(QStringLiteral("<td>")), 4);
        QVERIFY(c2.contains(QStringLiteral("<td><p>B2</p></td>")));
        const QString notes = QString::fromUtf8(z.value(QStringLiteral("OEBPS/notes.xhtml")));
        QVERIFY(notes.contains(QStringLiteral("An endnote's words.")) && notes.contains(QStringLiteral("chapter2.xhtml#enref1")));
        const QString nav = QString::fromUtf8(z.value(QStringLiteral("OEBPS/nav.xhtml")));
        QVERIFY(nav.contains(QStringLiteral("<a href=\"chapter1.xhtml#h1\">Chapter One</a>")));
        QVERIFY(nav.contains(QStringLiteral("Chapter Two</a><ol>\n<li><a href=\"chapter2.xhtml#h3\">Part A</a>")));   // nested under its chapter
        QVERIFY(!QImage::fromData(z.value(QStringLiteral("OEBPS/images/cover.jpg"))).isNull());
        QCOMPARE(z.value(QStringLiteral("OEBPS/images/pic1.png")), png);                        // a picture used as it is keeps its file

        // A cropped picture wider than the e-book's largest picture size
        // still comes out, scaled down.
        {
            auto big = Document::blank(QSizeF(3000, 2000));
            QImage photo(1000, 500, QImage::Format_RGB32);
            photo.fill(Qt::darkBlue);
            QByteArray jpg;
            QBuffer jb(&jpg);
            jb.open(QIODevice::WriteOnly);
            photo.save(&jb, "JPG");
            auto wide = std::make_shared<PictureItem>();
            wide->imageId = big->addImage(jpg, QStringLiteral("jpg"));
            wide->rect = QRectF(0, 0, 2800, 1400);
            wide->imgRect = QRectF(-100, 0, 3000, 1400);
            big->pages[0]->items.push_back(wide);
            const QString bigPath = dir.filePath(QStringLiteral("big.epub"));
            QVERIFY2(exportEpub(*big, bigPath, EpubOptions(), &err), qPrintable(err));
            QFile bf(bigPath);
            QVERIFY(bf.open(QIODevice::ReadOnly));
            QMap<QString, QByteArray> bz;
            QVERIFY(readZip(bf.readAll(), bz));
            const QImage out = QImage::fromData(bz.value(QStringLiteral("OEBPS/images/pic1.jpg")));
            QVERIFY(!out.isNull());
            QVERIFY(out.width() <= 1601 && out.width() >= 1000);
        }

        // A compressed entry that claims another size, or a huge one, is refused.
        ZipWriter zw;
        zw.add(QStringLiteral("a.txt"), QByteArray(5000, 'a'), true);
        const QByteArray good = zw.finish();
        QMap<QString, QByteArray> back;
        QVERIFY(readZip(good, back) && back.value(QStringLiteral("a.txt")) == QByteArray(5000, 'a'));
        const qsizetype dirAt = qFromLittleEndian<quint32>(good.constData() + good.size() - 22 + 16);
        for (const quint32 claimed : {4999u, 0x7FFFFFFFu}) {
            QByteArray bad = good;
            qToLittleEndian<quint32>(claimed, bad.data() + dirAt + 24);
            back.clear();
            QVERIFY(!readZip(bad, back));
        }
    }

    // docs/jpub-format.md stays true: its example opens as a publication,
    // and the property numbers it lists are the ones the code uses.
    void jpubSpecExample()
    {
        using namespace jp;
        QFile f(QStringLiteral(JP_TEST_DATA "/../../docs/jpub-format.md"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        // Git on Windows checks text out with CR LF line ends.
        const QString spec = QString::fromUtf8(f.readAll()).replace(QLatin1String("\r\n"), QLatin1String("\n"));
        const qsizetype a = spec.indexOf(QStringLiteral("```json\n")), b = spec.indexOf(QStringLiteral("```"), a + 8);
        QVERIFY(a > 0 && b > a);
        ZipWriter z;
        z.add(QStringLiteral("mimetype"), "application/x-jeffpub");
        z.add(QStringLiteral("document.json"), spec.mid(a + 8, b - a - 8).toUtf8());
        QString err;
        auto doc = publicationFromBytes(z.finish(), &err);
        QVERIFY2(doc, qPrintable(err));
        QCOMPARE(doc->pages.size(), 1);
        QCOMPARE(doc->pageSize(), QSizeF(612, 792));
        QCOMPARE(doc->pages[0]->items.size(), size_t(1));
        auto *t = dynamic_cast<TextItem *>(doc->pages[0]->items[0].get());
        QVERIFY(t);
        QCOMPARE(t->rect, QRectF(72, 72, 468, 100));
        const QTextDocument *sd = doc->storyDoc(t->storyId);
        QVERIFY(sd);
        QCOMPARE(sd->toPlainText(), QStringLiteral("Hello, world"));
        QTextCursor c(const_cast<QTextDocument *>(sd));
        c.setPosition(9);
        QCOMPARE(c.charFormat().fontWeight(), 700);
        QCOMPARE(doc->colors.c[1], QColor(0x1F, 0x4E, 0x79));

        const int qt[] = {QTextFormat::BlockAlignment, QTextFormat::LayoutDirection, QTextFormat::BlockTopMargin, QTextFormat::BlockBottomMargin,
                          QTextFormat::BlockLeftMargin, QTextFormat::BlockRightMargin, QTextFormat::TextIndent, QTextFormat::TabPositions,
                          QTextFormat::LineHeight, QTextFormat::LineHeightType, QTextFormat::PageBreakPolicy, QTextFormat::FontCapitalization,
                          QTextFormat::FontLetterSpacing, QTextFormat::FontKerning, QTextFormat::FontFamilies, QTextFormat::FontPointSize,
                          QTextFormat::FontWeight, QTextFormat::FontItalic, QTextFormat::FontUnderline, QTextFormat::FontOverline,
                          QTextFormat::FontStrikeOut, QTextFormat::TextUnderlineStyle, QTextFormat::TextUnderlineColor,
                          QTextFormat::TextVerticalAlignment, QTextFormat::IsAnchor, QTextFormat::AnchorHref, QTextFormat::ForegroundBrush,
                          QTextFormat::BackgroundBrush, QTextFormat::TextOutline, QTextFormat::ListStyle, QTextFormat::ListIndent, QTextFormat::ListStart};
        for (int n : qt) QVERIFY2(spec.contains(QRegularExpression(QStringLiteral("\\| (\\d+, )*%1(, \\d+)* \\|").arg(n))), qPrintable(QString::number(n)));
        for (int n = tp::ColorRefP; n <= tp::Tracking; ++n) QVERIFY2(spec.contains(QString::number(n)), qPrintable(QString::number(n)));
        for (int n = tp::StyleName; n <= tp::TocLevel; ++n) QVERIFY2(spec.contains(QString::number(n)), qPrintable(QString::number(n)));
    }

    // A long publication keeps all its text: .pub text past 64 KB of the
    // text stream used to wrap libmspub's 16-bit run positions, so every
    // story after the first ~32,000 characters opened empty.
    void longTextSurvivesPubRoundTrip()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        QStringList expected;
        for (int i = 0; i < 4; ++i) {
            if (i) doc->pages.push_back(std::make_shared<Page>());
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(36, 36, 540, 720);
            QString text;
            while (text.size() < 12000) text += QStringLiteral("Story %1 keeps every word of its text. ").arg(i + 1);
            text += QStringLiteral("End of story %1.").arg(i + 1);
            t->storyId = doc->createStory(text);
            expected << text;
            doc->pages[i]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("long.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->pages.size(), 4);
        for (int i = 0; i < 4; ++i) {
            auto *t = dynamic_cast<TextItem *>(back->pages[i]->items.empty() ? nullptr : back->pages[i]->items[0].get());
            QVERIFY(t);
            const QString got = back->storyDoc(t->storyId)->toPlainText().remove(QChar(0x00AD));
            QVERIFY2(got.endsWith(QStringLiteral("End of story %1.").arg(i + 1)), qPrintable(QStringLiteral("story %1: %2 characters").arg(i + 1).arg(got.size())));
            QCOMPARE(got.size(), expected[i].size());
        }
    }

    // Linked boxes keep their order when the file lists them in another
    // (here the second box is in front of the first): the chain follows each
    // box's place in it (0x28), not the order the boxes are drawn.
    void linkedBoxOrderFromPub()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto first = std::make_shared<TextItem>(), second = std::make_shared<TextItem>(), third = std::make_shared<TextItem>();
        first->rect = QRectF(36, 72, 160, 300);
        second->rect = QRectF(216, 72, 160, 300);
        third->rect = QRectF(396, 72, 160, 300);
        QString text;
        for (int i = 1; i <= 120; ++i) text += QStringLiteral("Sentence %1 of a story that runs through three boxes. ").arg(i);
        first->storyId = doc->createStory(QStringLiteral("START ") + text);
        second->storyId = third->storyId = first->storyId;
        first->nextId = second->id;
        second->nextId = third->id;
        // Drawn back to front: third, first, second.
        doc->pages[0]->items = {third, first, second};
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("order.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QVector<TextItem *> boxes;
        for (const auto &it : back->pages[0]->items)
            if (auto *t = dynamic_cast<TextItem *>(it.get())) boxes << t;
        QCOMPARE(boxes.size(), 3);
        auto at = [&](double x) { for (TextItem *t : boxes) if (std::abs(t->rect.x() - x) < 1) return t; return static_cast<TextItem *>(nullptr); };
        TextItem *a = at(36), *b = at(216), *c = at(396);
        QVERIFY(a && b && c);
        QVERIFY(!back->prevFrame(a->id));          // the left box starts the story
        QCOMPARE(a->nextId, b->id);
        QCOMPARE(b->nextId, c->id);
        QVERIFY(c->nextId.isEmpty());
        LayoutCache cache;
        RenderOptions opt;
        const auto fl = cache.textFrame(*back, *a, 1, opt);
        QVERIFY(fl.layout);
        const auto lines = fl.layout->lineInfo(fl.frame);
        QVERIFY(!lines.isEmpty());
        QVERIFY(lines.first().text.startsWith(QStringLiteral("START")));
    }

    // A .pub with many formatting pages: the text stream's index of
    // sections goes on in chained 512-byte blocks, as Publisher writes it
    // (19 entries in the first, 20 in each after). Written in one block, a
    // long index ran into the text and Publisher refused the file.
    void pubTextIndexChains()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(36, 36, 540, 720);
        QStringList paras;
        for (int i = 0; i < 400; ++i) paras << QStringLiteral("Paragraph %1 of a long story.").arg(i + 1);
        t->storyId = doc->createStory(paras.join(QChar('\n')));
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("many.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray q = cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        QVERIFY(q.size() > 1024);
        auto u16 = [&](qsizetype at) { return qFromLittleEndian<quint16>(q.constData() + at); };
        auto u32 = [&](qsizetype at) { return qFromLittleEndian<quint32>(q.constData() + at); };
        const int total = u16(0x0c);
        QVERIFY(total > 21);
        QCOMPARE(u32(0x14), quint32(q.size()));
        int seen = 0, blocks = 0;
        QStringList names;
        for (quint32 at = 0x18; at != 0xffffffffu; ++blocks) {
            QVERIFY(at + 8 <= quint32(q.size()) && blocks < 50);
            const int count = u16(at + 2);
            QVERIFY(count <= (at == 0x18 ? 19 : 20));
            // A block's entries stay in its own 512 bytes, clear of the text at 512.
            QVERIFY(at == 0x18 ? 0x20 + 24 * count <= 0x200 : at % 512 == 0 && 8 + 24 * count <= 512);
            for (int k = 0; k < count; ++k) names << QString::fromLatin1(q.mid(at + 8 + 24 * k + 2, 4));
            seen += count;
            at = u32(at + 4);
        }
        QCOMPARE(seen, total);
        QCOMPARE(u32(0x10), quint32(512 * blocks));
        QCOMPARE(names.first(), QStringLiteral("TEXT"));
        QVERIFY(names.contains(QStringLiteral("PL  ")) && names.count(QStringLiteral("FDPP")) >= 3);
        // Each formatting page names where its text starts: the page before's
        // last end (0 on the first), as Publisher writes it.
        for (const char *kind : {"FDPP", "FDPC"}) {
            quint32 prevEnd = 0;
            int pages = 0;
            for (quint32 at = 0x18; at != 0xffffffffu; at = u32(at + 4))
                for (int k = 0; k < u16(at + 2); ++k) {
                    const qsizetype e = at + 8 + 24 * k;
                    if (q.mid(e + 2, 4) != kind) continue;
                    const quint32 pg = u32(e + 16);
                    QCOMPARE(u32(pg + 4), prevEnd);
                    prevEnd = u32(pg + 8 + 4 * (u16(pg) - 1));
                    ++pages;
                }
            QVERIFY(pages >= 2);
        }
        // And it reads back whole.
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        const QString text = back->storyDoc(static_cast<TextItem *>(back->pages[0]->items[0].get())->storyId)->toPlainText();
        QVERIFY(text.startsWith(QStringLiteral("Paragraph 1 of")));
        QVERIFY(text.contains(QStringLiteral("Paragraph 400 of a long story.")));
    }

    // A vertical text box (a book's spine) saves as one: text flow 1 (top
    // to bottom), as Publisher writes it, not letters stacked in a tall box.
    void verticalTextBoxSavesToPub()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto spine = std::make_shared<TextItem>();
        spine->rect = QRectF(290, 36, 32, 700);
        spine->vertical = true;
        spine->storyId = doc->createStory(QStringLiteral("UNITED STATES BANKRUPTCY CODE"));
        doc->pages[0]->items.push_back(spine);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("spine.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        auto *t = dynamic_cast<TextItem *>(back->pages[0]->items[0].get());
        QVERIFY(t);
        QVERIFY(t->vertical);
        QCOMPARE(t->rotation, 0.0);
        QVERIFY(std::abs(t->rect.width() - 32) < 0.5 && std::abs(t->rect.height() - 700) < 0.5);
        // Its frame layout record is turned: width 700 points, height 32.
        const QByteArray q = cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        auto field = [](int id, quint32 v) {
            QByteArray b;
            b.append(char(id)).append(char(0x22));
            for (int k = 0; k < 4; ++k) b.append(char((v >> (8 * k)) & 0xff));
            return b;
        };
        QVERIFY(q.contains(field(0x04, 700 * 12700) + field(0x05, 32 * 12700)));
        // And its shape chunk says so (34 = 2), or Publisher stacks the letters.
        QVERIFY(cfb::readStream(path, QStringLiteral("Contents")).contains(QByteArray("\x34\x20\x02\x00\x00\x00", 6)));
    }

    // Connectors: a line's ends attach to connection sites (the middle of
    // each side: 0 top, 1 left, 2 bottom, 3 right) and follow the objects;
    // an elbow runs level or upright from each end as the site faces.
    void connectorsFollowObjects()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto a = std::make_shared<ShapeItem>(), b = std::make_shared<ShapeItem>();
        a->rect = QRectF(72, 72, 100, 60);
        b->rect = QRectF(300, 250, 100, 60);
        auto l = std::make_shared<LineItem>();
        l->route = LineItem::Elbow;
        l->start = LineItem::Glue{a->id, 3};
        l->end = LineItem::Glue{b->id, 0};
        doc->pages[0]->items = {a, b, l};
        QVERIFY(doc->routeConnectors());
        QCOMPARE(l->p1, QPointF(172, 102));
        QCOMPARE(l->p2, QPointF(350, 250));
        QVERIFY(!l->startVertical && l->endVertical);
        // Right side to top: out level, one corner, down into the top.
        QCOMPARE(l->routePoints(), (QVector<QPointF>{{172, 102}, {350, 102}, {350, 250}}));
        // The objects move and turn; the ends go with them.
        b->moveBy(40, 100);
        a->rotation = 90;
        QVERIFY(doc->routeConnectors());
        QCOMPARE(l->p2, QPointF(390, 350));
        QVERIFY(std::abs(l->p1.x() - 122) < 1e-9 && std::abs(l->p1.y() - 152) < 1e-9);
        QVERIFY(l->startVertical);   // the right side now faces down
        QVERIFY(!doc->routeConnectors());
        // Saved and opened again.
        LineItem back;
        back.fromJson(l->toJson());
        QCOMPARE(back.route, LineItem::Elbow);
        QVERIFY(back.start == l->start && back.end == l->end);
        // Copies of both objects and the line stay attached to each other; a
        // copy of the line alone comes loose.
        const ItemList copies = doc->cloneItems(doc->pages[0]->items);
        auto *cl = static_cast<LineItem *>(copies[2].get());
        QCOMPARE(cl->start.id, copies[0]->id);
        QCOMPARE(cl->end.id, copies[1]->id);
        QVERIFY(static_cast<LineItem *>(doc->cloneItem(*l).get())->start.id.isEmpty());
        // An object deleted: its end comes loose and stays where it was.
        doc->pages[0]->items = {a, l};
        QVERIFY(doc->routeConnectors());
        QVERIFY(l->end.id.isEmpty());
        QCOMPARE(l->p2, QPointF(390, 350));
    }

    // Drawing a connector from one shape's side to another's attaches it;
    // moving a shape takes the connector's end along; dragging the connector
    // away on its own detaches it.
    void connectorDrawnBetweenShapes()
    {
        jp::MainWindow w;
        w.resize(1200, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto a = std::make_shared<jp::ShapeItem>(), b = std::make_shared<jp::ShapeItem>();
        a->rect = QRectF(72, 72, 100, 60);
        b->rect = QRectF(300, 250, 100, 60);
        ed->addItem(a);
        ed->addItem(b);
        jp::Canvas *c = w.canvas();
        QWidget *vp = c->viewport();
        auto view = [&](QPointF page) { return c->pageToView(page).toPoint(); };
        auto move = [&](QPointF page) {
            const QPointF v = view(page);
            QMouseEvent mv(QEvent::MouseMove, v, vp->mapToGlobal(v), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(vp, &mv);
        };
        auto drag = [&](QPointF from, QPointF to) {
            QTest::qWait(QApplication::doubleClickInterval() + 50);
            QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, view(from));
            move((from + to) / 2);
            move(to);
            QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, view(to));
        };
        // Drawn from near A's right side to near B's left: the ends snap there.
        ed->setTool(jp::Tool::Arrow, QStringLiteral("elbow"));
        drag(QPointF(174, 104), QPointF(298, 278));
        const auto &items = ed->doc()->pages[0]->items;
        QCOMPARE(int(items.size()), 3);
        auto line = std::dynamic_pointer_cast<jp::LineItem>(items[2]);
        QVERIFY(line);
        QCOMPARE(line->route, jp::LineItem::Elbow);
        QCOMPARE(line->start, (jp::LineItem::Glue{a->id, 3}));
        QCOMPARE(line->end, (jp::LineItem::Glue{b->id, 1}));
        QCOMPARE(line->p1, QPointF(172, 102));
        QCOMPARE(line->p2, QPointF(300, 280));
        QCOMPARE(line->stroke.endArrow, jp::Arrow::Triangle);
        // B dragged down: the line's end goes with it.
        ed->setTool(jp::Tool::Select);
        drag(QPointF(370, 290), QPointF(370, 326));
        jp::Item *bNow = ed->doc()->item(b->id);
        QVERIFY(bNow && bNow->rect.top() > 270);
        line = std::dynamic_pointer_cast<jp::LineItem>(ed->doc()->itemPtr(line->id));
        QCOMPARE(line->p2, jp::connectionSite(*bNow, 1));
        QCOMPARE(line->p1, QPointF(172, 102));
        // The line dragged by its middle on its own: it comes loose.
        const QVector<QPointF> rp = line->routePoints();
        const QPointF mid = (rp[1] + rp[2]) / 2 + QPointF(0, 10);
        drag(mid, mid + QPointF(30, 0));
        line = std::dynamic_pointer_cast<jp::LineItem>(ed->doc()->itemPtr(line->id));
        QVERIFY(line->start.id.isEmpty() && line->end.id.isEmpty());
        QVERIFY(std::abs(line->p1.x() - 202) < 1.5 && std::abs(line->p1.y() - 102) < 1e-9);
        // Undo puts it back, attached.
        ed->undo();
        line = std::dynamic_pointer_cast<jp::LineItem>(ed->doc()->itemPtr(line->id));
        QCOMPARE(line->start.id, a->id);
    }

    // Regroup puts the objects of the group last ungrouped back together.
    // Deleting an object keeps the stories text refers to: a footnote's (the
    // field "footnote:<story>") and the text of a text box set in text.
    // Both counted as unused and were dropped with any deletion.
    void deleteKeepsStoriesTextRefersTo()
    {
        using namespace jp;
        Editor ed;
        ed.setDocument(Document::blank(QSizeF(612, 792)));
        Document *d = ed.doc();
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 300, 200);
        t->storyId = d->createStory(QStringLiteral("Text"));
        const QString note = d->createStory(QStringLiteral("A note."));
        const QString inner = d->createStory(QStringLiteral("Inner."));
        QTextCursor c(d->storyDoc(t->storyId));
        c.movePosition(QTextCursor::End);
        QTextCharFormat ff;
        ff.setProperty(tp::Field, QStringLiteral("footnote:") + note);
        c.insertText(QString(QChar::ObjectReplacementCharacter), ff);
        TextItem box;
        box.rect = QRectF(0, 0, 50, 20);
        box.storyId = inner;
        QTextCharFormat of;
        of.setProperty(tp::InlineObject, QString::fromUtf8(QJsonDocument(box.toJson()).toJson(QJsonDocument::Compact)));
        c.insertText(QString(QChar::ObjectReplacementCharacter), of);
        ed.addItem(t, false);
        auto s = std::make_shared<ShapeItem>();
        s->rect = QRectF(400, 400, 50, 50);
        ed.addItem(s);
        ed.deleteItems({s->id});
        QVERIFY(d->storyDoc(t->storyId));
        QVERIFY2(d->storyDoc(note), "the footnote's text was dropped");
        QVERIFY2(d->storyDoc(inner), "the text of the box set in text was dropped");
    }

    // Usage statistics: nothing is counted until the person says yes; then
    // commands count by id, in the form the collector takes, and a ping holds
    // only the install id, version, system, language, launches and counts.
    // Turning them off forgets what was counted and the id.
    void telemetryCountsOnlyWhenOn()
    {
        using namespace jp;
        telemetry::setEnabled(false);
        QVERIFY(telemetry::decided());
        telemetry::count(QStringLiteral("file.open.pub"));
        QVERIFY(telemetry::payload()[QStringLiteral("counts")].toObject().isEmpty());
        telemetry::setEnabled(true);
        telemetry::count(QStringLiteral("file.open.pub"));
        telemetry::count(QStringLiteral("file.open.pub"));
        telemetry::count(QStringLiteral("wrap.topBottom"));
        const QJsonObject p = telemetry::payload();
        const QJsonObject counts = p[QStringLiteral("counts")].toObject();
        QCOMPARE(counts[QStringLiteral("file.open.pub")].toInt(), 2);
        QCOMPARE(counts[QStringLiteral("wrap.topbottom")].toInt(), 1);
        QStringList keys = p.keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{"counts", "install", "lang", "launches", "os", "osVersion", "version"}));
        const QString id = p[QStringLiteral("install")].toString();
        QVERIFY(!QUuid::fromString(id).isNull());
        // What the collector checks (server/telemetry/main.py).
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")).match(id).hasMatch());
        QVERIFY(QRegularExpression(QStringLiteral("^[a-z]{1,16}$")).match(p[QStringLiteral("os")].toString()).hasMatch());
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9A-Za-z .()_+-]{0,40}$")).match(p[QStringLiteral("osVersion")].toString()).hasMatch());
        QVERIFY(QRegularExpression(QStringLiteral("^[A-Za-z_-]{0,20}$")).match(p[QStringLiteral("lang")].toString()).hasMatch());
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9A-Za-z.+-]{1,20}$")).match(p[QStringLiteral("version")].toString()).hasMatch());
        for (const QString &k : counts.keys()) QVERIFY(QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,47}$")).match(k).hasMatch());
        QCOMPARE(telemetry::payload()[QStringLiteral("install")].toString(), id);   // the same install each time
        // Once a day, and again the day an update is installed (0.1.37 had
        // pinged that day, so 0.1.38 waited until the next).
        Settings::get().setValue(QStringLiteral("telemetry/lastSent"), QDate::currentDate());
        Settings::get().setValue(QStringLiteral("telemetry/lastVersion"), QStringLiteral(JP_VERSION));
        QVERIFY(!telemetry::due());
        Settings::get().setValue(QStringLiteral("telemetry/lastVersion"), QVariant());   // before 0.1.39 none was kept
        QVERIFY(telemetry::due());
        Settings::get().setValue(QStringLiteral("telemetry/lastVersion"), QStringLiteral("0.1.1"));
        QVERIFY(telemetry::due());
        Settings::get().setValue(QStringLiteral("telemetry/lastVersion"), QStringLiteral(JP_VERSION));
        Settings::get().setValue(QStringLiteral("telemetry/lastSent"), QDate::currentDate().addDays(-1));
        QVERIFY(telemetry::due());
        telemetry::setEnabled(false);
        QVERIFY(!telemetry::due());
        QVERIFY(telemetry::payload()[QStringLiteral("counts")].toObject().isEmpty());
        QVERIFY(telemetry::payload()[QStringLiteral("install")].toString() != id);
        telemetry::setEnabled(false);
    }

    // Wrap Text > In Line with Text: a shape over a text box goes into its
    // text where its top left is (the start, here). Selected in the text,
    // Square brings it back onto the page where the text showed it. Both
    // undo.
    void moveObjectIntoAndOutOfText()
    {
        using namespace jp;
        Editor ed;
        ed.setDocument(Document::blank(QSizeF(612, 792)));
        ItemPtr t = ed.newTextBox(QRectF(72, 72, 300, 200), QStringLiteral("Hello world"));
        ed.addItem(t, false);
        const QString sid = static_cast<TextItem *>(t.get())->storyId;
        auto s = std::make_shared<ShapeItem>();
        s->rect = QRectF(70, 70, 30, 20);
        ed.addItem(s);
        QVERIFY(ed.canMoveIntoText());
        QVERIFY(ed.moveIntoText());
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 1);
        const QString inText = QString(QChar::ObjectReplacementCharacter) + QStringLiteral("Hello world");
        QCOMPARE(ed.doc()->storyDoc(sid)->toPlainText(), inText);
        ed.beginTextEdit(t->id, 0);
        QTextCursor c = ed.cursor();
        c.setPosition(0);
        c.setPosition(1, QTextCursor::KeepAnchor);
        ed.setCursor(c);
        QVERIFY(ed.selectionIsInlineObject());
        QVERIFY(ed.moveOutOfText(Wrap::Square));
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 2);
        QCOMPARE(ed.doc()->storyDoc(sid)->toPlainText(), QStringLiteral("Hello world"));
        const Item *out = ed.doc()->pages[0]->items[1].get();
        QVERIFY(out->type() == ItemType::Shape && out->wrap.mode == Wrap::Square);
        QVERIFY(std::abs(out->rect.width() - 30) < 0.01 && std::abs(out->rect.height() - 20) < 0.01);
        // At the start of the first line: the box's inset, then the object's left wrap distance.
        QVERIFY2(std::abs(out->rect.left() - (72 + 2.88 + 2.88)) < 0.5, qPrintable(QString::number(out->rect.left())));
        ed.undo();
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 1);
        QCOMPARE(ed.doc()->storyDoc(sid)->toPlainText(), inText);
        ed.undo();
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 2);
        QCOMPARE(ed.doc()->storyDoc(sid)->toPlainText(), QStringLiteral("Hello world"));
    }

    void regroupAfterUngroup()
    {
        using namespace jp;
        Editor ed;
        ed.setDocument(Document::blank(QSizeF(612, 792)));
        QStringList ids;
        for (int i = 0; i < 3; ++i) {
            auto s = std::make_shared<ShapeItem>();
            s->rect = QRectF(72 + 100 * i, 72, 72, 72);
            ed.addItem(s);
            ids << s->id;
        }
        QVERIFY(!ed.canRegroup());
        ed.select(ids);
        ed.groupSelection();
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 1);
        ed.ungroupSelection();
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 3);
        ed.clearSelection();
        QVERIFY(ed.canRegroup());
        ed.regroup();
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 1);
        auto *g = dynamic_cast<GroupItem *>(ed.doc()->pages[0]->items[0].get());
        QVERIFY(g && g->children.size() == 3);
        QCOMPARE(ed.selection(), QStringList{g->id});
        ed.undo();
        QCOMPARE(int(ed.doc()->pages[0]->items.size()), 3);
    }

    // Shapes > Lines: Curve (clicks, a smooth line through them, double-click
    // to end), Freeform (clicks joined straight; clicking the first point
    // closes it) and Scribble (one stroke by hand).
    void freehandDrawingTools()
    {
        jp::MainWindow w;
        w.resize(1200, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        jp::Canvas *c = w.canvas();
        QWidget *vp = c->viewport();
        auto view = [&](QPointF page) { return c->pageToView(page).toPoint(); };
        auto click = [&](QPointF page) {
            QTest::qWait(QApplication::doubleClickInterval() + 50);
            QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, view(page));
        };
        auto move = [&](QPointF page, Qt::MouseButtons held) {
            const QPointF v = view(page);
            QMouseEvent mv(QEvent::MouseMove, v, vp->mapToGlobal(v), Qt::NoButton, held, Qt::NoModifier);
            QApplication::sendEvent(vp, &mv);
        };
        const auto &items = ed->doc()->pages[0]->items;
        auto last = [&] { return std::dynamic_pointer_cast<jp::ShapeItem>(items.back()); };
        auto passesNear = [](const QPainterPath &path, QPointF pt) {
            for (double t = 0; t <= 1.0; t += 0.002)
                if (QLineF(path.pointAtPercent(t), pt).length() < 1.5) return true;
            return false;
        };
        // A curve through three points, ended with a double click on the third.
        ed->setTool(jp::Tool::Freeform, QStringLiteral("curve"));
        click(QPointF(100, 100));
        click(QPointF(200, 160));
        click(QPointF(300, 100));
        QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, view(QPointF(300, 100)));
        QCOMPARE(int(items.size()), 1);
        auto curve = last();
        QVERIFY(curve && curve->fill.isNone());
        QCOMPARE(ed->tool(), jp::Tool::Select);
        const QPainterPath cp = curve->customPath.translated(curve->rect.topLeft());
        QVERIFY(cp.elementAt(1).type == QPainterPath::CurveToElement);
        for (QPointF pt : {QPointF(100, 100), QPointF(200, 160), QPointF(300, 100)}) QVERIFY(passesNear(cp, pt));
        // A freeform triangle closed on its first point: filled.
        ed->setTool(jp::Tool::Freeform, QStringLiteral("freeform"));
        click(QPointF(100, 300));
        click(QPointF(250, 300));
        click(QPointF(175, 400));
        click(QPointF(100.5, 300.5));
        QCOMPARE(int(items.size()), 2);
        auto tri = last();
        QVERIFY(!tri->fill.isNone());
        QVERIFY(std::abs(tri->rect.width() - 150) < 1.5 && std::abs(tri->rect.height() - 100) < 1.5);
        // A scribble: one stroke.
        ed->setTool(jp::Tool::Freeform, QStringLiteral("scribble"));
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, view(QPointF(100, 500)));
        for (int i = 1; i <= 20; ++i) move(QPointF(100 + i * 10, 500 + (i % 2 ? 15 : -15)), Qt::LeftButton);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, view(QPointF(300, 485)));
        QCOMPARE(int(items.size()), 3);
        auto scribble = last();
        QVERIFY(scribble->fill.isNone());
        QVERIFY(scribble->customPath.elementCount() >= 15);
        // Saved to .pub and opened again: the open lines stay open and unfilled.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("free.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*ed->doc(), path, &err), qPrintable(err));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(int(back->pages[0]->items.size()), 3);
        auto *bc = dynamic_cast<jp::ShapeItem *>(back->pages[0]->items[0].get());
        QVERIFY(bc && bc->fill.isNone());
        QVERIFY(passesNear(bc->customPath.translated(bc->rect.topLeft()), QPointF(200, 160)));
        QVERIFY(!static_cast<jp::ShapeItem *>(back->pages[0]->items[1].get())->fill.isNone());
    }

    // Connectors in .pub: elbow and curved routes are Publisher's connector
    // shapes, turned and flipped so they start at the line's start, and the
    // drawing's connector rules keep them attached (checked by opening the
    // file in Publisher and moving a shape: the connectors followed it).
    void connectorsSaveToPub()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto a = std::make_shared<ShapeItem>(), b = std::make_shared<ShapeItem>();
        a->rect = QRectF(72, 72, 100, 60);
        b->rect = QRectF(300, 250, 100, 60);
        doc->pages[0]->items = {a, b};
        const struct { LineItem::Route route; int from, to; } specs[] = {
            {LineItem::Elbow, 3, 1}, {LineItem::Elbow, 2, 0}, {LineItem::Curved, 3, 1}, {LineItem::Curved, 2, 1}, {LineItem::Straight, 3, 1}};
        for (const auto &sp : specs) {
            auto l = std::make_shared<LineItem>();
            l->route = sp.route;
            l->start = LineItem::Glue{a->id, sp.from};
            l->end = LineItem::Glue{b->id, sp.to};
            l->stroke.endArrow = Arrow::Triangle;
            doc->pages[0]->items.push_back(l);
        }
        static_cast<LineItem *>(doc->pages[0]->items[2].get())->bend = 0.25;
        doc->routeConnectors();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("conn.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray escher = cfb::readStream(path, QStringLiteral("Escher/EscherStm"));
        // Five rules in a solver container; the bend as 5400 of 21600.
        QVERIFY(escher.contains(QByteArray("\x5f\x00\x05\xf0", 4)));
        QVERIFY(escher.contains(QByteArray("\x47\x01\x18\x15\x00\x00", 6)));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QVector<LineItem *> lines;
        QString aId, bId;
        for (const auto &it : back->pages[0]->items) {
            if (auto *l = dynamic_cast<LineItem *>(it.get())) lines << l;
            else if (it->rect.left() < 100) aId = it->id;
            else bId = it->id;
        }
        QCOMPARE(lines.size(), 5);
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(lines[i]->route, specs[i].route);
            QCOMPARE(lines[i]->start, (LineItem::Glue{aId, specs[i].from}));
            QCOMPARE(lines[i]->end, (LineItem::Glue{bId, specs[i].to}));
            QCOMPARE(lines[i]->stroke.endArrow, Arrow::Triangle);
        }
        QCOMPARE(lines[0]->bend, 0.25);
        QVERIFY(lines[1]->startVertical && lines[1]->endVertical);
        QVERIFY(lines[3]->startVertical && !lines[3]->endVertical);
    }

    // Page Setup's layout type is the DOCUMENT chunk's field 11 (1 booklet,
    // 3 folded card, 7 envelope; checked by choosing each in Publisher). A
    // booklet opens set to print two pages to a sheet, and its PDF holds
    // those sheets in folding order (8 and 1, 2 and 7...), as Publisher's does.
    void pubBookletLayout()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(396, 612));
        while (doc->pages.size() < 8) doc->addPage();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("booklet.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        // Make the file a booklet: its DOCUMENT chunk's 41 field (u32 0)
        // becomes 11 = 1, the same length.
        QFile in(path);
        QVERIFY(in.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(in.readAll(), &file));
        in.close();
        QByteArray c = file.stream(QStringLiteral("Contents"));
        const qsizetype at = c.indexOf(QByteArray("\x41\x20\x00\x00\x00\x00\x44\x70", 8));
        QVERIFY(at > 0);
        c.replace(at, 6, QByteArray("\x11\x20\x01\x00\x00\x00", 6));
        QVERIFY(file.setStream(QStringLiteral("Contents"), c));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(cfb::write(file));
        out.close();
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->setup.layout, PageSetup::Booklet);
        QCOMPARE(back->setup.sheet, QSizeF(792, 612));
        // Its PDF: four sheets of two pages, the first holding pages 8 and 1.
        for (int i = 0; i < 8; ++i) {
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(36, 36, 300, 60);
            t->storyId = back->createStory(QStringLiteral("Page %1").arg(i + 1));
            back->pages[i]->items.push_back(t);
        }
        MainWindow w;
        w.editor()->setDocument(std::move(back));
        const QString pdfPath = dir.filePath(QStringLiteral("booklet.pdf"));
        QVERIFY(w.exportPdfTo(pdfPath, MainWindow::PdfSettings()));
        QFile pf(pdfPath);
        QVERIFY(pf.open(QIODevice::ReadOnly));
        const QByteArray pdf = pf.readAll();
        QCOMPARE(pdf.count("/MediaBox [0 0 792.000000 612.000000]"), 4);
        // Reader pages instead when asked.
        MainWindow::PdfSettings single;
        single.booklet = false;
        QVERIFY(w.exportPdfTo(pdfPath, single));
        QFile pf2(pdfPath);
        QVERIFY(pf2.open(QIODevice::ReadOnly));
        QCOMPARE(pf2.readAll().count("/MediaBox [0 0 396.000000 612.000000]"), 8);
    }

    // Text wrapping in .pub: the shape record's field 04 low byte (0 none, 2
    // tight, 3 through, 4 top and bottom; left out for square), and the
    // distances in the drawing. Agrees with the wrapping Publisher reports for
    // 1,330 of the 1,342 objects in 284 publications.
    void pubWrapSetting()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        const Wrap::Mode modes[] = {Wrap::None, Wrap::Square, Wrap::Tight, Wrap::Through, Wrap::TopBottom};
        for (int i = 0; i < 5; ++i) {
            auto s = std::make_shared<ShapeItem>();
            s->rect = QRectF(72 + 90 * i, 72, 72, 72);
            s->wrap.mode = modes[i];
            s->wrap.left = 9 + i;
            doc->pages[0]->items.push_back(s);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("wrap.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(int(back->pages[0]->items.size()), 5);
        for (int i = 0; i < 5; ++i) {
            const Item *it = back->pages[0]->items[i].get();
            QCOMPARE(int(it->wrap.mode), int(modes[i]));
            QVERIFY(std::abs(it->wrap.left - (9 + i)) < 0.01);
            QVERIFY(std::abs(it->wrap.top - 2.88) < 0.01);
        }
    }

    // Wrapping counts a line's text (ascent and descent), not the spacing
    // below it: in Publisher a 48-point line keeps its full width while an
    // object starts below the bottom of its text, even inside the line's
    // spacing. And centered text wraps where it ends up: a line centered
    // below a picture stays whole.
    void wrapAroundObjectsLikePublisher()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 400, 300);
        t->insets = QMarginsF(0, 0, 0, 0);
        t->storyId = doc->createStory(QStringLiteral("Wide words here and more words follow on and on and on."));
        QTextCursor c(doc->storyDoc(t->storyId));
        c.select(QTextCursor::Document);
        QTextCharFormat cf;
        cf.setFontFamilies({QStringLiteral("Tinos")});
        cf.setFontPointSize(48);
        c.mergeCharFormat(cf);
        QTextBlockFormat bf;
        bf.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
        c.mergeBlockFormat(bf);
        auto pic = std::make_shared<ShapeItem>();
        doc->pages[0]->items = {t, pic};
        auto firstLineWidth = [&](double objectTop) {
            pic->rect = QRectF(300, objectTop + 2.88, 100, 40);
            LayoutCache cache;
            const auto fl = cache.textFrame(*doc, *t, 1, RenderOptions());
            return fl.layout->lineInfo(0).value(0).rect.width();
        };
        const double whole = firstLineWidth(400);
        // The text's bottom is about 53 points (1.107 em) below the top; the
        // line, with its spacing, about 62.
        QVERIFY(firstLineWidth(72 + 56) >= whole - 0.5);
        QVERIFY(firstLineWidth(72 + 40) < whole - 50);
        // Centered text below an object it would meet at the top of the box.
        t->valign = VAlign::Middle;
        doc->storyDoc(t->storyId)->setPlainText(QStringLiteral("CENTERED"));
        QTextCursor c2(doc->storyDoc(t->storyId));
        c2.select(QTextCursor::Document);
        cf.setFontPointSize(20);
        c2.mergeCharFormat(cf);
        pic->rect = QRectF(150, 72, 100, 60);
        LayoutCache cache;
        const auto fl = cache.textFrame(*doc, *t, 1, RenderOptions());
        const auto info = fl.layout->lineInfo(0);
        QCOMPARE(int(info.size()), 1);
        QVERIFY2(info[0].rect.left() < 10, qPrintable(QString::number(info[0].rect.left())));   // not pushed right of the object
    }

    // An object set in the text sits on the baseline inside its wrap
    // distances (2.88 pt), and one taller than the text's ascent lowers its
    // line by the difference; the next line keeps its usual distance.
    // Publisher (Oct 7), 24 pt wide boxes 6, 18 and 72 pt tall in 12 pt Times
    // New Roman, single spaced: line 2's baseline 2.64 + 2.88 + h + 2.88
    // below line 1's, the box 2.88 above it, line 3 12.72 below.
    void inlineObjectInLine()
    {
        using namespace jp;
        for (double h : {6.0, 18.0, 72.0}) {
            auto doc = Document::blank(QSizeF(612, 792));
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(72, 72, 120, 220);
            t->insets = QMarginsF(0, 0, 0, 0);
            t->storyId = doc->createStory(QStringLiteral("HxH\nHxH\nHxH"));
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat cf;
            cf.setFontFamilies({QStringLiteral("Times New Roman")});
            cf.setFontPointSize(12);
            c.mergeCharFormat(cf);
            QTextBlockFormat bf;
            bf.setTopMargin(0);
            bf.setBottomMargin(0);
            bf.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
            c.mergeBlockFormat(bf);
            ShapeItem box;
            box.rect = QRectF(400, 600, 24, h);
            c.setPosition(5);   // after line 2's "H"
            QTextCharFormat of = c.charFormat();
            of.setProperty(tp::InlineObject, QString::fromUtf8(QJsonDocument(box.toJson()).toJson(QJsonDocument::Compact)));
            c.insertText(QString(QChar::ObjectReplacementCharacter), of);
            doc->pages[0]->items = {t};
            LayoutCache cache;
            const auto fl = cache.textFrame(*doc, *t, 1, RenderOptions());
            const auto info = fl.layout->lineInfo(0);
            QCOMPARE(int(info.size()), 3);
            const auto objects = fl.layout->inlineObjects();
            QCOMPARE(int(objects.size()), 1);
            const QRectF r = objects[0].rect;
            const double b1 = info[0].baseline, b2 = info[1].baseline, b3 = info[2].baseline;
            const double normal = b3 - b2, descent = normal - b1;
            QVERIFY2(std::abs(r.height() - h) < 0.01 && std::abs(r.width() - 24) < 0.01, qPrintable(QStringLiteral("%1x%2").arg(r.width()).arg(r.height())));
            QVERIFY2(std::abs(r.bottom() - (b2 - 2.88)) < 0.05, qPrintable(QStringLiteral("h %1: bottom %2 baseline %3").arg(h).arg(r.bottom()).arg(b2)));
            QVERIFY2(std::abs((b2 - b1) - (descent + 2.88 + h + 2.88)) < 0.05,
                     qPrintable(QStringLiteral("h %1: b1 %2 b2 %3 b3 %4").arg(h).arg(b1).arg(b2).arg(b3)));
            QVERIFY(normal > 10 && normal < 16);
            // Across: after line 2's "H" (document position 4), inside the
            // side distances.
            const QVector<QRectF> hRects = fl.layout->rangeRects(0, 4, 5);
            QVERIFY(!hRects.isEmpty());
            QVERIFY2(std::abs(r.left() - (hRects.first().right() + 2.88)) < 0.05, qPrintable(QStringLiteral("%1 after %2").arg(r.left()).arg(hRects.first().right())));
            QCOMPARE(objects[0].docPos, 5);
            // The display text keeps one character for it, which isn't drawn.
            QCOMPARE(info[1].text.size(), 4);
        }
    }

    // An object set in text, saved as Publisher saves one: its U+FFFC's run
    // has 00 = 2, the story's EOBJ section gives its position and number,
    // the object sits on the last special page with that number (0f), and
    // an index (0x70, field 05 of the text index) ties number, story and
    // object together. Publisher showed such a copy exactly as its own file
    // (Oct 7); the object comes back with its size and wrap distances.
    void pubInlineObjectRoundTrip()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 200, 200);
        t->storyId = doc->createStory(QStringLiteral("Logo: here"));
        ShapeItem box;
        box.rect = QRectF(300, 500, 36, 24);
        box.fill = Fill::solid(ColorRef::rgb(Qt::red));
        box.wrap.left = 5;
        QTextCursor c(doc->storyDoc(t->storyId));
        c.setPosition(6);
        QTextCharFormat of = c.charFormat();
        of.setProperty(tp::InlineObject, QString::fromUtf8(QJsonDocument(box.toJson()).toJson(QJsonDocument::Compact)));
        c.insertText(QString(QChar::ObjectReplacementCharacter), of);
        doc->pages[0]->items = {t};
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("inline.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray q = cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        // EOBJ 0 (the first story): one object at 6, the story 12 long (with
        // its paragraph mark), number 1.
        QByteArray eobj;
        for (quint32 v : {1u, 4u, 0xff00u, 6u, 12u, 1u}) eobj.append(reinterpret_cast<const char *>(&v), 4);
        QVERIFY(q.contains(eobj));
        // The object's run: 00 (type 0x12) = 2.
        QVERIFY(q.contains(QByteArray::fromHex("001202000c22")));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(int(back->pages[0]->items.size()), 1);   // the object isn't on the page
        auto *bt = dynamic_cast<TextItem *>(back->pages[0]->items[0].get());
        QVERIFY(bt);
        QTextDocument *sd = back->storyDoc(bt->storyId);
        QCOMPARE(sd->toPlainText(), QStringLiteral("Logo: ") + QChar(QChar::ObjectReplacementCharacter) + QStringLiteral("here"));
        QTextCursor bc(sd);
        bc.setPosition(7);
        const ItemPtr obj = Item::fromJsonAny(QJsonDocument::fromJson(bc.charFormat().stringProperty(tp::InlineObject).toUtf8()).object());
        QVERIFY(obj && obj->type() == ItemType::Shape);
        QVERIFY(std::abs(obj->rect.width() - 36) < 0.01 && std::abs(obj->rect.height() - 24) < 0.01);
        QVERIFY(std::abs(obj->wrap.left - 5) < 0.01);
        QCOMPARE(static_cast<const ShapeItem *>(obj.get())->fill.color.resolve(back->colors), QColor(Qt::red));
        // JeffPub's own files keep it too.
        const QString jpath = dir.filePath(QStringLiteral("inline.jpub"));
        QVERIFY2(savePublication(*doc, jpath, QImage(), &err), qPrintable(err));
        auto again = loadPublication(jpath, &err);
        QVERIFY2(again, qPrintable(err));
        auto *jt = dynamic_cast<TextItem *>(again->pages[0]->items[0].get());
        QVERIFY(jt);
        QTextCursor jc(again->storyDoc(jt->storyId));
        jc.setPosition(7);
        QCOMPARE(jc.charFormat().stringProperty(tp::InlineObject), of.stringProperty(tp::InlineObject));

        // A damaged EOBJ section (its index entry and count claim more than
        // the stream holds) loses the object, never the publication's text.
        QFile in(path);
        QVERIFY(in.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(in.readAll(), &file));
        in.close();
        QByteArray quill = file.stream(QStringLiteral("Quill/QuillSub/CONTENTS"));
        const qsizetype entry = quill.indexOf("EOBJ") - 2;   // the section's index entry
        QVERIFY(entry > 0 && entry < 512);
        const quint32 eobjAt = qFromLittleEndian<quint32>(quill.constData() + entry + 16);
        qToLittleEndian<quint32>(0x00ffffff, quill.data() + entry + 20);   // its length
        qToLittleEndian<quint32>(100000, quill.data() + eobjAt);            // its count
        QVERIFY(file.setStream(QStringLiteral("Quill/QuillSub/CONTENTS"), quill));
        const QString damaged = dir.filePath(QStringLiteral("damaged.pub"));
        QFile out(damaged);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(cfb::write(file));
        out.close();
        auto rescued = importPublisherFile(damaged, &err);
        QVERIFY2(rescued, qPrintable(err));
        QVERIFY(!rescued->pages[0]->items.empty());
        auto *rt = dynamic_cast<TextItem *>(rescued->pages[0]->items.front().get());
        QVERIFY(rt);
        QCOMPARE(rescued->storyDoc(rt->storyId)->toPlainText(), QStringLiteral("Logo: here"));
    }

    // A text box set in text shows its own text (it's on no page, so it had
    // no chain of boxes, and its text was never laid out).
    void textBoxInTextShowsItsText()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto host = std::make_shared<TextItem>();
        host->rect = QRectF(72, 72, 300, 300);
        host->storyId = doc->createStory(QStringLiteral("Before "));
        TextItem inner;
        inner.rect = QRectF(0, 0, 150, 40);
        inner.storyId = doc->createStory(QStringLiteral("Inner words"));
        QTextCursor c(doc->storyDoc(host->storyId));
        c.movePosition(QTextCursor::End);
        QTextCharFormat of;
        of.setProperty(tp::InlineObject, QString::fromUtf8(QJsonDocument(inner.toJson()).toJson(QJsonDocument::Compact)));
        c.insertText(QString(QChar::ObjectReplacementCharacter), of);
        doc->pages[0]->items = {host};
        LayoutCache cache;
        const auto fl = cache.textFrame(*doc, inner, 1, RenderOptions());
        QVERIFY(fl.layout && fl.frame == 0);
        const auto lines = fl.layout->lineInfo(0);
        QVERIFY(!lines.isEmpty());
        QCOMPARE(QString(lines.first().text).remove(QChar(0x00AD)).trimmed(), QStringLiteral("Inner words"));
    }

    // Objects in text from a damaged or crafted file: a text box set in its
    // own story (drawing it drew it again, without end, and saving it kept
    // adding it), and sizes that aren't numbers or are absurd (they reached
    // Qt's fixed-point line positions).
    void inlineObjectsFromBadFilesStaySafe()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 300, 300);
        t->storyId = doc->createStory(QStringLiteral("Self: "));
        // First in its story, with no insets or wrap distances, so the box
        // fits in itself at every level.
        TextItem self;
        self.rect = QRectF(0, 0, 100, 40);
        self.insets = QMarginsF();
        self.wrap.left = self.wrap.top = self.wrap.right = self.wrap.bottom = 0;
        self.storyId = t->storyId;
        QTextCursor c(doc->storyDoc(t->storyId));
        QTextCharFormat of;
        of.setProperty(tp::InlineObject, QString::fromUtf8(QJsonDocument(self.toJson()).toJson(QJsonDocument::Compact)));
        c.insertText(QString(QChar::ObjectReplacementCharacter), of);
        c.movePosition(QTextCursor::End);
        // Absurd and not-a-number sizes.
        for (const char *json : {"{\"type\":\"shape\",\"rect\":[0,0,1e300,1e300]}", "{\"type\":\"shape\",\"rect\":[0,0,-50,-50]}",
                                 "{\"type\":\"shape\",\"rect\":[0,0,100,100],\"wrap\":{\"t\":1e300,\"b\":-1e300,\"l\":1e300,\"r\":0}}"}) {
            QTextCharFormat bad;
            bad.setProperty(tp::InlineObject, QString::fromLatin1(json));
            c.insertText(QString(QChar::ObjectReplacementCharacter), bad);
        }
        doc->pages[0]->items = {t};
        LayoutCache cache;
        const auto fl = cache.textFrame(*doc, *t, 1, RenderOptions());
        QVERIFY(fl.layout);
        for (const auto &ob : fl.layout->inlineObjects()) {
            QVERIFY(std::isfinite(ob.rect.left()) && std::isfinite(ob.rect.top()) && std::isfinite(ob.rect.width()) && std::isfinite(ob.rect.height()));
            QVERIFY(std::abs(ob.rect.top()) < 1e6 && ob.rect.width() >= 0 && ob.rect.height() >= 0);
        }
        for (const auto &li : fl.layout->lineInfo(0)) QVERIFY(std::isfinite(li.baseline) && std::abs(li.baseline) < 1e6);
        // Drawing ends (the box in its own text is drawn, but not inside itself).
        QImage img(612, 792, QImage::Format_ARGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        PaintContext ctx;
        ctx.doc = doc.get();
        ctx.cache = &cache;
        Renderer::paintItem(&p, ctx, *t);
        p.end();
        // Saving ends too.
        QTemporaryDir dir;
        QString err;
        QVERIFY2(exportPublisher(*doc, dir.filePath(QStringLiteral("self.pub")), &err), qPrintable(err));
    }

    // Many hyperlinks in one story (a link directory) open in moments and
    // all come back: Qt finished each insertion on its own (6,000 links took
    // 10 seconds), and the reader dropped every address past 10,000.
    void pubManyLinksOpenQuickly()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 400, 600);
        t->storyId = doc->createStory(QString());
        const int links = 12000;
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.beginEditBlock();
            for (int i = 0; i < links; ++i) {
                QTextCharFormat lf;
                lf.setAnchor(true);
                lf.setAnchorHref(QStringLiteral("https://example.com/%1").arg(i));
                c.insertText(QStringLiteral("site%1").arg(i), lf);
                c.insertText(QStringLiteral(" "), QTextCharFormat());
            }
            c.endEditBlock();
        }
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("links.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QElapsedTimer timer;
        timer.start();
        auto back = importPublisherFile(path, &err);
        const qint64 ms = timer.elapsed();
        QVERIFY(back);
        QSet<QString> found;
        for (const auto &st : back->stories)
            for (QTextBlock b = st->doc->begin(); b.isValid(); b = b.next())
                for (auto it = b.begin(); !it.atEnd(); ++it)
                    if (it.fragment().charFormat().isAnchor()) found << it.fragment().charFormat().anchorHref();
        QCOMPARE(found.size(), links);
        QVERIFY(found.contains(QStringLiteral("https://example.com/11999")));
        QVERIFY2(ms < 15000, qPrintable(QStringLiteral("%1 ms").arg(ms)));   // was over 40 s; a sanitizer build takes 5
        // And as .jpub, reopened just as quickly.
        const QString jpub = dir.filePath(QStringLiteral("links.jpub"));
        QVERIFY2(savePublication(*back, jpub, QImage(), &err), qPrintable(err));
        timer.restart();
        QVERIFY(loadPublication(jpub, &err));
        QVERIFY2(timer.elapsed() < 15000, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));
    }

    // Fields and hyperlinks are saved as Publisher saves them: each story's
    // TOKN section lists them (page numbers, dates and times in Publisher's
    // 17 formats, hyperlinks with their address) and they come back as
    // fields and links. Publisher counted the same fields in JeffPub's copies
    // of its samples as in its own files, and pictured them the same (Oct 7).
    void pubFieldsAndLinksRoundTrip()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto box = [&](const QString &text, const QString &field, double y) {
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(72, y, 300, 30);
            t->storyId = doc->createStory(text);
            if (!field.isEmpty()) {
                QTextCursor c(doc->storyDoc(t->storyId));
                c.movePosition(QTextCursor::End);
                QTextCharFormat ff;
                ff.setProperty(tp::Field, field);
                c.insertText(QString(QChar::ObjectReplacementCharacter), ff);
            }
            doc->pages[0]->items.push_back(t);
            return t;
        };
        box(QStringLiteral("f0 X"), QStringLiteral("page"), 72);
        box(QStringLiteral("Next "), QStringLiteral("page:next"), 112);
        box(QStringLiteral("Prev "), QStringLiteral("page:prev"), 152);
        box(QStringLiteral("Day "), QStringLiteral("datetime:MMMM d, yyyy"), 192);
        box(QStringLiteral("Clock "), QStringLiteral("datetime:h:mm:ss AP"), 232);
        auto linked = box(QStringLiteral("Visit "), QString(), 272);
        {
            QTextCursor c(doc->storyDoc(linked->storyId));
            c.movePosition(QTextCursor::End);
            QTextCharFormat lf;
            lf.setAnchor(true);
            lf.setAnchorHref(QStringLiteral("https://example.com/page"));
            c.insertText(QStringLiteral("example site"), lf);
            c.insertText(QStringLiteral(" now"), QTextCharFormat());
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("fields.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray q = cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        // A page number's section, byte for byte as Publisher wrote one for
        // "f0 X#", then the empty payload header just past it (without it
        // Publisher refused the file).
        const QByteArray page = QByteArray::fromHex("010000000c000000ffff01000400000006000000160000000022000000000122010000000222fbffffff0a0000000022ffffffff"
                                                    "00000000000000000eca59d6");
        QVERIFY(q.contains(page));
        // A date: its kind (1), format number (3), language and format.
        QVERIFY(q.contains(QByteArray::fromHex("10000100030009042000") + QByteArray(reinterpret_cast<const char *>(u"MMMM d, yyyy"), 24)));
        // A time: kind 12, number 15, "am/pm" for AP.
        QVERIFY(q.contains(QByteArray::fromHex("11000c000f0009042000") + QByteArray(reinterpret_cast<const char *>(u"h:mm:ss am/pm"), 26)));
        // The hyperlink: flags 0x8C0, its 12 letters, kind 1; its address.
        QVERIFY(q.contains(QByteArray::fromHex("160000000022c00800000122") + QByteArray::fromHex("0c0000000222") + QByteArray::fromHex("01000000")));
        QVERIFY(q.contains(QByteArray::fromHex("1800") + QByteArray(reinterpret_cast<const char *>(u"https://example.com/page"), 48)));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QStringList fields, links;
        for (const auto &it : back->pages[0]->items) {
            auto *t = dynamic_cast<TextItem *>(it.get());
            QVERIFY(t);
            for (QTextBlock b = back->storyDoc(t->storyId)->begin(); b.isValid(); b = b.next())
                for (auto f = b.begin(); !f.atEnd(); ++f) {
                    const QTextCharFormat cf = f.fragment().charFormat();
                    if (!cf.stringProperty(tp::Field).isEmpty()) fields << cf.stringProperty(tp::Field);
                    if (cf.isAnchor()) links << cf.anchorHref() + QLatin1Char('=') + f.fragment().text();
                }
        }
        QCOMPARE(fields, (QStringList{"page", "page:next", "page:prev", "datetime:MMMM d, yyyy", "datetime:h:mm:ss AP"}));
        QCOMPARE(links, QStringList{QStringLiteral("https://example.com/page=example site")});

        // A damaged TOKN section (its entry and count claim more than the
        // stream holds) loses its fields, never the publication's text.
        QFile in(path);
        QVERIFY(in.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(in.readAll(), &file));
        in.close();
        QByteArray quill = file.stream(QStringLiteral("Quill/QuillSub/CONTENTS"));
        for (qsizetype entry = quill.indexOf("TOKN") - 2; entry > 0 && entry < 512; entry = quill.indexOf("TOKN", entry + 3) - 2) {
            const quint32 at = qFromLittleEndian<quint32>(quill.constData() + entry + 16);
            qToLittleEndian<quint32>(0x00ffffff, quill.data() + entry + 20);
            qToLittleEndian<quint32>(50000, quill.data() + at);
        }
        QVERIFY(file.setStream(QStringLiteral("Quill/QuillSub/CONTENTS"), quill));
        const QString damaged = dir.filePath(QStringLiteral("damaged.pub"));
        QFile out(damaged);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(cfb::write(file));
        out.close();
        auto rescued = importPublisherFile(damaged, &err);
        QVERIFY2(rescued, qPrintable(err));
        QCOMPARE(int(rescued->pages[0]->items.size()), 6);
    }

    // Publisher's date formats are Windows' letters; Qt's differ only in
    // AM/PM and the one- and three-letter years.
    void pubDateFormats()
    {
        QCOMPARE(jp::pubDateFormat(QStringLiteral(" dddd, MMMM d, yyyy")), QStringLiteral("dddd, MMMM d, yyyy"));
        QCOMPARE(jp::pubDateFormat(QStringLiteral(" M/d/yyyy h:mm:ss am/pm")), QStringLiteral("M/d/yyyy h:mm:ss AP"));
        QCOMPARE(jp::pubDateFormat(QStringLiteral("h:mm tt")), QStringLiteral("h:mm AP"));
        QCOMPARE(jp::pubDateFormat(QStringLiteral("d-MMM-y")), QStringLiteral("d-MMM-yy"));
        QCOMPARE(jp::pubDateFormat(QStringLiteral("yyy")), QStringLiteral("yyyy"));
        // Every one of Publisher's own formats is in JeffPub's Date and Time list.
        for (const char *f : {" M/d/yyyy", " dddd, MMMM d, yyyy", " MMMM d, yyyy", " M/d/yy", " yyyy-MM-dd", " d-MMM-yy", " M.d.yyyy", " MMM. d, yy",
                              " d MMMM yyyy", " MMMM yy", " MMM-yy", " M/d/yyyy h:mm am/pm", " M/d/yyyy h:mm:ss am/pm", " h:mm am/pm", " h:mm:ss am/pm",
                              " HH:mm", " HH:mm:ss"})
            QVERIFY2(jp::dateTimeFormats().contains(jp::pubDateFormat(QString::fromLatin1(f))), f);
    }

    // "Next page" and "previous page" numbers show the page of the next or
    // previous box of a linked chain, and "#" in a box without one, as in
    // Publisher.
    void nextAndPreviousPageNumbers()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        doc->addPage();
        doc->addPage();
        auto a = std::make_shared<TextItem>();
        a->rect = QRectF(72, 72, 300, 100);
        a->storyId = doc->createStory(QStringLiteral("Story"));
        auto b = std::make_shared<TextItem>();
        b->rect = QRectF(72, 72, 300, 100);
        b->storyId = a->storyId;
        a->nextId = b->id;
        doc->pages[0]->items.push_back(a);
        doc->pages[2]->items.push_back(b);
        LayoutCache cache;
        FrameSpec sa = Renderer::frameSpec(*doc, *a, 1, RenderOptions()), sb = Renderer::frameSpec(*doc, *b, 3, RenderOptions());
        QCOMPARE(sa.ctx.resolve(QStringLiteral("page:next")), QStringLiteral("3"));
        QCOMPARE(sb.ctx.resolve(QStringLiteral("page:prev")), QStringLiteral("1"));
        QCOMPARE(sa.ctx.resolve(QStringLiteral("page:prev")), QStringLiteral("#"));
        QCOMPARE(sb.ctx.resolve(QStringLiteral("page:next")), QStringLiteral("#"));
        QCOMPARE(sb.ctx.resolve(QStringLiteral("page")), QStringLiteral("3"));
    }

    // A booklet is saved as Publisher saves one: each master in a right-hand
    // and a left-hand part, odd pages on the right part and even ones on the
    // left, pages after the first marked 0. Publisher read JeffPub's copy of
    // its booklet as its own (layout, spreads, masters) and pictured it the
    // same (Oct 7). On opening, the parts make one two-page master, and every
    // page comes back (pages marked 0 counted as special: one page did).
    // PDF/X-4 with the printer's own color profile: transparency kept,
    // pictures in CMYK with their see-through parts, the profile in the
    // output intent, the PDF/X-4 identification in the metadata.
    // Black overprints in a file for a printer, as the publication's
    // overprinting settings say (Publisher's defaults: text below 24 pt and
    // lines; not fills): the black ink prints over the colors under it,
    // which then show no white edge where the plates don't line up.
    void pdfXOverprint()
    {
        auto doc = Document::blank(QSizeF(400, 300));
        auto addText = [&](const QString &s, double pt, const QColor &c, const QRectF &r) {
            auto t = std::make_shared<TextItem>();
            t->rect = r;
            t->storyId = doc->createStory(s);
            QTextCursor cur(doc->storyDoc(t->storyId));
            cur.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setFontPointSize(pt);
            f.setForeground(c);
            cur.mergeCharFormat(f);
            doc->pages[0]->items.push_back(t);
        };
        auto red = std::make_shared<ShapeItem>();
        red->shape = QStringLiteral("rect");
        red->rect = QRectF(20, 20, 360, 260);
        red->fill = Fill::solid(ColorRef::rgb(QColor(220, 30, 40)));
        red->stroke.color = ColorRef::none();
        doc->pages[0]->items.push_back(red);
        addText(QStringLiteral("small"), 10, Qt::black, QRectF(40, 40, 150, 30));
        addText(QStringLiteral("BIG"), 36, Qt::black, QRectF(40, 90, 200, 60));
        auto line = std::make_shared<LineItem>();
        line->p1 = QPointF(40, 180);
        line->p2 = QPointF(300, 180);
        line->stroke.color = ColorRef::rgb(Qt::black);
        line->stroke.width = 2;
        doc->pages[0]->items.push_back(line);
        auto blackBox = std::make_shared<ShapeItem>();
        blackBox->shape = QStringLiteral("rect");
        blackBox->rect = QRectF(40, 200, 60, 40);
        blackBox->fill = Fill::solid(ColorRef::rgb(Qt::black));
        blackBox->stroke.color = ColorRef::none();
        doc->pages[0]->items.push_back(blackBox);

        // Saved with the publication.
        doc->print.overprint.textBelow = 30;
        {
            Document again;
            again.fromJson(doc->toJson());
            QVERIFY(again.print.overprint == doc->print.overprint);
        }
        doc->print.overprint = OverprintSettings();

        MainWindow w;
        w.editor()->setDocument(std::move(doc));
        Document *d = w.editor()->doc();
        QTemporaryDir dir;
        // The page's drawing, and the PDF still PDF/X that PDFium opens.
        auto drawing = [&](const QString &name) {
            const QString path = dir.filePath(name);
            MainWindow::PdfSettings ps;
            ps.pdfx = true;
            if (!w.exportPdfTo(path, ps)) return QByteArray();
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
                QFile::remove(qEnvironmentVariable("JP_SHOT_DIR") + "/overprint-" + name);
                QFile::copy(path, qEnvironmentVariable("JP_SHOT_DIR") + "/overprint-" + name);
            }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            const QByteArray bytes = f.readAll();
            if (!bytes.startsWith("%PDF-1.3") || !PdfDocument(bytes).isValid()) return QByteArray();
            QtPdf parsed;
            parsed.load(path);
            QByteArray content;
            for (const auto &o : parsed.objects) {
                const QByteArray dict = QtPdf::dictOf(o.body);
                bool ok = false;
                if (QtPdf::isStream(o.body) && dict.contains("/FlateDecode") && !dict.contains("/Length1") && !dict.contains("/Subtype"))
                    content += parsed.streamData(o, &ok);
                if (dict.contains("/Type /ExtGState") && dict.contains("/OP true")) content += "\n%switch " + dict + "\n";
            }
            return content;
        };
        auto between = [](const QByteArray &c, const QByteArray &from, const QByteArray &to) {
            const qsizetype a = c.indexOf(from);
            return a < 0 ? QByteArray() : c.mid(a, c.indexOf(to, a) - a);
        };
        const QByteArray smallText = "/F11 80 Tf", bigText = "/F11 288 Tf", lineStart = "40 180 m", boxStart = "666.666666 3333.33333 m";

        // Publisher's defaults: the 10-point black text and the black line
        // overprint; the 36-point text and the black box knock out.
        QByteArray c = drawing(QStringLiteral("defaults.pdf"));
        QVERIFY(!c.isEmpty());
        QVERIFY2(between(c, smallText, "ET").contains("/JPop2 gs"), c.constData());
        QVERIFY(!between(c, bigText, "ET").contains("/JPop"));
        QVERIFY(c.contains("/JPop1 gs\n" + lineStart));
        QVERIFY(c.contains(boxStart) && !c.mid(c.indexOf(boxStart) - 12, 12).contains("/JPop"));
        QVERIFY(c.contains("/OPM 1"));
        // The colored background never overprints.
        QVERIFY(!c.contains("/JPop3"));

        // Fills too; then a 90% black box counts only from a threshold below it.
        d->print.overprint.fills = true;
        c = drawing(QStringLiteral("fills.pdf"));
        QVERIFY(c.contains("/JPop2 gs\n" + boxStart));
        blackBox->fill = Fill::solid(ColorRef::rgb(QColor(25, 25, 25)));
        c = drawing(QStringLiteral("gray.pdf"));
        QVERIFY(c.contains(boxStart) && !c.mid(c.indexOf(boxStart) - 12, 12).contains("/JPop"));
        d->print.overprint.threshold = 85;
        c = drawing(QStringLiteral("gray85.pdf"));
        QVERIFY(c.contains("/JPop2 gs\n" + boxStart));
        // Text up to 40 points: the big text too.
        d->print.overprint.textBelow = 40;
        c = drawing(QStringLiteral("big.pdf"));
        QVERIFY(between(c, bigText, "ET").contains("/JPop2 gs"));
        // All off: nothing overprints.
        d->print.overprint.text = d->print.overprint.lines = d->print.overprint.fills = false;
        c = drawing(QStringLiteral("off.pdf"));
        QVERIFY(!c.isEmpty() && !c.contains("/JPop"));
    }

    void pdfX4WithOwnProfile()
    {
        const QByteArray icc = testCmykProfile();
        const QColorSpace cs = QColorSpace::fromIccProfile(icc);
        QVERIFY(cs.isValid());
        QCOMPARE(cs.colorModel(), QColorSpace::ColorModel::Cmyk);
        QTemporaryDir dir;
        const QString iccPath = dir.filePath(QStringLiteral("printer.icc"));
        {
            QFile f(iccPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(icc);
        }
        auto doc = Document::blank(QSizeF(400, 300));
        auto shape = std::make_shared<ShapeItem>();
        shape->shape = QStringLiteral("rect");
        shape->rect = QRectF(50, 50, 200, 100);
        shape->fill = Fill::solid(ColorRef::rgb(QColor(0, 0, 255)));
        shape->fill.transparency = 0.5;
        doc->pages[0]->items.push_back(shape);
        QImage img(8, 8, QImage::Format_ARGB32);
        img.fill(QColor(255, 0, 0, 120));
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = doc->addImage(png, QStringLiteral("png"));
        pic->rect = QRectF(200, 120, 100, 100);
        pic->imgRect = QRectF(0, 0, 100, 100);
        doc->pages[0]->items.push_back(pic);

        MainWindow w;
        w.editor()->setDocument(std::move(doc));
        MainWindow::PdfSettings ps;
        ps.pdfx = true;
        for (int i = 0; i < pdfXConditions().size(); ++i)
            if (pdfXConditions()[i].ownProfile) ps.pdfxCondition = i;
        QVERIFY(pdfXConditions().value(ps.pdfxCondition).x4);
        // Without the profile there's no PDF/X-4 (and no file).
        const QString none = dir.filePath(QStringLiteral("none.pdf"));
        QVERIFY(!w.exportPdfTo(none, ps));
        QVERIFY(!QFile::exists(none));
        ps.pdfxProfile = iccPath;
        const QString path = dir.filePath(QStringLiteral("x4.pdf"));
        QVERIFY(w.exportPdfTo(path, ps));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        QVERIFY(bytes.startsWith("%PDF-1.6"));
        QVERIFY(bytes.contains("pdfxid:GTS_PDFXVersion=\"PDF/X-4\""));

        QtPdf parsed;
        QVERIFY(parsed.load(path));
        bool intent = false, profile = false, seeThrough = false, cmykPicture = false, mask = false, trim = false;
        for (const auto &o : parsed.objects) {
            const QByteArray dict = QtPdf::dictOf(o.body);
            QVERIFY2(!dict.contains("/DeviceRGB"), dict.constData());
            if (dict.contains("/Type /OutputIntent")) intent = dict.contains("/S/GTS_PDFX") && dict.contains("/DestOutputProfile");
            if (dict.contains("/N 4") && QtPdf::isStream(o.body)) {
                bool ok = false;
                profile = parsed.streamData(o, &ok) == icc;
            }
            if (QRegularExpression(QStringLiteral("/ca\\s+0\\.[0-9]")).match(QString::fromLatin1(dict)).hasMatch()) seeThrough = true;
            if (dict.contains("/Subtype /Image") && dict.contains("/DeviceCMYK")) {
                cmykPicture = true;
                mask = mask || dict.contains("/SMask");
            }
            trim = trim || (dict.contains("/TrimBox") && dict.contains("/BleedBox"));
        }
        QVERIFY(intent);
        QVERIFY(profile);
        QVERIFY(seeThrough);
        QVERIFY(cmykPicture);
        QVERIFY(mask);
        QVERIFY(trim);
        // It opens, and the blue is half see-through (PDFium draws the page
        // with nothing behind it).
        const PdfDocument out(bytes);
        QVERIFY(out.isValid());
        const QImage page = out.render(0, QSize(400, 300));
        const QRgb mid = page.pixel(100, 100);
        QVERIFY2(qAlpha(mid) > 100 && qAlpha(mid) < 160 && qBlue(mid) > qRed(mid) + 40, qPrintable(QString::number(mid, 16)));
    }

    // ECI's profile, downloaded: used only when the download and the
    // profile in it are the files expected.
    void pdfXProfileDownloadChecked()
    {
        PdfXCondition c;
        c.profileFile = QStringLiteral("Profile.icc");
        const QByteArray icc = testCmykProfile();
        ZipWriter zw;
        zw.add(QStringLiteral("Profile.icc"), icc, true);
        zw.add(QStringLiteral("Profile_info.pdf"), QByteArray("%PDF-1.4"));
        const QByteArray zip = zw.finish();
        c.zipSha256 = QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex();
        c.profileSha256 = QCryptographicHash::hash(icc, QCryptographicHash::Sha256).toHex();
        QString err;
        QCOMPARE(pdfXProfileFromZip(c, zip, &err), icc);
        // A page of HTML (a moved file) or a changed zip: no profile.
        QVERIFY(pdfXProfileFromZip(c, QByteArray("<html>moved</html>"), &err).isEmpty());
        QVERIFY(err.contains(QLatin1String("isn't the file expected")));
        PdfXCondition other = c;
        other.profileSha256 = QByteArray(64, '0');
        QVERIFY(pdfXProfileFromZip(other, zip, &err).isEmpty());
        QVERIFY(err.contains(QLatin1String("profile")));
        // ECI's own: a FOGRA51 condition, checked by hash.
        bool eci = false;
        for (const PdfXCondition &x : pdfXConditions())
            if (x.identifier == QLatin1String("FOGRA51")) eci = x.x4 && x.profileUrl.startsWith(QLatin1String("https://")) && x.zipSha256.size() == 64 && x.profileSha256.size() == 64;
        QVERIFY(eci);
        // With ECI's download at hand (JP_ECI_ZIP), the real thing.
        if (qEnvironmentVariableIsSet("JP_ECI_ZIP")) {
            QFile f(qEnvironmentVariable("JP_ECI_ZIP"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            for (const PdfXCondition &x : pdfXConditions())
                if (x.identifier == QLatin1String("FOGRA51")) {
                    const QByteArray real = pdfXProfileFromZip(x, f.readAll(), &err);
                    QVERIFY2(!real.isEmpty(), qPrintable(err));
                    QCOMPARE(QColorSpace::fromIccProfile(real).colorModel(), QColorSpace::ColorModel::Cmyk);
                }
        }
    }

    // A PDF page placed as a picture: shown from PDFium's raster, exported
    // to PDF as vectors, kept as a PDF in .jpub and as a picture in .pub.
    void pdfPagePlacedAsPicture()
    {
        using namespace jp;
        QByteArray pdf;
        {
            QBuffer buf(&pdf);
            buf.open(QIODevice::WriteOnly);
            QPdfWriter w(&buf);
            w.setPageSize(QPageSize(QSizeF(300, 200), QPageSize::Point));
            w.setPageMargins(QMarginsF(0, 0, 0, 0));
            w.setResolution(72);
            QPainter p(&w);
            p.fillRect(QRectF(0, 0, 300, 200), Qt::blue);
            w.newPage();
            p.fillRect(QRectF(20, 20, 260, 160), QColor(220, 0, 0));
            p.end();
        }
        const QByteArray page2 = PdfDocument(pdf).extractPage(1);
        QVERIFY(!page2.isEmpty());

        auto doc = Document::blank(QSizeF(612, 792));
        const QString id = doc->addImage(page2, QStringLiteral("pdf"));
        const QImage shown = doc->image(id);
        QVERIFY(!shown.isNull());
        QCOMPARE(QColor(shown.pixel(shown.width() / 2, shown.height() / 2)), QColor(220, 0, 0));
        QVERIFY(qAlpha(shown.pixel(2, 2)) == 0);   // no page behind it
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = id;
        pic->rect = QRectF(100, 100, 300, 200);
        pic->imgRect = QRectF(0, 0, 300, 200);
        doc->pages[0]->items.push_back(pic);

        QTemporaryDir dir;
        QString err;
        // .jpub keeps the PDF.
        const QString jpub = dir.filePath(QStringLiteral("pdf.jpub"));
        QVERIFY2(savePublication(*doc, jpub, QImage(), &err), qPrintable(err));
        auto back = loadPublication(jpub, &err);
        QVERIFY(back);
        QCOMPARE(back->images.value(id).format, QStringLiteral("pdf"));
        QCOMPARE(back->images.value(id).bytes, page2);
        // .pub has no PDF pictures: a PNG of it.
        const QString pub = dir.filePath(QStringLiteral("pdf.pub"));
        QVERIFY2(exportPublisher(*doc, pub, &err), qPrintable(err));
        auto fromPub = importPublisherFile(pub, &err);
        QVERIFY(fromPub);
        QCOMPARE(fromPub->images.size(), 1);
        QCOMPARE(fromPub->images.first().format, QStringLiteral("png"));

        // Exported to PDF it's a filled area, not a picture.
        MainWindow w;
        w.editor()->setDocument(std::move(doc));
        const QString path = dir.filePath(QStringLiteral("placed.pdf"));
        MainWindow::PdfSettings ps;
        ps.preset = MainWindow::PdfSettings::HighQuality;
        QVERIFY(w.exportPdfTo(path, ps));
        QtPdf parsed;
        QVERIFY(parsed.load(path));
        for (const auto &o : parsed.objects) QVERIFY(!QtPdf::dictOf(o.body).contains("/Subtype /Image"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const PdfDocument out(f.readAll());
        QVERIFY(out.isValid());
        const QImage page = out.render(0, QSize(612, 792));
        QCOMPARE(QColor(page.pixel(250, 200)), QColor(220, 0, 0));
    }

    // A PDF page drawn as vectors looks like PDFium's own raster of it: text
    // (as glyph outlines), filled and dashed shapes, and a picture.
    void pdfPagePlaysLikeItsRaster()
    {
        QByteArray pdf;
        {
            QBuffer buf(&pdf);
            buf.open(QIODevice::WriteOnly);
            QPdfWriter w(&buf);
            w.setPageSize(QPageSize(QSizeF(200, 100), QPageSize::Point));
            w.setPageMargins(QMarginsF(0, 0, 0, 0));
            w.setResolution(72);
            QPainter p(&w);
            p.fillRect(QRectF(10, 10, 60, 40), QColor(200, 30, 30));
            // (Square caps would give the line's last, empty dash a square
            // in PDF readers but not in Qt; the check catches that, below.)
            p.setPen(QPen(Qt::blue, 3, Qt::DashLine, Qt::FlatCap));
            p.drawLine(QPointF(10, 70), QPointF(190, 70));
            QFont f(QStringLiteral("DejaVu Sans"));
            f.setPixelSize(24);
            p.setFont(f);
            p.setPen(Qt::black);
            p.drawText(QPointF(80, 40), QStringLiteral("Hgx"));
            p.save();
            p.translate(30, 95);
            p.rotate(-20);
            p.drawText(QPointF(0, 0), QStringLiteral("ab"));
            p.restore();
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::green);
            p.drawImage(QRectF(150, 10, 30, 30), img);
            w.newPage();
            p.fillRect(QRectF(0, 0, 50, 50), Qt::yellow);
            p.end();
        }
        jp::PdfDocument doc(pdf);
        QVERIFY(doc.isValid());
        QCOMPARE(doc.pageCount(), 2);
        QCOMPARE(doc.pageSize(0).toSize(), QSize(200, 100));
        const QSize px(800, 400);
        const QImage ref = doc.render(0, px).convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!ref.isNull());
        QImage played(px, QImage::Format_ARGB32);
        played.fill(Qt::transparent);
        {
            QPainter p(&played);
            doc.play(&p, 0, QRectF(QPointF(0, 0), QSizeF(px)));
        }
        // Over white, pixels differing by more than an edge's antialiasing.
        auto over = [](QRgb c) { const int a = qAlpha(c); return QColor(255 - a + qRed(c) * a / 255, 255 - a + qGreen(c) * a / 255, 255 - a + qBlue(c) * a / 255); };
        int differ = 0, inked = 0;
        for (int y = 0; y < px.height(); ++y)
            for (int x = 0; x < px.width(); ++x) {
                const QColor a = over(ref.pixel(x, y)), b = over(played.pixel(x, y));
                if (a != Qt::white) ++inked;
                if (std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) > 120) ++differ;
            }
        if (qEnvironmentVariableIsSet("JP_PDF_DEBUG")) {
            ref.save(qEnvironmentVariable("JP_PDF_DEBUG") + QStringLiteral("/ref.png"));
            played.save(qEnvironmentVariable("JP_PDF_DEBUG") + QStringLiteral("/played.png"));
            QFile out(qEnvironmentVariable("JP_PDF_DEBUG") + QStringLiteral("/t.pdf"));
            if (out.open(QIODevice::WriteOnly)) out.write(pdf);
        }
        QVERIFY(inked > 20000);
        QVERIFY2(differ < inked / 50, qPrintable(QStringLiteral("%1 of %2 inked pixels differ").arg(differ).arg(inked)));
        // The check finds nothing for the raster to draw.
        QCOMPARE(doc.rasterShare(0), 0.0);

        // In SVG the text is paths and only the picture is an image.
        QByteArray svg;
        {
            QBuffer buf(&svg);
            QSvgGenerator gen;
            gen.setOutputDevice(&buf);
            gen.setSize(QSize(200, 100));
            QPainter p(&gen);
            doc.play(&p, 0, QRectF(0, 0, 200, 100));
        }
        QCOMPARE(svg.count("<image"), 1);
        QVERIFY(svg.count("<path") >= 4);

        // One page taken out is a PDF of its own.
        jp::PdfDocument second(doc.extractPage(1));
        QVERIFY(second.isValid());
        QCOMPARE(second.pageCount(), 1);
        QCOMPARE(second.pageSize(0).toSize(), QSize(200, 100));
        QVERIFY(jp::PdfDocument::looksLikePdf(doc.extractPage(0)));
        QVERIFY(!jp::PdfDocument(QByteArray("not a pdf")).isValid());
        QVERIFY(doc.extractPage(5).isEmpty());
        // Cut short or scrambled, it's drawn as far as it goes, or not at all.
        for (const QByteArray &bad : {pdf.left(pdf.size() / 2), QByteArray(pdf).replace("obj", "jbo"), QByteArray("%PDF-1.4\n%%EOF")}) {
            jp::PdfDocument d(bad);
            QImage img(200, 100, QImage::Format_ARGB32_Premultiplied);
            QPainter p(&img);
            for (int i = -1; i <= d.pageCount(); ++i) {
                d.render(i, QSize(200, 100));
                d.play(&p, i, QRectF(0, 0, 200, 100));
                d.extractPage(i);
                QVERIFY(d.rasterShare(i) >= 0);
            }
        }
    }

    void pubBookletRoundTrip()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(396, 612));
        doc->setup.layout = PageSetup::Booklet;
        doc->setup.sheet = QSizeF(792, 612);
        for (int i = 1; i < 4; ++i) doc->addPage();
        auto mark = std::make_shared<ShapeItem>();
        mark->rect = QRectF(36, 36, 50, 50);
        mark->fill = Fill::solid(ColorRef::rgb(Qt::blue));
        doc->masters[0]->items.push_back(mark);
        for (int i = 0; i < 4; ++i) {
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(72, 300, 200, 40);
            t->storyId = doc->createStory(QStringLiteral("Page %1").arg(i + 1));
            doc->pages[i]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("booklet.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(int(back->setup.layout), int(PageSetup::Booklet));
        QCOMPARE(int(back->pages.size()), 4);
        QCOMPARE(int(back->masters.size()), 1);
        QVERIFY(back->masters[0]->twoPage);
        for (const auto &pg : back->pages) QCOMPARE(pg->masterId, back->masters[0]->id);
        // The master's mark on both halves of the spread.
        QCOMPARE(int(back->masters[0]->items.size()), 2);
        QList<double> xs;
        for (const auto &it : back->masters[0]->items) xs << it->rect.left();
        std::sort(xs.begin(), xs.end());
        QVERIFY2(std::abs(xs[0] - 36) < 0.5 && std::abs(xs[1] - (396 + 36)) < 0.5, qPrintable(QStringLiteral("%1 %2").arg(xs[0]).arg(xs[1])));
        // Each page keeps its own text.
        for (int i = 0; i < 4; ++i) {
            auto *t = dynamic_cast<TextItem *>(back->pages[i]->items.front().get());
            QVERIFY(t);
            QCOMPARE(back->storyDoc(t->storyId)->toPlainText(), QStringLiteral("Page %1").arg(i + 1));
        }
    }

    // A .pub page number field is a "#" whose character run has 00 = 5 (low
    // byte) and 22 = -1, as Publisher writes it; JeffPub showed the "#".
    void pubPageNumberField()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        doc->addPage();
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 200, 40);
        t->storyId = doc->createStory(QStringLiteral("Page "));
        QTextCursor c(doc->storyDoc(t->storyId));
        c.movePosition(QTextCursor::End);
        QTextCharFormat ff;
        ff.setProperty(tp::Field, QStringLiteral("page"));
        c.insertText(QString(QChar::ObjectReplacementCharacter), ff);
        doc->masters[0]->items.push_back(t);   // on every page
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("pn.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray q = cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        // The story text holds "#" where the number goes (UTF-16LE).
        QVERIFY(q.contains(QByteArray(reinterpret_cast<const char *>(u"Page #"), 12)));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QVERIFY(!back->masters.isEmpty() && !back->masters[0]->items.empty());
        auto *bt = dynamic_cast<TextItem *>(back->masters[0]->items[0].get());
        QVERIFY(bt);
        QTextCursor bc(back->storyDoc(bt->storyId));
        bc.movePosition(QTextCursor::End);   // charFormat() is the character before
        QCOMPARE(bc.charFormat().stringProperty(tp::Field), QStringLiteral("page"));
        QCOMPARE(back->storyDoc(bt->storyId)->toPlainText(), QStringLiteral("Page ") + QChar(QChar::ObjectReplacementCharacter));
    }

    // A .pub story marks an object set in its text with U+FFFC. Publisher
    // shows nothing there when the object doesn't come through; JeffPub
    // drew the font's "OBJ" box.
    void pubInlineObjectMarks()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("Before") + QChar(QChar::ObjectReplacementCharacter) + QStringLiteral("after"));
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("obj.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        auto *bt = dynamic_cast<TextItem *>(back->pages[0]->items[0].get());
        QVERIFY(bt);
        QCOMPARE(back->storyDoc(bt->storyId)->toPlainText(), QStringLiteral("Beforeafter"));
    }

    // In Publisher 2000 and 98 files the objects on the scratch area are
    // under a page chunk without a canvas size; it's not a page (Publisher
    // shows the samples with 2 pages; JeffPub showed an empty third).
    void pub2000ScratchIsNotAPage()
    {
        for (const char *name : {"poi-Sample2000.pub", "poi-Sample98.pub"}) {
            QString err;
            auto doc = jp::importPublisherFile(QStringLiteral(JP_TEST_DATA "/pub/") + QLatin1String(name), &err);
            QVERIFY2(doc, qPrintable(err));
            QCOMPARE(int(doc->pages.size()), 2);
            for (const auto &pg : doc->pages) QVERIFY(!pg->items.empty());
        }
    }

    // Publisher 2000 and 98 files give a run's font as a number in each
    // slot of its font container (not a container per slot, as later
    // versions do). The sample's second box says it's in Arial, and
    // Publisher shows it in Arial.
    void pub2000RunFonts()
    {
        for (const char *name : {"poi-Sample2000.pub", "poi-Sample98.pub"}) {
            jp::PubImportReport rep;
            QString err;
            auto doc = jp::importPublisherFile(QStringLiteral(JP_TEST_DATA "/pub/") + QLatin1String(name), &err, &rep);
            QVERIFY2(doc, qPrintable(err));
            QVERIFY2(rep.fontsUsed.contains(QStringLiteral("Arial")), qPrintable(QLatin1String(name) + rep.fontsUsed.join(QLatin1Char(','))));
            QString arialText;
            for (const auto &it : doc->pages[0]->items)
                if (auto *t = dynamic_cast<jp::TextItem *>(it.get())) {
                    QTextCursor c(doc->storyDoc(t->storyId));
                    c.movePosition(QTextCursor::NextCharacter);
                    if (c.charFormat().fontFamilies().toStringList().value(0) == QLatin1String("Arial")) arialText = doc->storyDoc(t->storyId)->toPlainText();
                }
            QVERIFY2(arialText.contains(QStringLiteral("Arial, 20 point")), qPrintable(arialText));
        }
    }

    // AutoFit Text in .pub lives in the story's record: 05 = 1 for best fit,
    // 3 for shrink text on overflow, and a 0c flag for grow the box, as
    // Publisher writes them when each setting is chosen. A box without one
    // keeps its overflowing text hidden, as Publisher shows it.
    void pubAutofitSetting()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        const TextItem::Autofit fits[] = {TextItem::NoAutofit, TextItem::BestFit, TextItem::ShrinkOnOverflow, TextItem::GrowBox};
        for (int i = 0; i < 4; ++i) {
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(72, 72 + 150 * i, 200, 60);
            t->storyId = doc->createStory(QStringLiteral("The quick brown fox jumps over the lazy dog. ").repeated(12));
            t->autofit = fits[i];
            doc->pages[0]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("fit.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray contents = cfb::readStream(path, QStringLiteral("Contents"));
        QVERIFY(contents.contains(QByteArray("\x05\x10\x01\x00\x07\x20", 6)));
        QVERIFY(contents.contains(QByteArray("\x05\x10\x03\x00\x07\x20", 6)));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QMap<int, int> got;
        for (const auto &it : back->pages[0]->items)
            if (auto *t = dynamic_cast<TextItem *>(it.get())) got[qRound(t->rect.top())] = t->autofit;
        QCOMPARE(got.size(), 4);
        for (int i = 0; i < 4; ++i) QCOMPARE(got.value(72 + 150 * i, -1), int(fits[i]));
    }

    // Fonts in .pub: JeffPub's look-alikes (Tinos) go under the standard
    // font's name (Times New Roman), unless the publication came from a .pub
    // that named the look-alike itself; then the name it had is kept.
    void pubKeepsFontsItCameWith()
    {
        using namespace jp;
        auto make = [](const QStringList &pubFonts) {
            auto doc = Document::blank(QSizeF(612, 792));
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(72, 72, 400, 100);
            t->storyId = doc->createStory(QStringLiteral("Set in Tinos"));
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setFontFamilies(QStringList{QStringLiteral("Tinos")});
            c.mergeCharFormat(f);
            doc->pages[0]->items.push_back(t);
            doc->pubFonts = pubFonts;
            return doc;
        };
        QTemporaryDir dir;
        auto fontAfter = [&](const Document &d) {
            const QString path = dir.filePath(QStringLiteral("fonts.pub"));
            QString err;
            if (!exportPublisher(d, path, &err)) return QStringLiteral("save failed: ") + err;
            PubImportReport rep;
            auto back = importPublisherFile(path, &err, &rep);
            if (!back) return QStringLiteral("open failed: ") + err;
            return rep.fontsUsed.join(QLatin1Char(','));
        };
        QCOMPARE(fontAfter(*make({})), QStringLiteral("Times New Roman"));
        QCOMPARE(fontAfter(*make({QStringLiteral("Tinos")})), QStringLiteral("Tinos"));
        // Opening a .pub remembers its fonts, and .jpub keeps the list.
        auto fromPub = make({QStringLiteral("Tinos")});
        const QString pubPath = dir.filePath(QStringLiteral("orig.pub"));
        QString err;
        QVERIFY(exportPublisher(*fromPub, pubPath, &err));
        auto opened = importPublisherFile(pubPath, &err);
        QVERIFY(opened && opened->pubFonts.contains(QStringLiteral("Tinos")));
        const QString jpub = dir.filePath(QStringLiteral("kept.jpub"));
        QVERIFY(savePublication(*opened, jpub, QImage(), &err));
        auto reloaded = loadPublication(jpub, &err);
        QVERIFY(reloaded && reloaded->pubFonts == opened->pubFonts);
    }

    // Publisher's single line height for fonts JeffPub draws with stand-ins,
    // in ems: each checked line by line against Publisher's own PDFs of 284
    // publications (Oct 7). The stand-ins' own tables were up to 0.4 em off
    // (Agency FB), and Arial Narrow and Bookman Old Style were read with
    // the wrong set of metrics.
    void publisherLineHeights()
    {
        using namespace jp;
        struct Case { const char *family; bool bold; double em; };
        const Case cases[] = {
            {"Arial Rounded MT Bold", true, (1938.0 + 432) / 2048},   // its 26 pt lines 30.06 pt apart
            {"Verdana", false, (1566.0 + 423 + 202) / 2048},
            {"Comic Sans MS", false, (1638.0 + 564) / 2048},
            {"Bookman Old Style", false, (1929.0 + 475) / 2048},
            {"Arial Narrow", false, (1916.0 + 434) / 2048},
            {"Times New Roman", true, (1387.0 + 442 + 307) / 2048},
            {"Times New Roman", false, (1420.0 + 442 + 307) / 2048},
            {"Agency FB", true, (2042.0 + 410) / 2048},
        };
        for (const Case &c : cases) {
            QFont f(QString::fromLatin1(c.family));
            f.setFamilies({QString::fromLatin1(c.family)});
            f.setBold(c.bold);
            f.setPointSizeF(12);
            const double em = naturalLineEm(f, QString::fromLatin1(c.family));
            QVERIFY2(std::abs(em - c.em) < 1e-6, qPrintable(QStringLiteral("%1: %2, not %3").arg(QLatin1String(c.family)).arg(em).arg(c.em)));
        }
    }

    // Line spacing of the fonts JeffPub knows Publisher's metrics for doesn't
    // depend on whether the font is installed: with the real Gill Sans MT on
    // Windows, lines came out 8% tighter than Publisher's. Here a bundled
    // font renamed "Arial" stands in for an installed Arial.
    void knownLineMetricsWhenInstalled()
    {
        using namespace jp;
        const double notInstalled = naturalLineEm(QFont(QStringLiteral("Arial")), QStringLiteral("Arial"));
        QFile f(QStringLiteral(JP_TEST_DATA "/../../resources/fonts/Arimo-Regular.ttf"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray font = f.readAll();
        // Same length, so the font's tables stay where they are.
        font.replace(QByteArray("\x00" "A\x00r\x00i\x00m\x00o", 10), QByteArray("\x00" "A\x00r\x00i\x00" "a\x00l", 10));
        font.replace(QByteArray("Arimo"), QByteArray("Arial"));
        const int id = QFontDatabase::addApplicationFontFromData(font);
        QVERIFY(id >= 0);
        QVERIFY(QFontDatabase::applicationFontFamilies(id).contains(QStringLiteral("Arial")));
        QFont arial(QStringLiteral("Arial"));
        arial.setPointSizeF(10);
        const double installed = naturalLineEm(arial, QStringLiteral("Arial"));
        QFontDatabase::removeApplicationFont(id);
        QVERIFY2(std::abs(installed - notInstalled) < 1e-6, qPrintable(QStringLiteral("%1 vs %2").arg(installed).arg(notInstalled)));
        QVERIFY(std::abs(notInstalled - (1491.0 + 431 + 307) / 2048) < 1e-6);
    }

    // A run's fonts are kept per script (slots in container 0x24); slot 0 is
    // the one for Latin text, as Publisher writes it. A run with fonts for
    // other scripts only (older newsletters: Courier New in slots 3-7,
    // Sendnya in slot 0x0c) takes its Latin font from its style, as
    // Publisher shows it, not the first slot's font.
    void pubLatinFontSlot()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 400, 100);
        // The file's first font (Arial) is the one a run without a Latin
        // font falls back to.
        t->storyId = doc->createStory(QStringLiteral("Set in Arial\nTyped in Courier New"));
        QTextCursor c(doc->storyDoc(t->storyId));
        c.select(QTextCursor::Document);
        QTextCharFormat f;
        f.setFontFamilies(QStringList{QStringLiteral("Arial")});
        c.mergeCharFormat(f);
        c.movePosition(QTextCursor::End);
        c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        f.setFontFamilies(QStringList{QStringLiteral("Courier New")});
        c.mergeCharFormat(f);
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("slots.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        PubImportReport rep;
        QVERIFY(importPublisherFile(path, &err, &rep));
        QVERIFY(rep.fontsUsed.contains(QStringLiteral("Courier New")));
        // Move the Latin and two other slots to slots Publisher uses for other
        // scripts: 00 -> 06, 01 -> 07, 02 -> 0c.
        QFile in(path);
        QVERIFY(in.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(in.readAll(), &file));
        in.close();
        QByteArray q = file.stream(QStringLiteral("Quill/QuillSub/CONTENTS"));
        const QByteArray tail("\x88\x08\x00\x00\x00\x00\x18", 7);
        int moved = 0;
        for (qsizetype at = q.indexOf(tail); at > 0; at = q.indexOf(tail, at + 1)) {
            const char slot = q[at - 1];
            if (slot == 0 || slot == 1 || slot == 2) {
                q[at - 1] = slot == 0 ? 6 : slot == 1 ? 7 : 0x0c;
                ++moved;
            }
        }
        QVERIFY(moved >= 3);
        QVERIFY(file.setStream(QStringLiteral("Quill/QuillSub/CONTENTS"), q));
        const QString patched = dir.filePath(QStringLiteral("patched.pub"));
        QFile out(patched);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(cfb::write(file));
        out.close();
        PubImportReport rep2;
        auto back = importPublisherFile(patched, &err, &rep2);
        QVERIFY2(back, qPrintable(err));
        QVERIFY2(!rep2.fontsUsed.contains(QStringLiteral("Courier New")), qPrintable(rep2.fontsUsed.join(',')));
    }

    // Footnotes at the bottom of the column their reference lands in (under
    // a rule, numbered in order, the text kept above them); a line whose
    // note won't fit moves on with it; endnotes under "Notes" after the
    // story; editing, undo, numbers outside layout, and saving.
    void footnotesAndEndnotes()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto a = std::make_shared<jp::TextItem>();
        a->rect = QRectF(72, 72, 300, 200);
        a->storyId = doc->createStory();
        auto b2 = std::make_shared<jp::TextItem>();
        b2->rect = QRectF(72, 400, 300, 300);
        b2->storyId = a->storyId;
        a->nextId = b2->id;
        doc->pages[0]->items.push_back(a);
        doc->pages[0]->items.push_back(b2);
        QTextDocument *sd = doc->storyDoc(a->storyId);
        {
            QTextCursor c(sd);
            for (int i = 0; i < 12; ++i) {
                if (i) c.insertBlock();
                c.insertText(QStringLiteral("Paragraph %1 with enough words to fill a line of the box.").arg(i + 1));
            }
        }
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        auto at = [&](int block, bool end) {
            ed->beginTextEdit(a->id);
            QTextCursor c(sd->findBlockByNumber(block));
            c.movePosition(QTextCursor::EndOfBlock);
            ed->setCursor(c);
            return end;
        };
        at(0, false);
        const QString n1 = jp::addNote(ed, false, QStringLiteral("The first note."));
        at(1, false);
        const QString n2 = jp::addNote(ed, false, QStringLiteral("The second note, long enough to take two lines in a box of this width."));
        at(11, true);
        const QString e1 = jp::addNote(ed, true, QStringLiteral("An endnote."));
        QVERIFY(!n1.isEmpty() && !n2.isEmpty() && !e1.isEmpty());
        // The reference is the number, superscript.
        QCOMPARE(sd->findBlockByNumber(0).text().right(1), QString(QChar::ObjectReplacementCharacter));
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        auto lay = [&] { return cache.textFrame(*ed->doc(), *a, 1, opt); };
        auto fl = lay();
        QVERIFY(fl.layout);
        const auto &notes = fl.layout->notes();
        QCOMPARE(notes.size(), 3);
        QCOMPARE(notes[0].number, 1);
        QCOMPARE(notes[1].number, 2);
        QVERIFY(!notes[0].endnote && notes[2].endnote && notes[2].number == 1);
        // Both footnotes at the bottom of the first box, in order; no line
        // of text runs into them.
        QCOMPARE(notes[0].frame, 0);
        QCOMPARE(notes[1].frame, 0);
        QVERIFY(notes[0].rect.bottom() <= notes[1].rect.top() + 0.01);
        QVERIFY2(std::abs(notes[1].rect.bottom() + 2 - (200 - a->insets.bottom())) < 1.5, qPrintable(QString::number(notes[1].rect.bottom())));
        for (const QRectF &r : fl.layout->lineRects(0)) QVERIFY(r.bottom() <= notes[0].rect.top());
        QVERIFY(fl.layout->lineInfo(0).first().text.contains(QLatin1Char('1')));
        // The endnote after the story, in the second box.
        QCOMPARE(notes[2].frame, 1);
        double lastLine = 0;
        for (const QRectF &r : fl.layout->lineRects(1)) lastLine = std::max(lastLine, r.bottom());
        QVERIFY(notes[2].rect.top() > lastLine);
        // Numbers outside layout (exports).
        jp::FieldContext ctx;
        ctx.doc = ed->doc();
        QCOMPARE(ctx.resolve(QStringLiteral("footnote:") + n2), QStringLiteral("2"));
        QCOMPARE(ctx.resolve(QStringLiteral("endnote:") + e1), QStringLiteral("1"));
        // A long note that can't fit with its line: both move to the next box.
        at(5, false);
        const QString big = jp::addNote(ed, false, QStringLiteral("A very long note. ").repeated(30));
        fl = lay();
        int bigIdx = -1;
        for (int i = 0; i < fl.layout->notes().size(); ++i)
            if (fl.layout->notes()[i].storyId == big) bigIdx = i;
        QVERIFY(bigIdx >= 0);
        QCOMPARE(fl.layout->notes()[bigIdx].frame, 1);

        // The line with its number went too (the paragraph's earlier lines stay).
        bool refInSecond = false;
        const int para6 = sd->findBlockByNumber(5).position();
        for (const auto &li : fl.layout->lineInfo(1)) refInSecond |= li.docStart == para6 && li.text.contains(QLatin1Char('3'));
        QVERIFY(refInSecond);
        // Editing: at the reference, the note's own text.
        at(0, false);
        bool isEnd = true;
        QCOMPARE(jp::noteAtCursor(ed, &isEnd), n1);
        QVERIFY(!isEnd);
        jp::setNoteText(ed, n1, QStringLiteral("Changed."));
        QCOMPARE(ed->doc()->storyDoc(n1)->toPlainText(), QStringLiteral("Changed."));
        ed->undo();
        QCOMPARE(ed->doc()->storyDoc(n1)->toPlainText(), QStringLiteral("The first note."));
        // Saved and opened again: the same notes.
        QString err;
        auto back = jp::publicationFromBytes(jp::publicationBytes(*ed->doc(), QImage()), &err);
        QVERIFY2(back, qPrintable(err));
        jp::TextItem *ba = nullptr;
        for (const auto &it : back->pages[0]->items)
            if (it->id == a->id) ba = static_cast<jp::TextItem *>(it.get());
        QVERIFY(ba);
        jp::LayoutCache cache2;
        const auto fl2 = cache2.textFrame(*back, *ba, 1, opt);
        QCOMPARE(fl2.layout->notes().size(), 4);
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            ed->endTextEdit();
            w.resize(1200, 900);
            w.show();
            QTest::qWait(100);
            w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/notes.png");
        }
    }

    // About: the third-party table fits its card at a modest window size
    // (its last rows and long license names were cut off).
    void aboutLicensesFit()
    {
        for (const QSize size : {QSize(1000, 700), QSize(1400, 900)}) {
            jp::MainWindow w;
            w.resize(size);
            w.show();
            w.showBackstage(QStringLiteral("about"));
            QTest::qWait(100);   // the scroll area lays out in steps
            QLabel *table = nullptr;
            for (QLabel *l : w.findChildren<QLabel *>())
                if (l->isVisibleTo(&w) && l->text().contains(QLatin1String("<table")) && l->text().contains(QLatin1String("ISBN range table"))) table = l;
            QVERIFY(table);
            // A copy of the label's own document (its wrapping and margin),
            // laid out at the label's width.
            QTextDocument *own = table->findChild<QTextDocument *>();
            QVERIFY(own);
            std::unique_ptr<QTextDocument> copy(own->clone());
            QTextDocument &doc = *copy;
            doc.setDefaultFont(own->defaultFont());
            doc.setDefaultTextOption(own->defaultTextOption());
            doc.setDocumentMargin(own->documentMargin());
            doc.setTextWidth(table->contentsRect().width());
            QVERIFY2(doc.idealWidth() <= table->contentsRect().width() + 1,
                     qPrintable(QStringLiteral("%1 > %2").arg(doc.idealWidth()).arg(table->contentsRect().width())));
            QVERIFY2(doc.size().height() <= table->contentsRect().height() + 1,
                     qPrintable(QStringLiteral("%1 > %2 at %3").arg(doc.size().height()).arg(table->contentsRect().height()).arg(size.width())));
            for (QLabel *l : table->window()->findChildren<QLabel *>())
                if (l->isVisibleTo(&w) && l->wordWrap() && l->heightForWidth(l->width()) > 0)
                    QVERIFY2(l->height() >= l->heightForWidth(l->width()), qPrintable(l->text().left(40)));
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR") && size.width() == 1000) {
                table->window()->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/about.png");
                table->parentWidget()->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/about-licenses.png");
            }
        }
    }

    // Hyphenation in each bundled language matches LibreOffice's (libhyphen
    // 2.8 with the same pattern files, at least two letters each side),
    // including German compounds split in two stages and words with
    // apostrophes and hyphens. Checked on 26,700 dictionary words; these are
    // a sample.
    void hyphenationByLanguage()
    {
        const struct { const char *lang, *word, *points; } cases[] = {
            {"", "hyphenation", "2,6"},
            {"en-US", "kettledrum", "3,6"},
            {"en-GB", "kettledrum", "3,6"},
            {"es-MX", "constitucional", "4,6,8,11"},
            {"fr-FR", "démystificateur", "2,5,7,9,11"},
            {"de-DE", "Senkungsbewegung", "3,8,10,12"},
            {"de-DE", "reservierungsnummer", "2,5,8,13,16"},
            {"de-DE", "premierentermin", "3,5,6,9,12"},
            {"de-AT", "Biographieforschung", "3,6,10,13"},
            {"it-IT", "scalpare", "4"},
            {"nl-NL", "zeeëgelsoorten", "3,4,7,11"},
            {"pt-BR", "latinolátrico", "2,4,6,11"},
            {"pt-PT", "exemplarismo", "5,7,10"},
            {"en-US", "children's", "4"},
            // English breaks with two letters after the hyphen, as Publisher's
            // own layout does (nev-er, locat-ed: 37 of 494 breaks it made).
            {"en-US", "never", "3"},
            {"en-US", "located", "2,5"},
            {"en-US", "recently", "2,6"},
            {"fr-FR", "aujourd'hui", "2,6"},
            {"ja-JP", "hyphenation", ""},
        };
        for (const auto &c : cases) {
            QStringList got;
            for (int p : jp::hyphenationPoints(QString::fromUtf8(c.word), QString::fromLatin1(c.lang))) got << QString::number(p);
            QCOMPARE(got.join(','), QString::fromLatin1(c.points));
        }
        // Layout breaks a word with its own language's patterns.
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 130, 200);
        t->storyId = doc->createStory(QStringLiteral("Wir planen die Reservierungsnummer."));
        doc->pages[0]->items.push_back(t);
        auto firstLine = [&](const QString &lang) {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setProperty(jp::tp::Language, lang);
            c.mergeCharFormat(f);
            jp::LayoutCache cache;
            jp::RenderOptions opt;
            const auto fl = cache.textFrame(*doc, *t, 1, opt);
            return fl.layout ? fl.layout->lineInfo(0).value(0).text.remove(QChar(0x00AD)) : QString();
        };
        // German breaks it (Reser-vie-rungs-num-mer); as English it's an
        // unknown word, and Japanese has no patterns, so it stays whole.
        const QString de = firstLine(QStringLiteral("de-DE"));
        QVERIFY2(QRegularExpression(QStringLiteral("Reser(vie(rungs(num)?)?)?$")).match(de).hasMatch(), qPrintable(de));
        for (const char *other : {"en-US", "ja-JP"}) {
            const QString line = firstLine(QString::fromLatin1(other));
            QVERIFY2(!line.contains(QStringLiteral("Reser")), qPrintable(line));
        }
    }

    // Which bundled dictionary text in a language uses.
    void dictionaryMatch()
    {
        // Windows: a folder whose name doesn't fit the system's code page
        // still opens (spelling was silently off for such user names).
#ifdef Q_OS_WIN
        {
            QTemporaryDir tmp;
            const QString odd = tmp.filePath(QString::fromUtf8("\u5b57\u5178 \u03a9"));
            QVERIFY(QDir().mkpath(odd));
            QFile f(odd + QStringLiteral("/x.dic"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("1\nword\n");
            f.close();
            const QByteArray name = jp::dict::hunspellFileName(f.fileName());
            QVERIFY(QFile::exists(QFile::decodeName(name)));
        }
#endif
        QCOMPARE(jp::dict::match(QString()), QStringLiteral("en-US"));
        QCOMPARE(jp::dict::match(QStringLiteral("en-US")), QStringLiteral("en-US"));
        QCOMPARE(jp::dict::match(QStringLiteral("en_gb")), QStringLiteral("en-GB"));
        QCOMPARE(jp::dict::match(QStringLiteral("en-NZ")), QStringLiteral("en-GB"));
        QCOMPARE(jp::dict::match(QStringLiteral("en")), QStringLiteral("en-US"));
        QCOMPARE(jp::dict::match(QStringLiteral("es-AR")), QStringLiteral("es-MX"));
        QCOMPARE(jp::dict::match(QStringLiteral("es-ES")), QStringLiteral("es-ES"));
        QCOMPARE(jp::dict::match(QStringLiteral("fr-CA")), QStringLiteral("fr-FR"));
        QCOMPARE(jp::dict::match(QStringLiteral("de-CH")), QStringLiteral("de-DE"));
        QCOMPARE(jp::dict::match(QStringLiteral("pt-AO")), QStringLiteral("pt-PT"));
        QCOMPARE(jp::dict::match(QStringLiteral("pt")), QStringLiteral("pt-BR"));
        QCOMPARE(jp::dict::match(QStringLiteral("nl-BE")), QStringLiteral("nl-NL"));
        QCOMPARE(jp::dict::match(QStringLiteral("ja-JP")), QString());
        // Every listed language has its spelling and hyphenation files.
        for (const jp::dict::Language &l : jp::dict::languages()) {
            QVERIFY2(jp::dict::hasSpelling(l.code), qPrintable(l.code));
            QVERIFY2(!jp::dict::hyphenationFile(l.code).isEmpty(), qPrintable(l.code));
        }
        QCOMPARE(jp::dict::languageName(QStringLiteral("pt-BR")), QStringLiteral("Portuguese (Brazil)"));
    }

    // Spelling checks each word in its own language: accented words, both
    // apostrophes, the German dictionary's older encoding, and no checking
    // for a language without a dictionary.
    void spellingByLanguage()
    {
        const struct { const char *lang, *word; bool ok; } words[] = {
            {"es-MX", "acción", true}, {"es-ES", "niño", true}, {"es-MX", "accion", false},
            {"fr-FR", "l’homme", true}, {"fr-FR", "l'homme", true}, {"fr-FR", "aujourd’hui", true}, {"fr-FR", "naïve", true}, {"fr-FR", "hommme", false},
            {"de-DE", "Straße", true}, {"de-DE", "größer", true}, {"de-DE", "Reservierungsnummer", true}, {"de-DE", "grösser", false},
            {"it-IT", "città", true}, {"it-IT", "dell’anno", true}, {"it-IT", "cittá", false},
            {"nl-NL", "één", true}, {"nl-NL", "fietsen", true}, {"nl-NL", "fietssen", false},
            {"pt-BR", "coração", true}, {"pt-PT", "acção", false}, {"pt-BR", "coracao", false},
            {"en-GB", "colour", true}, {"en-US", "colour", false}, {"en-CA", "colour", true}, {"en-AU", "organise", true},
            {"en-US", "don’t", true}, {"en-US", "don't", true},
        };
        for (const auto &w : words) {
            const QString code = jp::dict::match(QString::fromLatin1(w.lang));
            QVERIFY2(jp::dict::spell(code, QString::fromUtf8(w.word)) == w.ok, qPrintable(QStringLiteral("%1 %2").arg(code, QString::fromUtf8(w.word))));
        }
        QVERIFY(jp::spellingSuggestions(QStringLiteral("grösser"), QStringLiteral("de-DE")).contains(QStringLiteral("größer")));
        QVERIFY(jp::spellingSuggestions(QStringLiteral("accion"), QStringLiteral("es-MX")).contains(QStringLiteral("acción")));

        // In a story: each word by its own language.
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("Teh colour. Die Straße ist größer. Esto es una pruebba. こんにちは xyzzyq."));
        doc->pages[0]->items.push_back(t);
        QTextDocument *sd = doc->storyDoc(t->storyId);
        auto mark = [&](const QString &from, const QString &to, const QString &lang) {
            const QString text = sd->toPlainText();
            QTextCursor c(sd);
            c.setPosition(int(text.indexOf(from)));
            c.setPosition(int(text.indexOf(to) + to.size()), QTextCursor::KeepAnchor);
            QTextCharFormat f;
            f.setProperty(jp::tp::Language, lang);
            c.mergeCharFormat(f);
        };
        mark(QStringLiteral("Die"), QStringLiteral("größer."), QStringLiteral("de-DE"));
        mark(QStringLiteral("Esto"), QStringLiteral("pruebba."), QStringLiteral("es-ES"));
        mark(QStringLiteral("こんにちは"), QStringLiteral("xyzzyq."), QStringLiteral("ja-JP"));
        w.editor()->setDocument(std::move(doc));
        QStringList flagged;
        const QString text = sd->toPlainText();
        for (const auto &r : w.editor()->misspelledIn(t->storyId)) flagged << text.mid(r.first, r.second - r.first);
        QCOMPARE(flagged, (QStringList{QStringLiteral("Teh"), QStringLiteral("colour"), QStringLiteral("pruebba")}));
    }

    // Check spelling as you type: a misspelled word gets a range, a correct
    // one doesn't, and suggestions include the fix.
    void spellingAsYouType()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("This is teh test."));
        doc->pages[0]->items.push_back(t);
        w.editor()->setDocument(std::move(doc));
        const auto ranges = w.editor()->misspelledIn(t->storyId);
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first(), qMakePair(8, 11));
        QVERIFY(jp::spellingSuggestions(QStringLiteral("teh")).contains(QStringLiteral("the")));
        QVERIFY(w.editor()->renderOptions().misspelled.contains(t->storyId));
    }

    // Saving as .pub: pages, formatted text boxes, rectangles and ovals
    // come back through the independent .pub reader (libmspub).
    void pubWriterRoundTrip()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 120);
        t->storyId = doc->createStory();
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            QTextCharFormat plain;
            plain.setFontFamilies(QStringList{QStringLiteral("Arimo")});
            plain.setFontPointSize(18);
            c.insertText(QStringLiteral("Hello "), plain);
            QTextCharFormat bold = plain;
            bold.setFontWeight(QFont::Bold);
            c.insertText(QStringLiteral("bold "), bold);
            QTextCharFormat red = plain;
            red.setForeground(QColor(200, 0, 0));
            red.setFontItalic(true);
            c.insertText(QStringLiteral("red italic"), red);
            QTextBlockFormat center;
            center.setAlignment(Qt::AlignHCenter);
            c.insertBlock(center, plain);
            c.insertText(QStringLiteral("Second paragraph, centered"));
        }
        doc->pages[0]->items.push_back(t);
        auto r = std::make_shared<jp::ShapeItem>();
        r->shape = QStringLiteral("rect");
        r->rect = QRectF(72, 300, 200, 100);
        r->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(220, 30, 30)));
        r->stroke = jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 2);
        doc->pages[0]->items.push_back(r);
        auto e = std::make_shared<jp::ShapeItem>();
        e->shape = QStringLiteral("ellipse");
        e->rect = QRectF(320, 300, 150, 150);
        e->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(30, 60, 200)));
        e->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(e);

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("written.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) QFile::copy(path, qEnvironmentVariable("JP_SHOT_DIR") + "/jeffpub-test.pub");
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QCOMPARE(back->pages.size(), 1);
        QCOMPARE(back->pageSize(), QSizeF(612, 792));
        // Text: both paragraphs, with bold, italic and color where they were.
        const jp::TextItem *bt = nullptr;
        int shapes = 0;
        QVector<QRectF> shapeRects;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Text) bt = static_cast<const jp::TextItem *>(it.get());
            if (it->type() == jp::ItemType::Shape) { ++shapes; shapeRects << it->rect; }
        });
        QVERIFY(bt);
        QVERIFY(std::abs(bt->rect.left() - 72) < 1 && std::abs(bt->rect.width() - 300) < 1);
        QTextDocument *sd = back->storyDoc(bt->storyId);
        QVERIFY(sd);
        QCOMPARE(sd->blockCount(), 2);
        QCOMPARE(sd->begin().text(), QStringLiteral("Hello bold red italic"));
        QCOMPARE(sd->begin().next().text(), QStringLiteral("Second paragraph, centered"));
        QVERIFY(sd->begin().next().blockFormat().alignment() & Qt::AlignHCenter);
        bool sawBold = false, sawItalicRed = false;
        for (auto it = sd->begin().begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (it.fragment().text().contains(QStringLiteral("bold"))) sawBold = f.fontWeight() >= QFont::Bold;
            if (it.fragment().text().contains(QStringLiteral("red"))) {
                const QString cref = f.stringProperty(jp::tp::ColorRefP);
                const QColor c = cref.isEmpty() ? f.foreground().color() : jp::ColorRef::fromString(cref).resolve(back->colors);
                sawItalicRed = f.fontItalic() && c.red() > 150 && c.green() < 60;
            }
            if (it.fragment().text().startsWith(QStringLiteral("Hello"))) QVERIFY(std::abs(f.fontPointSize() - 18) < 0.1);
        }
        QVERIFY(sawBold);
        QVERIFY(sawItalicRed);
        // Arimo is saved as Arial, which it matches width for width.
        QCOMPARE(sd->begin().begin().fragment().charFormat().fontFamilies().toStringList().value(0), QStringLiteral("Arial"));
        QVERIFY2(shapes >= 2, qPrintable(QString::number(shapes)));
    }

    // Text Art saved as Publisher's own Text Art reads back with its words,
    // warp and settings.
    void pubWriterTextArt()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        struct Spec { const char *text, *transform; bool bold, italic; double spacing, rotation; QColor color; };
        const Spec specs[] = {{"test19 Plain", "plain", true, false, 1.0, 0, QColor(200, 30, 30)},
                              {"Arch Up", "archUp", false, false, 1.0, 0, QColor(30, 90, 200)},
                              {"Around the circle", "circle", true, false, 1.2, 20, QColor(30, 150, 60)},
                              {"Wave", "waveUp", false, true, 1.0, 0, QColor(150, 60, 160)},
                              {"Inflate", "inflate", true, false, 0.9, 0, QColor(220, 140, 20)},
                              {"Slant Up", "slantUp", false, false, 1.0, 0, QColor(20, 20, 20)}};
        QVector<std::shared_ptr<jp::TextArtItem>> made;
        for (int i = 0; i < 6; ++i) {
            auto ta = std::make_shared<jp::TextArtItem>();
            ta->text = QString::fromLatin1(specs[i].text);
            ta->transform_ = QString::fromLatin1(specs[i].transform);
            ta->font = QStringLiteral("Arimo");
            ta->bold = specs[i].bold;
            ta->italic = specs[i].italic;
            ta->spacing = specs[i].spacing;
            ta->rotation = specs[i].rotation;
            ta->rect = QRectF(60 + (i % 2) * 260, 60 + (i / 2) * 230, 220, 150);
            ta->fill = jp::Fill::solid(jp::ColorRef::rgb(specs[i].color));
            ta->stroke = jp::Stroke::none();
            doc->pages[0]->items.push_back(ta);
            made << ta;
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test19-textart.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test20-textart";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
            // The same with an ordinary text box too.
            auto withText = jp::Document::blank(QSizeF(612, 792));
            for (const auto &ta : made) withText->pages[0]->items.push_back(ta->clone());
            auto label = std::make_shared<jp::TextItem>();
            label->rect = QRectF(60, 720, 400, 30);
            label->storyId = withText->createStory(QStringLiteral("test21 Text Art with a text box"));
            withText->pages[0]->items.push_back(label);
            QFile::remove(qEnvironmentVariable("JP_SHOT_DIR") + "/test21-textart-and-text.pub");
            QVERIFY(jp::exportPublisher(*withText, qEnvironmentVariable("JP_SHOT_DIR") + "/test21-textart-and-text.pub", &e2));
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QVector<const jp::TextArtItem *> got;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::TextArt) got << static_cast<const jp::TextArtItem *>(it.get());
        });
        QCOMPARE(got.size(), 6);
        for (const auto &want : made) {
            const jp::TextArtItem *g = nullptr;
            for (const jp::TextArtItem *c : got)
                if (c->text == want->text) g = c;
            QVERIFY2(g, qPrintable(want->text));
            QCOMPARE(g->transform_, want->transform_);
            QCOMPARE(g->bold, want->bold);
            QCOMPARE(g->italic, want->italic);
            QCOMPARE(g->font, QStringLiteral("Arial"));
            QVERIFY(std::abs(g->spacing - want->spacing) < 0.01);
            QVERIFY2(std::abs(g->rotation - want->rotation) < 1, qPrintable(QString::number(g->rotation)));
            QVERIFY(QLineF(g->rect.center(), want->rect.center()).length() < 1 && std::abs(g->rect.width() - want->rect.width()) < 1);
            QCOMPARE(g->fill.color.resolve(back->colors), want->fill.color.resolve(doc->colors));
        }
    }

    // Shapes holding text keep their text, placed where JeffPub puts it.
    void pubWriterShapeText()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        const QStringList kinds = {QStringLiteral("rect"), QStringLiteral("star5"), QStringLiteral("wedgeRoundRectCallout")};
        for (int i = 0; i < kinds.size(); ++i) {
            auto sh = std::make_shared<jp::ShapeItem>();
            sh->shape = kinds[i];
            sh->rect = QRectF(72, 72 + i * 200, 250, 150);
            sh->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(250, 220, 120)));
            sh->stroke = jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 1);
            sh->storyId = doc->createStory(QStringLiteral("test18 text in a %1").arg(kinds[i]));
            sh->valign = i == 0 ? jp::VAlign::Top : jp::VAlign::Middle;
            doc->pages[0]->items.push_back(sh);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test18-shape-text.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test18-shape-text";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QStringList texts;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            QString sid;
            if (it->type() == jp::ItemType::Text) sid = static_cast<const jp::TextItem *>(it.get())->storyId;
            if (it->type() == jp::ItemType::Shape) sid = static_cast<const jp::ShapeItem *>(it.get())->storyId;
            if (const QTextDocument *d = sid.isEmpty() ? nullptr : back->storyDoc(sid)) texts << d->toPlainText();
        });
        for (const QString &k : kinds) QVERIFY2(texts.contains(QStringLiteral("test18 text in a %1").arg(k)), qPrintable(texts.join(" | ")));
    }

    // Preset shapes saved to .pub (Publisher's own where they match, freeforms
    // otherwise) read back with the same outline.
    void pubWriterShapes()
    {
        const QStringList ids = {"roundRect", "triangle", "hexagon", "foldedCorner", "fcDecision", "fcPredefined", "star5", "star12",
                                 "heart", "cloud", "rightArrow", "curvedRightArrow", "chevron", "wave", "ribbon2", "smiley", "sun",
                                 "moon", "lightning", "donut", "blockArc", "wedgeRoundRectCallout", "cloudCallout", "mathMultiply",
                                 "bracePair", "arc"};
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto label = std::make_shared<jp::TextItem>();
        label->rect = QRectF(54, 30, 500, 30);
        label->storyId = doc->createStory(QStringLiteral("test16 shapes"));
        doc->pages[0]->items.push_back(label);
        QVector<std::shared_ptr<jp::ShapeItem>> made;
        for (int i = 0; i < ids.size(); ++i) {
            auto sh = std::make_shared<jp::ShapeItem>();
            sh->shape = ids[i];
            sh->rect = QRectF(60 + (i % 5) * 104, 80 + (i / 5) * 100, 80, 60);
            const bool open = jp::shapeDef(ids[i]) && jp::shapeDef(ids[i])->open;
            sh->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor::fromHsv((i * 37) % 360, 160, 220)));
            sh->stroke = open ? jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 2) : jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 1);
            doc->pages[0]->items.push_back(sh);
            made << sh;
        }
        // A right triangle as drawn, and the same one flipped and turned 30°.
        for (int k = 0; k < 2; ++k) {
            auto t = std::make_shared<jp::ShapeItem>();
            t->shape = QStringLiteral("rtTriangle");
            t->rect = QRectF(160 + k * 200, 650, 100, 80);
            t->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(220, 120, 30)));
            t->stroke = jp::Stroke::none();
            if (k) {
                t->flipH = true;
                t->rotation = 30;
            }
            doc->pages[0]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test16-shapes.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test16-shapes";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        // Each shape's outline lands where it was (filled area overlap).
        auto area = [](const QPainterPath &p, bool stroke) {
            QImage img(612, 792, QImage::Format_Grayscale8);
            img.fill(0);
            QPainter g(&img);
            if (stroke) g.strokePath(p, QPen(Qt::white, 3));
            else g.fillPath(p, Qt::white);
            return img;
        };
        QVector<const jp::ShapeItem *> got;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Shape) got << static_cast<const jp::ShapeItem *>(it.get());
        });
        for (const auto &sh : made) {
            auto outline = [](const jp::ShapeItem *x) {
                QPainterPath p = x->customPath.isEmpty() ? jp::shapePath(x->shape, x->rect.size()) : x->customPath;
                return x->transform().map(p);
            };
            const jp::ShapeItem *match = nullptr;
            for (const jp::ShapeItem *g : got)
                if (QLineF(outline(g).boundingRect().center(), outline(sh.get()).boundingRect().center()).length() < 10) match = g;
            QVERIFY2(match, qPrintable(sh->shape));
            const bool open = jp::shapeDef(sh->shape)->open;
            QPainterPath gp = match->customPath;
            if (gp.isEmpty()) gp = jp::shapePath(match->shape, match->rect.size(), match->adj);
            // Publisher's own shapes come back as the same presets.
            if (!jp::presetForPubShapeType(jp::pubShapeType(sh->shape)).isEmpty()) QCOMPARE(match->shape, sh->shape);
            const QImage a = area(sh->transform().map(jp::shapePath(sh->shape, sh->rect.size())), open);
            const QImage b = area(match->transform().map(gp), open);
            qint64 both = 0, any = 0;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x) {
                    const bool pa = a.constScanLine(y)[x], pb = b.constScanLine(y)[x];
                    both += pa && pb;
                    any += pa || pb;
                }
            QVERIFY2(any && 100.0 * both / any > 97, qPrintable(QStringLiteral("%1 %2%").arg(sh->shape).arg(100.0 * both / std::max<qint64>(1, any))));
        }
    }

    // Gradients, picture fills, transparency and shadows survive .pub.
    void pubWriterFills()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto text = [&](const QRectF &r, const QString &s) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = r;
            t->storyId = doc->createStory(s);
            doc->pages[0]->items.push_back(t);
            return t;
        };
        text(QRectF(54, 30, 500, 30), QStringLiteral("test26 fills"));
        const QColor red(220, 30, 30), blue(30, 60, 200), yellow(250, 220, 40), green(30, 160, 60);
        struct Case { QString label; jp::Fill fill; jp::Stroke stroke = jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 1); bool shadow = false; };
        QVector<Case> cases;
        for (double a : {90.0, 0.0, 45.0, 135.0, 270.0})
            cases << Case{QStringLiteral("linear %1").arg(a), jp::Fill::gradient(jp::ColorRef::rgb(red), jp::ColorRef::rgb(blue), a)};
        jp::Fill three = jp::Fill::gradient(jp::ColorRef::rgb(red), jp::ColorRef::rgb(green), 0);
        three.stops = {{0, jp::ColorRef::rgb(red), 0}, {0.5, jp::ColorRef::rgb(yellow), 0}, {1, jp::ColorRef::rgb(green), 0}};
        cases << Case{QStringLiteral("three colors"), three};
        jp::Fill radial = jp::Fill::gradient(jp::ColorRef::rgb(Qt::white), jp::ColorRef::rgb(blue));
        radial.gradType = jp::Fill::Radial;
        cases << Case{QStringLiteral("radial"), radial};
        jp::Fill fade = jp::Fill::gradient(jp::ColorRef::rgb(blue), jp::ColorRef::rgb(blue), 0);
        fade.stops = {{0, jp::ColorRef::rgb(blue), 0}, {1, jp::ColorRef::rgb(blue), 1}};
        cases << Case{QStringLiteral("fade out"), fade};
        cases << Case{QStringLiteral("50% clear"), jp::Fill::solid(jp::ColorRef::rgb(blue), 0.5)};
        jp::Stroke thick = jp::Stroke::line(jp::ColorRef::rgb(red), 8);
        thick.transparency = 0.5;
        cases << Case{QStringLiteral("clear line"), jp::Fill::solid(jp::ColorRef::rgb(yellow)), thick};
        cases << Case{QStringLiteral("shadow"), jp::Fill::solid(jp::ColorRef::rgb(yellow)), jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 1), true};
        QImage checks(32, 32, QImage::Format_RGB32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) checks.setPixel(x, y, ((x / 8 + y / 8) % 2) ? qRgb(30, 60, 200) : qRgb(250, 220, 40));
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        checks.save(&buf, "PNG");
        jp::Fill pic;
        pic.type = jp::Fill::Picture;
        pic.imageId = doc->addImage(png, QStringLiteral("png"));
        cases << Case{QStringLiteral("picture"), pic};
        jp::Fill tiled = pic;
        tiled.type = jp::Fill::Texture;
        cases << Case{QStringLiteral("texture"), tiled};
        jp::Fill pattern;
        pattern.type = jp::Fill::Pattern;
        pattern.color = jp::ColorRef::rgb(blue);
        pattern.color2 = jp::ColorRef::rgb(yellow);
        pattern.pattern = 9;
        cases << Case{QStringLiteral("pattern"), pattern};
        QVector<std::shared_ptr<jp::ShapeItem>> made;
        for (int i = 0; i < cases.size(); ++i) {
            auto sh = std::make_shared<jp::ShapeItem>();
            sh->shape = QStringLiteral("rect");
            sh->rect = QRectF(60 + (i % 4) * 130, 80 + (i / 4) * 150, 100, 90);
            sh->fill = cases[i].fill;
            sh->stroke = cases[i].stroke;
            if (cases[i].shadow) {
                sh->fx.shadow.on = true;
                sh->fx.shadow.distance = 6;
                sh->fx.shadow.color = jp::ColorRef::rgb(Qt::black);
                sh->fx.shadow.transparency = 0.5;
            }
            doc->pages[0]->items.push_back(sh);
            made << sh;
            text(QRectF(sh->rect.left(), sh->rect.bottom() + 8, 120, 24), cases[i].label);
        }
        // A text box with a gradient, a border and a shadow.
        auto box = text(QRectF(60, 700, 300, 50), QStringLiteral("A text box with a gradient and a shadow"));
        box->fill = jp::Fill::gradient(jp::ColorRef::rgb(yellow), jp::ColorRef::rgb(Qt::white), 90);
        box->stroke = jp::Stroke::line(jp::ColorRef::rgb(blue), 2);
        box->fx.shadow.on = true;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test26-fills.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test26-fills";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QVector<const jp::Item *> got;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Shape || it->type() == jp::ItemType::Picture) got << it.get();
        });
        auto near = [&](const QRectF &r) -> const jp::Item * {
            for (const jp::Item *g : got)
                if (QLineF(g->rect.center(), r.center()).length() < 3 && std::abs(g->rect.width() - r.width()) < 3) return g;
            return nullptr;
        };
        auto angleDiff = [](double a, double b) { return std::abs(std::remainder(a - b, 360.0)); };
        for (int i = 0; i < cases.size(); ++i) {
            const jp::Item *g = near(made[i]->rect);
            QVERIFY2(g, qPrintable(cases[i].label));
            const jp::Fill &want = cases[i].fill;
            if (want.type == jp::Fill::Picture) {
                QCOMPARE(g->type(), jp::ItemType::Picture);
                continue;
            }
            if (want.type == jp::Fill::Texture || want.type == jp::Fill::Pattern) {
                // Textures and patterns come back as tiled pictures, pixel for pixel.
                QCOMPARE(g->type(), jp::ItemType::Shape);
                const auto *sh = static_cast<const jp::ShapeItem *>(g);
                QCOMPARE(int(sh->fill.type), int(jp::Fill::Texture));
                const QImage want = cases[i].fill.type == jp::Fill::Pattern ? cases[i].fill.patternTile(doc->colors) : checks;
                QCOMPARE(back->image(sh->fill.imageId).convertToFormat(QImage::Format_ARGB32), want.convertToFormat(QImage::Format_ARGB32));
                continue;
            }
            QCOMPARE(g->type(), jp::ItemType::Shape);
            const auto *sh = static_cast<const jp::ShapeItem *>(g);
            QCOMPARE(int(sh->fill.type), int(want.type));
            if (want.type == jp::Fill::Gradient) {
                QCOMPARE(int(sh->fill.gradType == jp::Fill::Linear), int(want.gradType == jp::Fill::Linear));
                if (want.gradType == jp::Fill::Linear)
                    QVERIFY2(angleDiff(sh->fill.angle, want.angle) < 0.5, qPrintable(QStringLiteral("%1 -> %2").arg(want.angle).arg(sh->fill.angle)));
                const QVector<jp::GradientStop> ws = want.stops.isEmpty()
                    ? QVector<jp::GradientStop>{{0, want.color, 0}, {1, want.color2, 0}} : want.stops;
                QCOMPARE(sh->fill.stops.size(), ws.size());
                for (int k = 0; k < ws.size(); ++k) {
                    QVERIFY(std::abs(sh->fill.stops[k].pos - ws[k].pos) < 0.011);
                    QCOMPARE(sh->fill.stops[k].color.resolve(back->colors).rgb(), ws[k].color.resolve(doc->colors).rgb());
                    QVERIFY2(std::abs(sh->fill.stops[k].transparency - ws[k].transparency) < 0.011, qPrintable(cases[i].label));
                }
            } else {
                QVERIFY(std::abs(sh->fill.transparency - want.transparency) < 0.01);
            }
            QVERIFY2(std::abs(sh->stroke.transparency - cases[i].stroke.transparency) < 0.01, qPrintable(cases[i].label));
            QCOMPARE(sh->fx.shadow.on, cases[i].shadow);
            if (cases[i].shadow) {
                QVERIFY(std::abs(sh->fx.shadow.distance - 6) < 0.1);
                QVERIFY(angleDiff(sh->fx.shadow.angle, 45) < 0.5);
                QVERIFY(std::abs(sh->fx.shadow.transparency - 0.5) < 0.01);
            }
        }
    }

    // Picture settings and pictures cut to a shape survive .pub.
    void pubWriterPictures()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto label = [&](const QRectF &r, const QString &s) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = r;
            t->storyId = doc->createStory(s);
            doc->pages[0]->items.push_back(t);
        };
        label(QRectF(54, 30, 500, 30), QStringLiteral("test27 pictures"));
        QImage photo(120, 90, QImage::Format_RGB32);
        for (int y = 0; y < photo.height(); ++y)
            for (int x = 0; x < photo.width(); ++x) photo.setPixel(x, y, qRgb(x * 2, y * 2, 255 - x));
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        photo.save(&buf, "PNG");
        const QString id = doc->addImage(png, QStringLiteral("png"));
        struct Case { QString label; std::function<void(jp::PictureItem &)> set; };
        const QVector<Case> cases = {
            {QStringLiteral("as is"), [](jp::PictureItem &) {}},
            {QStringLiteral("brighter, less contrast"), [](jp::PictureItem &p) { p.brightness = 30; p.contrast = -20; }},
            {QStringLiteral("darker, more contrast"), [](jp::PictureItem &p) { p.brightness = -25; p.contrast = 40; }},
            {QStringLiteral("grayscale"), [](jp::PictureItem &p) { p.recolor = jp::PictureItem::Grayscale; }},
            {QStringLiteral("black and white"), [](jp::PictureItem &p) { p.recolor = jp::PictureItem::BlackWhite; }},
            {QStringLiteral("washout"), [](jp::PictureItem &p) { p.recolor = jp::PictureItem::Washout; }},
            {QStringLiteral("sepia"), [](jp::PictureItem &p) { p.recolor = jp::PictureItem::Sepia; }},
            {QStringLiteral("recolored red"), [](jp::PictureItem &p) { p.recolor = jp::PictureItem::ColorTint; p.recolorColor = jp::ColorRef::rgb(QColor(200, 0, 0)); }},
            {QStringLiteral("clear color"), [](jp::PictureItem &p) { p.hasTransparentColor = true; p.transparentColor = QColor(0, 0, 255); }},
            {QStringLiteral("oval"), [](jp::PictureItem &p) { p.maskShape = QStringLiteral("ellipse"); }},
            {QStringLiteral("triangle, cropped"), [](jp::PictureItem &p) { p.maskShape = QStringLiteral("triangle"); p.imgRect = QRectF(-30, -20, 160, 120); }},
            {QStringLiteral("hexagon, gray, turned"), [](jp::PictureItem &p) { p.maskShape = QStringLiteral("hexagon"); p.recolor = jp::PictureItem::Grayscale; p.rotation = 20; }},
            {QStringLiteral("cropped"), [](jp::PictureItem &p) { p.imgRect = QRectF(-25, -10, 150, 100); }},
            {QStringLiteral("40% see-through"), [](jp::PictureItem &p) { p.transparency = 0.4; }},
        };
        QVector<std::shared_ptr<jp::PictureItem>> made;
        for (int i = 0; i < cases.size(); ++i) {
            auto pic = std::make_shared<jp::PictureItem>();
            pic->imageId = id;
            pic->rect = QRectF(60 + (i % 4) * 130, 80 + (i / 4) * 150, 100, 75);
            pic->imgRect = QRectF(0, 0, 100, 75);
            cases[i].set(*pic);
            doc->pages[0]->items.push_back(pic);
            made << pic;
            label(QRectF(pic->rect.left(), pic->rect.bottom() + 10, 125, 24), cases[i].label);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test27-pictures.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test27-pictures";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QVector<const jp::PictureItem *> got;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Picture) got << static_cast<const jp::PictureItem *>(it.get());
        });
        QCOMPARE(got.size(), made.size());
        for (int i = 0; i < made.size(); ++i) {
            const jp::PictureItem *want = made[i].get();
            const jp::PictureItem *g = nullptr;
            for (const jp::PictureItem *c : got)
                if (QLineF(c->rect.center(), want->rect.center()).length() < 1.5) g = c;
            QVERIFY2(g, qPrintable(cases[i].label));
            QVERIFY2(std::abs(g->rect.width() - want->rect.width()) < 1 && std::abs(g->rect.height() - want->rect.height()) < 1, qPrintable(cases[i].label));
            QVERIFY2(std::abs(std::remainder(g->rotation - want->rotation, 360.0)) < 0.5, qPrintable(cases[i].label));
            QCOMPARE(g->maskShape, want->maskShape);
            if (want->maskShape != QLatin1String("rect")) {
                // Cut out with its settings applied: the picture shows as it did.
                const QImage a = jp::Renderer::processedImage(*doc, *want, QSizeF());
                const QImage shown = back->image(g->imageId);
                QVERIFY(!shown.isNull());
                const QRectF ir = want->imgRect;
                const QPointF probe(want->rect.width() * 0.5, want->rect.height() * 0.6);
                const QColor wa = a.pixelColor(int((probe.x() - ir.left()) / ir.width() * a.width()), int((probe.y() - ir.top()) / ir.height() * a.height()));
                const QColor gb = shown.pixelColor(int(probe.x() / want->rect.width() * shown.width()), int(probe.y() / want->rect.height() * shown.height()));
                QVERIFY2(std::abs(wa.red() - gb.red()) < 12 && std::abs(wa.green() - gb.green()) < 12 && std::abs(wa.blue() - gb.blue()) < 12,
                         qPrintable(QStringLiteral("%1: %2 vs %3").arg(cases[i].label, wa.name(), gb.name())));
                continue;
            }
            QVERIFY2(std::abs(g->transparency - want->transparency) < 0.01, qPrintable(cases[i].label));
            if (want->transparency < 0.001)
                QVERIFY2(QLineF(g->imgRect.topLeft(), want->imgRect.topLeft()).length() < 0.5 && std::abs(g->imgRect.width() - want->imgRect.width()) < 0.5
                             && std::abs(g->imgRect.height() - want->imgRect.height()) < 0.5,
                         qPrintable(QStringLiteral("%1: %2,%3 %4x%5").arg(cases[i].label).arg(g->imgRect.x()).arg(g->imgRect.y()).arg(g->imgRect.width()).arg(g->imgRect.height())));
            QVERIFY2(std::abs(g->brightness - want->brightness) < 0.05, qPrintable(cases[i].label));
            QVERIFY2(std::abs(g->contrast - want->contrast) < 0.05, qPrintable(QStringLiteral("%1 %2").arg(cases[i].label).arg(g->contrast)));
            QCOMPARE(int(g->recolor), int(want->recolor));
            if (want->recolor == jp::PictureItem::ColorTint) QCOMPARE(g->recolorColor.resolve(back->colors), want->recolorColor.resolve(doc->colors));
            // Gray and black and white pictures are saved already converted,
            // since the other program shows the saved colors.
            if (want->recolor == jp::PictureItem::Grayscale || want->recolor == jp::PictureItem::BlackWhite) {
                const QImage saved = back->image(g->imageId);
                QVERIFY(!saved.isNull());
                for (int y = 0; y < saved.height(); y += 7)
                    for (int x = 0; x < saved.width(); x += 7) {
                        const QColor px = saved.pixelColor(x, y);
                        QVERIFY2(std::abs(px.red() - px.green()) <= 2 && std::abs(px.green() - px.blue()) <= 2, qPrintable(cases[i].label));
                        if (want->recolor == jp::PictureItem::BlackWhite) QVERIFY2(px.red() <= 2 || px.red() >= 253, qPrintable(cases[i].label));
                    }
            }
            QCOMPARE(g->hasTransparentColor, want->hasTransparentColor);
            if (want->hasTransparentColor) QCOMPARE(g->transparentColor, want->transparentColor);
        }
    }

    // A chain of linked text boxes (across two pages) is one story in .pub.
    void pubWriterLinked()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->addPage();
        QString text = QStringLiteral("test14 linked boxes.");
        for (int i = 1; i <= 60; ++i) text += QStringLiteral(" Sentence %1 flows on through the linked boxes.").arg(i);
        const QString story = doc->createStory(text);
        const QRectF rects[3] = {QRectF(72, 72, 220, 150), QRectF(320, 72, 220, 150), QRectF(72, 72, 220, 200)};
        QVector<std::shared_ptr<jp::TextItem>> boxes;
        for (int i = 0; i < 3; ++i) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = rects[i];
            t->storyId = story;
            if (i) boxes.last()->nextId = t->id;
            boxes << t;
            doc->pages[i < 2 ? 0 : 1]->items.push_back(t);
        }
        QTemporaryDir dir;
        // Also two linked boxes on one page, the simplest chain.
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            auto two = jp::Document::blank(QSizeF(612, 792));
            const QString st = two->createStory(QStringLiteral("test15 two linked boxes.") + text.mid(20));
            auto a = std::make_shared<jp::TextItem>(), b = std::make_shared<jp::TextItem>();
            a->rect = rects[0];
            b->rect = rects[1];
            a->storyId = b->storyId = st;
            a->nextId = b->id;
            two->pages[0]->items.push_back(a);
            two->pages[0]->items.push_back(b);
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test15-linked-one-page.pub";
            QFile::remove(out);
            QString e;
            QVERIFY(jp::exportPublisher(*two, out, &e));
        }
        const QString path = dir.filePath(QStringLiteral("test14-linked.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test14-linked.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QVector<jp::TextItem *> got;
        for (int pi = 0; pi < back->pages.size(); ++pi)
            jp::walkItems(back->pages[pi]->items, [&](const jp::ItemPtr &it) {
                if (it->type() == jp::ItemType::Text) got << static_cast<jp::TextItem *>(it.get());
            });
        QCOMPARE(got.size(), 3);
        // One chain, in order, holding the whole text.
        QVERIFY(!back->prevFrame(got[0]->id));
        QCOMPARE(got[0]->nextId, got[1]->id);
        QCOMPARE(got[1]->nextId, got[2]->id);
        QVERIFY(got[2]->nextId.isEmpty());
        QCOMPARE(got[1]->storyId, got[0]->storyId);
        QCOMPARE(back->storyDoc(got[0]->storyId)->toPlainText(), text);
        for (int i = 0; i < 3; ++i) QVERIFY(std::abs(got[i]->rect.left() - rects[i].left()) < 1 && std::abs(got[i]->rect.height() - rects[i].height()) < 1);
    }

    // Character effects, indents, spacing and line spacing written to .pub
    // read back as they were.
    void pubWriterTextFormats()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 468, 500);
        t->storyId = doc->createStory();
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            QTextCharFormat plain;
            plain.setFontFamilies(QStringList{QStringLiteral("Arimo")});
            plain.setFontPointSize(12);
            QTextBlockFormat b0;
            b0.setBottomMargin(0);
            c.setBlockFormat(b0);
            c.insertText(QStringLiteral("test24 "), plain);
            QTextCharFormat u = plain;
            u.setFontUnderline(true);
            c.insertText(QStringLiteral("underline"), u);
            c.insertText(QStringLiteral(" "), plain);
            QTextCharFormat sc = plain;
            sc.setFontCapitalization(QFont::SmallCaps);
            c.insertText(QStringLiteral("smallcaps"), sc);
            c.insertText(QStringLiteral(" "), plain);
            QTextCharFormat ac = plain;
            ac.setFontCapitalization(QFont::AllUppercase);
            c.insertText(QStringLiteral("allcaps"), ac);
            c.insertText(QStringLiteral(" x"), plain);
            QTextCharFormat sup = plain;
            sup.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            c.insertText(QStringLiteral("2"), sup);
            c.insertText(QStringLiteral(" H"), plain);
            QTextCharFormat sub = plain;
            sub.setVerticalAlignment(QTextCharFormat::AlignSubScript);
            c.insertText(QStringLiteral("2"), sub);
            c.insertText(QStringLiteral("O"), plain);
            QTextBlockFormat ind;
            ind.setLeftMargin(72);
            ind.setTextIndent(-18);
            ind.setRightMargin(36);
            ind.setTopMargin(12);
            ind.setBottomMargin(0);
            c.insertBlock(ind, plain);
            c.insertText(QStringLiteral("Indented one inch with a hanging first line and a half inch on the right, after twelve points."));
            QTextBlockFormat dbl;
            dbl.setLineHeight(200, QTextBlockFormat::ProportionalHeight);
            dbl.setBottomMargin(0);
            c.insertBlock(dbl, plain);
            c.insertText(QStringLiteral("Double spaced. Double spaced. Double spaced. Double spaced. Double spaced. Double spaced."));
            QTextBlockFormat fixed;
            fixed.setLineHeight(24, QTextBlockFormat::FixedHeight);
            fixed.setBottomMargin(0);
            c.insertBlock(fixed, plain);
            c.insertText(QStringLiteral("Exactly 24 points between lines. Exactly 24 points between lines. Exactly 24 points."));
        }
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test24-formats.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test24-formats";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        const jp::TextItem *bt = nullptr;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Text) bt = static_cast<const jp::TextItem *>(it.get());
        });
        QVERIFY(bt);
        QTextDocument *d = back->storyDoc(bt->storyId);
        QCOMPARE(d->blockCount(), 4);
        QHash<QString, QTextCharFormat> fmt;
        for (auto it = d->begin().begin(); !it.atEnd(); ++it) fmt[it.fragment().text().trimmed()] = it.fragment().charFormat();
        QVERIFY(fmt.value(QStringLiteral("underline")).fontUnderline());
        QCOMPARE(fmt.value(QStringLiteral("smallcaps")).fontCapitalization(), QFont::SmallCaps);
        QCOMPARE(fmt.value(QStringLiteral("allcaps")).fontCapitalization(), QFont::AllUppercase);
        QCOMPARE(fmt.value(QStringLiteral("2")).verticalAlignment() == QTextCharFormat::AlignSuperScript ||
                     fmt.value(QStringLiteral("2")).verticalAlignment() == QTextCharFormat::AlignSubScript, true);
        int supers = 0, subs = 0;
        for (auto it = d->begin().begin(); !it.atEnd(); ++it) {
            if (it.fragment().text() != QStringLiteral("2")) continue;
            supers += it.fragment().charFormat().verticalAlignment() == QTextCharFormat::AlignSuperScript;
            subs += it.fragment().charFormat().verticalAlignment() == QTextCharFormat::AlignSubScript;
        }
        QCOMPARE(supers, 1);
        QCOMPARE(subs, 1);
        const QTextBlockFormat bi = d->begin().next().blockFormat();
        QVERIFY2(std::abs(bi.leftMargin() - 72) < 0.5 && std::abs(bi.textIndent() + 18) < 0.5 && std::abs(bi.rightMargin() - 36) < 0.5,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(bi.leftMargin()).arg(bi.textIndent()).arg(bi.rightMargin())));
        QVERIFY(std::abs(bi.topMargin() - 12) < 0.5);
        QVERIFY(std::abs(d->begin().blockFormat().bottomMargin()) < 0.01);
        const QTextBlockFormat bd = d->begin().next().next().blockFormat();
        QCOMPARE(bd.lineHeightType(), int(QTextBlockFormat::ProportionalHeight));
        QVERIFY2(std::abs(bd.lineHeight() - 200) < 1, qPrintable(QString::number(bd.lineHeight())));
        const QTextBlockFormat bf = d->lastBlock().blockFormat();
        QCOMPARE(bf.lineHeightType(), int(QTextBlockFormat::FixedHeight));
        QVERIFY2(std::abs(bf.lineHeight() - 24) < 0.1, qPrintable(QString::number(bf.lineHeight())));
    }

    // Strikethrough, letter spacing, tab stops and lists written to .pub read back.
    void pubWriterTabsLists()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 468, 400);
        t->storyId = doc->createStory();
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            QTextCharFormat plain;
            plain.setFontFamilies(QStringList{QStringLiteral("Arimo")});
            plain.setFontPointSize(12);
            c.insertText(QStringLiteral("test25 "), plain);
            QTextCharFormat st = plain;
            st.setFontStrikeOut(true);
            c.insertText(QStringLiteral("strike"), st);
            c.insertText(QStringLiteral(" "), plain);
            QTextCharFormat tr = plain;
            tr.setFontLetterSpacingType(QFont::PercentageSpacing);
            tr.setFontLetterSpacing(125);
            c.insertText(QStringLiteral("tracked"), tr);
            c.insertText(QStringLiteral(" "), plain);
            QTextCharFormat kn = plain;
            kn.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            kn.setFontLetterSpacing(2);
            c.insertText(QStringLiteral("kerned"), kn);
            c.insertText(QStringLiteral(" "), plain);
            QTextCharFormat both = plain;
            both.setProperty(jp::tp::Tracking, 87.5);
            both.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            both.setFontLetterSpacing(-1);
            c.insertText(QStringLiteral("both"), both);
            QTextBlockFormat tabs;
            QList<QTextOption::Tab> tl;
            tl << QTextOption::Tab(72, QTextOption::LeftTab) << QTextOption::Tab(216, QTextOption::CenterTab) << QTextOption::Tab(360, QTextOption::RightTab);
            tabs.setTabPositions(tl);
            tabs.setProperty(jp::tp::TabLeaders, QStringLiteral(" .-"));   // none, dots, dashes
            c.insertBlock(tabs, plain);
            c.insertText(QStringLiteral("\tleft\tcenter\tright"));
            QTextBlockFormat item;
            item.setLeftMargin(18);
            item.setTextIndent(-18);
            QTextListFormat bl;
            bl.setStyle(QTextListFormat::ListDisc);
            bl.setIndent(0);
            c.insertBlock(item, plain);
            c.insertText(QStringLiteral("bullet one"));
            QTextList *bullets = c.createList(bl);
            c.insertBlock(item, plain);
            c.insertText(QStringLiteral("bullet two"));
            bullets->add(c.block());
            QTextListFormat nl;
            nl.setStyle(QTextListFormat::ListUpperRoman);
            nl.setNumberSuffix(QStringLiteral("."));
            nl.setIndent(0);
            c.insertBlock(item, plain);
            c.insertText(QStringLiteral("number one"));
            QTextList *numbers = c.createList(nl);
            c.insertBlock(item, plain);
            c.insertText(QStringLiteral("number two"));
            numbers->add(c.block());
        }
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test25-tabs-lists.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test25-tabs-lists";
            QFile::remove(out + ".pub");
            QFile::copy(path, out + ".pub");
            QString e2;
            jp::savePublication(*doc, out + ".jpub", QImage(), &e2);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        const jp::TextItem *bt = nullptr;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Text) bt = static_cast<const jp::TextItem *>(it.get());
        });
        QVERIFY(bt);
        QTextDocument *d = back->storyDoc(bt->storyId);
        QCOMPARE(d->blockCount(), 6);
        QHash<QString, QTextCharFormat> fmt;
        for (auto it = d->begin().begin(); !it.atEnd(); ++it) fmt[it.fragment().text().trimmed()] = it.fragment().charFormat();
        QVERIFY(fmt.value(QStringLiteral("strike")).fontStrikeOut());
        // Tracking (a percentage) and kerning (points) come back apart, and
        // a run can have both.
        QVERIFY(std::abs(jp::tp::trackingOf(fmt.value(QStringLiteral("tracked"))) - 125) < 0.5);
        QVERIFY(std::abs(jp::tp::kerningOf(fmt.value(QStringLiteral("tracked")))) < 0.001);
        QVERIFY(std::abs(jp::tp::kerningOf(fmt.value(QStringLiteral("kerned"))) - 2) < 0.05);
        QVERIFY(std::abs(jp::tp::trackingOf(fmt.value(QStringLiteral("kerned"))) - 100) < 0.01);
        QVERIFY(std::abs(jp::tp::trackingOf(fmt.value(QStringLiteral("both"))) - 87.5) < 0.5);
        QVERIFY(std::abs(jp::tp::kerningOf(fmt.value(QStringLiteral("both"))) + 1) < 0.05);
        const QList<QTextOption::Tab> got = d->begin().next().blockFormat().tabPositions();
        QCOMPARE(got.size(), 3);
        QVERIFY(std::abs(got[1].position - 216) < 0.5 && got[1].type == QTextOption::CenterTab);
        // Leaders: none before the left stop, dots before center, dashes before right.
        QCOMPARE(d->begin().next().blockFormat().stringProperty(jp::tp::TabLeaders), QStringLiteral(" .-"));
        QVERIFY(std::abs(got[2].position - 360) < 0.5 && got[2].type == QTextOption::RightTab);
        const QTextBlock b1 = d->begin().next().next(), b2 = b1.next(), n1 = b2.next(), n2 = n1.next();
        QVERIFY(b1.textList() && b1.textList() == b2.textList());
        QCOMPARE(b1.textList()->format().style(), QTextListFormat::ListDisc);
        QVERIFY(n1.textList() && n1.textList() == n2.textList() && n1.textList() != b1.textList());
        QCOMPARE(n1.textList()->format().style(), QTextListFormat::ListUpperRoman);
        QCOMPARE(n1.textList()->format().numberSuffix(), QStringLiteral("."));
        QVERIFY(std::abs(n2.blockFormat().leftMargin() - 18) < 0.5 && std::abs(n2.blockFormat().textIndent() + 18) < 0.5);
    }

    // Objects on the master page are saved there and come back there, once.
    void pubWriterMaster()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792), QStringLiteral("Letter"), 2);
        QVERIFY(!doc->masters.isEmpty());
        auto mt = std::make_shared<jp::TextItem>();
        mt->rect = QRectF(72, 700, 300, 40);
        mt->storyId = doc->createStory(QStringLiteral("test23 master page footer"));
        doc->masters.first()->items.push_back(mt);
        auto bar = std::make_shared<jp::ShapeItem>();
        bar->rect = QRectF(72, 690, 468, 6);
        bar->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(30, 60, 200)));
        bar->stroke = jp::Stroke::none();
        doc->masters.first()->items.push_back(bar);
        for (int i = 0; i < 2; ++i) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(72, 72, 300, 40);
            t->storyId = doc->createStory(QStringLiteral("Page %1 text").arg(i + 1));
            doc->pages[i]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test23-master.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test23-master.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QCOMPARE(back->pages.size(), 2);
        const jp::MasterPage *m = back->master(QStringLiteral("A"));
        QVERIFY(m);
        QCOMPARE(int(m->items.size()), 2);
        bool footer = false;
        jp::walkItems(m->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Text)
                footer = back->storyDoc(static_cast<const jp::TextItem *>(it.get())->storyId)->toPlainText() == QStringLiteral("test23 master page footer");
        });
        QVERIFY(footer);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(back->pages[i]->masterId, QStringLiteral("A"));
            QCOMPARE(int(back->pages[i]->items.size()), 1);
        }
    }

    // A story's automatic hyphenation setting survives .pub.
    void pubWriterHyphenation()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        for (int i = 0; i < 2; ++i) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(72, 72 + i * 100, 300, 60);
            t->storyId = doc->createStory(i ? QStringLiteral("not hyphenated") : QStringLiteral("hyphenated"));
            t->hyphenate = i == 0;
            doc->pages[0]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("hyph.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test29-hyphenation.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        int checked = 0;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() != jp::ItemType::Text) return;
            const auto *t = static_cast<const jp::TextItem *>(it.get());
            QCOMPARE(t->hyphenate, back->storyDoc(t->storyId)->toPlainText() == QStringLiteral("hyphenated"));
            ++checked;
        });
        QCOMPARE(checked, 2);
    }

    // Named paragraph styles: the style sheet keeps them, and paragraphs keep
    // which one they use.
    void pubWriterStyles()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        jp::TextStyle pq;
        pq.name = QStringLiteral("Pull Quote");
        pq.basedOn = QStringLiteral("Normal");
        pq.next = QStringLiteral("Normal");
        pq.chr.setFontPointSize(16);
        pq.chr.setFontWeight(QFont::Bold);
        pq.chr.setFontItalic(true);
        pq.chr.setFontFamilies(QStringList{QStringLiteral("Georgia")});
        pq.blk.setAlignment(Qt::AlignHCenter);
        pq.blk.setTopMargin(12);
        pq.blk.setBottomMargin(6);
        pq.blk.setProperty(jp::tp::StyleName, pq.name);
        doc->styles << pq;
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 400, 300);
        t->storyId = doc->createStory(QString());
        doc->pages[0]->items.push_back(t);
        QTextDocument *d = doc->storyDoc(t->storyId);
        QTextCursor c(d);
        auto para = [&](const QString &text, const QString &style, bool first) {
            const jp::TextStyle *st = doc->style(style);
            QTextBlockFormat bf = st->blk;
            bf.setProperty(jp::tp::StyleName, style);
            if (first) c.setBlockFormat(bf);
            else c.insertBlock(bf);
            c.insertText(text, st->chr);
        };
        para(QStringLiteral("test30 A title"), QStringLiteral("Title"), true);
        para(QStringLiteral("Plain text in the Normal style."), QStringLiteral("Normal"), false);
        para(QStringLiteral("A pull quote, in its own style"), QStringLiteral("Pull Quote"), false);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("styles.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test30-styles.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        const jp::TextItem *bt = nullptr;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Text) bt = static_cast<const jp::TextItem *>(it.get());
        });
        QVERIFY(bt);
        QTextBlock b = back->storyDoc(bt->storyId)->begin();
        QCOMPARE(b.blockFormat().stringProperty(jp::tp::StyleName), QStringLiteral("Title"));
        QVERIFY(b.next().blockFormat().stringProperty(jp::tp::StyleName).isEmpty());
        QCOMPARE(b.next().next().blockFormat().stringProperty(jp::tp::StyleName), QStringLiteral("Pull Quote"));
        // Bold and italic are saved relative to the style, so they come back
        // as they were: the title and pull quote bold, the plain text not.
        auto firstFormat = [](const QTextBlock &blk) { return blk.begin().fragment().charFormat(); };
        QVERIFY(firstFormat(b).fontWeight() >= QFont::Bold);
        QVERIFY(firstFormat(b.next()).fontWeight() < QFont::DemiBold && !firstFormat(b.next()).fontItalic());
        QVERIFY(firstFormat(b.next().next()).fontWeight() >= QFont::Bold && firstFormat(b.next().next()).fontItalic());
        const jp::TextStyle *got = back->style(QStringLiteral("Pull Quote"));
        QVERIFY(got);
        QVERIFY(std::abs(got->chr.fontPointSize() - 16) < 0.01);
        QVERIFY(got->chr.fontWeight() >= QFont::Bold && got->chr.fontItalic());
        QCOMPARE(got->chr.fontFamilies().toStringList().value(0), QStringLiteral("Georgia"));
        QCOMPARE(got->blk.alignment() & Qt::AlignHorizontal_Mask, Qt::AlignHCenter);
        QVERIFY(std::abs(got->blk.topMargin() - 12) < 0.01);
    }

    // Two master pages: each page keeps its own master and its objects.
    void pubWriterTwoMasters()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792), QStringLiteral("Letter"), 3);
        auto b = std::make_shared<jp::MasterPage>();
        b->abbr = QStringLiteral("B");
        b->id = QStringLiteral("B");
        b->name = QStringLiteral("Chapter openers");
        doc->masters << b;
        auto footer = [&](jp::MasterPage *m, const QString &text, const QColor &c) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(72, 700, 300, 40);
            t->storyId = doc->createStory(text);
            m->items.push_back(t);
            auto bar = std::make_shared<jp::ShapeItem>();
            bar->rect = QRectF(72, 690, 468, 6);
            bar->fill = jp::Fill::solid(jp::ColorRef::rgb(c));
            bar->stroke = jp::Stroke::none();
            m->items.push_back(bar);
        };
        footer(doc->masters[0].get(), QStringLiteral("test28 master A (blue bar)"), QColor(30, 60, 200));
        footer(b.get(), QStringLiteral("test28 master B (red bar)"), QColor(200, 30, 30));
        const QStringList uses = {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("A")};
        for (int i = 0; i < 3; ++i) {
            doc->pages[i]->masterId = uses[i];
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(72, 72, 400, 40);
            t->storyId = doc->createStory(QStringLiteral("Page %1 uses master %2").arg(i + 1).arg(uses[i]));
            doc->pages[i]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test28-masters.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));   // nothing left out
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test28-masters.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QCOMPARE(back->pages.size(), 3);
        QCOMPARE(back->masters.size(), 2);
        for (int i = 0; i < 3; ++i) QCOMPARE(back->pages[i]->masterId, uses[i]);
        auto footerText = [&](const QString &id) {
            QString text;
            const jp::MasterPage *m = back->master(id);
            if (!m) return text;
            jp::walkItems(m->items, [&](const jp::ItemPtr &it) {
                if (it->type() == jp::ItemType::Text) text = back->storyDoc(static_cast<const jp::TextItem *>(it.get())->storyId)->toPlainText();
            });
            return text;
        };
        QCOMPARE(footerText(QStringLiteral("A")), QStringLiteral("test28 master A (blue bar)"));
        QCOMPARE(footerText(QStringLiteral("B")), QStringLiteral("test28 master B (red bar)"));
    }

    // Two pages written to .pub read back as two pages, each with its own objects.
    void pubWriterPages()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->addPage();
        QCOMPARE(doc->pages.size(), 2);
        for (int i = 0; i < 2; ++i) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(72, 72 + 100 * i, 300, 60);
            t->storyId = doc->createStory(i == 0 ? QStringLiteral("test13 page one") : QStringLiteral("Page two"));
            doc->pages[i]->items.push_back(t);
            auto sh = std::make_shared<jp::ShapeItem>();
            sh->shape = i == 0 ? QStringLiteral("rect") : QStringLiteral("ellipse");
            sh->rect = QRectF(100, 400, 150, 100);
            sh->fill = jp::Fill::solid(jp::ColorRef::rgb(i == 0 ? QColor(220, 30, 30) : QColor(30, 60, 200)));
            sh->stroke = jp::Stroke::none();
            doc->pages[i]->items.push_back(sh);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("test13-two-pages.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test13-two-pages.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        QCOMPARE(back->pages.size(), 2);
        for (int i = 0; i < 2; ++i) {
            QString text;
            int shapes = 0;
            jp::walkItems(back->pages[i]->items, [&](const jp::ItemPtr &it) {
                if (it->type() == jp::ItemType::Text) {
                    const QTextDocument *d = back->storyDoc(static_cast<const jp::TextItem *>(it.get())->storyId);
                    if (d) text = d->toPlainText();
                }
                if (it->type() == jp::ItemType::Shape) ++shapes;
            });
            QCOMPARE(text, i == 0 ? QStringLiteral("test13 page one") : QStringLiteral("Page two"));
            QCOMPARE(shapes, 1);
        }
    }

    // A table written to .pub reads back with its grid, text, merge, fills and rules.
    void pubWriterTable()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TableItem>();
        t->rows = 3;
        t->cols = 2;
        t->colW = {100, 150};
        t->rowH = {30, 40, 50};
        t->rect = QRectF(72, 144, 0, 0);
        t->syncRect();
        t->cells.resize(6);
        const QStringList texts = {QStringLiteral("Name"), QStringLiteral("Amount"), QStringLiteral("Paper"), QStringLiteral("$5.00"),
                                   QStringLiteral("Total due"), QString()};
        for (int i = 0; i < 6; ++i) t->cells[i].storyId = doc->createStory(texts[i]);
        const QColor gold(255, 230, 120), blue(0, 0, 200);
        for (int c = 0; c < 2; ++c) {
            t->cell(0, c).fill = jp::Fill::solid(jp::ColorRef::rgb(gold));
            t->cell(0, c).border.bottom = jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 1.5);
        }
        t->cell(1, 1).border.left = jp::Stroke::line(jp::ColorRef::rgb(blue), 1);
        t->cell(2, 0).colSpan = 2;
        t->cell(2, 1).covered = true;
        doc->pages[0]->items.push_back(t);

        // The table alone, then with a text box too (two stories).
        QTemporaryDir dir;
        QString path;
        for (const QString name : {QStringLiteral("test10-table.pub"), QStringLiteral("test11-table-text.pub")}) {
            if (name.startsWith(QLatin1String("test11"))) {
                auto label = std::make_shared<jp::TextItem>();
                label->rect = QRectF(72, 36, 400, 40);
                label->storyId = doc->createStory(QStringLiteral("test11-table-text"));
                doc->pages[0]->items.insert(doc->pages[0]->items.begin(), label);
            }
            path = dir.filePath(name);
            QString err;
            QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
            QVERIFY2(err.isEmpty(), qPrintable(err));
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
                const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/" + name;
                QFile::remove(out);
                QFile::copy(path, out);
            }
        }
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        const jp::TableItem *bt = nullptr;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Table) bt = static_cast<const jp::TableItem *>(it.get());
        });
        QVERIFY(bt);
        QCOMPARE(bt->rows, 3);
        QCOMPARE(bt->cols, 2);
        QVERIFY2(std::abs(bt->rect.left() - 72) < 1 && std::abs(bt->rect.top() - 144) < 1,
                 qPrintable(QStringLiteral("%1,%2").arg(bt->rect.left()).arg(bt->rect.top())));
        QVERIFY(std::abs(bt->colW.value(0) - 100) < 1 && std::abs(bt->colW.value(1) - 150) < 1);
        for (int r = 0; r < 3; ++r) QVERIFY2(std::abs(bt->rowH.value(r) - t->rowH[r]) < 1, qPrintable(QString::number(bt->rowH.value(r))));
        auto text = [&](int r, int c) {
            const QTextDocument *d = back->storyDoc(bt->cell(r, c).storyId);
            return d ? d->toPlainText() : QString();
        };
        QCOMPARE(text(0, 0), QStringLiteral("Name"));
        QCOMPARE(text(0, 1), QStringLiteral("Amount"));
        QCOMPARE(text(1, 1), QStringLiteral("$5.00"));
        QCOMPARE(text(2, 0), QStringLiteral("Total due"));
        QCOMPARE(bt->cell(2, 0).colSpan, 2);
        QCOMPARE(bt->cell(0, 1).fill.color.resolve(back->colors), gold);
        QVERIFY(bt->cell(1, 0).fill.type != jp::Fill::Solid);
        const jp::Stroke under = bt->cell(0, 0).border.bottom;
        QVERIFY(!under.isNone() && std::abs(under.width - 1.5) < 0.1);
        QCOMPARE(under.color.resolve(back->colors), QColor(Qt::black));
        QVERIFY(!bt->cell(1, 1).border.left.isNone());
        QCOMPARE(bt->cell(1, 1).border.left.color.resolve(back->colors), blue);
        QVERIFY(bt->cell(1, 0).border.left.isNone());
    }

    // The eyedropper picks the color under the click from the window.
    void eyedropperPicks()
    {
        QWidget win;
        win.resize(200, 100);
        win.setAutoFillBackground(true);
        QPalette pal = win.palette();
        pal.setColor(QPalette::Window, QColor(200, 30, 40));
        win.setPalette(pal);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QColor got;
        jp::pickColorFromWindow(&win, [&](const QColor &c) { got = c; });
        QWidget *overlay = nullptr;
        for (QWidget *c : win.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) overlay = c;
        QVERIFY(overlay);
        QTest::mouseClick(overlay, Qt::LeftButton, {}, QPoint(50, 50));
        QCOMPARE(got, QColor(200, 30, 40));
    }

    // Inserting a symbol puts it in the text and at the front of the recently used list.
    void recentSymbols()
    {
        const QVariant before = jp::Settings::get().value(QStringLiteral("symbols/recent"));
        jp::Settings::get().setValue(QStringLiteral("symbols/recent"), QStringList());
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("x"));
        doc->pages[0]->items.push_back(t);
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        const QString id = ed->doc()->pages[0]->items[0]->id;
        ed->select(id);
        ed->beginTextEdit(id, 1);
        QVERIFY(jp::insertSymbol(&w, ed, QString(QChar(0x00A7)), QString()));
        QVERIFY(jp::insertSymbol(&w, ed, QString(QChar(0x2122)), QStringLiteral("Symbol")));
        const QString sid = static_cast<const jp::TextItem *>(ed->doc()->pages[0]->items[0].get())->storyId;
        QCOMPARE(ed->doc()->storyDoc(sid)->toPlainText(), QString(QChar('x')) + QChar(0x00A7) + QChar(0x2122));
        const QStringList r = jp::recentSymbols();
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0], QString(QChar(0x2122)) + QStringLiteral("\tSymbol"));
        QCOMPARE(r[1], QString(QChar(0x00A7)));
        jp::Settings::get().setValue(QStringLiteral("symbols/recent"), before);
    }

    // A right-to-left paragraph keeps its direction when saved and reopened.
    void rightToLeftParagraph()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QString::fromUtf8("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            QTextBlockFormat f;
            f.setLayoutDirection(Qt::RightToLeft);
            f.setAlignment(Qt::AlignRight);
            c.mergeBlockFormat(f);
        }
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("rtl.jpub"));
        QString err;
        QVERIFY2(jp::savePublication(*doc, path, QImage(), &err), qPrintable(err));
        auto back = jp::loadPublication(path, &err);
        QVERIFY2(back, qPrintable(err));
        const jp::TextItem *bt = static_cast<const jp::TextItem *>(back->pages[0]->items.front().get());
        QCOMPARE(back->storyDoc(bt->storyId)->begin().blockFormat().layoutDirection(), Qt::RightToLeft);
    }

    // Measurement boxes keep three decimal places in any unit, without rounding.
    void measurementsThreeDecimals()
    {
        jp::Settings &st = jp::Settings::get();
        const jp::Unit was = st.unit();
        st.setUnit(jp::Unit::Inch);
        jp::MeasureSpin m;
        m.setRange(0, 72 * 240);
        double pt = 0;
        QVERIFY(st.parse(QStringLiteral("8.125"), &pt));
        m.setValue(pt);
        QCOMPARE(m.text(), QStringLiteral("8.125\""));
        QCOMPARE(m.value(), 585.0);
        QVERIFY(st.parse(QStringLiteral("11"), &pt));
        m.setValue(pt);
        QCOMPARE(m.text(), QStringLiteral("11\""));
        QVERIFY(st.parse(QStringLiteral("0.003"), &pt));
        m.setValue(pt);
        QCOMPARE(m.text(), QStringLiteral("0.003\""));
        st.setUnit(jp::Unit::Centimeter);
        QVERIFY(st.parse(QStringLiteral("1.234"), &pt));
        m.setValue(pt);
        QCOMPARE(m.text(), QStringLiteral("1.234 cm"));
        st.setUnit(jp::Unit::Point);
        QVERIFY(st.parse(QStringLiteral("10.125"), &pt));
        m.setValue(pt);
        QCOMPARE(m.text(), QStringLiteral("10.125 pt"));
        st.setUnit(was);
        jp::DecimalSpin d;
        d.setRange(0, 1000);
        d.setSuffix(QStringLiteral(" pt"));
        d.setValue(12.345);
        QCOMPARE(d.text(), QStringLiteral("12.345 pt"));
        d.setValue(12);
        QCOMPARE(d.text(), QStringLiteral("12 pt"));
    }

    // Lines, turned shapes and pictures written to .pub read back in place.
    void pubWriterObjects()
    {
        auto save = [](const jp::Document &d, const QString &name, QTemporaryDir &dir) {
            const QString path = dir.filePath(name);
            QString err;
            if (!jp::exportPublisher(d, path, &err) || !err.isEmpty()) qWarning() << err;
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
                const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/" + name;
                QFile::remove(out);
                QFile::copy(path, out);
            }
            return path;
        };
        auto line = std::make_shared<jp::LineItem>();
        line->p1 = QPointF(100, 520);
        line->p2 = QPointF(400, 450);
        line->syncRect();
        line->stroke = jp::Stroke::line(jp::ColorRef::rgb(QColor(200, 0, 0)), 3);
        line->stroke.dash = jp::Stroke::DashLine;
        line->stroke.endArrow = jp::Arrow::Triangle;
        auto rect = std::make_shared<jp::ShapeItem>();
        rect->rect = QRectF(72, 100, 200, 80);
        rect->rotation = 30;
        rect->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(30, 160, 60)));
        rect->stroke = jp::Stroke::none();
        auto oval = std::make_shared<jp::ShapeItem>();
        oval->shape = QStringLiteral("ellipse");
        oval->rect = QRectF(350, 100, 160, 60);
        oval->rotation = 90;
        oval->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(30, 60, 200)));
        oval->stroke = jp::Stroke::none();
        QImage img(40, 20, QImage::Format_RGB32);
        img.fill(QColor(220, 30, 30));
        for (int y = 0; y < 20; ++y)
            for (int x = 20; x < 40; ++x) img.setPixelColor(x, y, QColor(30, 30, 220));
        QByteArray png;
        {
            QBuffer b(&png);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "PNG");
        }

        QTemporaryDir dir;
        // Each object alone, with a label (text already opens in Publisher).
        auto one = [&](const jp::ItemPtr &it, const QString &name, std::function<void(jp::Document &)> prep = {}) {
            auto d = jp::Document::blank(QSizeF(612, 792));
            if (prep) prep(*d);
            auto label = std::make_shared<jp::TextItem>();
            label->rect = QRectF(72, 36, 400, 40);
            label->storyId = d->createStory();
            QTextCursor(d->storyDoc(label->storyId)).insertText(QFileInfo(name).completeBaseName());
            d->pages[0]->items.push_back(label);
            d->pages[0]->items.push_back(it->clone());
            return save(*d, name, dir);
        };
        one(line, QStringLiteral("test6-line.pub"));
        one(rect, QStringLiteral("test7-rotated.pub"));
        auto pic = std::make_shared<jp::PictureItem>();
        pic->rect = QRectF(100, 600, 160, 80);
        pic->imgRect = QRectF(0, 0, 160, 80);
        one(pic, QStringLiteral("test8-picture.pub"), [&](jp::Document &d) { pic->imageId = d.addImage(png, QStringLiteral("png"), QStringLiteral("/tmp/two-colors.png")); });

        auto doc = jp::Document::blank(QSizeF(612, 792));
        pic->imageId = doc->addImage(png, QStringLiteral("png"), QStringLiteral("/tmp/two-colors.png"));
        for (const jp::ItemPtr &it : jp::ItemList{line, rect, oval, pic}) doc->pages[0]->items.push_back(it);
        const QString path = save(*doc, QStringLiteral("test9-all.pub"), dir);
        QString e1;
        auto back = jp::importPublisherFile(path, &e1);
        QVERIFY2(back, qPrintable(e1));
        const jp::LineItem *bl = nullptr;
        const jp::PictureItem *bp = nullptr;
        QVector<const jp::ShapeItem *> shapes;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() == jp::ItemType::Line) bl = static_cast<const jp::LineItem *>(it.get());
            if (it->type() == jp::ItemType::Picture) bp = static_cast<const jp::PictureItem *>(it.get());
            if (it->type() == jp::ItemType::Shape) shapes << static_cast<const jp::ShapeItem *>(it.get());
        });
        auto near = [](QPointF a, QPointF b) { return QLineF(a, b).length() < 1.5; };
        QVERIFY(bl);
        QVERIFY2(near(bl->p1, line->p1) && near(bl->p2, line->p2),
                 qPrintable(QStringLiteral("%1,%2 %3,%4").arg(bl->p1.x()).arg(bl->p1.y()).arg(bl->p2.x()).arg(bl->p2.y())));
        QVERIFY(std::abs(bl->stroke.width - 3) < 0.1);
        QVERIFY(bl->stroke.dash != jp::Stroke::SolidLine);
        QCOMPARE(bl->stroke.color.resolve(back->colors), QColor(200, 0, 0));
        QVERIFY(bl->stroke.endArrow != jp::Arrow::None);
        QCOMPARE(bl->stroke.startArrow, jp::Arrow::None);
        QCOMPARE(shapes.size(), 2);
        // The rectangle and the oval come back turned, as themselves.
        for (const jp::ShapeItem *s : shapes) {
            const QString got = QStringLiteral("rot %1 %2,%3 %4x%5").arg(s->rotation).arg(s->rect.x()).arg(s->rect.y()).arg(s->rect.width()).arg(s->rect.height());
            QVERIFY2(s->customPath.isEmpty(), qPrintable(got));
            if (s->shape == QLatin1String("rect")) {
                QVERIFY2(std::abs(s->rotation - 30) < 0.5, qPrintable(got));
                QVERIFY2(QLineF(s->rect.center(), rect->rect.center()).length() < 1 && std::abs(s->rect.width() - 200) < 1 &&
                             std::abs(s->rect.height() - 80) < 1, qPrintable(got));
            } else {
                QCOMPARE(s->shape, QStringLiteral("ellipse"));
                QVERIFY2(std::abs(s->rotation - 90) < 0.5 && QLineF(s->rect.center(), oval->rect.center()).length() < 1
                             && std::abs(s->rect.width() - 160) < 1 && std::abs(s->rect.height() - 60) < 1, qPrintable(got));
            }
        }
        QVERIFY(bp);
        QVERIFY(std::abs(bp->rect.left() - 100) < 1 && std::abs(bp->rect.width() - 160) < 1 && std::abs(bp->rect.height() - 80) < 1);
        const QImage got = back->image(bp->imageId);
        QCOMPARE(got.size(), QSize(40, 20));
        QCOMPARE(got.pixelColor(5, 5), QColor(220, 30, 30));
        QCOMPARE(got.pixelColor(35, 5), QColor(30, 30, 220));
    }

    // Catalog merge: the first cell of the catalog area repeats for each
    // record, a pageful at a time; objects outside the area stay on every page.
    void catalogMerge()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->merge.fields = {QStringLiteral("Name"), QStringLiteral("Price")};
        const QStringList names = {"Apples", "Bread", "Cheese", "Dates", "Eggs"};
        for (int i = 0; i < names.size(); ++i) doc->merge.rows << QStringList{names[i], QString::number(i + 1)};
        doc->catalog.pageId = doc->pages[0]->id;
        doc->catalog.rect = QRectF(36, 72, 540, 684);
        doc->catalog.rows = 2;
        doc->catalog.cols = 2;
        QCOMPARE(doc->catalog.perPage(), 4);
        QCOMPARE(doc->catalog.cell(3), QRectF(306, 414, 270, 342));
        auto title = std::make_shared<jp::TextItem>();
        title->rect = QRectF(36, 20, 540, 40);
        title->storyId = doc->createStory(QStringLiteral("Our catalog"));
        doc->pages[0]->items.push_back(title);
        auto box = std::make_shared<jp::TextItem>();
        box->rect = QRectF(48, 84, 200, 30);
        box->storyId = doc->createStory();
        {
            QTextCursor c(doc->storyDoc(box->storyId));
            QTextCharFormat cf;
            cf.setProperty(jp::tp::Field, QStringLiteral("merge:Name"));
            c.insertText(QString(QChar::ObjectReplacementCharacter), cf);
            c.insertText(QStringLiteral(" $"), QTextCharFormat());
            cf.setProperty(jp::tp::Field, QStringLiteral("merge:Price"));
            c.insertText(QString(QChar::ObjectReplacementCharacter), cf);
        }
        doc->pages[0]->items.push_back(box);
        QVERIFY(doc->catalog.inTemplate(box->bounds()));
        QVERIFY(!doc->catalog.inTemplate(title->bounds()));

        // The area survives saving.
        QString err;
        auto again = jp::publicationFromBytes(jp::publicationBytes(*doc, QImage()), &err);
        QVERIFY2(again, qPrintable(err));
        QCOMPARE(again->catalog.pageId, doc->catalog.pageId);
        QCOMPARE(again->catalog.rect, doc->catalog.rect);
        QCOMPARE(again->catalog.cols, 2);

        auto merged = jp::mergeToNewPublication(*doc);
        QCOMPARE(merged->pages.size(), 2);
        QVERIFY(!merged->catalog.isActive());
        auto texts = [&](int page) {
            QMap<QString, QPointF> out;
            for (const auto &it : merged->pages[page]->items)
                if (it->type() == jp::ItemType::Text)
                    out.insert(merged->storyDoc(static_cast<const jp::TextItem *>(it.get())->storyId)->toPlainText(), it->rect.topLeft());
            return out;
        };
        const auto p1 = texts(0), p2 = texts(1);
        QCOMPARE(p1.size(), 5);
        QCOMPARE(p2.size(), 2);
        QVERIFY(p1.contains("Our catalog") && p2.contains("Our catalog"));
        QCOMPARE(p1.value("Apples $1"), QPointF(48, 84));
        QCOMPARE(p1.value("Bread $2"), QPointF(318, 84));
        QCOMPARE(p1.value("Cheese $3"), QPointF(48, 426));
        QCOMPARE(p1.value("Dates $4"), QPointF(318, 426));
        QCOMPARE(p2.value("Eggs $5"), QPointF(48, 84));

        // Previewing a record paints the next records in the other cells.
        jp::PaintContext ctx;
        ctx.doc = doc.get();
        jp::LayoutCache cache;
        ctx.cache = &cache;
        ctx.opt.output = true;
        auto inkIn = [&](int record, const QRectF &r) {
            ctx.opt.mergeRecord = record;
            QImage img(612, 792, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            jp::Renderer::paintPage(&p, ctx, 0);
            p.end();
            int dark = 0;
            for (int y = int(r.top()); y < int(r.bottom()); ++y)
                for (int x = int(r.left()); x < int(r.right()); ++x) dark += qGray(img.pixel(x, y)) < 128;
            return dark;
        };
        const QRectF cell4(306, 414, 270, 60);
        QVERIFY(inkIn(0, cell4) > 20);      // Dates in the fourth cell
        QCOMPARE(inkIn(-1, cell4), 0);       // unmerged output prints only the template
        QCOMPARE(inkIn(4, QRectF(306, 72, 270, 60)), 0);   // Eggs is the last: nothing after it

        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            jp::MainWindow w;
            w.resize(1400, 900);
            w.editor()->setDocument(std::move(doc));
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            w.showTaskPane(QStringLiteral("catalog"));
            QTest::qWait(200);
            w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/catalog-design.png");
            w.editor()->setMergeRecord(0);
            QTest::qWait(200);
            w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/catalog-preview.png");
        }
    }

    // Spot colors: saved with their names, offered in the color drop-downs,
    // and printed on plates of their own (tints as lighter ink), knocked out
    // of the process plates.
    void spotColorPlates()
    {
        const QColor spot(0, 90, 170);
        QCOMPARE(jp::spotAmount(spot, spot), 1.0);
        QVERIFY(std::abs(jp::spotAmount(jp::mix(spot, Qt::white, 0.6), spot) - 0.4) < 0.02);
        QCOMPARE(jp::spotAmount(QColor(200, 30, 30), spot), -1.0);
        QCOMPARE(jp::spotAmount(Qt::white, spot), -1.0);

        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->print.model = jp::PrintInfo::ProcessPlusSpot;
        doc->print.spotColors = {spot};
        doc->print.spotNames = {QStringLiteral("Harbor Blue")};
        auto box = [&](const QRectF &r, const QColor &c) {
            auto s = std::make_shared<jp::ShapeItem>();
            s->rect = r;
            s->fill = jp::Fill::solid(jp::ColorRef::rgb(c));
            s->stroke = jp::Stroke::none();
            doc->pages[0]->items.push_back(s);
        };
        box(QRectF(72, 72, 100, 100), spot);                                // the spot color
        box(QRectF(200, 72, 100, 100), jp::mix(spot, Qt::white, 0.5));     // a 50% tint
        box(QRectF(330, 72, 100, 100), QColor(200, 30, 30));                // red, process
        QString err;
        auto again = jp::publicationFromBytes(jp::publicationBytes(*doc, QImage()), &err);
        QCOMPARE(again->print.spotName(0), QStringLiteral("Harbor Blue"));
        QCOMPARE(again->print.spotColors.first(), spot);
        QCOMPARE(jp::plateName(4, doc.get()), QStringLiteral("Harbor Blue"));

        auto ink = [](const QImage &plate, const QPointF &pt) { return 1 - qGray(plate.pixel(int(pt.x() * 72 / 72), int(pt.y()))) / 255.0; };
        const QImage spotPlate = jp::renderPlate(*doc, 0, 4, 72);
        QVERIFY(ink(spotPlate, QPointF(122, 122)) > 0.95);
        QVERIFY(std::abs(ink(spotPlate, QPointF(250, 122)) - 0.5) < 0.06);
        QVERIFY(ink(spotPlate, QPointF(380, 122)) < 0.02);
        const QImage cyan = jp::renderPlate(*doc, 0, 0, 72), magenta = jp::renderPlate(*doc, 0, 1, 72);
        QVERIFY(ink(cyan, QPointF(122, 122)) < 0.02);       // knocked out
        QVERIFY(ink(cyan, QPointF(250, 122)) < 0.02);
        QVERIFY(ink(magenta, QPointF(380, 122)) > 0.7);     // red stays on the process plates
        // Text colors go through the plates too.
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 300, 400, 80);
        t->storyId = doc->createStory(QStringLiteral("HHHHHHHH"));
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setFontPointSize(48);
            f.setProperty(jp::tp::ColorRefP, jp::ColorRef::rgb(spot).toString());
            c.mergeCharFormat(f);
        }
        doc->pages[0]->items.push_back(t);
        auto inkIn = [](const QImage &plate, const QRect &r) {
            double sum = 0;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) sum += 1 - qGray(plate.pixel(x, y)) / 255.0;
            return sum;
        };
        QVERIFY(inkIn(jp::renderPlate(*doc, 0, 4, 72), QRect(72, 300, 400, 80)) > 300);
        QVERIFY(inkIn(jp::renderPlate(*doc, 0, 0, 72), QRect(72, 300, 400, 80)) < 5);

        // The Commercial Print tab lists the spot colors.
        jp::MainWindow w;
        w.editor()->setDocument(std::move(doc));
        // A PDF names the ink: Separation colors at the spot's strength.
        {
            QTemporaryDir pd;
            const QString pdfPath = pd.filePath(QStringLiteral("spot.pdf"));
            QVERIFY(w.exportPdfTo(pdfPath, jp::MainWindow::PdfSettings()));
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) { QFile::remove(qEnvironmentVariable("JP_SHOT_DIR") + "/spot.pdf"); QFile::copy(pdfPath, qEnvironmentVariable("JP_SHOT_DIR") + "/spot.pdf"); }
            QFile pf(pdfPath);
            QVERIFY(pf.open(QIODevice::ReadOnly));
            const QByteArray bytes = pf.readAll();
            QVERIFY(bytes.contains("/Separation /Harbor#20Blue /DeviceCMYK"));
            QVERIFY(bytes.contains("startxref"));
            QByteArray text;
            for (qsizetype at = 0; (at = bytes.indexOf(">>\nstream\n", at)) >= 0; at += 10) {
                const qsizetype st = at + 10, en = bytes.indexOf("\nendstream", st);
                bool ok = false;
                text += jp::QtPdf::inflate(bytes.mid(st, en - st), &ok);   // pictures aren't zlib: nothing
            }
            QVERIFY2(text.contains("/CSspot0 cs 1 scn"), text.left(300).constData());
            QVERIFY(text.contains("/CSspot0 cs 0.5 scn") || text.contains("/CSspot0 cs 0.4"));
            QVERIFY(text.contains("/CSpcmyk cs"));   // the red stays process
        }
        QTimer::singleShot(0, [] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *list = dlg->findChild<QListWidget *>();
            QVERIFY(list);
            QCOMPARE(list->count(), 1);
            QCOMPARE(list->item(0)->text(), QStringLiteral("Harbor Blue"));
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) dlg->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/commercial-print.png");
            dlg->reject();
        });
        jp::documentPropertiesDialog(&w, w.editor(), 1);
    }

    // A color given as ink amounts keeps them: in the file, in tints and
    // shades, from the Colors dialog, and in the PDF of a process-color
    // publication.
    // The New Publication page lists its template categories in view, and
    // picking one shows that category's templates.
    void newPageCategories()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        w.showBackstage(QStringLiteral("new"));
        QApplication::processEvents();
        QListWidget *cats = nullptr;
        for (QListWidget *l : w.findChildren<QListWidget *>())
            if (l->count() > 3 && l->item(0)->data(Qt::UserRole).toString() == QLatin1String("Featured")) cats = l;
        QVERIFY(cats);
        QVERIFY(cats->isVisible());
        QCOMPARE(cats->item(cats->count() - 1)->data(Qt::UserRole).toString(), QStringLiteral("My Templates"));
        QListWidget *grid = nullptr;
        for (QListWidget *l : cats->parentWidget()->findChildren<QListWidget *>())
            if (l != cats && l->viewMode() == QListView::IconMode) grid = l;
        QVERIFY(grid);
        cats->setCurrentRow(1);
        QApplication::processEvents();
        const QString label = cats->item(1)->text();
        const int want = label.mid(label.lastIndexOf('(') + 1).chopped(1).toInt();
        QVERIFY(want > 0);
        QCOMPARE(grid->count(), want);
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            cats->setCurrentRow(0);
            QApplication::processEvents();
            w.screenshotTo(qEnvironmentVariable("JP_SHOT_DIR") + "/new-page.png");
        }
    }

    // Clicking a category lists its templates at once with blank pages; the
    // thumbnails are drawn afterward between events, a newer click replaces
    // the work left, and thumbnails already made come back at once.
    void newPageThumbnailsFillAfter()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        w.showBackstage(QStringLiteral("new"));
        QApplication::processEvents();
        QListWidget *cats = nullptr, *grid = nullptr;
        for (QListWidget *l : w.findChildren<QListWidget *>())
            if (l->count() > 3 && l->item(0)->data(Qt::UserRole).toString() == QLatin1String("Featured")) cats = l;
        QVERIFY(cats);
        for (QListWidget *l : cats->parentWidget()->findChildren<QListWidget *>())
            if (l != cats && l->viewMode() == QListView::IconMode) grid = l;
        QVERIFY(grid);
        auto row = [&](const QString &key) {
            for (int i = 0; i < cats->count(); ++i)
                if (cats->item(i)->data(Qt::UserRole).toString() == key) return i;
            return -1;
        };
        const int calendars = row(QStringLiteral("Calendars")), banners = row(QStringLiteral("Banners"));
        QVERIFY(calendars > 0 && banners > 0);
        // Distinct thumbnails in the grid (all blank placeholders count as one).
        auto distinct = [&] {
            QSet<qint64> keys;
            for (int i = 0; i < grid->count(); ++i) keys.insert(grid->item(i)->icon().cacheKey());
            return keys.size();
        };
        cats->setCurrentRow(calendars);
        QVERIFY(grid->count() >= 2);
        QCOMPARE(distinct(), 1);   // listed, nothing drawn yet
        cats->setCurrentRow(banners);   // before any calendar was drawn
        const int bannerCount = grid->count();
        QVERIFY(bannerCount >= 2);
        QTRY_COMPARE(distinct(), bannerCount);
        cats->setCurrentRow(calendars);
        QTRY_COMPARE(distinct(), grid->count());
        cats->setCurrentRow(banners);
        QCOMPARE(distinct(), bannerCount);   // kept, no waiting
    }

    // How a .pub file packs process inks, checked against values Publisher
    // wrote (three reference files) and read (single inks, checked one at a
    // time in its color dialog).
    void pubInkPacking()
    {
        using P = QPair<quint32, quint32>;
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(250 / 255.f, 194 / 255.f, 34 / 255.f, 77 / 255.f)), P(0x4585F5E8u, 308u));
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(0.6f, 0.4f, 0.4f, 1.0f)), P(0x4CCD33E8u, 1021u));
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(192 / 255.f, 0.4f, 0, 0)), P(0x00CD8188u, 0u));
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(1, 0, 0, 0)), P(0x0001FF08u, 0u));   // read as C 100
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(0, 1, 0, 0)), P(0x0001FE88u, 0u));   // read as M 100
        // Only the inks used are packed, in C M Y K order.
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(0, 0, 1, 0)), P(0x0001FE48u, 0u));                // read as Y 100
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(0, 0, 0, 1)), P(0x0001FE28u, 0u));                // read as K 100
        QCOMPARE(jp::packPubInks(QColor::fromCmykF(0, 128 / 255.f, 1, 51 / 255.f)), P(0x67FF00E8u, 0u));   // read as M 50 Y 100 K 20
    }

    // JeffPub opens with the window size it had when it last closed.
    void windowSizeKept()
    {
        {
            jp::MainWindow w;
            w.show();
            w.resize(640, 480);   // within the test screen (a restored window is kept on screen)
            QApplication::processEvents();
            QVERIFY(w.close());
        }
        {
            jp::MainWindow again;
            QCOMPARE(again.size(), QSize(640, 480));
        }
        jp::Settings::get().setValue(QStringLiteral("ui/windowGeometry"), QVariant());
        jp::MainWindow fresh;
        QCOMPARE(fresh.size(), QSize(1400, 900));
    }

    // Character scaling (0x20, tenths of a percent, like tracking): a flyer's
    // motto at 100.1% came out 40 times as wide (read as 10010%). The test
    // file turns a saved tracking value into a scaling one in place.
    void pubTextScaleRead()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 400, 100);
        t->storyId = doc->createStory(QStringLiteral("Ever Vigilant Ever Ready"));
        doc->pages[0]->items.push_back(t);
        QTextCursor all(doc->storyDoc(t->storyId));
        all.select(QTextCursor::Document);
        QTextCharFormat tf;
        tf.setProperty(jp::tp::Tracking, 123.4);
        all.mergeCharFormat(tf);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("scale.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        jp::cfb::File c;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY2(jp::cfb::read(f.readAll(), &c, &err), qPrintable(err));
        }
        const QString quill = QStringLiteral("Quill/QuillSub/CONTENTS");
        QByteArray q = c.stream(quill);
        const QByteArray tracking("\x1f\x1a\xd2\x04", 4), scaling("\x20\x1a\xdc\x05", 4);   // 1234 -> 1500 (150%)
        QVERIFY(q.contains(tracking));
        q.replace(tracking, scaling);
        QVERIFY(c.setStream(quill, q));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(jp::cfb::write(c));
        }
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QTextDocument *story = nullptr;
        for (const auto &it : back->pages[0]->items)
            if (auto tx = std::dynamic_pointer_cast<jp::TextItem>(it)) story = back->storyDoc(tx->storyId);
        QVERIFY(story);
        QTextCursor at(story);
        at.setPosition(3);
        QCOMPARE(at.charFormat().fontStretch(), 150);
    }

    // Saving keeps spot inks and character scaling: the fill names its ink
    // exactly as Publisher's covers do, and both come back on opening.
    void pubWriterSpotAndScaling()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        const QColor ink = QColor::fromCmyk(192, 102, 0, 0);
        doc->print.model = jp::PrintInfo::SpotColors;
        doc->print.spotColors = {ink};
        doc->print.spotNames = {QStringLiteral("PANTONE 2727 C")};
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(72, 72, 300, 200);
        box->fill = jp::Fill::solid(jp::ColorRef::inks(ink, QColor(0x3d, 0x7e, 0xdb)));
        box->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(box);
        auto text = [&](const QRectF &r, const QString &words, int stretch) {
            auto t = std::make_shared<jp::TextItem>();
            t->rect = r;
            t->storyId = doc->createStory(words);
            doc->pages[0]->items.push_back(t);
            if (stretch != 100) {
                QTextCursor all(doc->storyDoc(t->storyId));
                all.select(QTextCursor::Document);
                QTextCharFormat wide;
                wide.setFontStretch(stretch);
                all.mergeCharFormat(wide);
            }
        };
        text(QRectF(72, 400, 400, 40), QStringLiteral("Wide words"), 150);   // checked below
        // Labels for checking the file in Publisher.
        text(QRectF(72, 280, 400, 30), QStringLiteral("A: the box above is PANTONE 2727 C (a spot color)"), 100);
        text(QRectF(72, 440, 400, 30), QStringLiteral("B: the words above are scaled 150%"), 100);
        text(QRectF(72, 500, 400, 30), QStringLiteral("Narrow words"), 80);
        text(QRectF(72, 540, 400, 30), QStringLiteral("C: the words above are scaled 80%"), 100);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("spot-scale.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test33-spot-and-scaling.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        jp::cfb::File c;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY2(jp::cfb::read(f.readAll(), &c, &err), qPrintable(err));
        }
        const QString name = QStringLiteral("P2,#003d007e00db0000,PANTONE 2727 C");
        QVERIFY(c.stream(QStringLiteral("Escher/EscherStm")).contains(QByteArray(reinterpret_cast<const char *>(name.utf16()), name.size() * 2)));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QVERIFY(back->print.usesSpots());
        QCOMPARE(back->print.spotNames, QStringList{QStringLiteral("PANTONE 2727 C")});
        QTextDocument *story = nullptr;
        for (const auto &it : back->pages[0]->items)
            if (auto tx = std::dynamic_pointer_cast<jp::TextItem>(it); tx && !story) story = back->storyDoc(tx->storyId);
        QVERIFY(story);
        QCOMPARE(story->toPlainText().trimmed(), QStringLiteral("Wide words"));
        QTextCursor at(story);
        at.setPosition(2);
        QCOMPARE(at.charFormat().fontStretch(), 150);
    }

    // A .pub saved by JeffPub carries a preview picture of page 1 as
    // Publisher writes one (a metafile copying in a 24-bit bitmap), which the
    // Open page then shows, and each run's language, which comes back.
    void pubPreviewAndLanguages()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto band = std::make_shared<jp::ShapeItem>();
        band->shape = QStringLiteral("rect");
        band->rect = QRectF(0, 0, 612, 300);
        band->fill = jp::Fill::solid(jp::ColorRef::rgb(QColor(200, 30, 40)));
        band->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(band);
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 360, 468, 300);
        t->storyId = doc->createStory(QStringLiteral("This text is in English.\nEste texto está en español.\nCe texte est en français.\nDieser Text ist auf Deutsch."));
        doc->pages[0]->items.push_back(t);
        const char *langs[] = {"en-US", "es-MX", "fr-FR", "de-DE"};
        QTextBlock b = doc->storyDoc(t->storyId)->begin();
        for (const char *lang : langs) {
            QTextCursor c(b);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            QTextCharFormat f;
            f.setProperty(jp::tp::Language, QString::fromLatin1(lang));
            f.setFontPointSize(24);
            c.mergeCharFormat(f);
            b = b.next();
        }
        w.editor()->setDocument(std::move(doc));
        const QImage thumb = w.pageThumbnail(0, 160);
        QCOMPARE(qMax(thumb.width(), thumb.height()), 160);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("preview.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*w.editor()->doc(), path, &err, thumb), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test35-preview-and-languages.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        // Publisher's layout: property 17, a clipboard metafile picture whose
        // metafile fills white and copies in the bitmap.
        const QByteArray si = jp::cfb::readStream(path, QStringLiteral("\x05SummaryInformation"));
        const qsizetype at = si.indexOf(QByteArray::fromHex("47000000"));
        QVERIFY(at > 0);
        QCOMPARE(si.mid(at + 8, 10), QByteArray::fromHex("ffffffff030000000800"));
        const QByteArray wmf = si.mid(at + 24);
        QCOMPARE(wmf.left(6), QByteArray::fromHex("010009000003"));
        for (const char *rec : {"05000000 0b02", "05000000 0c02", "07000000 fc02", "04000000 2d01", "09000000 1d06", "4009", "03000000 0000"})
            QVERIFY2(wmf.contains(QByteArray::fromHex(QByteArray(rec).replace(' ', ""))), rec);
        // Read back as the Open page shows it: red at the top, white below.
        const QImage back = jp::publicationThumbnail(path);
        QVERIFY(!back.isNull());
        QVERIFY(qAbs(double(back.width()) / back.height() - double(thumb.width()) / thumb.height()) < 0.02);   // drawn at the Open page's size
        const QColor top = back.pixelColor(back.width() / 2, 5), low = back.pixelColor(5, back.height() - 5);
        QVERIFY2(top.red() > 180 && top.green() < 60, qPrintable(top.name()));
        QVERIFY2(low.lightness() > 240, qPrintable(low.name()));
        // Each run's language code in both of Publisher's places (Spanish
        // (Mexico) 0x080A; Publisher showed all four languages, test35, Oct 6).
        const QByteArray quill = jp::cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        QVERIFY(quill.contains(QByteArray::fromHex("12220a080000")) && quill.contains(QByteArray::fromHex("3e220a080000")));
        QVERIFY(quill.contains(QByteArray::fromHex("12220c040000")) && quill.contains(QByteArray::fromHex("12220704" "0000")));
        // The languages, through the independent reader.
        auto reopened = jp::importPublisherFile(path, &err);
        QVERIFY2(reopened, qPrintable(err));
        QTextDocument *story = nullptr;
        for (const auto &it : reopened->pages[0]->items)
            if (auto tx = std::dynamic_pointer_cast<jp::TextItem>(it)) story = reopened->storyDoc(tx->storyId);
        QVERIFY(story);
        QStringList got;
        for (QTextBlock blk = story->begin(); blk.isValid(); blk = blk.next()) {
            QTextCursor c(blk);
            c.movePosition(QTextCursor::NextCharacter);
            got << c.charFormat().stringProperty(jp::tp::Language);
        }
        QCOMPARE(got.mid(0, 4), (QStringList{"en-US", "es-MX", "fr-FR", "de-DE"}));
        // A bigger picture is fitted to Publisher's size.
        QVERIFY(jp::exportPublisher(*w.editor()->doc(), path, &err, w.pageThumbnail(0, 2000)));
        const QImage fitted = jp::publicationThumbnail(path);
        QVERIFY(!fitted.isNull());
        const QByteArray si2 = jp::cfb::readStream(path, QStringLiteral("\x05SummaryInformation"));
        const qsizetype copy = si2.indexOf(QByteArray::fromHex("2000cc00"));   // DIBBitBlt's SRCCOPY, then six numbers and the bitmap
        QVERIFY(copy > 0);
        QVERIFY(qFromLittleEndian<quint32>(si2.constData() + copy + 4 + 12 + 4) < 160);              // bitmap width
        QCOMPARE(qFromLittleEndian<quint32>(si2.constData() + copy + 4 + 12 + 8), quint32(160));   // and height
    }

    // A right-to-left paragraph through .pub: written as in a sample made in
    // Publisher (0x06 = 0 and 0x3A = 0xF3FF on that paragraph only; 0x3A
    // appears in none of 247 left-to-right reference files), read back.
    void pubRightToLeft()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 400, 100);
        t->storyId = doc->createStory(QStringLiteral("Left to Right\nRight to Left"));
        doc->pages[0]->items.push_back(t);
        QTextBlock second = doc->storyDoc(t->storyId)->begin().next();
        QVERIFY(second.isValid());
        QTextCursor c(second);
        QTextBlockFormat bf = second.blockFormat();
        bf.setLayoutDirection(Qt::RightToLeft);
        c.setBlockFormat(bf);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("rtl.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray quill = jp::cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"));
        // Both in the right-to-left paragraph's property list (kept in id order).
        QCOMPARE(quill.count(QByteArray::fromHex("3a12fff3")), 1);
        const qsizetype at = quill.indexOf(QByteArray::fromHex("3a12fff3"));
        QVERIFY(quill.mid(at - 40, 40).contains(QByteArray::fromHex("062200000000")));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QTextDocument *story = nullptr;
        for (const auto &it : back->pages[0]->items)
            if (auto tx = std::dynamic_pointer_cast<jp::TextItem>(it)) story = back->storyDoc(tx->storyId);
        QVERIFY(story);
        QVERIFY(story->begin().blockFormat().layoutDirection() != Qt::RightToLeft);
        QCOMPARE(story->begin().next().blockFormat().layoutDirection(), Qt::RightToLeft);
        // Left-aligned (its start) is at the right edge for right-to-left text.
        jp::FrameSpec fs;
        fs.size = QSizeF(400, 100);
        fs.insets = QMarginsF(0, 0, 0, 0);
        jp::StoryLayout lay;
        lay.build(story, {fs}, jp::LayoutEnv());
        const QTextBlock b1 = story->begin(), b2 = b1.next();
        const QRectF ltr = lay.rangeRects(0, b1.position(), b1.position() + b1.length() - 1).value(0);
        const QRectF rtl = lay.rangeRects(0, b2.position(), b2.position() + b2.length() - 1).value(0);
        QVERIFY2(ltr.left() < 5 && rtl.right() > 395, qPrintable(QStringLiteral("%1 %2").arg(ltr.left()).arg(rtl.right())));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test34-right-to-left.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
    }

    // Every dash style comes back from .pub as it was saved (square dots
    // came back as dashes, and dash-dot patterns as plain dashes).
    void pubDashStylesRoundTrip()
    {
        using S = jp::Stroke;
        const S::Dash dashes[] = {S::RoundDot, S::SquareDot, S::DashLine, S::DashDot, S::LongDash, S::LongDashDot, S::LongDashDotDot};
        auto doc = jp::Document::blank(QSizeF(612, 792));
        for (int i = 0; i < 7; ++i) {
            auto line = std::make_shared<jp::LineItem>();
            line->p1 = QPointF(72, 72 + i * 60);
            line->p2 = QPointF(400, 72 + i * 60);
            line->rect = QRectF(line->p1, line->p2).normalized();
            line->stroke = S::line(jp::ColorRef::rgb(Qt::black), 4);
            line->stroke.dash = dashes[i];
            doc->pages[0]->items.push_back(line);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("dashes.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QList<int> got;
        for (const auto &it : back->pages[0]->items)
            if (auto l = std::dynamic_pointer_cast<jp::LineItem>(it)) got << int(l->stroke.dash);
        QList<int> want;
        for (S::Dash d : dashes) want << int(d);
        QCOMPARE(got, want);
    }

    // Several ruler guides at once: quick adds no longer stack at the page's
    // middle, and the Ruler Guides dialog adds, moves, removes and adds a
    // series of evenly spaced guides in one step.
    void rulerGuidesSeveral()
    {
        QVector<double> g{396};
        QCOMPARE(jp::RulerGuides::freeSpot(g, 396, 36, 792), 432.0);
        g << 432;
        QCOMPARE(jp::RulerGuides::freeSpot(g, 396, 36, 792), 468.0);
        QCOMPARE(jp::RulerGuides::freeSpot(g, 100, 36, 792), 100.0);
        QVector<double> s;
        QCOMPARE(jp::RulerGuides::addSeries(&s, 72, 72, 20, 612), 8);   // 72..576, stops at the page edge
        QCOMPARE(s.last(), 576.0);
        QCOMPARE(jp::RulerGuides::addSeries(&s, 72, 144, 3, 612), 0);   // all already there

        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.act(QStringLiteral("pd.addH"))->trigger();
        w.act(QStringLiteral("pd.addH"))->trigger();
        w.act(QStringLiteral("pd.addV"))->trigger();
        const jp::RulerGuides &rg = w.editor()->surface()->guides;
        QCOMPARE(rg.h, (QVector<double>{396, 432}));
        QCOMPARE(rg.v, (QVector<double>{306}));

        // The dialog, as a user would: a guide at 2 in on the horizontal side,
        // a series of three vertical guides from 1 in every 1 in, then remove
        // the 432 pt horizontal one.
        QTimer::singleShot(0, &w, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            const auto boxes = d->findChildren<QGroupBox *>(QString(), Qt::FindDirectChildrenOnly);
            QCOMPARE(boxes.size(), 2);
            auto button = [](QWidget *in, const QString &text) {
                for (auto *b : in->findChildren<QPushButton *>())
                    if (b->text() == text) return b;
                return static_cast<QPushButton *>(nullptr);
            };
            auto spins = [](QWidget *in) { return in->findChildren<jp::MeasureSpin *>(); };
            QWidget *hBox = boxes[0], *vBox = boxes[1];
            spins(hBox).first()->setValue(144);
            button(hBox, QStringLiteral("Add"))->click();
            auto *series = vBox->findChild<QGroupBox *>();
            series->findChild<QSpinBox *>()->setValue(3);
            const auto ss = spins(series);
            ss[0]->setValue(72);
            ss[1]->setValue(72);
            button(vBox, QStringLiteral("Add Series"))->click();
            auto *hList = hBox->findChild<QListWidget *>();
            QCOMPARE(hList->count(), 3);
            hList->setCurrentRow(2);   // 432 pt, sorted after 144 and 396
            button(hBox, QStringLiteral("Remove"))->click();
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) d->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/ruler-guides.png");
            d->accept();
        });
        jp::rulerGuidesDialog(&w, w.editor());
        QCOMPARE(rg.h, (QVector<double>{144, 396}));
        QCOMPARE(rg.v, (QVector<double>{72, 144, 216, 306}));
        // One undo step takes the dialog's changes back.
        w.editor()->undoStack()->undo();
        QCOMPARE(w.editor()->surface()->guides.h, (QVector<double>{396, 432}));
    }

    // A saved PDF opens in the system's viewer when the option is on (the
    // default), not when it's off, and never for command-line exports.
    void pdfOpensAfterSaving()
    {
        QStringList opened;
        const auto hook = jp::MainWindow::openFileHook;
        jp::MainWindow::openFileHook = [&](const QString &path) { opened << path; return true; };
        const bool was = jp::MainWindow::openPdfAfterSaving();
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QTemporaryDir dir;
        const QString a = dir.filePath(QStringLiteral("a.pdf")), b = dir.filePath(QStringLiteral("b.pdf")), c = dir.filePath(QStringLiteral("c.pdf"));
        jp::MainWindow::setOpenPdfAfterSaving(true);
        QVERIFY(w.exportPdfTo(a, jp::MainWindow::PdfSettings()));   // a window not shown, as from the command line
        w.show();
        QVERIFY(w.exportPdfTo(b, jp::MainWindow::PdfSettings()));
        jp::MainWindow::setOpenPdfAfterSaving(false);
        QVERIFY(w.exportPdfTo(c, jp::MainWindow::PdfSettings()));
        QCOMPARE(opened, QStringList{b});
        jp::MainWindow::setOpenPdfAfterSaving(was);
        jp::MainWindow::openFileHook = hook;
    }

    // Barcodes: ISBN-10 and -13 with their check digits, price add-ons, and
    // every symbology's bars (the patterns pinned here decoded correctly with
    // an independent reader, zbar: ISBN with price as 978030640615751995).
    void barcodeEncodings()
    {
        using namespace jp::barcode;
        QString err;
        QCOMPARE(isbn13(QStringLiteral("0-306-40615-2"), &err), QStringLiteral("9780306406157"));
        QCOMPARE(isbn13(QStringLiteral("978-0-306-40615-7"), &err), QStringLiteral("9780306406157"));
        QVERIFY(isbn13(QStringLiteral("978-0-306-40615-0"), &err).isEmpty());
        QVERIFY(err.contains(QLatin1String("should be 7")));
        QCOMPARE(isbn13(QStringLiteral("080442957X"), &err), QStringLiteral("9780804429573"));
        QCOMPARE(isbnCaption(QStringLiteral("978-0-306-40615-7")), QStringLiteral("ISBN 978-0-306-40615-7"));
        // Standard hyphens for the English-language groups (as printed on 88
        // ISBNs in Michigan Legal Publishing's PDFs, all reproduced).
        QCOMPARE(hyphenateIsbn(QStringLiteral("9781640021631")), QStringLiteral("978-1-64002-163-1"));
        QCOMPARE(hyphenateIsbn(QStringLiteral("9781942842187")), QStringLiteral("978-1-942842-18-7"));
        QCOMPARE(hyphenateIsbn(QStringLiteral("9780306406157")), QStringLiteral("978-0-306-40615-7"));
        QCOMPARE(hyphenateIsbn(QStringLiteral("9780199535569")), QStringLiteral("978-0-19-953556-9"));
        QCOMPARE(isbnCaption(QStringLiteral("9781640021631")), QStringLiteral("ISBN 978-1-64002-163-1"));
        // Other groups wait for the ISBN agency's table (downloaded by the
        // dialog). A few of its rules, in its format:
        QVERIFY(hyphenateIsbn(QStringLiteral("9783161484100")).isEmpty());
        auto rules = [](const char *tag, const char *prefix, const char *list) {
            QByteArray r;
            for (const QByteArray &rule : QByteArray(list).split(' '))
                r += "<Rule><Range>" + rule.split(':')[0] + "</Range><Length>" + rule.split(':')[1] + "</Length></Rule>";
            return "<" + QByteArray(tag) + "><Prefix>" + prefix + "</Prefix><Agency>x</Agency><Rules>" + r + "</Rules></" + tag + ">";
        };
        const QByteArray xml = "<?xml version='1.0' encoding='utf-8'?><ISBNRangeMessage><MessageDate>x</MessageDate><EAN.UCCPrefixes>"
            + rules("EAN.UCC", "978", "0000000-5999999:1 6000000-6499999:3 9990000-9999999:5")
            + rules("EAN.UCC", "979", "0000000-0999999:0 1000000-1599999:2 8000000-8999999:1") + "</EAN.UCCPrefixes><RegistrationGroups>"
            + rules("Group", "978-3", "0400000-1999999:2") + rules("Group", "979-8", "8850000-8999999:5")
            + rules("Group", "978-99937", "0000000-1999999:1") + rules("Group", "978-1", "5500000-6499999:5") + "</RegistrationGroups></ISBNRangeMessage>";
        QVERIFY(!loadIsbnRanges("<html>Not found</html>"));
        QVERIFY(loadIsbnRanges(xml));
        QCOMPARE(hyphenateIsbn(QStringLiteral("9783161484100")), QStringLiteral("978-3-16-148410-0"));    // Germany
        QCOMPARE(hyphenateIsbn(QStringLiteral("9798886450002")), QStringLiteral("979-8-88645-000-2"));    // the newer US prefix
        QCOMPARE(hyphenateIsbn(QStringLiteral("9789993701231")), QStringLiteral("978-99937-0-123-1"));   // a five-digit group
        QCOMPARE(hyphenateIsbn(QStringLiteral("9781640021631")), QStringLiteral("978-1-64002-163-1"));
        QVERIFY(hyphenateIsbn(QStringLiteral("9790000000001")).isEmpty());   // 979-0 isn't an ISBN range (music)
        QCOMPARE(isbnCaption(QStringLiteral("979-0-000000-00-1")), QStringLiteral("ISBN 979-0-000000-00-1"));
        QVERIFY(!loadIsbnRanges(QByteArray()));   // back to the built-in ranges
        QVERIFY(hyphenateIsbn(QStringLiteral("9783161484100")).isEmpty());
        QCOMPARE(isbnCaption(QStringLiteral("978-3-16-148410-0")), QStringLiteral("ISBN 978-3-16-148410-0"));
        QCOMPARE(priceAddOn(Currency::UsDollar, 19.95, &err), QStringLiteral("51995"));
        QCOMPARE(priceAddOn(Currency::CanadianDollar, 24.99, &err), QStringLiteral("62499"));
        QCOMPARE(priceAddOn(Currency::Pound, 7.5, &err), QStringLiteral("00750"));
        QCOMPARE(priceAddOn(Currency::None, 0, &err), QStringLiteral("90000"));
        QCOMPARE(eanCheckDigit(QStringLiteral("400638133393")), 1);

        auto modules = [](const Layout &l) {
            const double unit = 0.33 * 72 / 25.4;
            double x0 = 1e9, x1 = 0;
            for (const QRectF &r : l.bars) { x0 = std::min(x0, r.left()); x1 = std::max(x1, r.right()); }
            QString m(int(std::lround((x1 - x0) / unit)), QLatin1Char('0'));
            for (const QRectF &r : l.bars)
                for (int k = int(std::lround((r.left() - x0) / unit)); k < int(std::lround((r.right() - x0) / unit)); ++k) m[k] = QLatin1Char('1');
            return m;
        };
        auto bars = [&](Type t, const QString &data, const QString &addOn = QString()) {
            Options o;
            o.type = t;
            o.data = data;
            o.addOn = addOn;
            const Layout l = make(o);
            return l.error.isEmpty() ? modules(l) : l.error;
        };
        QCOMPARE(bars(Type::Ean13, QStringLiteral("4006381333931")),
                 QStringLiteral("10100011010100111010111101111010001001011001101010100001010000101000010111010010000101100110101"));
        QCOMPARE(bars(Type::Ean8, QStringLiteral("96385074")), QStringLiteral("1010001011010111101111010110111010101001110111001010001001011100101"));
        QCOMPARE(bars(Type::UpcA, QStringLiteral("036000291452")),
                 QStringLiteral("10100011010111101010111100011010001101000110101010110110011101001100110101110010011101101100101"));
        QCOMPARE(bars(Type::Isbn, QStringLiteral("0-306-40615-2"), QStringLiteral("51995")),
                 QStringLiteral("1010111011000100101001110111101010011101011110101010111001110010101000011001101001110100010010100000000010110110001010110011010001011010010111010110001"));
        QCOMPARE(bars(Type::Code128, QStringLiteral("AB12345CD7")),
                 QStringLiteral("1101001000010100011000100010110001001110011010111011110111011011101011101100010111101110100010001101011000100011101101110101110111101100011101011"));
        QCOMPARE(bars(Type::Code39, QStringLiteral("MLP")), QStringLiteral("1000101110111010111011101010001010111010100011101011101110100010100010111011101"));
        // What can't be drawn says why.
        QVERIFY(bars(Type::Ean13, QStringLiteral("4006381333932")).contains(QLatin1String("should be 1")));
        QVERIFY(bars(Type::Code39, QStringLiteral("a~b")).contains(QLatin1String("Code 39")));
    }

    // Insert > Barcode places one group: the white quiet zone and the bars
    // with their digits as a single vector outline.
    void barcodeInsert()
    {
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        jp::Settings::get().setValue(QStringLiteral("barcode/type"), 0);
        QTimer::singleShot(0, &w, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            d->findChild<QLineEdit *>()->setText(QStringLiteral("978-0-306-40615-7"));
            for (auto *r : d->findChildren<QRadioButton *>())
                if (r->text() == QLatin1String("Price:")) r->click();
            d->findChild<QDoubleSpinBox *>()->setValue(19.95);
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) d->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/barcode-dialog.png");
            for (auto *b : d->findChildren<QPushButton *>())
                if (b->text() == QLatin1String("Insert")) b->click();
        });
        jp::barcodeDialog(&w, w.editor());
        const auto &items = w.editor()->doc()->pages[0]->items;
        QCOMPARE(int(items.size()), 1);
        auto g = std::dynamic_pointer_cast<jp::GroupItem>(items[0]);
        QVERIFY(g);
        QCOMPARE(g->altText, QStringLiteral("Barcode: ISBN 978-0-306-40615-7"));
        QCOMPARE(int(g->children.size()), 2);
        auto bars = std::dynamic_pointer_cast<jp::ShapeItem>(g->children[1]);
        QVERIFY(bars && !bars->customPath.isEmpty());
        // Main symbol plus add-on: 167 modules (2.17 in) by about 1.2 in at 100%.
        QVERIFY2(std::abs(g->rect.width() - 167 * 0.33 * 72 / 25.4) < 0.01 && g->rect.height() > 80 && g->rect.height() < 100,
                 qPrintable(QStringLiteral("%1 x %2").arg(g->rect.width()).arg(g->rect.height())));
        QCOMPARE(g->barcode.value(QLatin1String("priceMode")).toInt(), 1);

        // With the barcode selected, Barcode edits it: a pasted ISBN gets its
        // hyphens, "no suggested price" replaces the price, and the new
        // barcode takes the old one's place (one undo step back).
        const QString id = g->id;
        g->moveBy(-100, 50);
        const QPointF where = g->rect.topLeft();
        w.editor()->select(id);
        QTimer::singleShot(0, &w, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            QCOMPARE(d->windowTitle(), QStringLiteral("Edit Barcode"));
            auto *isbn = d->findChild<QLineEdit *>();
            QCOMPARE(isbn->text(), QStringLiteral("978-0-306-40615-7"));
            isbn->setText(QStringLiteral("9781640021631"));
            QCOMPARE(isbn->text(), QStringLiteral("978-1-64002-163-1"));
            for (auto *r : d->findChildren<QRadioButton *>())
                if (r->text().startsWith(QLatin1String("No suggested price"))) r->click();
            for (auto *b : d->findChildren<QPushButton *>())
                if (b->text() == QLatin1String("Update")) b->click();
        });
        jp::barcodeDialog(&w, w.editor());
        QCOMPARE(int(items.size()), 1);
        auto g2 = std::dynamic_pointer_cast<jp::GroupItem>(items[0]);
        QVERIFY(g2 && g2 != g);
        QCOMPARE(g2->id, id);
        QCOMPARE(g2->rect.topLeft(), where);
        QCOMPARE(g2->altText, QStringLiteral("Barcode: ISBN 978-1-64002-163-1"));
        QCOMPARE(g2->barcode.value(QLatin1String("priceMode")).toInt(), 2);
        // Saved and opened again, it can still be edited.
        auto copy = jp::publicationFromBytes(jp::publicationBytes(*w.editor()->doc(), QImage()), nullptr);
        QVERIFY(copy);
        auto g3 = std::dynamic_pointer_cast<jp::GroupItem>(copy->pages[0]->items[0]);
        QVERIFY(g3);
        QCOMPARE(g3->barcode, g2->barcode);
        w.editor()->undoStack()->undo();
        QCOMPARE(std::dynamic_pointer_cast<jp::GroupItem>(w.editor()->doc()->pages[0]->items[0])->altText, QStringLiteral("Barcode: ISBN 978-0-306-40615-7"));
    }

    // The Open page shows a .pub file's own preview: the picture kept in its
    // summary information (property 17; here a bitmap, in Publisher's own
    // files a metafile, both read the same way).
    void pubPreviewThumbnail()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("preview.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        QVERIFY(jp::publicationThumbnail(path).isNull());   // JeffPub's own .pub has none
        // A 40 x 30 red picture as a clipboard bitmap (a BMP without its file header).
        QImage red(40, 30, QImage::Format_RGB32);
        red.fill(QColor(200, 20, 30));
        QByteArray bmp;
        QBuffer buf(&bmp);
        buf.open(QIODevice::WriteOnly);
        red.save(&buf, "BMP");
        const QByteArray dib = bmp.mid(14);
        auto u16 = [](quint16 v) { QByteArray b(2, 0); qToLittleEndian(v, b.data()); return b; };
        auto u32 = [](quint32 v) { QByteArray b(4, 0); qToLittleEndian(v, b.data()); return b; };
        const QByteArray cf = u32(0xFFFFFFFF) + u32(8) + dib;
        const QByteArray prop = u32(0x47) + u32(quint32(cf.size())) + cf;
        const QByteArray section = u32(quint32(16 + prop.size())) + u32(1) + u32(17) + u32(16) + prop;
        const QByteArray fmtid = QByteArray::fromHex("E0859FF2F94F6810AB9108002B27B3D9");
        const QByteArray set = u16(0xFFFE) + u16(0) + u32(0x00020006) + QByteArray(16, 0) + u32(1) + fmtid + u32(48) + section;
        jp::cfb::File c;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY(jp::cfb::read(f.readAll(), &c, &err));
        }
        QVERIFY(c.setStream(QStringLiteral("\x05SummaryInformation"), set));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(jp::cfb::write(c));
        }
        // Reading one stream straight from the file gives the same bytes.
        QCOMPARE(jp::cfb::readStream(path, QStringLiteral("\x05SummaryInformation")), set);
        QCOMPARE(jp::cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS")), c.stream(QStringLiteral("Quill/QuillSub/CONTENTS")));
        const QImage th = jp::publicationThumbnail(path);
        QVERIFY(!th.isNull());
        QCOMPARE(th.width() * 3, th.height() * 4);
        QCOMPARE(th.pixelColor(th.width() / 2, th.height() / 2), QColor(200, 20, 30));
    }

    // A barcode is one object on the page: clicking it again and dragging
    // moves the bars and the white quiet zone together (a click used to
    // reach inside the group and move only the bars); a second barcode
    // doesn't land on the first; double-clicking opens it for editing.
    void barcodeMovesAsOne()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QApplication::processEvents();
        jp::barcode::Options o;
        o.data = QStringLiteral("978-1-64002-163-1");
        const jp::barcode::Layout l = jp::barcode::make(o);
        const QJsonObject settings{{"type", 0}, {"data", o.data}, {"priceMode", 0}};
        // Two inserted the way the dialog does: the second offset from the first.
        auto insert = [&] {
            QTimer::singleShot(0, &w, [&] {
                auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(d);
                d->findChild<QLineEdit *>()->setText(o.data);
                for (auto *r : d->findChildren<QRadioButton *>())
                    if (r->text() == QLatin1String("No add-on")) r->click();
                for (auto *b : d->findChildren<QPushButton *>())
                    if (b->text() == QLatin1String("Insert")) b->click();
            });
            w.editor()->select(QStringList());
            jp::barcodeDialog(&w, w.editor());
        };
        insert();
        insert();
        auto &items = w.editor()->doc()->pages[0]->items;
        QCOMPARE(int(items.size()), 2);
        QCOMPARE(items[1]->rect.topLeft() - items[0]->rect.topLeft(), QPointF(18, 18));
        // The second (selected) barcode: click its bars, drag an inch right.
        auto g = std::dynamic_pointer_cast<jp::GroupItem>(items[1]);
        const QRectF before = g->children[0]->rect, barsBefore = g->children[1]->rect;
        jp::Canvas *c = w.canvas();
        QWidget *vp = c->viewport();
        auto view = [&](QPointF page) { return c->pageToView(page).toPoint(); };
        QPointF onBar;
        for (const QRectF &r : l.bars)
            if (r.width() > 1.5) { onBar = g->rect.topLeft() + r.center(); break; }
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, view(onBar));
        QCOMPARE(w.editor()->selection(), QStringList{g->id});
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, view(onBar));
        QTest::mouseMove(vp, view(onBar + QPointF(36, 0)));
        QTest::mouseMove(vp, view(onBar + QPointF(72, 0)));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, view(onBar + QPointF(72, 0)));
        g = std::dynamic_pointer_cast<jp::GroupItem>(w.editor()->doc()->pages[0]->items[1]);
        QVERIFY2(std::abs(g->children[0]->rect.left() - before.left() - 72) < 1.5, qPrintable(QString::number(g->children[0]->rect.left() - before.left())));
        QCOMPARE(g->children[1]->rect.left() - barsBefore.left(), g->children[0]->rect.left() - before.left());
        // Double-click: the Edit Barcode dialog.
        QString title;
        QTimer::singleShot(0, &w, [&] {
            if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                title = d->windowTitle();
                d->reject();
            }
        });
        QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, view(onBar + QPointF(72, 0)));
        QCOMPARE(title, QStringLiteral("Edit Barcode"));
    }

    // A spot color names its ink in the fill's extra drawing properties
    // (0x01A1, as Publisher writes "P2,#003d007e00db0000,PANTONE 2727 C" on
    // book covers whose PDFs print that ink on its own plate): the
    // publication opens with that spot color, and its PDFs keep the ink.
    void pubSpotColorRead()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->print.model = jp::PrintInfo::ProcessCMYK;
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(0, 0, 612, 792);
        box->fill = jp::Fill::solid(jp::ColorRef::inks(QColor::fromCmykF(192 / 255.f, 102 / 255.f, 0, 0), QColor(0x3d, 0x7e, 0xdb)));
        box->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(box);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("spot.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        jp::cfb::File c;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY2(jp::cfb::read(f.readAll(), &c, &err), qPrintable(err));
        }
        // Add the name to the record of extra properties that holds the
        // fill's shown color (0x019E), growing the records around it.
        const QString text = QStringLiteral("P2,#003d007e00db0000,PANTONE 2727 C");
        QByteArray name(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
        name.append(2, '\0');
        auto u16 = [](quint16 v) { QByteArray b(2, 0); qToLittleEndian(v, b.data()); return b; };
        auto u32 = [](quint32 v) { QByteArray b(4, 0); qToLittleEndian(v, b.data()); return b; };
        int added = 0;
        std::function<QByteArray(const QByteArray &)> rebuild = [&](const QByteArray &in) {
            QByteArray out;
            int at = 0;
            while (at + 8 <= in.size()) {
                quint16 vi = qFromLittleEndian<quint16>(in.constData() + at);
                const quint16 type = qFromLittleEndian<quint16>(in.constData() + at + 2);
                const quint32 len = qFromLittleEndian<quint32>(in.constData() + at + 4);
                if (qint64(at) + 8 + len > in.size()) {   // not a record: the stream keeps a value between drawings
                    out += in.mid(at, 4);
                    at += 4;
                    continue;
                }
                QByteArray body = in.mid(at + 8, len);
                if ((vi & 0xF) == 0xF) {
                    body = rebuild(body);
                } else if (type == 0xF122) {
                    const int n = vi >> 4;
                    bool shown = false;
                    for (int k = 0; k < n && k * 6 + 6 <= body.size(); ++k)
                        shown |= (qFromLittleEndian<quint16>(body.constData() + k * 6) & 0x3FFF) == 0x019E;
                    if (shown) {
                        body = body.left(n * 6) + u16(0xC1A1) + u32(quint32(name.size())) + body.mid(n * 6) + name;
                        vi = quint16((vi & 0xF) | ((n + 1) << 4));
                        ++added;
                    }
                }
                out += u16(vi) + u16(type) + u32(quint32(body.size())) + body;
                at += 8 + int(len);
            }
            return out + in.mid(at);
        };
        QVERIFY(c.setStream(QStringLiteral("Escher/EscherStm"), rebuild(c.stream(QStringLiteral("Escher/EscherStm")))));
        QCOMPARE(added, 1);
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(jp::cfb::write(c));
        }
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->print.model, jp::PrintInfo::SpotColors);
        QCOMPARE(back->print.spotNames, QStringList{QStringLiteral("PANTONE 2727 C")});
        const QColor ink = back->print.spotColors.value(0).toCmyk();
        QCOMPARE(ink.cyan(), 192);
        QCOMPARE(ink.magenta(), 102);
        // The page still shows Publisher's screen color.
        bool found = false;
        for (const auto &it : back->pages[0]->items)
            if (auto sh = std::dynamic_pointer_cast<jp::ShapeItem>(it); sh && sh->fill.type == jp::Fill::Solid) {
                QCOMPARE(sh->fill.color.shownValue(), QColor(0x3d, 0x7e, 0xdb));
                found = true;
            }
        QVERIFY(found);
    }

    // Process inks survive saving as .pub: the file keeps the color as shown
    // and the inks (checked against values reference files hold).
    void pubWriterProcessInks()
    {
        struct Ink { float c, m, y, k; QColor shown; const char *label; };
        const Ink inks[] = {{0.98f, 0.761f, 0.133f, 0.302f, QColor(51, 61, 97), "navy C98 M76 Y13 K30"},
                            {0.6f, 0.4f, 0.4f, 1.0f, QColor(29, 31, 25), "rich black C60 M40 Y40 K100"},
                            {0.753f, 0.4f, 0, 0, QColor(91, 125, 179), "blue C75 M40"},
                            {0, 0, 1.0f, 0, QColor(255, 242, 0), "yellow Y100"},
                            {0, 0, 0, 1.0f, QColor(35, 31, 32), "black K100"},
                            {0, 0.5f, 1.0f, 0.2f, QColor(220, 140, 30), "orange M50 Y100 K20"},
                            {0.3f, 0, 0, 0.6f, QColor(80, 100, 110), "slate C30 K60"}};
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->print.model = jp::PrintInfo::ProcessCMYK;
        for (int i = 0; i < 7; ++i) {
            auto box = std::make_shared<jp::ShapeItem>();
            box->rect = QRectF(72, 50 + i * 100, 200, 80);
            box->fill = jp::Fill::solid(jp::ColorRef::inks(QColor::fromCmykF(inks[i].c, inks[i].m, inks[i].y, inks[i].k), inks[i].shown));
            box->stroke = jp::Stroke::none();
            doc->pages[0]->items.push_back(box);
            auto t = std::make_shared<jp::TextItem>();
            t->rect = QRectF(290, 70 + i * 100, 260, 40);
            t->storyId = doc->createStory(QString::fromLatin1(inks[i].label));
            doc->pages[0]->items.push_back(t);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("inks.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            const QString out = qEnvironmentVariable("JP_SHOT_DIR") + "/test31-process-colors.pub";
            QFile::remove(out);
            QFile::copy(path, out);
        }
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->print.model, jp::PrintInfo::ProcessCMYK);
        int found = 0;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() != jp::ItemType::Shape) return;
            const jp::ColorRef c = static_cast<const jp::ShapeItem *>(it.get())->fill.color;
            const QColor k = c.rgbValue();
            for (const Ink &in : inks)
                if (c.shownValue() == in.shown) {
                    QCOMPARE(k.spec(), QColor::Cmyk);
                    QVERIFY2(std::abs(k.cyanF() - in.c) < 0.003 && std::abs(k.magentaF() - in.m) < 0.003 && std::abs(k.yellowF() - in.y) < 0.003
                                 && std::abs(k.blackF() - in.k) < 0.003, in.label);
                    ++found;
                }
        });
        QCOMPARE(found, 7);
    }

    // Process inks survive reading on a system whose language writes
    // decimals with a comma (German, French...): nothing on the way may go
    // through locale-dependent text.
    void processInksAnyLocale()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(72, 72, 200, 100);
        box->fill = jp::Fill::solid(jp::ColorRef::inks(QColor::fromCmykF(250 / 255.f, 194 / 255.f, 34 / 255.f, 77 / 255.f), QColor(51, 61, 97)));
        box->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(box);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("inks.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        const std::string before = setlocale(LC_NUMERIC, nullptr);
        const char *comma = nullptr;
        for (const char *l : {"de_DE.UTF-8", "de_DE.utf8", "de_AT.utf8", "fr_FR.UTF-8", "fr_FR.utf8"})
            if (setlocale(LC_NUMERIC, l)) { comma = l; break; }
        if (!comma) QSKIP("no comma-decimal locale on this system");
        char probe[16];
        snprintf(probe, sizeof probe, "%.1f", 0.5);
        auto back = jp::importPublisherFile(path, &err);
        setlocale(LC_NUMERIC, before.c_str());
        QCOMPARE(QString::fromLatin1(probe), QStringLiteral("0,5"));   // the locale really writes commas
        QVERIFY2(back, qPrintable(err));
        int found = 0;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() != jp::ItemType::Shape) return;
            const QColor k = static_cast<const jp::ShapeItem *>(it.get())->fill.color.rgbValue();
            QCOMPARE(k.spec(), QColor::Cmyk);
            QVERIFY2(std::abs(k.cyanF() - 250 / 255.0) < 0.003 && std::abs(k.blackF() - 77 / 255.0) < 0.003, qPrintable(jp::colorToString(k)));
            ++found;
        });
        QCOMPARE(found, 1);
    }

    // Table rules keep their width when read on a system that writes
    // decimals with a comma.
    void tableBordersAnyLocale()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TableItem>();
        t->rows = 2;
        t->cols = 2;
        t->colW = {100, 150};
        t->rowH = {30, 40};
        t->rect = QRectF(72, 144, 0, 0);
        t->syncRect();
        t->cells.resize(4);
        for (int i = 0; i < 4; ++i) t->cells[i].storyId = doc->createStory(QStringLiteral("cell"));
        for (int c = 0; c < 2; ++c) t->cell(0, c).border.bottom = jp::Stroke::line(jp::ColorRef::rgb(Qt::black), 1.5);
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("rules.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        const std::string before = setlocale(LC_NUMERIC, nullptr);
        const char *comma = nullptr;
        for (const char *l : {"de_DE.UTF-8", "de_DE.utf8", "de_AT.utf8", "fr_FR.UTF-8", "fr_FR.utf8"})
            if (setlocale(LC_NUMERIC, l)) { comma = l; break; }
        if (!comma) QSKIP("no comma-decimal locale on this system");
        auto back = jp::importPublisherFile(path, &err);
        setlocale(LC_NUMERIC, before.c_str());
        QVERIFY2(back, qPrintable(err));
        double widest = 0;
        jp::walkItems(back->pages[0]->items, [&](const jp::ItemPtr &it) {
            if (it->type() != jp::ItemType::Table) return;
            for (const jp::TableCell &c : static_cast<const jp::TableItem *>(it.get())->cells)
                for (const jp::Stroke *st : {&c.border.left, &c.border.right, &c.border.top, &c.border.bottom})
                    if (!st->isNone()) widest = std::max(widest, st->width);
        });
        QVERIFY2(std::abs(widest - 1.5) < 0.05, qPrintable(QString::number(widest)));
    }

    // A process color from a .pub file: the screen shows the color the file
    // shows, and a CMYK PDF and the plates get its exact inks.
    void processInksShownColor()
    {
        const jp::ColorRef ref = jp::ColorRef::inks(QColor::fromCmykF(0.6f, 0.4f, 0.4f, 1.0f), QColor(29, 31, 25));
        QCOMPARE(ref.toString(), QStringLiteral("cmyk(60,40,40,100)=#1D1F19"));
        QCOMPARE(jp::ColorRef::fromString(ref.toString()), ref);
        QVERIFY(ref != jp::ColorRef::rgb(QColor::fromCmykF(0.6f, 0.4f, 0.4f, 1.0f)));
        jp::ColorScheme scheme;
        QCOMPARE(ref.resolve(scheme).toRgb(), QColor(29, 31, 25));
        {
            jp::InkOutput inks;
            const QColor c = ref.resolve(scheme);
            QCOMPARE(c.spec(), QColor::Cmyk);
            QVERIFY(std::abs(c.cyanF() - 0.6) < 0.002 && std::abs(c.blackF() - 1.0) < 0.002);
        }
        QCOMPARE(ref.resolve(scheme).toRgb(), QColor(29, 31, 25));
        QColor seen;
        jp::setColorFilter([&](const QColor &c) { seen = c; return c; });
        ref.resolve(scheme);
        jp::setColorFilter({});
        QCOMPARE(seen.spec(), QColor::Cmyk);   // plates see the inks

        // A process-color publication's PDF carries the inks.
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->print.model = jp::PrintInfo::ProcessCMYK;
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(72, 72, 200, 100);
        box->fill = jp::Fill::solid(ref);
        box->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(box);
        w.editor()->setDocument(std::move(doc));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("inks.pdf"));
        QVERIFY(w.exportPdfTo(path, jp::MainWindow::PdfSettings()));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray pdf = f.readAll();
        QByteArray text;
        for (qsizetype at = 0; (at = pdf.indexOf("stream", at)) >= 0; at += 6) {
            qsizetype start = at + 6;
            if (pdf.mid(start, 1) == "\n") ++start;
            const qsizetype end = pdf.indexOf("endstream", start);
            if (end < 0) break;
            bool ok = false;
            text += jp::QtPdf::inflate(pdf.mid(start, end - start), &ok);
        }
        bool found = false;
        for (const QByteArray &l : text.split('\n')) {
            const QList<QByteArray> v = l.split(' ');
            if (v.size() == 5 && v[4] == "scn")
                found = found || (std::abs(v[0].toDouble() - 0.6) < 1e-3 && std::abs(v[1].toDouble() - 0.4) < 1e-3 && std::abs(v[2].toDouble() - 0.4) < 1e-3
                                  && std::abs(v[3].toDouble() - 1.0) < 1e-3);
        }
        QVERIFY(found);
        QVERIFY(!jp::keepInks());
    }

    void cmykColors()
    {
        const QColor ink = QColor::fromCmykF(0.2f, 0.0f, 0.55f, 0.1f);
        const jp::ColorRef ref = jp::ColorRef::rgb(ink);
        QCOMPARE(ref.toString(), QStringLiteral("cmyk(20,0,55,10)"));
        const jp::ColorRef back = jp::ColorRef::fromString(ref.toString());
        QCOMPARE(back.rgbValue().spec(), QColor::Cmyk);
        QCOMPARE(back, ref);
        QCOMPARE(jp::ColorRef::fromString(QStringLiteral("#12AB34")).rgbValue(), QColor(0x12, 0xab, 0x34));
        QCOMPARE(jp::colorFromString(QStringLiteral("cmyk(0,100,0,0,128)")).alpha(), 128);
        const QColor tint = jp::mix(ink, Qt::white, 0.5);
        QCOMPARE(tint.spec(), QColor::Cmyk);
        QVERIFY(std::abs(tint.cyanF() - 0.1) < 0.002 && std::abs(tint.yellowF() - 0.275) < 0.002 && std::abs(tint.blackF() - 0.05) < 0.002);
        const QColor shade = jp::mix(ink, Qt::black, 0.5);
        QVERIFY(std::abs(shade.blackF() - 0.55) < 0.002 && std::abs(shade.cyanF() - 0.1) < 0.002);

        // The Colors dialog's CMYK entry.
        QColor picked;
        QTimer::singleShot(0, [] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *model = dlg->findChild<QComboBox *>();
            model->setCurrentIndex(2);
            const auto spins = dlg->findChildren<jp::DecimalSpin *>();
            QCOMPARE(spins.size(), 4);
            const double inks[] = {35, 5, 0, 20};
            for (int i = 0; i < 4; ++i) spins[i]->setValue(inks[i]);
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
                auto *tabs = dlg->findChild<QTabWidget *>();
                tabs->setCurrentIndex(1);
                dlg->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/colors-custom.png");
                tabs->setCurrentIndex(0);
                dlg->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/colors-standard.png");
            }
            dlg->accept();
        });
        picked = jp::colorsDialog(nullptr, Qt::red);
        QCOMPARE(picked.spec(), QColor::Cmyk);
        QVERIFY(std::abs(picked.cyanF() - 0.35) < 0.002 && std::abs(picked.magentaF() - 0.05) < 0.002 && std::abs(picked.blackF() - 0.2) < 0.002);

        // A process-color publication's PDF paints in CMYK.
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        doc->print.model = jp::PrintInfo::ProcessCMYK;
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(72, 72, 200, 100);
        box->fill = jp::Fill::solid(ref);
        box->stroke = jp::Stroke::none();
        doc->pages[0]->items.push_back(box);
        w.editor()->setDocument(std::move(doc));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cmyk.pdf"));
        QVERIFY(w.exportPdfTo(path, jp::MainWindow::PdfSettings()));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) { QFile::remove(qEnvironmentVariable("JP_SHOT_DIR") + "/cmyk.pdf"); QFile::copy(path, qEnvironmentVariable("JP_SHOT_DIR") + "/cmyk.pdf"); }
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray pdf = f.readAll();
        // Page contents are deflated: inflate every stream and look for the
        // fill color as ink amounts ("c m y k k").
        QByteArray text;
        for (qsizetype at = 0; (at = pdf.indexOf("stream", at)) >= 0; at += 6) {
            qsizetype start = at + 6;
            if (pdf.mid(start, 2) == "\r\n") start += 2;
            else if (pdf.mid(start, 1) == "\n") ++start;
            const qsizetype end = pdf.indexOf("endstream", start);
            if (end < 0) break;
            QByteArray z = pdf.mid(start, end - start);
            bool ok = false;
            const QByteArray plain = jp::QtPdf::inflate(z, &ok);
            text += ok ? plain : z;
        }
        // Qt sets a CMYK color space and gives the inks with "scn".
        QVERIFY(text.contains("cmyk cs"));
        bool found = false;
        for (const QByteArray &l : text.split('\n')) {
            const QList<QByteArray> v = l.split(' ');
            if (v.size() == 5 && v[4] == "scn")
                found = found || (std::abs(v[0].toDouble() - 0.2) < 1e-3 && v[1].toDouble() < 1e-3 && std::abs(v[2].toDouble() - 0.55) < 1e-3
                                  && std::abs(v[3].toDouble() - 0.1) < 1e-3);
        }
        QVERIFY(found);
    }

    // Create PDF presets shrink pictures, Commercial press adds room for marks,
    // and a page range exports only those pages; printing separations makes
    // one sheet per plate with printer's marks.
    void pdfPresetsAndPrintMarks()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        for (int i = 0; i < 3; ++i) doc->pages.push_back(std::make_shared<jp::Page>());
        QImage noise(1600, 1600, QImage::Format_RGB32);
        quint32 seed = 7;
        for (int y = 0; y < noise.height(); ++y)
            for (int x = 0; x < noise.width(); ++x) { seed = seed * 1664525u + 1013904223u; noise.setPixel(x, y, seed >> 8); }
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        noise.save(&buf, "PNG");
        auto item = std::make_shared<jp::PictureItem>();
        item->imageId = doc->addImage(png, "png");
        item->rect = QRectF(72, 72, 288, 288);   // 4 in: 400 dpi
        item->imgRect = QRectF(0, 0, 288, 288);
        item->fill = jp::Fill();
        doc->pages[0]->items.push_back(item);
        w.editor()->setDocument(std::move(doc));
        QTemporaryDir dir;
        auto pdfBytes = [&](jp::MainWindow::PdfSettings::Preset preset, int from = 0, int to = -1) {
            jp::MainWindow::PdfSettings s;
            s.preset = preset;
            s.from = from;
            s.to = to;
            const QString path = dir.filePath(QStringLiteral("p%1-%2-%3.pdf").arg(int(preset)).arg(from).arg(to));
            w.exportPdfTo(path, s);
            QFile f(path);
            f.open(QIODevice::ReadOnly);
            return f.readAll();
        };
        auto pageCount = [](const QByteArray &pdf) { return int(pdf.count("/Type /Page\n") + pdf.count("/Type /Page\r") + pdf.count("/Type /Page ") + pdf.count("/Type /Page/")); };
        const QByteArray minimum = pdfBytes(jp::MainWindow::PdfSettings::Minimum);
        const QByteArray high = pdfBytes(jp::MainWindow::PdfSettings::HighQuality);
        const QByteArray press = pdfBytes(jp::MainWindow::PdfSettings::CommercialPress);
        QVERIFY2(minimum.size() * 4 < high.size(), qPrintable(QStringLiteral("%1 vs %2").arg(minimum.size()).arg(high.size())));
        QVERIFY(high.size() < press.size());
        QCOMPARE(pageCount(high), 4);
        // 612 x 792 plus 48 pt of marks on every side.
        const int mb = press.indexOf("/MediaBox");
        QVERIFY2(press.mid(mb, 40).contains("708.000000 888.000000"), press.mid(mb, 40).constData());
        QCOMPARE(pageCount(pdfBytes(jp::MainWindow::PdfSettings::Standard, 1, 2)), 2);

        // Separations on tabloid paper with every mark: 4 pages x 2 plates.
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(dir.filePath("plates.pdf"));
        printer.setPageSize(QPageSize(QPageSize::Tabloid));
        printer.setResolution(150);
        printer.setFullPage(true);
        QJsonObject opts{{"layout", "one"}, {"separations", true}, {"plates", "CK"}, {"cropMarks", true}, {"bleedMarks", true},
                         {"registration", true}, {"densityBars", true}, {"colorBars", true}, {"jobInfo", true}, {"allowBleeds", true}};
        jp::printDocument(w.editor(), &printer, opts);
        QFile f(dir.filePath("plates.pdf"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(pageCount(f.readAll()), 8);
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) QFile::copy(dir.filePath("plates.pdf"), qEnvironmentVariable("JP_SHOT_DIR") + "/plates.pdf");
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) QFile::copy(dir.filePath("p3-0--1.pdf"), qEnvironmentVariable("JP_SHOT_DIR") + "/press.pdf");
        // A plate image: white stays blank, pure cyan is full ink on cyan only.
        QImage rgb(2, 1, QImage::Format_RGB32);
        rgb.setPixel(0, 0, qRgb(255, 255, 255));
        rgb.setPixel(1, 0, qRgb(0, 255, 255));
        QCOMPARE(qGray(jp::separationPlate(rgb, 0).pixel(0, 0)), 255);
        QCOMPARE(qGray(jp::separationPlate(rgb, 0).pixel(1, 0)), 0);
        QCOMPARE(qGray(jp::separationPlate(rgb, 1).pixel(1, 0)), 255);
        QCOMPARE(qGray(jp::separationPlate(rgb, 3).pixel(1, 0)), 255);
    }

    // Edit Wrap Points: start from the outline, drag a point, add one on an
    // edge, delete it, and text wraps around the new outline.
    // A line may break after a hyphen before a digit, as in Publisher:
    // "(555) 012-" then "3456" (Qt's own rules keep the number whole).
    void breakAfterHyphenBeforeDigit()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->storyId = doc->createStory(QStringLiteral("Call us at (555) 012-3456 today"));
        doc->pages[0]->items.push_back(t);
        LayoutCache cache;
        RenderOptions opt;
        auto firstLine = [&](double width) {
            t->rect = QRectF(72, 72, width, 200);
            QString s = cache.textFrame(*doc, *t, 1, opt).layout->lineInfo(0).value(0).text;
            return s.remove(QChar(0x00AD)).remove(QChar(0x200B)).trimmed();
        };
        // Just too narrow for the whole number: the line ends at its hyphen.
        double w = 40;
        while (w < 400 && !firstLine(w).contains(QLatin1String("012-"))) w += 1;
        QCOMPARE(firstLine(w), QStringLiteral("Call us at (555) 012-"));
        QVERIFY(!firstLine(w).contains(QLatin1String("3456")));
    }

    // An outline set inside its shape (Publisher's text box borders and many
    // design frames, the inset-pen flag in a .pub) draws nothing outside the
    // shape's edge, and keeps the setting through a .pub.
    void insetOutlineStaysInside()
    {
        auto doc = Document::blank(QSizeF(200, 200));
        auto box = std::make_shared<ShapeItem>();
        box->shape = QStringLiteral("rect");
        box->rect = QRectF(40, 40, 120, 120);
        box->fill = Fill::none();
        box->stroke = Stroke::line(ColorRef::rgb(Qt::black), 16);
        box->stroke.inset = true;
        doc->pages[0]->items.push_back(box);
        auto draw = [&](Document &d) {
            PaintContext ctx;
            ctx.doc = &d;
            ctx.opt.output = true;
            return Renderer::renderToImage(ctx, 0, 1.0).convertToFormat(QImage::Format_RGB32);
        };
        auto dark = [](const QImage &img, int x, int y) { return qGray(img.pixel(x, y)) < 128; };
        QImage img = draw(*doc);
        QVERIFY(!dark(img, 36, 100) && !dark(img, 100, 36));   // nothing outside the edge
        QVERIFY(dark(img, 42, 100) && dark(img, 54, 100));      // the full width inside it
        QVERIFY(!dark(img, 60, 100));
        // Through a .pub: Publisher's inset-pen flag.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("inset.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto back = importPublisher(f.readAll(), nullptr);
        QVERIFY(back);
        const ShapeItem *got = nullptr;
        for (const auto &it : back->pages[0]->items)
            if (auto *s = dynamic_cast<const ShapeItem *>(it.get()); s && !s->stroke.isNone()) got = s;
        QVERIFY(got);
        QVERIFY(got->stroke.inset);
        QVERIFY(!dark(draw(*back), 36, 100));
    }

    // A text box that only touches another pushes none of its text aside
    // (Publisher's designs butt boxes together, their wrap distances
    // reaching past each other's insets): a name in a short box lost its
    // only line. One that overlaps still does.
    void touchingTextBoxesDontWrap()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto box = [&](const QRectF &r, const QString &text) {
            auto t = std::make_shared<TextItem>();
            t->rect = r;
            t->insets = QMarginsF(2.85, 2.85, 2.85, 2.85);
            t->storyId = doc->createStory(text);
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setFontPointSize(8);
            c.mergeCharFormat(f);
            doc->pages[0]->items.push_back(t);
            return t;
        };
        auto name = box(QRectF(72, 100, 150, 12.7), QStringLiteral("Jeff"));
        auto above = box(QRectF(72, 80, 150, 20), QStringLiteral("Title"));   // touches the name's top
        LayoutCache cache;
        RenderOptions opt;
        QCOMPARE(cache.textFrame(*doc, *name, 1, opt).layout->lineInfo(0).size(), 1);
        above->rect.moveTop(84);   // now 4 points into the name's text area
        QCOMPARE(cache.textFrame(*doc, *name, 1, opt).layout->lineInfo(0).size(), 0);
    }

    // An object over a whole text box never pushes its text aside, whatever
    // its wrap or fill (Publisher's pictures of a box under rectangles with
    // each wrap); one over part of it does. Sign and newsletter designs
    // frame their text boxes with an empty rectangle set to wrap Through,
    // and JeffPub emptied the boxes.
    void coveringObjectDoesntWrap()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->id = QStringLiteral("box");
        t->rect = QRectF(92, 92, 140, 150);
        t->storyId = doc->createStory(QStringLiteral("The quick brown fox jumps over the lazy dog again and again."));
        doc->pages[0]->items.push_back(t);
        auto frame = std::make_shared<ShapeItem>();
        frame->id = QStringLiteral("frame");
        frame->shape = QStringLiteral("rect");
        frame->fill = Fill::none();
        frame->stroke.color = ColorRef::rgb(Qt::red);
        doc->pages[0]->items.push_back(frame);
        // Its text then sits at the top, whatever its vertical alignment
        // (Publisher's pictures of middle-aligned boxes under each wrap).
        t->valign = VAlign::Middle;
        RenderOptions opt;
        for (Wrap::Mode mode : {Wrap::Square, Wrap::Tight, Wrap::Through, Wrap::TopBottom}) {
            frame->wrap.mode = mode;
            frame->rect = QRectF(72, 72, 180, 190);   // all around the box
            QVERIFY2(jp::Renderer::wrapObstacles(*doc, *t).isEmpty(), qPrintable(QString::number(int(mode))));
            QCOMPARE(jp::Renderer::frameSpec(*doc, *t, 1, opt).valign, VAlign::Top);
            frame->rect = QRectF(162, 82, 90, 170);   // over its right half
            QCOMPARE(jp::Renderer::wrapObstacles(*doc, *t).size(), 1);
            QCOMPARE(jp::Renderer::frameSpec(*doc, *t, 1, opt).valign, VAlign::Middle);
        }
        frame->wrap.mode = Wrap::None;
        frame->rect = QRectF(72, 72, 180, 190);
        QCOMPARE(jp::Renderer::frameSpec(*doc, *t, 1, opt).valign, VAlign::Middle);
    }

    void editWrapPoints()
    {
        jp::MainWindow w;
        w.resize(1200, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 400, 300);
        t->storyId = doc->createStory(QString("Text flows around the shape. ").repeated(60));
        doc->pages[0]->items.push_back(t);
        auto sh = std::make_shared<jp::ShapeItem>();
        sh->shape = QStringLiteral("ellipse");
        sh->rect = QRectF(150, 150, 100, 100);
        sh->fill = jp::Fill::solid(jp::ColorRef::rgb(Qt::red));
        doc->pages[0]->items.push_back(sh);
        w.editor()->setDocument(std::move(doc));
        jp::Document *d = w.editor()->doc();
        auto *tb = static_cast<jp::TextItem *>(d->pages[0]->items[0].get());
        jp::Item *s = d->pages[0]->items[1].get();
        w.editor()->select(s->id);
        QTest::qWait(50);
        w.act(QStringLiteral("wrap.edit"))->trigger();
        QCOMPARE(w.editor()->wrapItem, s->id);
        QCOMPARE(int(s->wrap.mode), int(jp::Wrap::Tight));
        const int n = s->wrap.points.size();
        QVERIFY2(n >= 8 && n <= 24, qPrintable(QString::number(n)));
        // The starting outline follows the ellipse.
        const QRectF wb = s->transform().map(s->wrap.points).boundingRect();
        QVERIFY(std::abs(wb.left() - 150) < 2 && std::abs(wb.right() - 250) < 2);
        jp::Canvas *c = w.canvas();
        QWidget *vp = c->viewport();
        auto view = [&](QPointF page) { return c->pageToView(page).toPoint(); };
        // Drag the rightmost point 100 pt further right.
        int ri = 0;
        for (int i = 1; i < n; ++i)
            if (s->wrap.points[i].x() > s->wrap.points[ri].x()) ri = i;
        const QPointF from = s->transform().map(s->wrap.points[ri]), to = from + QPointF(100, 0);
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, view(from));
        QTest::mouseMove(vp, view(from + QPointF(50, 0)));
        QTest::mouseMove(vp, view(to));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, view(to));
        QCOMPARE(s->wrap.points.size(), n);
        QVERIFY(std::abs(s->transform().map(s->wrap.points[ri]).x() - to.x()) < 2);
        double right = 0;
        for (const QPolygonF &o : jp::Renderer::wrapObstacles(*d, *tb)) right = std::max(right, o.boundingRect().right());
        // Obstacles are in the text box's own coordinates.
        QVERIFY2(right > to.x() - tb->rect.left() - 1, qPrintable(QString::number(right)));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) { QTest::qWait(100); c->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/wrappoints.png"); }
        // Dragging the middle of an edge adds a point.
        const QPointF a = s->transform().map(s->wrap.points[0]), b = s->transform().map(s->wrap.points[1]);
        const QPointF mid = (a + b) / 2;
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, view(mid));
        QTest::mouseMove(vp, view(mid + QPointF(0, -10)));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, view(mid + QPointF(0, -10)));
        QCOMPARE(s->wrap.points.size(), n + 1);
        // Ctrl+click deletes it again.
        QTest::mouseClick(vp, Qt::LeftButton, Qt::ControlModifier, view(mid + QPointF(0, -10)));
        QCOMPARE(s->wrap.points.size(), n);
        // Undo walks back to the starting outline.
        w.editor()->undo();
        w.editor()->undo();
        w.editor()->undo();
        jp::Item *s2 = w.editor()->doc()->pages[0]->items[1].get();
        QCOMPARE(s2->wrap.points.size(), n);
        QVERIFY(std::abs(s2->transform().map(s2->wrap.points).boundingRect().right() - 250) < 2);
        // Esc finishes.
        w.editor()->select(s2->id);
        w.editor()->setWrapItem(s2->id);
        QTest::keyClick(vp, Qt::Key_Escape);
        QVERIFY(w.editor()->wrapItem.isEmpty());
    }

    // Edit Points: drag a corner, Ctrl+click a point to delete it, Ctrl+click an
    // edge to add one, all with real mouse events on the canvas.
    void editPoints()
    {
        jp::MainWindow w;
        w.resize(1200, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto sh = std::make_shared<jp::ShapeItem>();
        sh->shape = QStringLiteral("rect");
        sh->rect = QRectF(200, 200, 100, 100);
        sh->fill = jp::Fill::solid(jp::ColorRef::rgb(Qt::red));
        doc->pages[0]->items.push_back(sh);
        w.editor()->setDocument(std::move(doc));
        jp::ShapeItem *s = static_cast<jp::ShapeItem *>(w.editor()->doc()->pages[0]->items[0].get());
        w.editor()->select(s->id);
        QTest::qWait(50);
        w.act(QStringLiteral("shape.editPoints"))->trigger();
        QCOMPARE(w.editor()->pointsItem, s->id);
        QVERIFY(!s->customPath.isEmpty());
        jp::Canvas *c = w.canvas();
        QWidget *vp = c->viewport();
        auto view = [&](QPointF page) { return c->pageToView(page).toPoint(); };
        // Drag the bottom-right corner (300,300) to (340,330).
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, view(QPointF(300, 300)));
        QTest::mouseMove(vp, view(QPointF(320, 315)));
        QTest::mouseMove(vp, view(QPointF(340, 330)));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, view(QPointF(340, 330)));
        QRectF b = s->transform().map(s->customPath).boundingRect();
        QVERIFY2(std::abs(b.right() - 340) < 2 && std::abs(b.bottom() - 330) < 2, qPrintable(QStringLiteral("bounds %1,%2 %3x%4").arg(b.x()).arg(b.y()).arg(b.width()).arg(b.height())));
        // The frame follows the outline.
        QVERIFY(std::abs(s->rect.right() - 340) < 2);
        const int before = s->customPath.elementCount();
        // Ctrl+click the middle of the top edge adds a point.
        QTest::mousePress(vp, Qt::LeftButton, Qt::ControlModifier, view(QPointF(250, 200)));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::ControlModifier, view(QPointF(250, 200)));
        QCOMPARE(s->customPath.elementCount(), before + 1);
        // Ctrl+click that new point deletes it again.
        QTest::mousePress(vp, Qt::LeftButton, Qt::ControlModifier, view(QPointF(250, 200)));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::ControlModifier, view(QPointF(250, 200)));
        QCOMPARE(s->customPath.elementCount(), before);
        // Undo restores the drag's starting outline step by step.
        w.editor()->undo();
        w.editor()->undo();
        w.editor()->undo();
        b = w.editor()->doc()->pages[0]->items[0]->transform().map(static_cast<jp::ShapeItem *>(w.editor()->doc()->pages[0]->items[0].get())->customPath).boundingRect();
        QVERIFY2(std::abs(b.right() - 300) < 1, "undo did not restore the corner");
    }

    // A narrow window collapses ribbon groups into buttons instead of cutting
    // them off, and a collapsed group opens with its commands.
    void ribbonCollapses()
    {
        jp::MainWindow w;
        w.resize(800, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(50);
        jp::RibbonTab *home = w.findChild<jp::Ribbon *>()->current();
        QVERIFY(home->collapsedCount() > 0);
        w.resize(2400, 700);
        QTest::qWait(50);
        QCOMPARE(home->collapsedCount(), 0);
    }

    // Compound-file round trip: every stream, class id and flag survives a
    // read, write and read; libmspub imports the rewritten file the same way.
    void cfbRoundTrip_data()
    {
        QTest::addColumn<QString>("path");
        QDirIterator it(QStringLiteral(JP_TEST_DATA "/pub"), {"*.pub"}, QDir::Files);
        QStringList files;
        while (it.hasNext()) files << it.next();
        files.sort();
        for (const QString &f : files) QTest::newRow(qPrintable(QFileInfo(f).fileName())) << f;
    }
    void cfbRoundTrip()
    {
        QFETCH(QString, path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        jp::cfb::File a, b;
        QString err;
        QVERIFY2(jp::cfb::read(original, &a, &err), qPrintable(err));
        const QByteArray rewritten = jp::cfb::write(a);
        QVERIFY2(jp::cfb::read(rewritten, &b, &err), qPrintable(err));
        QCOMPARE(b.streamPaths(), a.streamPaths());
        for (const QString &sp : a.streamPaths()) QVERIFY2(b.stream(sp) == a.stream(sp), qPrintable(sp));
        QCOMPARE(b.entries.size(), a.entries.size());
        for (int i = 0; i < a.entries.size(); ++i) {
            QCOMPARE(b.entries[i].clsid, a.entries[i].clsid);
            QCOMPARE(b.entries[i].stateBits, a.entries[i].stateBits);
        }
        // Independent reader: libmspub sees the same publication.
        QTemporaryDir dir;
        const QString out = dir.filePath(QStringLiteral("rewritten.pub"));
        QFile o(out);
        QVERIFY(o.open(QIODevice::WriteOnly));
        o.write(rewritten);
        o.close();
        QString e1, e2;
        auto d1 = jp::importPublisherFile(path, &e1);
        auto d2 = jp::importPublisherFile(out, &e2);
        QCOMPARE(bool(d2), bool(d1));
        if (d1) {
            QCOMPARE(d2->pages.size(), d1->pages.size());
            // Item ids are generated afresh, so compare what is drawn.
            jp::LayoutCache c1, c2;
            jp::PaintContext p1, p2;
            p1.doc = d1.get(); p1.cache = &c1; p1.opt.output = true;
            p2.doc = d2.get(); p2.cache = &c2; p2.opt.output = true;
            for (int i = 0; i < d1->pages.size(); ++i)
                QVERIFY2(jp::Renderer::renderToImage(p1, i, 0.5) == jp::Renderer::renderToImage(p2, i, 0.5), qPrintable(QStringLiteral("page %1 differs").arg(i + 1)));
        }
    }

    // Selecting text shows its size in the ribbon's size box, and the number
    // has room to be seen (the field inside the box mustn't be padded away).
    void fontSizeBoxShowsSelection()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("Hello world"));
        doc->pages[0]->items.push_back(t);
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat f;
            f.setFontPointSize(24);
            c.mergeCharFormat(f);
        }
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        const QString id = ed->doc()->pages[0]->items[0]->id;
        ed->select(id);
        ed->beginTextEdit(id, 0);
        QTextCursor c = ed->cursor();
        c.setPosition(0);
        c.setPosition(5, QTextCursor::KeepAnchor);
        ed->setCursor(c);
        // The ribbon catches up after a moment (longer on a slow machine).
        jp::SizeCombo *box = nullptr;
        auto shown = [&] {
            for (jp::SizeCombo *s : w.findChildren<jp::SizeCombo *>())
                if (s->isVisible()) { box = s; break; }
            return box && box->currentText() == QStringLiteral("24");
        };
        QTRY_VERIFY_WITH_TIMEOUT(shown(), 3000);
        QLineEdit *le = box->lineEdit();
        // Room for the widest size even with Windows' larger UI fonts (125%).
        QFont big = le->font();
        big.setPointSizeF(big.pointSizeF() * 1.25);
        QVERIFY2(le->contentsRect().width() >= QFontMetrics(big).horizontalAdvance(QStringLiteral("288")),
                 qPrintable(QStringLiteral("size field is %1 px wide").arg(le->contentsRect().width())));
    }

    // Tabbed dialogs open wide enough for every tab, and their color
    // pickers draw as full-width fields.
    void dialogTabsFit()
    {
        jp::installUiPolish();
        jp::MainWindow w;
        w.resize(1200, 800);
        w.show();
        jp::Fill fill;
        bool checked = false;
        QTimer::singleShot(300, [&] {
            QWidget *mod = QApplication::activeModalWidget();
            if (!mod) return;
            auto *tabs = mod->findChild<QTabWidget *>();
            if (tabs) {
                QTabBar *bar = tabs->tabBar();
                checked = true;
                for (int i = 0; i < bar->count(); ++i)
                    if (!bar->rect().contains(bar->tabRect(i))) checked = false;
                for (auto *b : mod->findChildren<jp::ColorButton *>())
                    if (b->isVisible() && b->width() < 120) checked = false;
            }
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) mod->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/fillfx.png");
            mod->close();
        });
        jp::fillEffectsDialog(&w, w.editor(), fill, QStringLiteral("Format Background"));
        QVERIFY(checked);
    }

    // A custom page size (business cards, ten to a sheet) is saved, offered
    // again, and applied with its sheet margins, apart from margin guides.
    void customPageSizes()
    {
        jp::installUiPolish();
        const QJsonArray before = jp::Settings::get().customPageSizes();
        jp::Settings::get().setCustomPageSizes(QJsonArray());
        jp::MainWindow w;
        w.resize(1200, 800);
        w.show();
        QString perSheet;
        QTimer::singleShot(300, [&] {
            QWidget *mod = QApplication::activeModalWidget();
            if (!mod) return;
            mod->findChild<QLineEdit *>("pageSizeName")->setText(QStringLiteral("Test Card"));
            mod->findChild<QComboBox *>("pageLayout")->setCurrentIndex(jp::PageSetup::MultiplePerSheet);
            auto set = [&](const char *n, double pt) { mod->findChild<jp::MeasureSpin *>(n)->setValue(pt); };
            set("pageWidth", 252);
            set("pageHeight", 144);
            set("paperWidth", 612);
            set("paperHeight", 792);
            set("sideMargin", 54);
            set("topMargin", 36);
            set("gapH", 0);
            set("gapV", 0);
            perSheet = mod->findChild<QLabel *>("perSheet")->text();
            QTimer::singleShot(100, [mod] {
                if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) mod->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/newpagesize.png");
                if (auto *box = mod->findChild<QDialogButtonBox *>()) box->button(QDialogButtonBox::Ok)->click();
            });
        });
        QVERIFY(jp::createPageSizeDialog(&w, w.editor()));
        QCOMPARE(perSheet, QStringLiteral("2 across × 5 down = 10 per sheet"));
        const auto saved = jp::customPageSizes();
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved[0].first, QStringLiteral("Test Card"));
        const jp::PageSetup &ps = w.editor()->doc()->setup;
        QCOMPARE(ps.layout, jp::PageSetup::MultiplePerSheet);
        QCOMPARE(ps.size, QSizeF(252, 144));
        QCOMPARE(ps.sideMargin, 54.0);
        QCOMPARE(ps.gridCols, 2);
        QCOMPARE(ps.gridRows, 5);
        QCOMPARE(ps.sizeName, QStringLiteral("Test Card"));
        jp::Settings::get().setCustomPageSizes(before);
    }

    // The thesaurus finds synonyms, including for inflected words.
    void thesaurusFinds()
    {
        for (const char *w : {"happy", "Happy", "running", "houses", "quickly", "bigger"})
            QVERIFY2(!jp::thesaurusLookup(QString::fromLatin1(w)).isEmpty(), w);
        QVERIFY(jp::thesaurusLookup("zzxqv").isEmpty());
    }

    // Every font list row is the same height, and every name stays inside it.
    void fontListRows()
    {
        jp::FontCombo combo;
        auto *view = combo.view();
        auto *model = combo.model();
        const int n = qMin(model->rowCount(), 400);
        QStyleOptionViewItem opt;
        opt.initFrom(view);
        opt.font = view->font();
        const int h = view->itemDelegate()->sizeHint(opt, model->index(4, 0)).height();
        QVERIFY(h >= 20 && h <= 40);
        QImage img(260, h * n, QImage::Format_ARGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        for (int i = 0; i < n; ++i) {
            const QModelIndex idx = model->index(i, 0);
            QCOMPARE(view->itemDelegate()->sizeHint(opt, idx).height(), h);
            opt.rect = QRect(0, i * h, 260, h);
            view->itemDelegate()->paint(&p, opt, idx);
        }
        p.end();
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) img.save(qEnvironmentVariable("JP_SHOT_DIR") + "/fontlist.png");
        // Every row draws something.
        for (int i = 0; i < n; ++i) {
            bool ink = false;
            for (int y = i * h; y < (i + 1) * h && !ink; ++y)
                for (int x = 0; x < 260 && !ink; ++x) ink = qGray(img.pixel(x, y)) < 200;
            QVERIFY2(ink, qPrintable(model->index(i, 0).data().toString()));
        }
    }

    // Update check compares versions numerically, previews before releases.
    // Updates are looked for at every launch, even after a check earlier
    // the same day, unless automatic checks are off.
    void updateCheckEveryLaunch()
    {
        jp::Settings &st = jp::Settings::get();
        const QVariant oldAuto = st.value(QStringLiteral("updates/auto")), oldLast = st.value(QStringLiteral("updates/lastCheck"));
        st.setValue(QStringLiteral("updates/lastCheck"), QDate::currentDate());
        st.setValue(QStringLiteral("updates/auto"), true);
        QVERIFY(jp::Updater::checksOnStartup());
        st.setValue(QStringLiteral("updates/auto"), false);
        QVERIFY(!jp::Updater::checksOnStartup());
        st.setValue(QStringLiteral("updates/auto"), oldAuto.isValid() ? oldAuto : QVariant(true));
        st.setValue(QStringLiteral("updates/lastCheck"), oldLast);
    }

    // The update offer shows what is new, formatted, without the install
    // steps or the preview notice the release page carries.
    void updateOfferNotes()
    {
        const QString body = QStringLiteral(
            "**Early preview for testing. Not finished software.** JeffPub is an independent open-source project.\r\n\r\n"
            "### Install\r\n- **Windows:** download **JeffPub-Setup.exe** and run it.\r\n"
            "- **Debian:** run `sudo apt install ./jeffpub_0.1.19_amd64.deb`.\r\n\r\n"
            "JeffPub checks for new versions once a day.\r\n\r\n"
            "### New in preview 19\r\n- **The New Publication page opens categories at once.** Thumbnails fill in.\r\n- Banners are quicker.\r\n");
        const QString h = jp::Updater::releaseHighlights(body);
        QVERIFY(h.startsWith(QLatin1String("**New in preview 19**")));
        QVERIFY(h.contains(QLatin1String("opens categories at once")));
        QVERIFY(h.contains(QLatin1String("- Banners are quicker.")));
        for (const char *gone : {"Early preview", "Install", "sudo apt", "once a day", "\r"}) QVERIFY2(!h.contains(QLatin1String(gone)), gone);
        // Notes without a "New in" section keep their other sections.
        QCOMPARE(jp::Updater::releaseHighlights(QStringLiteral("Preview.\n\n### Install\n- run it\n\n### Changes\n- quicker")),
                 QStringLiteral("**Changes**\n\n- quicker"));

        const QString oldSheet = qApp->styleSheet();
        const QIcon oldIcon = QApplication::windowIcon();
        qApp->setStyleSheet(jp::modernStyleSheet());
        Q_INIT_RESOURCE(resources);   // the app icon lives in the static jpcore library
        QApplication::setWindowIcon(QIcon(QStringLiteral(":/app.png")));
        std::unique_ptr<QDialog> d(jp::Updater::offerDialog(nullptr, QStringLiteral("0.1.19"), h, true, QStringLiteral("https://github.com/JeffOffice/jeffpub/releases/tag/v0.1.19")));
        d->show();
        QApplication::processEvents();
        auto *notes = d->findChild<QTextBrowser *>();
        QVERIFY(notes);
        const QString shown = notes->toPlainText();
        QVERIFY(shown.contains(QLatin1String("The New Publication page opens categories at once.")));
        QVERIFY(!shown.contains(QLatin1Char('*')));   // Markdown formatted, not shown as text
        QPushButton *now = nullptr;
        for (QPushButton *b : d->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Update Now")) now = b;
        QVERIFY(now && now->isDefault() && now->property("primary").toBool());
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) d->grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/update-offer.png");
        now->click();
        QCOMPARE(d->result(), int(jp::Updater::Install));
        d.reset();
        qApp->setStyleSheet(oldSheet);
        QApplication::setWindowIcon(oldIcon);
    }

    void updateVersionOrder()
    {
        QVERIFY(jp::Updater::isNewer("v0.1.6", "0.1.0"));
        QVERIFY(jp::Updater::isNewer("v0.1.6", "v0.1.0-preview5"));
        QVERIFY(jp::Updater::isNewer("v0.1.0-preview5", "v0.1.0-preview4"));
        // An installer runs only from this project's releases, with a plain
        // version tag and the SHA-256 GitHub publishes.
        const QString url = QStringLiteral("https://github.com/JeffOffice/jeffpub/releases/download/v0.1.17/JeffPub-Setup.exe");
        const QString digest = QStringLiteral("sha256:63e329c3c1a9aea0f90af5e2519c9315af365cb424a7bd00abb3ad9d201573e7");
        QVERIFY(jp::Updater::trustedInstaller(url, QStringLiteral("v0.1.17"), digest));
        QVERIFY(!jp::Updater::trustedInstaller(QStringLiteral("https://example.com/JeffPub-Setup.exe"), QStringLiteral("v0.1.17"), digest));
        QVERIFY(!jp::Updater::trustedInstaller(QStringLiteral("http://github.com/JeffOffice/jeffpub/releases/download/v0.1.17/x.exe"), QStringLiteral("v0.1.17"), digest));
        QVERIFY(!jp::Updater::trustedInstaller(url, QStringLiteral("../../Startup/evil"), digest));   // the tag names the saved file
        QVERIFY(!jp::Updater::trustedInstaller(url, QStringLiteral("v0.1.17"), QString()));          // nothing to check against
        QVERIFY(jp::Updater::isNewer("0.1.10", "0.1.9"));
        QVERIFY(jp::Updater::isNewer("v0.5.0", "0.1.39"));   // the beta after the 0.1 previews
        QVERIFY(!jp::Updater::isNewer("v0.1.6", "0.1.6"));
        QVERIFY(!jp::Updater::isNewer("v0.1.0-preview5", "0.1.6"));
        QVERIFY(jp::Updater::isNewer("v0.1.0", "v0.1.0-preview5"));
        // The newest version is offered whatever order the list comes in (a
        // release published again is listed first, as the newest made).
        auto rel = [](const char *tag, bool draft = false) { return QJsonObject{{"tag_name", QString::fromLatin1(tag)}, {"draft", draft}}; };
        const QJsonArray list{rel("v0.1.9"), rel("v0.5.2"), rel("v0.6.0", true), rel("v0.5.1"), rel("v0.1.39")};
        const auto newer = jp::Updater::newerReleases(list, QStringLiteral("0.5.0"));
        QCOMPARE(newer.size(), 2);
        QCOMPARE(newer[0].value("tag_name").toString(), QStringLiteral("v0.5.2"));
        QCOMPARE(newer[1].value("tag_name").toString(), QStringLiteral("v0.5.1"));
        QVERIFY(jp::Updater::newerReleases(list, QStringLiteral("0.5.2")).isEmpty());
    }

    void movesJeffPub79Settings()
    {
        // JeffPub 79's settings come over, except what setup already chose.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings old(dir.filePath("old.ini"), QSettings::IniFormat), now(dir.filePath("new.ini"), QSettings::IniFormat);
        old.setValue("telemetry/install", "6f1c2d3e-0000-4000-8000-000000000079");
        old.setValue("telemetry/enabled", true);
        old.setValue("recent", QStringList{"/home/a/flyer.jpub", "/home/a/menu.pub"});
        now.setValue("telemetry/enabled", false);
        jp::copyMissingSettings(old, now);
        QCOMPARE(now.value("telemetry/install").toString(), QStringLiteral("6f1c2d3e-0000-4000-8000-000000000079"));
        QCOMPARE(now.value("recent").toStringList(), (QStringList{"/home/a/flyer.jpub", "/home/a/menu.pub"}));
        QCOMPARE(now.value("telemetry/enabled").toBool(), false);

        // Its folder of templates and building blocks moves to the new name.
        const QString from = dir.filePath("JeffPub/JeffPub 79"), to = dir.filePath("JeffOffice/JeffPub");
        QVERIFY(QDir().mkpath(from + "/Templates") && QDir().mkpath(from + "/BuildingBlocks"));
        for (const QString f : {QStringLiteral("/Templates/club.jpub"), QStringLiteral("/BuildingBlocks/logo.json")}) {
            QFile file(from + f);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("kept");
        }
        QVERIFY(jp::moveDataFolder(from, to));
        QVERIFY(QFile::exists(to + "/Templates/club.jpub") && QFile::exists(to + "/BuildingBlocks/logo.json"));
        QVERIFY(!QFileInfo::exists(from));
        // Never over a folder the renamed program already has.
        QVERIFY(QDir().mkpath(from));
        QVERIFY(!jp::moveDataFolder(from, to));
        QVERIFY(QFile::exists(to + "/Templates/club.jpub"));
    }

    // Folded cards and envelopes keep their layout through a .pub, written
    // as Publisher writes them (DOCUMENT field 11: 3 and 7; a card's pages as
    // spreads, flags 06 and 0b); Publisher kept both when saving them again.
    void pubFoldedCardAndEnvelope()
    {
        for (const auto &[id, layout, code] : {std::tuple{QStringLiteral("greeting-birthday"), PageSetup::FoldedCard, 3u},
                                                std::tuple{QStringLiteral("envelope-10"), PageSetup::Envelope, 7u}}) {
            const TemplateInfo *t = findTemplate(id);
            QVERIFY(t);
            auto doc = t->build(TemplateOptions());
            QCOMPARE(doc->setup.layout, layout);
            QTemporaryDir dir;
            const QString path = dir.filePath(id + QStringLiteral(".pub"));
            QString err;
            QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
            const QByteArray contents = cfb::readStream(path, QStringLiteral("Contents"));
            QByteArray field("\x11\x20\x00\x00\x00\x00", 6);
            field[2] = char(code);
            QVERIFY(contents.contains(field));
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            auto back = importPublisher(f.readAll(), nullptr);
            QVERIFY(back);
            QCOMPARE(back->setup.layout, layout);
            QCOMPARE(back->pages.size(), doc->pages.size());
            QCOMPARE(back->pageSize(), doc->pageSize());
        }
    }

    // Gradients keep their direction and colors through a .pub: Publisher
    // runs the first color toward 270 + the file's angle (its own pictures
    // of 113 gradients), and from the center the first color is the center.
    void pubGradientsRoundTrip()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto add = [&](Fill::GradType type, double angle, double x) {
            auto s = std::make_shared<ShapeItem>();
            s->shape = QStringLiteral("rect");
            s->rect = QRectF(x, 72, 80, 120);
            s->stroke.color = ColorRef::none();
            s->fill.type = Fill::Gradient;
            s->fill.gradType = type;
            s->fill.angle = angle;
            s->fill.stops = {GradientStop{0, ColorRef::rgb(QColor(200, 0, 0)), 0}, GradientStop{1, ColorRef::rgb(QColor(0, 0, 200)), 0}};
            s->fill.color = s->fill.stops.first().color;
            s->fill.color2 = s->fill.stops.last().color;
            doc->pages[0]->items.push_back(s);
        };
        const double angles[] = {0, 90, 180, 270};
        for (int i = 0; i < 4; ++i) add(Fill::Linear, angles[i], 36 + 90 * i);
        add(Fill::Radial, 0, 400);
        add(Fill::PathGrad, 0, 490);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("grad.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto back = importPublisher(f.readAll(), nullptr);
        QVERIFY(back);
        QVector<const ShapeItem *> got;
        for (const auto &it : back->pages[0]->items)
            if (auto *s = dynamic_cast<const ShapeItem *>(it.get()); s && s->fill.type == Fill::Gradient) got << s;
        std::sort(got.begin(), got.end(), [](auto *a, auto *b) { return a->rect.x() < b->rect.x(); });
        QCOMPARE(got.size(), 6);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(got[i]->fill.gradType, Fill::Linear);
            QVERIFY2(std::abs(std::remainder(got[i]->fill.angle - angles[i], 360.0)) < 0.5, qPrintable(QString::number(got[i]->fill.angle)));
        }
        QCOMPARE(got[4]->fill.gradType, Fill::Radial);
        QCOMPARE(got[5]->fill.gradType, Fill::PathGrad);
        for (int i : {4, 5}) {
            const auto &st = got[i]->fill.stops;
            QVERIFY(st.size() >= 2);
            QCOMPARE(st.first().color.rgbValue(), QColor(200, 0, 0));   // the center
            QCOMPARE(st.last().color.rgbValue(), QColor(0, 0, 200));
        }
    }

    // A picture keeps its quarter turns and its own fill through a .pub:
    // rotated flowers lost their quarter turns (the outline's turn is folded
    // within 45 degrees), and pinwheel art lost the colors its shape's fill
    // shows through the metafile's gaps.
    void pubPictureTurnsAndFill()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        QImage img(40, 20, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QPainter(&img).fillRect(0, 0, 20, 20, Qt::blue);
        QByteArray png;
        {
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
        }
        const double turns[] = {0, 90, 200, 290};
        for (int i = 0; i < 4; ++i) {
            auto pic = std::make_shared<PictureItem>();
            pic->imageId = doc->addImage(png, QStringLiteral("png"));
            pic->rect = QRectF(72 + 130 * i, 300, 120, 60);
            pic->rotation = turns[i];
            if (i == 1) pic->fill = Fill::solid(ColorRef::rgb(QColor(0x33, 0x66, 0x66)));
            doc->pages[0]->items.push_back(pic);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("pics.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto back = importPublisher(f.readAll(), nullptr);
        QVERIFY(back);
        QVector<const PictureItem *> got;
        for (const auto &it : back->pages[0]->items)
            if (auto *p = dynamic_cast<const PictureItem *>(it.get())) got << p;
        std::sort(got.begin(), got.end(), [](auto *a, auto *b) { return a->rect.center().x() < b->rect.center().x(); });
        QCOMPARE(got.size(), 4);
        for (int i = 0; i < 4; ++i) {
            QVERIFY2(std::abs(std::remainder(got[i]->rotation - turns[i], 360.0)) < 0.5, qPrintable(QStringLiteral("%1: %2").arg(i).arg(got[i]->rotation)));
            QVERIFY2(std::abs(got[i]->rect.width() - 120) < 0.5 && std::abs(got[i]->rect.height() - 60) < 0.5,
                     qPrintable(QStringLiteral("%1: %2x%3").arg(i).arg(got[i]->rect.width()).arg(got[i]->rect.height())));
        }
        QCOMPARE(got[1]->fill.type, Fill::Solid);
        QCOMPARE(got[1]->fill.color.rgbValue(), QColor(0x33, 0x66, 0x66));
        QCOMPARE(got[0]->fill.type, Fill::NoFill);
    }

    // A freeform's formulas can refer to earlier ones (up to 256 back),
    // each up to three times: evaluated afresh at every reference, a chain
    // of them took exponential time, so a crafted file hung on opening.
    // Each formula is now worked out once per point.
    void pubFormulaChainIsFast()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto s = std::make_shared<ShapeItem>();
        s->shape = QStringLiteral("rect");
        s->rect = QRectF(100, 100, 200, 150);
        QPainterPath path;
        path.moveTo(0, 0);
        path.lineTo(200, 20);
        path.lineTo(120, 150);
        path.closeSubpath();
        s->customPath = path;
        s->fill = Fill::solid(ColorRef::rgb(Qt::red));
        doc->pages[0]->items.push_back(s);
        QTemporaryDir dir;
        const QString pathName = dir.filePath(QStringLiteral("chain.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, pathName, &err), qPrintable(err));
        QFile f(pathName);
        QVERIFY(f.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(f.readAll(), &file, &err));
        const QByteArray escher = cfb::readStream(pathName, QStringLiteral("Escher/EscherStm"));

        // 22 formulas: the first is 1, each next the average of the one
        // before taken twice (still 1); every point's x refers to the last.
        const int n = 22;
        QByteArray guides(6 + 8 * n, 0);
        qToLittleEndian<quint16>(n, guides.data());
        qToLittleEndian<quint16>(n, guides.data() + 2);
        qToLittleEndian<quint16>(8, guides.data() + 4);
        for (int i = 0; i < n; ++i) {
            char *g = guides.data() + 6 + 8 * i;
            if (i == 0) {
                qToLittleEndian<quint16>(0, g);   // a + b - c
                qToLittleEndian<qint16>(1, g + 2);
            } else {
                qToLittleEndian<quint16>(0x6002, g);   // (a + b) / 2, a and b refer
                qToLittleEndian<qint16>(qint16(0x400 | (i - 1)), g + 2);
                qToLittleEndian<qint16>(qint16(0x400 | (i - 1)), g + 4);
            }
        }
        // The formulas go with the freeform's points (0xC145), whose x
        // values are set to refer to the last formula.
        const QByteArray rebuilt = editFirstOpt(escher, [&](QByteArray &body, quint16 &head) {
            const int pointsAt = optComplexAt(body, head, 0x0145);
            if (pointsAt < 0) return false;
            const int points = qFromLittleEndian<quint16>(body.constData() + pointsAt);
            for (int i = 0; i < points; ++i) qToLittleEndian<quint32>(0x80000000u | (n - 1), body.data() + pointsAt + 6 + 8 * i);
            addOptProp(body, head, 0xC156, 0, guides);
            return true;
        });
        QVERIFY(!rebuilt.isEmpty());
        QVERIFY(file.setStream(QStringLiteral("Escher/EscherStm"), rebuilt));
        QElapsedTimer t;
        t.start();
        auto back = importPublisher(cfb::write(file), nullptr);
        QVERIFY(back);
        QVERIFY2(t.elapsed() < 1500, qPrintable(QStringLiteral("%1 ms").arg(t.elapsed())));
    }

    // A freeform's coordinate space (0x0140-0x0143) is any 32-bit range;
    // its width came from a 32-bit subtraction, which overflows (undefined
    // behavior; the sanitizer build reports it) for -2^31 to 2^31-1.
    void pubHugeCoordinateSpace()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto s = std::make_shared<ShapeItem>();
        s->shape = QStringLiteral("rect");
        s->rect = QRectF(100, 100, 200, 150);
        QPainterPath path;
        path.moveTo(0, 0);
        path.lineTo(200, 20);
        path.lineTo(120, 150);
        path.closeSubpath();
        s->customPath = path;
        s->fill = Fill::solid(ColorRef::rgb(Qt::red));
        doc->pages[0]->items.push_back(s);
        QTemporaryDir dir;
        const QString name = dir.filePath(QStringLiteral("space.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, name, &err), qPrintable(err));
        QFile f(name);
        QVERIFY(f.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(f.readAll(), &file, &err));
        const QByteArray rebuilt = editFirstOpt(cfb::readStream(name, QStringLiteral("Escher/EscherStm")), [&](QByteArray &body, quint16 &head) {
            if (optComplexAt(body, head, 0x0145) < 0) return false;
            for (int i = 0; i < (head >> 4); ++i)
                if (qFromLittleEndian<quint16>(body.constData() + i * 6) == 0x0142) qToLittleEndian<quint32>(0x7FFFFFFFu, body.data() + i * 6 + 2);
            addOptProp(body, head, 0x0140, 0x80000000u);
            return true;
        });
        QVERIFY(!rebuilt.isEmpty());
        QVERIFY(file.setStream(QStringLiteral("Escher/EscherStm"), rebuilt));
        auto back = importPublisher(cfb::write(file), nullptr);
        QVERIFY(back);
        for (const auto &it : back->pages[0]->items) {
            const QRectF r = it->rect;
            QVERIFY(std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) && std::isfinite(r.height()));
        }
    }

    // Publisher's default text box shadow is stored as "the line color,
    // lightened halfway" (0x107F02F2): gray for a black line. libmspub
    // lightened the reference's own bytes into pink.
    void pubShadowFromLineColor()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 200, 100);
        t->storyId = doc->createStory(QStringLiteral("Shadowed"));
        t->fill = Fill::solid(ColorRef::rgb(Qt::white));
        t->stroke.color = ColorRef::rgb(Qt::black);
        t->stroke.width = 1;
        t->fx.shadow.on = true;
        t->fx.shadow.distance = 6;
        t->fx.shadow.color = ColorRef::rgb(QColor(0x12, 0x34, 0x56));
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("shadow.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(f.readAll(), &file, &err));
        QByteArray escher = cfb::readStream(path, QStringLiteral("Escher/EscherStm"));
        auto prop = [](quint16 id, quint32 value) {
            QByteArray b(6, 0);
            qToLittleEndian<quint16>(id, b.data());
            qToLittleEndian<quint32>(value, b.data() + 2);
            return b;
        };
        QCOMPARE(int(escher.count(prop(0x0201, 0x00563412))), 1);
        escher.replace(prop(0x0201, 0x00563412), prop(0x0201, 0x107F02F2));
        QVERIFY(file.setStream(QStringLiteral("Escher/EscherStm"), escher));
        auto back = importPublisher(cfb::write(file), nullptr);
        QVERIFY(back);
        const Item *got = nullptr;
        for (const auto &it : back->pages[0]->items)
            if (it->fx.shadow.on) got = it.get();
        QVERIFY(got);
        const QColor c = got->fx.shadow.color.rgbValue();
        QVERIFY2(std::abs(c.red() - 128) <= 2 && std::abs(c.green() - 128) <= 2 && std::abs(c.blue() - 128) <= 2, qPrintable(c.name()));
    }

    // A line's corners go through a .pub: Publisher's designs store round
    // corners (0x01D6 = 2) and JeffPub drew them mitered.
    void pubLineCornersRoundTrip()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        const Qt::PenJoinStyle joins[] = {Qt::MiterJoin, Qt::RoundJoin, Qt::BevelJoin};
        for (int i = 0; i < 3; ++i) {
            auto s = std::make_shared<ShapeItem>();
            s->shape = QStringLiteral("triangle");
            s->rect = QRectF(72 + 160 * i, 100, 120, 100);
            s->fill = Fill::none();
            s->stroke.color = ColorRef::rgb(Qt::black);
            s->stroke.width = 12;
            s->stroke.join = joins[i];
            doc->pages[0]->items.push_back(s);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("joins.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto back = importPublisher(f.readAll(), nullptr);
        QVERIFY(back);
        QVector<const Item *> got;
        for (const auto &it : back->pages[0]->items)
            if (!it->stroke.isNone()) got << it.get();
        std::sort(got.begin(), got.end(), [](auto *a, auto *b) { return a->rect.x() < b->rect.x(); });
        QCOMPARE(got.size(), 3);
        for (int i = 0; i < 3; ++i) QCOMPARE(got[i]->stroke.join, joins[i]);
    }

    // An empty rectangle's outline comes back on its frame: libmspub moved
    // the outline's top edge out by half the line (it applies one edge per
    // side line, and a plain outline is one line), so a sign's 12-pt frame
    // drawn inside its edge sat 6 pt too high.
    void pubOutlineKeepsFrame()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        for (bool inset : {false, true}) {
            auto s = std::make_shared<ShapeItem>();
            s->shape = QStringLiteral("rect");
            s->rect = QRectF(inset ? 340 : 72, 100, 200, 150);
            s->fill = Fill::none();
            s->stroke.color = ColorRef::rgb(Qt::black);
            s->stroke.width = 12;
            s->stroke.inset = inset;
            doc->pages[0]->items.push_back(s);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("frame.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto back = importPublisher(f.readAll(), nullptr);
        QVERIFY(back);
        int n = 0;
        for (const auto &it : back->pages[0]->items) {
            if (it->stroke.isNone()) continue;
            ++n;
            const QRectF want(it->rect.x() < 300 ? 72 : 340, 100, 200, 150);
            QVERIFY2(std::abs(it->rect.top() - want.top()) < 0.1 && std::abs(it->rect.bottom() - want.bottom()) < 0.1
                         && std::abs(it->rect.left() - want.left()) < 0.1 && std::abs(it->rect.right() - want.right()) < 0.1,
                     qPrintable(QStringLiteral("%1,%2 %3x%4").arg(it->rect.x()).arg(it->rect.y()).arg(it->rect.width()).arg(it->rect.height())));
        }
        QCOMPARE(n, 2);
    }

    // Gradient angles as Publisher reads them (its pictures of JeffPub's
    // files and of its own): a stored angle a of 0 or more runs the first
    // color toward 270 - a, a negative one toward 90 - a. A gradient turns
    // with its shape only when the tertiary fill flags (0x01BF) carry 0x20
    // with its use bit; otherwise it keeps its direction on the page.
    // JeffPub saved 45 degrees as 135 (mirrored) and never set the flag.
    void pubGradientAnglesAsPublisher()
    {
        struct Case { double angle, rotation; bool flipH, flipV; };
        const Case cases[] = {{45, 0, false, false}, {45, 0, true, false}, {45, 0, false, true}, {135, 90, false, false}, {60, 0, false, false}, {300, 30, false, false}};
        auto doc = Document::blank(QSizeF(792, 612));
        int i = 0;
        for (const Case &c : cases) {
            auto s = std::make_shared<ShapeItem>();
            s->shape = QStringLiteral("rect");
            s->rect = QRectF(36 + 120 * i++, 72, 100, 100);
            s->rotation = c.rotation;
            s->flipH = c.flipH;
            s->flipV = c.flipV;
            s->stroke.color = ColorRef::none();
            s->fill.type = Fill::Gradient;
            s->fill.gradType = Fill::Linear;
            s->fill.angle = c.angle;
            s->fill.stops = {GradientStop{0, ColorRef::rgb(QColor(200, 0, 0)), 0}, GradientStop{1, ColorRef::rgb(QColor(0, 0, 200)), 0}};
            s->fill.color = s->fill.stops.first().color;
            s->fill.color2 = s->fill.stops.last().color;
            doc->pages[0]->items.push_back(s);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("grad.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto prop = [](quint16 id, quint32 value) {
            QByteArray b(6, 0);
            qToLittleEndian<quint16>(id, b.data());
            qToLittleEndian<quint32>(value, b.data() + 2);
            return b;
        };
        QByteArray escher = cfb::readStream(path, QStringLiteral("Escher/EscherStm"));
        QVERIFY(escher.contains(prop(0x018b, 225u << 16)));   // 45 degrees, as Publisher's dialog stores 225
        QCOMPARE(int(escher.count(prop(0x01bf, 0x00600020))), 6);
        // Where each first color runs on the page.
        auto shown = [](const ShapeItem *s) {
            double a = s->fill.angle;
            if (s->flipH) a = 180 - a;
            if (s->flipV) a = -a;
            return a + s->rotation;
        };
        auto read = [&](const QByteArray &bytes) {
            auto back = importPublisher(bytes, nullptr);
            QVector<const ShapeItem *> got;
            if (back) {
                for (const auto &it : back->pages[0]->items)
                    if (auto *s = dynamic_cast<const ShapeItem *>(it.get()); s && s->fill.type == Fill::Gradient) got << s;
                std::sort(got.begin(), got.end(), [](auto *a, auto *b) { return a->rect.center().x() < b->rect.center().x(); });
            }
            return std::make_pair(std::move(back), got);
        };
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        {
            auto [back, got] = read(bytes);
            QCOMPARE(got.size(), 6);
            for (int k = 0; k < 6; ++k) {
                const Case &c = cases[k];
                double want = c.angle;
                if (c.flipH) want = 180 - want;
                if (c.flipV) want = -want;
                want += c.rotation;
                QVERIFY2(std::abs(std::remainder(shown(got[k]) - want, 360.0)) < 0.5, qPrintable(QStringLiteral("%1: %2").arg(k).arg(shown(got[k]))));
            }
        }
        // Without the flag Publisher keeps each gradient's stored direction
        // on the page, whatever the shape's turns and flips.
        cfb::File file;
        QVERIFY(cfb::read(bytes, &file, &err));
        escher.replace(prop(0x01bf, 0x00600020), prop(0x01bf, 0));
        QVERIFY(file.setStream(QStringLiteral("Escher/EscherStm"), escher));
        {
            auto [back, got] = read(cfb::write(file));
            QCOMPARE(got.size(), 6);
            for (int k = 0; k < 6; ++k)
                QVERIFY2(std::abs(std::remainder(shown(got[k]) - cases[k].angle, 360.0)) < 0.5, qPrintable(QStringLiteral("%1: %2").arg(k).arg(shown(got[k]))));
        }
    }

    // Best fit grows text only as far as the box holds it: a banner's short
    // headline grew to fill the width, its one line three times taller than
    // the box (Publisher's banner designs fit theirs inside).
    // Best Fit judges a line by its text, not the spacing under it: sign
    // designs' one-line headlines with 125-130% line spacing shrank to
    // 0.81-0.84 although Publisher draws them at the size the file stores.
    void bestFitIgnoresSpacingUnderLastLine()
    {
        auto doc = Document::blank(QSizeF(792, 612));
        auto t = std::make_shared<TextItem>();
        t->insets = QMarginsF(0, 0, 0, 0);
        t->autofit = TextItem::BestFit;
        t->fitAsStored = true;
        t->storyId = doc->createStory(QStringLiteral("Private"));
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat big;
            big.setFontPointSize(100);
            c.mergeCharFormat(big);
        }
        doc->pages[0]->items.push_back(t);
        LayoutCache cache;
        RenderOptions opt;
        // The line's own height, at single spacing in a tall box.
        t->rect = QRectF(36, 36, 700, 400);
        const auto single = cache.textFrame(*doc, *t, 1, opt).layout->lineInfo(0);
        QCOMPARE(single.size(), 1);
        const double h = single.first().rect.height();
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextBlockFormat bf;
            bf.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
            c.mergeBlockFormat(bf);
        }
        t->rect = QRectF(36, 36, 700, h * 1.1);   // holds the text, not 130% of it
        const auto fl = cache.textFrame(*doc, *t, 1, opt);
        QCOMPARE(fl.fitScale, 1.0);
    }

    void bestFitStaysInsideBox()
    {
        auto doc = Document::blank(QSizeF(4320, 612));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(528, 155, 3263, 301);
        t->autofit = TextItem::BestFit;
        t->storyId = doc->createStory(QStringLiteral("Register Here"));
        {
            QTextCursor c(doc->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            QTextCharFormat big;
            big.setFontPointSize(72);
            c.mergeCharFormat(big);
        }
        doc->pages[0]->items.push_back(t);
        LayoutCache cache;
        RenderOptions opt;
        const auto fl = cache.textFrame(*doc, *t, 1, opt);
        QVERIFY(fl.layout && !fl.layout->overflow());
        const auto lines = fl.layout->lineInfo(0);
        QVERIFY(!lines.isEmpty());
        const double room = t->rect.height();
        for (const auto &li : lines) QVERIFY2(li.rect.bottom() <= room + 0.5, qPrintable(QString::number(li.rect.bottom())));
        // And it still grows: the line fills most of the box's height.
        QVERIFY2(lines.last().rect.bottom() - lines.first().rect.top() > 0.6 * room, qPrintable(QString::number(fl.fitScale)));

        // From a .pub, the text keeps the size the file stores (Publisher
        // fitted it): no growing, and shrinking only when it doesn't fit.
        t->fitAsStored = true;
        {
            TextItem copy;
            copy.fromJson(t->toJson());
            QVERIFY(copy.fitAsStored);
        }
        QCOMPARE(cache.textFrame(*doc, *t, 1, opt).fitScale, 1.0);
        t->rect = QRectF(528, 155, 250, 100);   // two lines of 72 points don't fit
        const double shrunk = cache.textFrame(*doc, *t, 1, opt).fitScale;
        QVERIFY2(shrunk < 1.0, qPrintable(QString::number(shrunk)));
    }

    // A run that doesn't say it's bold isn't, whatever the paragraph's first
    // run is: after a bold lead-in, the rest of a .pub paragraph (its runs
    // state only what they turn on) came out bold and underlined.
    void runsDontInheritFirstRun()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        const QString text = QStringLiteral("and the rest of this paragraph is plain text that wraps across several lines of a narrow box");
        auto box = [&](bool explicitPlain, double y) {
            auto t = std::make_shared<TextItem>();
            t->rect = QRectF(72, y, 220, 200);
            t->storyId = doc->createStory();
            QTextCursor c(doc->storyDoc(t->storyId));
            QTextCharFormat lead;
            lead.setFontFamilies({QStringLiteral("Carlito")});
            lead.setFontPointSize(12);
            lead.setFontWeight(QFont::Bold);
            lead.setFontUnderline(true);
            c.insertText(QStringLiteral("NOTE: "), lead);
            QTextCharFormat rest;
            rest.setFontFamilies({QStringLiteral("Carlito")});
            rest.setFontPointSize(12);
            if (explicitPlain) {
                rest.setFontWeight(QFont::Normal);
                rest.setFontUnderline(false);
            }
            c.insertText(text, rest);
            doc->pages[0]->items.push_back(t);
            return t;
        };
        auto unsaid = box(false, 72), said = box(true, 400);
        LayoutCache cache;
        RenderOptions opt;
        auto lines = [&](const std::shared_ptr<TextItem> &t) {
            QStringList out;
            for (const auto &li : cache.textFrame(*doc, *t, 1, opt).layout->lineInfo(0)) out << li.text;
            return out;
        };
        const QStringList a = lines(unsaid), b = lines(said);
        QVERIFY(b.size() >= 3);
        QCOMPARE(a, b);
    }

    // Publisher's superscript keeps flags in the high byte (0xF001): "5th"
    // read back from such a file keeps its raised letters.
    void pubSuperscriptHighByte()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("5th"));
        QTextCursor c(doc->storyDoc(t->storyId));
        c.setPosition(1);
        c.setPosition(3, QTextCursor::KeepAnchor);
        QTextCharFormat up;
        up.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        c.mergeCharFormat(up);
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sup.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        cfb::File file;
        QVERIFY(cfb::read(f.readAll(), &file));
        f.close();
        QByteArray q = file.stream(QStringLiteral("Quill/QuillSub/CONTENTS"));
        const QByteArray mark("\x0f\x12\x01\x00", 4);
        QCOMPARE(q.count(mark), 1);
        q.replace(mark, QByteArray("\x0f\x12\x01\xf0", 4));
        QVERIFY(file.setStream(QStringLiteral("Quill/QuillSub/CONTENTS"), q));
        auto back = importPublisher(cfb::write(file), nullptr);
        QVERIFY(back);
        bool raised = false;
        for (const auto &st : back->stories)
            for (QTextBlock b = st->doc->begin(); b.isValid(); b = b.next())
                for (auto it = b.begin(); !it.atEnd(); ++it)
                    if (it.fragment().text().startsWith(QStringLiteral("th")))
                        raised = it.fragment().charFormat().verticalAlignment() == QTextCharFormat::AlignSuperScript;
        QVERIFY(raised);
    }

    // A link with no color of its own shows as a link: the .pub reader's
    // fallback black made e-mail links black where Publisher shows them blue.
    void pubLinkKeepsLinkColor()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(72, 72, 300, 100);
        t->storyId = doc->createStory(QStringLiteral("Write to us"));
        QTextCursor c(doc->storyDoc(t->storyId));
        c.setPosition(9);
        c.setPosition(11, QTextCursor::KeepAnchor);
        QTextCharFormat link;
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("mailto:office@example.com"));
        c.mergeCharFormat(link);
        doc->pages[0]->items.push_back(t);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("link.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto back = importPublisher(f.readAll(), nullptr);
        QVERIFY(back);
        int links = 0;
        for (const auto &st : back->stories)
            for (QTextBlock b = st->doc->begin(); b.isValid(); b = b.next())
                for (auto it = b.begin(); !it.atEnd(); ++it)
                    if (it.fragment().charFormat().isAnchor()) {
                        ++links;
                        QVERIFY2(!it.fragment().charFormat().hasProperty(tp::ColorRefP), qPrintable(it.fragment().charFormat().stringProperty(tp::ColorRefP)));
                    }
        QVERIFY(links > 0);
    }

    // Publisher's numbering "(none)" (list kind 255) is no list: the sample
    // newsletter's paragraph with it took a number.
    void pubNoneNumberingIsNoList()
    {
        QFile f(QStringLiteral(JP_TEST_DATA "/pub/poi-SampleNewsletter.pub"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto doc = importPublisher(f.readAll(), nullptr);
        QVERIFY(doc);
        int numbered = 0;
        for (const auto &st : doc->stories)
            for (QTextBlock b = st->doc->begin(); b.isValid(); b = b.next())
                if (b.textList() && !jp::isBulletList(b.textList()->format().style())) ++numbered;
        QCOMPARE(numbered, 0);
        // Its bulleted paragraphs take their bullet from their paragraph
        // style (they have no list of their own), as in Publisher.
        int bulleted = 0;
        for (const auto &st : doc->stories)
            for (QTextBlock b = st->doc->begin(); b.isValid(); b = b.next())
                if (b.textList() && jp::isBulletList(b.textList()->format().style()) && !b.text().trimmed().isEmpty()) ++bulleted;
        QCOMPARE(bulleted, 6);   // 3 with a list of their own, 3 from their style
    }

    void metafileWmfRenders()
    {
        QFile f(QStringLiteral(JP_TEST_DATA "/pub/poi-SampleBrochure.pub"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto doc = importPublisher(f.readAll(), nullptr);
        QVERIFY(doc);
        int wmf = 0;
        for (auto it = doc->images.cbegin(); it != doc->images.cend(); ++it) {
            if (it->format != "wmf") continue;
            ++wmf;
            Metafile m;
            QVERIFY(m.load(it->bytes));
            const QImage img = m.toImage(200);
            int painted = 0;
            for (int y = 0; y < img.height(); y += 4)
                for (int x = 0; x < img.width(); x += 4) painted += qAlpha(img.pixel(x, y)) > 0;
            QVERIFY(painted > 10);
        }
        QVERIFY(wmf > 0);
        // Recolored, a metafile changes every color it draws (border art's
        // tinted pieces): nothing comes out but the new color.
        const auto first = std::find_if(doc->images.cbegin(), doc->images.cend(), [](const ImageData &d) { return d.format == "wmf"; });
        Metafile m;
        QVERIFY(m.load(first->bytes));
        QImage img(200, 200, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        {
            QPainter p(&img);
            m.play(&p, QRectF(0, 0, 200, 200), [](const QColor &c) { return QColor(0, 128, 128, c.alpha()); });
        }
        int teal = 0, other = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QRgb c = img.pixel(x, y);
                if (qAlpha(c) < 250) continue;
                (qRed(c) < 10 && std::abs(qGreen(c) - 128) < 10 && std::abs(qBlue(c) - 128) < 10 ? teal : other)++;
            }
        QVERIFY2(teal > 100 && other == 0, qPrintable(QStringLiteral("%1 %2").arg(teal).arg(other)));
        // And a recolored metafile prints as vectors, not as a picture.
        auto one = Document::blank(QSizeF(300, 300));
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = one->addImage(first->bytes, QStringLiteral("wmf"));
        pic->rect = pic->imgRect = QRectF(20, 20, 200, 200);
        pic->imgRect.moveTo(0, 0);
        pic->recolor = PictureItem::ColorTint;
        pic->recolorColor = ColorRef::rgb(QColor(0, 128, 128));
        one->pages[0]->items.push_back(pic);
        MainWindow w;
        w.editor()->setDocument(std::move(one));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("tinted.pdf"));
        QVERIFY(w.exportPdfTo(path, MainWindow::PdfSettings()));
        QtPdf parsed;
        QVERIFY(parsed.load(path));
        for (const auto &o : parsed.objects) QVERIFY(!QtPdf::dictOf(o.body).contains("/Subtype /Image"));
    }
    void colorRefRoundTrip()
    {
        const ColorRef a = ColorRef::scheme(Accent2, 40);
        QCOMPARE(ColorRef::fromString(a.toString()), a);
        QCOMPARE(ColorRef::fromString("#1F5FAD").resolve(builtinColorSchemes()[0]), QColor("#1F5FAD"));
        QVERIFY(builtinColorSchemes().size() >= 90);
    }
    void documentRoundTrip()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<TextItem>();
        t->storyId = doc->createStory("Hello world\nSecond paragraph");
        doc->pages[0]->items.push_back(t);
        auto s = std::make_shared<ShapeItem>();
        s->shape = "star5";
        doc->pages[0]->items.push_back(s);
        QTextCursor c(doc->storyDoc(t->storyId));
        c.movePosition(QTextCursor::End);
        QTextCharFormat f;
        f.setProperty(tp::Field, QStringLiteral("page"));
        c.insertText(QString(QChar::ObjectReplacementCharacter), f);

        const QByteArray bytes = publicationBytes(*doc, QImage());
        QString err;
        auto back = publicationFromBytes(bytes, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->pages[0]->items.size(), size_t(2));
        auto *t2 = static_cast<TextItem *>(back->pages[0]->items[0].get());
        QCOMPARE(back->storyDoc(t2->storyId)->toPlainText(), doc->storyDoc(t->storyId)->toPlainText());
        QCOMPARE(QJsonDocument(back->toJson()).toJson(), QJsonDocument(doc->toJson()).toJson());
    }
    void layoutFlowsAcrossFrames()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        QString text;
        for (int i = 0; i < 40; ++i) text += QStringLiteral("Paragraph %1 has enough words to wrap across a narrow column of text.\n").arg(i);
        const QString sid = doc->createStory(text);
        FrameSpec a; a.size = QSizeF(144, 144); a.insets = QMarginsF(3, 3, 3, 3);
        FrameSpec b = a;
        LayoutEnv env; env.colors = doc->colors; env.fonts = doc->fonts;
        StoryLayout one;
        one.build(doc->storyDoc(sid), {a}, env);
        QVERIFY(one.overflow());
        StoryLayout two;
        two.build(doc->storyDoc(sid), {a, b}, env);
        QVERIFY(two.firstPosition(1) > two.firstPosition(0));
        QVERIFY(two.lastPosition(0) <= two.firstPosition(1));
        // Caret round trip
        int fr = -1; QRectF r;
        QVERIFY(two.caretRect(5, &fr, &r));
        QCOMPARE(fr, 0);
        QCOMPARE(two.hitTest(0, QPointF(r.x() + 0.1, r.center().y())), 5);
    }
    void fieldsDisplay()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        const QString sid = doc->createStory("Page ");
        QTextCursor c(doc->storyDoc(sid));
        c.movePosition(QTextCursor::End);
        QTextCharFormat f;
        f.setProperty(tp::Field, QStringLiteral("page"));
        c.insertText(QString(QChar::ObjectReplacementCharacter), f);
        FrameSpec a; a.size = QSizeF(300, 100); a.ctx.pageNumber = 12;
        LayoutEnv env; env.colors = doc->colors; env.fonts = doc->fonts;
        StoryLayout l;
        l.build(doc->storyDoc(sid), {a}, env);
        QVERIFY(!l.overflow());
        int fr; QRectF r5, r6;
        QVERIFY(l.caretRect(5, &fr, &r5));
        QVERIFY(l.caretRect(6, &fr, &r6));
        QVERIFY(r6.x() - r5.x() > 8);   // "12" is wider than one character
    }
    void renderSmoke()
    {
        auto doc = Document::blank(QSizeF(612, 792));
        doc->colors = *findColorScheme("Seventy-Nine");
        auto &items = doc->pages[0]->items;
        auto band = std::make_shared<ShapeItem>();
        band->rect = QRectF(0, 0, 612, 230);
        band->fill = Fill::gradient(ColorRef::scheme(Accent1), ColorRef::scheme(Accent2), 0);
        band->stroke = Stroke::none();
        items.push_back(band);
        auto wa = std::make_shared<TextArtItem>();
        wa->rect = QRectF(60, 40, 492, 140);
        wa->text = "Summer Concert";
        applyTextArtStyle(*wa, textArtStyles()[11]);
        wa->fill = Fill::solid(ColorRef::rgb(Qt::white));
        wa->fx.shadow.on = true;
        items.push_back(wa);
        int x = 40;
        for (const char *sh : {"star5", "heart", "rightArrow", "cloud", "wedgeRoundRectCallout", "fcDocument", "irregularSeal1"}) {
            auto s = std::make_shared<ShapeItem>();
            s->shape = sh;
            s->rect = QRectF(x, 260, 70, 70);
            s->fill = Fill::solid(ColorRef::scheme(Accent3));
            s->fx.shadow.on = std::string(sh) == "heart";
            items.push_back(s);
            x += 78;
        }
        auto t = std::make_shared<TextItem>();
        t->rect = QRectF(40, 360, 250, 380);
        t->columns = 2;
        t->stroke = Stroke::line(ColorRef::scheme(Accent4), 1);
        QString text;
        for (int i = 0; i < 6; ++i) text += "Bring a blanket and join your neighbors for an evening of music under the stars. Food trucks open at six.\n";
        t->storyId = doc->createStory(text);
        QTextCursor c(doc->storyDoc(t->storyId));
        QTextBlockFormat bf;
        bf.setProperty(tp::DropCapLines, 3);
        c.mergeBlockFormat(bf);
        items.push_back(t);
        auto t2 = std::make_shared<TextItem>();
        t2->rect = QRectF(310, 360, 260, 200);
        t2->storyId = doc->createStory("");
        t->nextId = t2->id;
        items.push_back(t2);
        auto pic = std::make_shared<PictureItem>();
        pic->rect = QRectF(330, 580, 220, 160);
        pic->maskShape = "ellipse";
        items.push_back(pic);
        LayoutCache cache;
        PaintContext ctx;
        ctx.doc = doc.get();
        ctx.cache = &cache;
        const QImage img = Renderer::renderToImage(ctx, 0, 1.5);
        QVERIFY(!img.isNull());
        const QString out = qEnvironmentVariable("JP_RENDER_OUT");
        if (!out.isEmpty()) img.save(out);
    }
};

QTEST_MAIN(Tests)
#include "tests.moc"
