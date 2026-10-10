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
#include "PolygonUtils.h"   // libmspub's shape table
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
#include <QStatusBar>
#include <QTabBar>
#include <QMenu>
#include <QTranslator>
#include <QTabWidget>
#include "app/dialogs.h"
#include "app/editor.h"
#include "app/mainwindow.h"
#include "app/recovery.h"
#include "app/i18n.h"
#include "app/keytips.h"
#include "app/help.h"
#include "app/taskpane.h"
#include "app/onlinepictures.h"
#include "app/focusring.h"
#include "app/keyboardnav.h"
#include "app/pagespane.h"
#include "app/theme.h"
#include "app/ribbon.h"
#include "app/ribbonbuilder.h"
#include "app/telemetry.h"
#include "app/updater.h"
#include "app/toc.h"
#include "app/notes.h"
#include "app/iconpicker.h"
#include "app/widgets.h"
#include "app/settings.h"
#include "canvas/canvas.h"
#include <cstdio>
#include <tuple>
#include <QTemporaryDir>
#include <QFileDialog>
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
#include <QSpinBox>
#include <QInputDialog>
#include <QStandardPaths>
#include <QRadioButton>
#include <QGroupBox>
#include <QtEndian>
#include <QLockFile>
#include <QStyleHints>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QScopeGuard>
#include <QToolButton>
#include <QSlider>
#include <QAccessible>
#include <QGridLayout>
#include <QListWidget>
#include <clocale>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

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

    // Underlines keep their kind: Double Underline (Ctrl+Shift+D) made a
    // single one, and the Font dialog's Words only and Double did too; .pub
    // files kept only single ones, both opening and saving.
    void underlineKindsKept()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 400, 200), QStringLiteral("one two\nthree four\nfive six\nseven eight\nnine ten")));
        ed->addItem(box);
        auto runFormat = [&](int block) {
            QTextCursor c(ed->doc()->storyDoc(box->storyId)->findBlockByNumber(block));
            c.movePosition(QTextCursor::NextCharacter);
            return c.charFormat();
        };
        auto selectBlock = [&](int block) {
            ed->beginTextEdit(box->id);
            QTextCursor c(ed->editDoc()->findBlockByNumber(block));
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            ed->setCursor(c);
        };
        selectBlock(0);
        w.act(QStringLiteral("fmt.underlineDouble"))->trigger();
        ed->endTextEdit();
        QCOMPARE(runFormat(0).underlineStyle(), QTextCharFormat::SingleUnderline);
        QCOMPARE(runFormat(0).intProperty(jp::tp::UnderlineKind), 1);
        const struct { int block; QTextCharFormat::UnderlineStyle style; int kind; } want[] = {
            {1, QTextCharFormat::SingleUnderline, 2}, {2, QTextCharFormat::DotLine, 4}, {3, QTextCharFormat::DashDotLine, 0}, {4, QTextCharFormat::WaveUnderline, 1}};
        for (const auto &u : want) {
            selectBlock(u.block);
            ed->toggleUnderline(u.style, u.kind);
            ed->endTextEdit();
        }
        QCOMPARE(runFormat(1).intProperty(jp::tp::UnderlineKind), 2);
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            QString e;
            jp::savePublication(*ed->doc(), qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/underlines.jpub"), QImage(), &e);
        }
        // Through a .pub file and back.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("underlines.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*ed->doc(), path, &err), qPrintable(err));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QTextDocument *story = nullptr;
        for (auto it = back->stories.cbegin(); it != back->stories.cend(); ++it)
            if ((*it)->doc->toPlainText().startsWith(QLatin1String("one two"))) story = (*it)->doc.get();
        QVERIFY(story);
        auto backFormat = [&](int block) {
            QTextCursor c(story->findBlockByNumber(block));
            c.movePosition(QTextCursor::NextCharacter);
            return c.charFormat();
        };
        QCOMPARE(backFormat(0).intProperty(jp::tp::UnderlineKind), 1);
        QCOMPARE(backFormat(1).intProperty(jp::tp::UnderlineKind), 2);
        QCOMPARE(backFormat(2).underlineStyle(), QTextCharFormat::DotLine);
        QCOMPARE(backFormat(2).intProperty(jp::tp::UnderlineKind), 4);
        QCOMPARE(backFormat(3).underlineStyle(), QTextCharFormat::DashDotLine);
        QCOMPARE(backFormat(4).underlineStyle(), QTextCharFormat::WaveUnderline);
        QCOMPARE(backFormat(4).intProperty(jp::tp::UnderlineKind), 1);
    }

    // Bullets and Numbering's "Indent list by" sets the list's indent (it
    // was ignored), and a style takes what it doesn't set from the style
    // it's based on (Based on was saved but never used).
    void listIndentAndBasedOn()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 400, 200), QStringLiteral("first\nsecond")));
        ed->addItem(box);
        ed->beginTextEdit(box->id, 0);
        double shown = -1;
        QTimer::singleShot(0, [&shown] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!d) return;
            if (auto *indent = d->findChild<QDoubleSpinBox *>(QStringLiteral("indent"))) {
                shown = indent->value();
                indent->setValue(36);
            }
            d->accept();   // whatever was found, so a failure can't leave it open
        });
        w.act(QStringLiteral("para.bulletsDialog"))->trigger();
        QCOMPARE(shown, 18.0);
        QTextBlockFormat bf = ed->editDoc()->firstBlock().blockFormat();
        QCOMPARE(bf.leftMargin(), 36.0);
        QCOMPARE(bf.textIndent(), -36.0);
        QVERIFY(ed->editDoc()->firstBlock().textList());
        ed->endTextEdit();
        // A base style with line spacing the dialog doesn't show; one based on it with a size.
        jp::TextStyle base;
        base.name = QStringLiteral("Base");
        base.blk.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
        base.chr.setFontItalic(true);
        jp::TextStyle child;
        child.name = QStringLiteral("Child");
        child.basedOn = QStringLiteral("Base");
        child.chr.setFontPointSize(20);
        ed->doc()->styles << base << child;
        const jp::TextStyle r = ed->doc()->resolvedStyle(QStringLiteral("Child"));
        QCOMPARE(r.chr.fontPointSize(), 20.0);
        QVERIFY(r.chr.fontItalic());
        QCOMPARE(r.blk.lineHeight(), 150.0);
        ed->beginTextEdit(box->id, 0);
        ed->applyStyle(QStringLiteral("Child"));
        const QTextBlock b = ed->editDoc()->firstBlock();
        QCOMPARE(b.blockFormat().lineHeight(), 150.0);
        QTextCursor c(b);
        c.movePosition(QTextCursor::NextCharacter);
        QVERIFY(c.charFormat().fontItalic());
        QCOMPARE(c.charFormat().fontPointSize(), 20.0);
        ed->endTextEdit();
        // A loop of bases ends.
        ed->doc()->styles[ed->doc()->styles.size() - 2].basedOn = QStringLiteral("Child");
        QCOMPARE(ed->doc()->resolvedStyle(QStringLiteral("Child")).chr.fontPointSize(), 20.0);
    }

    // Text Art's Word Justify widens the spaces and Letter Justify the gaps
    // between letters (Word Justify was left-aligned, and Letter Justify
    // stretched the letters, which is Stretch Justify).
    void textArtJustify()
    {
        jp::TextArtItem w;
        w.text = QStringLiteral("ab cd\nabcdefghijklmnopqrstuv");
        w.font = QStringLiteral("DejaVu Sans");
        // The top line's ink: how much there is, and how far it reaches.
        auto topLine = [&](int align) {
            w.align = align;
            const QPainterPath path = jp::textArtPath(w, QSizeF(600, 200));
            QImage img(600, 100, QImage::Format_Grayscale8);
            img.fill(255);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.fillPath(path, Qt::black);
            p.end();
            int ink = 0, right = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixelColor(x, y).value() < 128) { ++ink; right = std::max(right, x); }
            return std::make_pair(ink, right);
        };
        const auto left = topLine(0), word = topLine(3), letter = topLine(4), stretch = topLine(5);
        QVERIFY2(left.second < 300, qPrintable(QString::number(left.second)));
        for (const auto &j : {word, letter, stretch}) QVERIFY2(j.second > 540, qPrintable(QString::number(j.second)));
        // Moved, not widened: as much ink as left-aligned; stretched letters have far more.
        QVERIFY2(std::abs(word.first - left.first) < left.first / 20, qPrintable(QStringLiteral("%1 %2").arg(word.first).arg(left.first)));
        QVERIFY2(std::abs(letter.first - left.first) < left.first / 20, qPrintable(QStringLiteral("%1 %2").arg(letter.first).arg(left.first)));
        QVERIFY2(stretch.first > left.first * 1.5, qPrintable(QStringLiteral("%1 %2").arg(stretch.first).arg(left.first)));
    }

    // With the whole table selected, Insert Below adds at the bottom (it
    // added above the first row) and Delete Rows asks for a row (it deleted
    // the first); Cell Margins changes table cells (it did nothing).
    void tableCommandsWithTheTableSelected()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto t = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 288, 72), 2, 2));
        ed->addItem(t);
        ed->doc()->storyDoc(t->cell(0, 0).storyId)->setPlainText(QStringLiteral("top"));
        ed->select(t->id);
        w.act(QStringLiteral("tbl.insBelow"))->trigger();
        QCOMPARE(t->rows, 3);
        QCOMPARE(ed->doc()->storyDoc(t->cell(0, 0).storyId)->toPlainText(), QStringLiteral("top"));
        w.act(QStringLiteral("tbl.insRight"))->trigger();
        QCOMPARE(t->cols, 3);
        QCOMPARE(ed->doc()->storyDoc(t->cell(0, 0).storyId)->toPlainText(), QStringLiteral("top"));
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        w.act(QStringLiteral("tbl.delCol"))->trigger();
        QCOMPARE(t->rows, 3);
        QCOMPARE(t->cols, 3);
        w.act(QStringLiteral("tbmargin.Wide"))->trigger();
        for (const auto &c : t->cells) QCOMPARE(c.margins, QMarginsF(14.4, 14.4, 14.4, 14.4));
        ed->beginTextEdit(t->id, 0, 1, 1);
        w.act(QStringLiteral("tbmargin.None"))->trigger();
        QCOMPARE(t->cell(1, 1).margins, QMarginsF());
        QCOMPARE(t->cell(0, 0).margins, QMarginsF(14.4, 14.4, 14.4, 14.4));
        // In a cell, Delete Rows deletes that row.
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        QCOMPARE(t->rows, 2);
    }

    // Several table cells at once, as in Publisher: dragging across cells, or
    // Shift+click, selects a block, shaded the way selected text is; a block
    // that touches part of a merged cell takes the whole cell.
    void tableCellsSelectedAsABlock()
    {
        // The rectangle between two cells grows over every merged cell it touches.
        jp::TableItem model;
        model.rows = 4;
        model.cols = 4;
        model.colW = QVector<double>(4, 50);
        model.rowH = QVector<double>(4, 20);
        model.cells.resize(16);
        model.cell(1, 1).rowSpan = 2;
        model.cell(1, 1).colSpan = 2;
        for (auto [r, c] : {std::pair{1, 2}, {2, 1}, {2, 2}}) model.cell(r, c).covered = true;
        QCOMPARE(model.cellsBetween(0, 0, 1, 1), (jp::CellRange{0, 0, 2, 2}));
        QCOMPARE(model.cellsBetween(2, 2, 3, 3), (jp::CellRange{1, 1, 3, 3}));   // given in any order
        QCOMPARE(model.cellsBetween(3, 3, 2, 0), (jp::CellRange{1, 0, 3, 3}));
        QCOMPARE(model.cellsBetween(0, 0, 0, 3), (jp::CellRange{0, 0, 0, 3}));
        int row = -1, col = -1;
        model.cellAt(QPointF(120, 50), &row, &col);   // under the merged cell
        QCOMPARE(std::make_pair(row, col), std::make_pair(1, 1));
        model.cellAt(QPointF(-30, 500), &row, &col);   // outside: the nearest cell
        QCOMPARE(std::make_pair(row, col), std::make_pair(3, 0));

        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 360, 160), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        jp::Canvas *cv = w.canvas();
        QTest::qWait(50);
        auto at = [&](int r, int c) { return cv->pageToView(table()->transform().map(table()->cellRect(r, c).center())).toPoint(); };
        auto block = [&] {
            const jp::CellRange b = ed->cellBlock().range;
            return QVector<int>{b.r0, b.c0, b.r1, b.c1};
        };
        auto drag = [&](const QPoint &from, const QPoint &to) {
            QTest::mousePress(cv->viewport(), Qt::LeftButton, Qt::NoModifier, from);
            for (int k = 1; k <= 4; ++k) {
                const QPoint p = from + (to - from) * k / 4;
                QMouseEvent mv(QEvent::MouseMove, QPointF(p), cv->viewport()->mapToGlobal(QPointF(p)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(cv->viewport(), &mv);
            }
            QTest::mouseRelease(cv->viewport(), Qt::LeftButton, Qt::NoModifier, to);
        };

        // A click, or a drag inside one cell, is text editing as before.
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::NoModifier, at(3, 3));
        QVERIFY(ed->isEditingText());
        QVERIFY(!ed->hasCellBlock());
        drag(at(3, 3) - QPoint(8, 0), at(3, 3) + QPoint(8, 0));
        QVERIFY(ed->isEditingText());
        QVERIFY(!ed->hasCellBlock());
        const QImage plain = cv->viewport()->grab().toImage();

        // Dragging across cells selects the block: the table stays the selected object.
        drag(at(0, 0), at(1, 2));
        QVERIFY(!ed->isEditingText());
        QCOMPARE(ed->selection(), QStringList{id});
        QCOMPARE(block(), (QVector<int>{0, 0, 1, 2}));
        // Shaded as selected text is, in the cells of the block only.
        const QImage shaded = cv->viewport()->grab().toImage();
        for (auto [r, c] : {std::pair{0, 0}, {0, 2}, {1, 1}})
            QVERIFY2(shaded.pixelColor(at(r, c)) != plain.pixelColor(at(r, c)), qPrintable(QStringLiteral("%1,%2").arg(r).arg(c)));
        for (auto [r, c] : {std::pair{2, 0}, {0, 3}, {2, 2}, {1, 3}})
            QVERIFY2(shaded.pixelColor(at(r, c)) == plain.pixelColor(at(r, c)), qPrintable(QStringLiteral("%1,%2").arg(r).arg(c)));
        // A drag up and to the left, shaded where it is and nowhere else.
        drag(at(2, 2), at(1, 1));
        QCOMPARE(block(), (QVector<int>{1, 1, 2, 2}));
        const QImage lower = cv->viewport()->grab().toImage();
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) lower.save(qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/table-cells.png"));
        for (auto [r, c] : {std::pair{1, 1}, {2, 2}, {1, 2}})
            QVERIFY2(lower.pixelColor(at(r, c)) != plain.pixelColor(at(r, c)), qPrintable(QStringLiteral("%1,%2").arg(r).arg(c)));
        for (auto [r, c] : {std::pair{0, 0}, {0, 1}, {3, 1}, {2, 3}})
            QVERIFY2(lower.pixelColor(at(r, c)) == plain.pixelColor(at(r, c)), qPrintable(QStringLiteral("%1,%2").arg(r).arg(c)));
        drag(at(2, 2), at(2, 2) + QPoint(3, 0));
        QVERIFY(ed->isEditingText());   // not far enough to leave the cell

        // Shift+click extends from the cell the cursor is in, then from where the block began.
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::NoModifier, at(1, 1));
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(2, 3));
        QVERIFY(!ed->isEditingText());
        QCOMPARE(block(), (QVector<int>{1, 1, 2, 3}));
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(0, 0));
        QCOMPARE(block(), (QVector<int>{0, 0, 1, 1}));
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(3, 1));
        QCOMPARE(block(), (QVector<int>{1, 1, 3, 1}));
        // A click in a cell ends the block and puts the cursor there.
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::NoModifier, at(2, 2));
        QVERIFY(ed->isEditingText());
        QVERIFY(!ed->hasCellBlock());
        QCOMPARE(std::make_pair(ed->textTarget().row, ed->textTarget().col), std::make_pair(2, 2));

        // A block that touches part of a merged cell grows to hold all of it.
        ed->change(QStringLiteral("Merge"), [&] {
            table()->cell(1, 1).rowSpan = 2;
            table()->cell(1, 1).colSpan = 2;
            for (auto [r, c] : {std::pair{1, 2}, {2, 1}, {2, 2}}) table()->cell(r, c).covered = true;
        });
        drag(at(0, 0), at(1, 1));
        QCOMPARE(block(), (QVector<int>{0, 0, 2, 2}));
        ed->selectCells(id, 2, 0, 2, 1);   // the second is under the merged cell
        QCOMPARE(block(), (QVector<int>{1, 0, 2, 2}));
        QTest::mouseClick(cv->viewport(), Qt::LeftButton, Qt::ShiftModifier, at(3, 3));
        QCOMPARE(block(), (QVector<int>{1, 0, 3, 3}));
        // Esc puts the block away and leaves the table selected; Enter types in the cell the block began with.
        QTest::keyClick(cv, Qt::Key_Escape);
        QVERIFY(!ed->hasCellBlock());
        QCOMPARE(ed->selection(), QStringList{id});
        ed->selectCells(id, 3, 2, 1, 1);
        QTest::keyClick(cv, Qt::Key_Return);
        QVERIFY(ed->isEditingText());
        QCOMPARE(std::make_pair(ed->textTarget().row, ed->textTarget().col), std::make_pair(3, 2));
        // Choosing another object, or selecting the table again, puts the block away.
        ed->selectCells(id, 0, 0, 1, 0);
        ed->select(id);
        QVERIFY(!ed->hasCellBlock());
        ed->selectCells(id, 0, 0, 1, 0);
        ed->clearSelection();
        QVERIFY(!ed->hasCellBlock());
    }

    // Table Layout > Select has Select Row and Select Column beside Select
    // Cell and Select Table, reached by the KeyTips of the Select button: a
    // row or column of the cell the cursor is in, or every row or column a
    // block covers (merged cells whole).
    void tableSelectRowAndColumn()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 360, 160), 4, 3));
        ed->addItem(made);
        const QString id = made->id;
        auto block = [&] {
            const jp::CellRange b = ed->cellBlock().range;
            return QVector<int>{b.r0, b.c0, b.r1, b.c1};
        };
        const int steps = ed->undoStack()->index();
        ed->beginTextEdit(id, 0, 1, 2);
        w.act(QStringLiteral("tbl.selectRow"))->trigger();
        QVERIFY(!ed->isEditingText());
        QCOMPARE(ed->selection(), QStringList{id});
        QCOMPARE(block(), (QVector<int>{1, 0, 1, 2}));
        ed->beginTextEdit(id, 0, 1, 2);
        w.act(QStringLiteral("tbl.selectCol"))->trigger();
        QCOMPARE(block(), (QVector<int>{0, 2, 3, 2}));
        // From a block: every row, or every column, that it covers.
        ed->selectCells(id, 1, 0, 2, 1);
        w.act(QStringLiteral("tbl.selectRow"))->trigger();
        QCOMPARE(block(), (QVector<int>{1, 0, 2, 2}));
        QCOMPARE(ed->cellBlock().anchorRow, 1);
        ed->selectCells(id, 1, 0, 2, 1);
        w.act(QStringLiteral("tbl.selectCol"))->trigger();
        QCOMPARE(block(), (QVector<int>{0, 0, 3, 1}));
        // A merged cell in the row comes whole.
        auto *t = static_cast<jp::TableItem *>(ed->doc()->item(id));
        t->cell(2, 1).rowSpan = 2;
        t->cell(3, 1).covered = true;
        ed->beginTextEdit(id, 0, 2, 0);
        w.act(QStringLiteral("tbl.selectRow"))->trigger();
        QCOMPARE(block(), (QVector<int>{2, 0, 3, 2}));
        // With the whole table selected there is no cell to take a row from.
        ed->select(id);
        w.act(QStringLiteral("tbl.selectRow"))->trigger();
        QVERIFY(!ed->hasCellBlock());
        w.act(QStringLiteral("tbl.selectCol"))->trigger();
        QVERIFY(!ed->hasCellBlock());
        // Selecting is not an edit: nothing to undo.
        QCOMPARE(ed->undoStack()->index(), steps);
        // Select Cell from a block goes to its first cell, all its text selected.
        ed->doc()->storyDoc(t->cell(1, 0).storyId)->setPlainText(QStringLiteral("hello"));
        ed->selectCells(id, 1, 0, 2, 1);
        w.act(QStringLiteral("tbl.selectCell"))->trigger();
        QVERIFY(ed->isEditingText());
        QCOMPARE(std::make_pair(ed->textTarget().row, ed->textTarget().col), std::make_pair(1, 0));
        QCOMPARE(ed->cursor().selectedText(), QStringLiteral("hello"));

        // The Select button's menu lists all four, and its KeyTips open it.
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r);
        ed->select(id);
        const QString described = r->describe(true);
        const QRegularExpression line(QStringLiteral("ribbon\\.tableSelect.*menu=\\[([^\\]]*)\\].*keytip=(\\w+)"));
        const QRegularExpressionMatch m = line.match(described);
        QVERIFY2(m.hasMatch(), "the Select button");
        const QStringList entries = m.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        QCOMPARE(entries.size(), 4);
        for (const char *cmd : {"tbl.selectCell", "tbl.selectRow", "tbl.selectCol", "tbl.selectTable"})
            QVERIFY2(m.captured(1).contains(QLatin1String(cmd)), cmd);
        QCOMPARE(m.captured(2), QStringLiteral("SL"));
        QStringList shown;
        QTimer::singleShot(300, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                for (QAction *a : menu->actions()) shown << a->objectName();
                menu->close();
            }
        });
        QTest::keyPress(&w, Qt::Key_Alt);
        QTest::keyRelease(&w, Qt::Key_Alt);
        for (Qt::Key k : {Qt::Key_J, Qt::Key_L, Qt::Key_S, Qt::Key_L}) QTest::keyClick(&w, k);
        QCOMPARE(shown, (QStringList{"tbl.selectCell", "tbl.selectRow", "tbl.selectCol", "tbl.selectTable"}));
    }

    // Every cell-formatting command applies to every cell in the block, as
    // one undo step: fill, borders, diagonals, alignment, cell margins, and
    // the text's font, size, bold, italic, underline, color, and alignment.
    void tableBlockFormatsEveryCell()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 360, 160), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) ed->doc()->storyDoc(table()->cell(r, c).storyId)->setPlainText(QStringLiteral("cell %1,%2").arg(r).arg(c));
        for (auto &cl : table()->cells) {   // plain cells, so every command has something to change
            cl.fill = jp::Fill();
            cl.border = jp::CellBorder();
        }
        auto inBlock = [](int r, int c) { return r >= 1 && r <= 2 && c >= 1 && c <= 2; };
        auto story = [&](int r, int c) { return ed->doc()->storyDoc(table()->cell(r, c).storyId); };
        auto charFormat = [&](int r, int c) {
            QTextCursor cur(story(r, c));
            cur.setPosition(3);
            return cur.charFormat();
        };
        auto cellJson = [&](int r, int c) {
            const jp::TableCell &cl = table()->cell(r, c);
            return QJsonObject{{"fill", cl.fill.toJson()}, {"top", cl.border.top.toJson()}, {"bottom", cl.border.bottom.toJson()}, {"left", cl.border.left.toJson()},
                               {"right", cl.border.right.toJson()}, {"diag", cl.diagonal}, {"valign", int(cl.valign)},
                               {"margins", QJsonArray{cl.margins.left(), cl.margins.top(), cl.margins.right(), cl.margins.bottom()}}};
        };
        auto cellsJson = [&] {
            QVector<QJsonObject> all;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) all << cellJson(r, c);
            return all;
        };
        auto textFormats = [&] {
            QVector<QString> all;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) {
                    const QTextCharFormat f = charFormat(r, c);
                    all << QStringLiteral("%1 %2 %3 %4 %5 %6 %7").arg(f.fontWeight()).arg(f.fontItalic()).arg(int(f.underlineStyle())).arg(f.fontFamilies().toStringList().join(QLatin1Char('+')))
                               .arg(f.fontPointSize()).arg(f.stringProperty(jp::tp::ColorRefP), QString::number(int(story(r, c)->firstBlock().blockFormat().alignment())));
                }
            return all;
        };
        // Runs one command on the block: it changes exactly the block's cells, in
        // one undo step, and the block stays selected, also after undoing it.
        // `prepare` first puts every cell in the state the command should change.
        auto command = [&](const char *what, const std::function<void()> &run, const std::function<bool(int, int)> &changed, const std::function<void(jp::TableCell &)> &prepare = nullptr) {
            if (prepare)
                for (auto &cl : table()->cells) prepare(cl);
            ed->selectCells(id, 1, 1, 2, 2);
            w.refreshUi();   // the ribbon catches up: text commands are on for selected cells
            const int steps = ed->undoStack()->index();
            const auto cellsBefore = cellsJson();
            const auto textBefore = textFormats();
            run();
            QVERIFY2(ed->undoStack()->index() == steps + 1, what);
            QVERIFY2(ed->hasCellBlock(), what);
            const auto cellsAfter = cellsJson();
            const auto textAfter = textFormats();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) {
                    const int k = r * 4 + c;
                    const bool did = cellsAfter[k] != cellsBefore[k] || textAfter[k] != textBefore[k];
                    QVERIFY2(did == inBlock(r, c), qPrintable(QStringLiteral("%1 at %2,%3").arg(QLatin1String(what)).arg(r).arg(c)));
                    if (inBlock(r, c)) QVERIFY2(changed(r, c), qPrintable(QStringLiteral("%1 at %2,%3").arg(QLatin1String(what)).arg(r).arg(c)));
                }
            ed->undo();
            QVERIFY2(cellsJson() == cellsBefore && textFormats() == textBefore, what);
            QVERIFY2(ed->hasCellBlock(), what);
        };
        auto cellFill = [&]() -> jp::ColorButton * {
            for (auto *b : w.findChildren<jp::ColorButton *>())
                if (b->toolTip() == QStringLiteral("Cell Fill")) return b;
            return nullptr;
        };
        QVERIFY(cellFill());

        const jp::ColorRef red = jp::ColorRef::fromString(QStringLiteral("#ff0000"));
        command("fill", [&] { Q_EMIT cellFill()->colorPicked(red); }, [&](int r, int c) { return table()->cell(r, c).fill == jp::Fill::solid(red); });
        command("fill removed", [&] { Q_EMIT cellFill()->colorPicked(jp::ColorRef()); }, [&](int r, int c) { return table()->cell(r, c).fill.isNone(); },
                [&](jp::TableCell &cl) { cl.fill = jp::Fill::solid(red); });
        command("diagonal down", [&] { w.act(QStringLiteral("tbl.diagDown"))->trigger(); }, [&](int r, int c) { return table()->cell(r, c).diagonal == 1; });
        command("diagonal up", [&] { w.act(QStringLiteral("tbl.diagUp"))->trigger(); }, [&](int r, int c) { return table()->cell(r, c).diagonal == 2; });
        command("align bottom", [&] { w.act(QStringLiteral("valign.2"))->trigger(); }, [&](int r, int c) { return table()->cell(r, c).valign == jp::VAlign::Bottom; });
        command("align middle", [&] { w.act(QStringLiteral("valign.1"))->trigger(); }, [&](int r, int c) { return table()->cell(r, c).valign == jp::VAlign::Middle; });
        command("margins", [&] { w.act(QStringLiteral("tbmargin.Wide"))->trigger(); }, [&](int r, int c) { return table()->cell(r, c).margins == QMarginsF(14.4, 14.4, 14.4, 14.4); });
        command("all borders", [&] { w.act(QStringLiteral("border.all"))->trigger(); }, [&](int r, int c) {
            const jp::CellBorder &b = table()->cell(r, c).border;
            return !b.top.isNone() && !b.bottom.isNone() && !b.left.isNone() && !b.right.isNone();
        });
        command("no borders", [&] { w.act(QStringLiteral("border.none"))->trigger(); }, [&](int r, int c) {
            const jp::CellBorder &b = table()->cell(r, c).border;
            return b.top.isNone() && b.bottom.isNone() && b.left.isNone() && b.right.isNone();
        }, [](jp::TableCell &cl) { cl.border.top = cl.border.bottom = cl.border.left = cl.border.right = jp::Stroke::line(jp::ColorRef::fromString(QStringLiteral("#000000"))); });
        for (auto &cl : table()->cells) cl.border = jp::CellBorder();
        // Outside Borders draws round the block, not round each cell in it.
        ed->selectCells(id, 1, 1, 2, 2);
        const int plain = ed->undoStack()->index();
        w.act(QStringLiteral("border.none"))->trigger();
        w.act(QStringLiteral("border.outside"))->trigger();
        QVERIFY(!table()->cell(1, 1).border.top.isNone() && !table()->cell(1, 1).border.left.isNone());
        QVERIFY(table()->cell(1, 1).border.right.isNone() && table()->cell(1, 1).border.bottom.isNone());
        QVERIFY(!table()->cell(2, 2).border.bottom.isNone() && !table()->cell(2, 2).border.right.isNone());
        QVERIFY(table()->cell(2, 2).border.left.isNone() && table()->cell(2, 2).border.top.isNone());
        ed->undoStack()->setIndex(plain);

        // The text of every cell in the block, whole.
        command("bold", [&] { w.act(QStringLiteral("fmt.bold"))->trigger(); }, [&](int r, int c) { return charFormat(r, c).fontWeight() >= QFont::DemiBold; });
        command("italic", [&] { w.act(QStringLiteral("fmt.italic"))->trigger(); }, [&](int r, int c) { return charFormat(r, c).fontItalic(); });
        command("underline", [&] { w.act(QStringLiteral("fmt.underline"))->trigger(); }, [&](int r, int c) { return charFormat(r, c).underlineStyle() != QTextCharFormat::NoUnderline; });
        command("font", [&] { ed->setFontFamily(QStringLiteral("DejaVu Serif")); }, [&](int r, int c) { return charFormat(r, c).fontFamilies().toStringList() == QStringList{"DejaVu Serif"}; });
        command("size", [&] { ed->setFontSize(23); }, [&](int r, int c) { return charFormat(r, c).fontPointSize() == 23.0; });
        command("color", [&] { ed->setTextColor(red); }, [&](int r, int c) { return charFormat(r, c).stringProperty(jp::tp::ColorRefP) == red.toString(); });
        command("center", [&] { w.act(QStringLiteral("para.center"))->trigger(); }, [&](int r, int c) { return story(r, c)->firstBlock().blockFormat().alignment().testFlag(Qt::AlignHCenter); });
        command("right", [&] { w.act(QStringLiteral("para.right"))->trigger(); }, [&](int r, int c) { return story(r, c)->firstBlock().blockFormat().alignment().testFlag(Qt::AlignRight); });
        // All the text of a cell, not just its start.
        ed->doc()->storyDoc(table()->cell(1, 1).storyId)->setPlainText(QStringLiteral("one\ntwo"));
        ed->selectCells(id, 1, 1, 2, 2);
        w.refreshUi();
        w.act(QStringLiteral("fmt.bold"))->trigger();
        QTextCursor last(story(1, 1));
        last.movePosition(QTextCursor::End);
        QVERIFY(last.charFormat().fontWeight() >= QFont::DemiBold);
        // The Home tab's boxes show the block's first cell.
        ed->selectCells(id, 1, 1, 1, 1);
        ed->setFontSize(31);
        ed->selectCells(id, 1, 1, 2, 2);
        QCOMPARE(ed->currentCharFormat().fontPointSize(), 31.0);
        ed->selectCells(id, 2, 2, 3, 3);
        QVERIFY(ed->currentCharFormat().fontPointSize() != 31.0);

        // With the cursor in one cell, a cell command still takes only that cell.
        for (auto &cl : table()->cells) cl.fill = jp::Fill();
        ed->beginTextEdit(id, 0, 3, 3);
        Q_EMIT cellFill()->colorPicked(red);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) QCOMPARE(table()->cell(r, c).fill == jp::Fill::solid(red), r == 3 && c == 3);
        // A merged cell in the block is bordered as the one cell it is: its far edges, not its inner ones.
        ed->endTextEdit();
        for (auto &cl : table()->cells) cl.border = jp::CellBorder();
        table()->cell(1, 1).rowSpan = 2;
        table()->cell(1, 1).colSpan = 2;
        for (auto [r, c] : {std::pair{1, 2}, {2, 1}, {2, 2}}) table()->cell(r, c).covered = true;
        ed->selectCells(id, 0, 0, 2, 2);
        w.act(QStringLiteral("border.outside"))->trigger();
        const jp::CellBorder &merged = table()->cell(1, 1).border;
        QVERIFY(merged.right.isNone() == false && merged.bottom.isNone() == false);
        QVERIFY(merged.top.isNone() && merged.left.isNone());
        QVERIFY(!table()->cell(0, 0).border.top.isNone() && table()->cell(0, 0).border.bottom.isNone());
    }

    // Merge Cells merges the block; Delete Rows and Delete Columns remove
    // every row and column it covers, merged cells kept consistent; the
    // Insert commands go beside the block.
    void tableBlockMergesAndDeletes()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 400, 200), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        auto text = [&](int r, int c) { return ed->doc()->storyDoc(table()->cell(r, c).storyId)->toPlainText(); };
        auto fill = [&] {
            for (int r = 0; r < table()->rows; ++r)
                for (int c = 0; c < table()->cols; ++c) ed->doc()->storyDoc(table()->cell(r, c).storyId)->setPlainText(QStringLiteral("%1%2").arg(r).arg(c));
        };
        fill();

        // Merge the block: one cell with the text of all, in reading order.
        ed->selectCells(id, 1, 1, 2, 2);
        int steps = ed->undoStack()->index();
        w.act(QStringLiteral("tbl.merge"))->trigger();
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QCOMPARE(table()->cell(1, 1).rowSpan, 2);
        QCOMPARE(table()->cell(1, 1).colSpan, 2);
        for (auto [r, c] : {std::pair{1, 2}, {2, 1}, {2, 2}}) QVERIFY(table()->cell(r, c).covered);
        QVERIFY(!table()->cell(1, 1).covered);
        QCOMPARE(text(1, 1), QStringLiteral("11\n12\n21\n22"));
        QVERIFY(text(2, 2).isEmpty());   // the text moved
        QVERIFY(table()->cell(0, 0).rowSpan == 1 && !table()->cell(0, 1).covered && !table()->cell(3, 3).covered);
        ed->undo();
        QCOMPARE(table()->cell(1, 1).rowSpan, 1);
        QVERIFY(!table()->cell(2, 2).covered);
        QCOMPARE(text(2, 2), QStringLiteral("22"));

        // A row of cells merges into one; a block over a merged cell takes all of it.
        ed->selectCells(id, 0, 0, 0, 3);
        w.act(QStringLiteral("tbl.merge"))->trigger();
        QCOMPARE(table()->cell(0, 0).colSpan, 4);
        QCOMPARE(text(0, 0), QStringLiteral("00\n01\n02\n03"));
        ed->selectCells(id, 0, 1, 1, 1);   // touches the merged row
        w.act(QStringLiteral("tbl.merge"))->trigger();
        QCOMPARE(table()->cell(0, 0).rowSpan, 2);
        QCOMPARE(table()->cell(0, 0).colSpan, 4);
        QCOMPARE(text(0, 0), QStringLiteral("00\n01\n02\n03\n10\n11\n12\n13"));
        // Split Cells puts a merged cell in a block back.
        ed->selectCells(id, 0, 0, 0, 0);
        w.act(QStringLiteral("tbl.split"))->trigger();
        QCOMPARE(table()->cell(0, 0).colSpan, 1);
        QVERIFY(!table()->cell(1, 3).covered);
        ed->undo();
        ed->undo();
        ed->undo();
        QCOMPARE(table()->cell(0, 0).colSpan, 1);

        // Delete Rows: every row the block covers, in one step.
        ed->selectCells(id, 1, 0, 2, 1);
        steps = ed->undoStack()->index();
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QCOMPARE(table()->rows, 2);
        QCOMPARE(table()->rowH.size(), 2);
        QCOMPARE(table()->cells.size(), 8);
        QCOMPARE(text(0, 0), QStringLiteral("00"));
        QCOMPARE(text(1, 3), QStringLiteral("33"));
        QCOMPARE(table()->rect.height(), table()->rowH[0] + table()->rowH[1]);
        ed->undo();
        QCOMPARE(table()->rows, 4);
        QCOMPARE(text(2, 3), QStringLiteral("23"));

        // Delete Columns likewise, the table keeping its width.
        const double width = table()->rect.width();
        ed->selectCells(id, 0, 1, 1, 2);
        steps = ed->undoStack()->index();
        w.act(QStringLiteral("tbl.delCol"))->trigger();
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QCOMPARE(table()->cols, 2);
        QCOMPARE(table()->colW.size(), 2);
        QCOMPARE(text(2, 0), QStringLiteral("20"));
        QCOMPARE(text(2, 1), QStringLiteral("23"));
        QVERIFY(std::abs(table()->rect.width() - width) < 0.001);
        ed->undo();
        QCOMPARE(table()->cols, 4);

        // A table keeps at least one row and one column.
        ed->selectCells(id, 0, 0, 3, 3);
        steps = ed->undoStack()->index();
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        w.act(QStringLiteral("tbl.delCol"))->trigger();
        QCOMPARE(table()->rows, 4);
        QCOMPARE(table()->cols, 4);
        QCOMPARE(ed->undoStack()->index(), steps);

        // Insert goes above or below, left or right, of the whole block.
        ed->selectCells(id, 1, 1, 2, 2);
        w.act(QStringLiteral("tbl.insAbove"))->trigger();
        QCOMPARE(table()->rows, 5);
        QCOMPARE(text(2, 0), QStringLiteral("10"));
        ed->undo();
        ed->selectCells(id, 1, 1, 2, 2);
        w.act(QStringLiteral("tbl.insBelow"))->trigger();
        QCOMPARE(table()->rows, 5);
        QCOMPARE(text(2, 0), QStringLiteral("20"));
        QCOMPARE(text(3, 0), QString());
        QCOMPARE(text(4, 0), QStringLiteral("30"));
        ed->undo();
        ed->selectCells(id, 1, 1, 2, 2);
        w.act(QStringLiteral("tbl.insLeft"))->trigger();
        QCOMPARE(table()->cols, 5);
        QCOMPARE(text(1, 2), QStringLiteral("11"));
        ed->undo();
        ed->selectCells(id, 1, 1, 2, 2);
        w.act(QStringLiteral("tbl.insRight"))->trigger();
        QCOMPARE(table()->cols, 5);
        QCOMPARE(text(1, 2), QStringLiteral("12"));
        QCOMPARE(text(1, 3), QString());
        QCOMPARE(text(1, 4), QStringLiteral("13"));
        ed->undo();

        // Delete or Backspace with a block clears the cells' text, in one step, and keeps the table.
        ed->selectCells(id, 1, 1, 2, 2);
        steps = ed->undoStack()->index();
        QTest::keyClick(w.canvas(), Qt::Key_Delete);
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QVERIFY(ed->doc()->item(id));
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) QCOMPARE(text(r, c).isEmpty(), r >= 1 && r <= 2 && c >= 1 && c <= 2);
        QVERIFY(ed->hasCellBlock());
        ed->undo();
        QCOMPARE(text(1, 1), QStringLiteral("11"));

        // A merged cell reaching into the deleted rows shrinks; one whose first row goes starts in the next.
        ed->change(QStringLiteral("Merge"), [&] {
            table()->cell(1, 3).rowSpan = 3;
            table()->cell(2, 3).covered = table()->cell(3, 3).covered = true;
        });
        ed->selectCells(id, 0, 0, 1, 0);   // rows 0 and 1; the merged cell began in row 1
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        QCOMPARE(table()->rows, 2);
        QVERIFY(!table()->cell(0, 3).covered);
        QCOMPARE(table()->cell(0, 3).rowSpan, 2);
        QVERIFY(table()->cell(1, 3).covered);
        QCOMPARE(text(0, 3), QStringLiteral("13"));   // it keeps its text
        ed->undo();
        QCOMPARE(table()->rows, 4);
        ed->selectCells(id, 2, 0, 2, 0);   // a row inside it
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        QCOMPARE(table()->rows, 3);
        QCOMPARE(table()->cell(1, 3).rowSpan, 2);
        QVERIFY(table()->cell(2, 3).covered);
        ed->undo();
        ed->selectCells(id, 1, 3, 3, 3);   // all its rows
        w.act(QStringLiteral("tbl.delRow"))->trigger();
        QCOMPARE(table()->rows, 1);
        QVERIFY(!table()->cell(0, 3).covered);
        ed->undo();
        // The columns of a merged cell go together.
        ed->change(QStringLiteral("Merge"), [&] {
            table()->cell(0, 1).colSpan = 2;
            table()->cell(0, 2).covered = true;
        });
        ed->selectCells(id, 0, 2, 0, 2);   // grows to columns 1 and 2
        w.act(QStringLiteral("tbl.delCol"))->trigger();
        QCOMPARE(table()->cols, 2);
        ed->undo();
        QCOMPARE(table()->cols, 4);
    }

    // A row or column inserted through a merged cell makes that cell bigger,
    // and the new cells under it are covered (the new row had two live cells
    // inside the merge, and the rows below it covered cells with nothing over
    // them). Then edits in any order, undone and redone, keep the grid whole.
    void tableEditsKeepMergedCellsWhole()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 300, 150), 3, 3));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        // What is wrong with the grid, if anything: every cell lies under exactly one
        // live cell's span, and spans stay inside the table.
        auto problem = [&]() -> QString {
            const jp::TableItem *t = table();
            if (!t) return QStringLiteral("the table is gone");
            if (t->rows < 1 || t->cols < 1 || t->cells.size() != t->rows * t->cols || t->colW.size() != t->cols || t->rowH.size() != t->rows)
                return QStringLiteral("sizes: %1x%2, %3 cells, %4 widths, %5 heights").arg(t->rows).arg(t->cols).arg(t->cells.size()).arg(t->colW.size()).arg(t->rowH.size());
            QVector<int> under(t->cells.size(), 0);
            for (int r = 0; r < t->rows; ++r)
                for (int c = 0; c < t->cols; ++c) {
                    const jp::TableCell &cl = t->cell(r, c);
                    if (!ed->doc()->storyDoc(cl.storyId)) return QStringLiteral("(%1,%2) has no text").arg(r).arg(c);
                    if (cl.covered) continue;
                    if (cl.rowSpan < 1 || cl.colSpan < 1 || r + cl.rowSpan > t->rows || c + cl.colSpan > t->cols)
                        return QStringLiteral("(%1,%2) spans %3x%4 in a %5x%6 table").arg(r).arg(c).arg(cl.rowSpan).arg(cl.colSpan).arg(t->rows).arg(t->cols);
                    for (int rr = r; rr < r + cl.rowSpan; ++rr)
                        for (int cc = c; cc < c + cl.colSpan; ++cc) {
                            ++under[rr * t->cols + cc];
                            if ((rr != r || cc != c) && !t->cell(rr, cc).covered) return QStringLiteral("(%1,%2) is live under the cell at (%3,%4)").arg(rr).arg(cc).arg(r).arg(c);
                        }
                }
            for (int i = 0; i < under.size(); ++i)
                if (under[i] != 1) return QStringLiteral("(%1,%2) is under %3 cells").arg(i / t->cols).arg(i % t->cols).arg(under[i]);
            return QString();
        };
        auto mergeBlock = [&] {
            ed->selectCells(id, 0, 0, 1, 1);
            w.act(QStringLiteral("tbl.merge"))->trigger();
            QCOMPARE(table()->cell(0, 0).rowSpan, 2);
        };

        // Insert Below with the cursor beside a merged cell: the merged cell reaches the new row.
        mergeBlock();
        ed->beginTextEdit(id, 0, 0, 2);
        w.act(QStringLiteral("tbl.insBelow"))->trigger();
        QCOMPARE(table()->rows, 4);
        QCOMPARE(table()->cell(0, 0).rowSpan, 3);
        QCOMPARE(table()->cell(0, 0).colSpan, 2);
        QVERIFY(table()->cell(2, 0).covered && table()->cell(2, 1).covered);
        QVERIFY(!table()->cell(2, 2).covered);
        QVERIFY(!table()->cell(3, 0).covered && table()->cell(3, 0).rowSpan == 1);
        QCOMPARE(problem(), QString());
        ed->undo();
        QCOMPARE(table()->rows, 3);
        QCOMPARE(table()->cell(0, 0).rowSpan, 2);
        QCOMPARE(problem(), QString());
        // Above the merge's first row it is only moved down; above its second row it grows.
        ed->beginTextEdit(id, 0, 0, 2);
        w.act(QStringLiteral("tbl.insAbove"))->trigger();
        QCOMPARE(table()->cell(0, 0).rowSpan, 1);
        QCOMPARE(table()->cell(1, 0).rowSpan, 2);
        QCOMPARE(problem(), QString());
        ed->undo();
        ed->beginTextEdit(id, 0, 1, 2);
        w.act(QStringLiteral("tbl.insAbove"))->trigger();
        QCOMPARE(table()->rows, 4);
        QCOMPARE(table()->cell(0, 0).rowSpan, 3);
        QVERIFY(table()->cell(1, 0).covered && table()->cell(1, 1).covered && !table()->cell(1, 2).covered);
        QCOMPARE(problem(), QString());
        ed->undo();
        // The same for columns.
        ed->beginTextEdit(id, 0, 2, 0);
        w.act(QStringLiteral("tbl.insRight"))->trigger();
        QCOMPARE(table()->cols, 4);
        QCOMPARE(table()->cell(0, 0).colSpan, 3);
        QCOMPARE(table()->cell(0, 0).rowSpan, 2);
        QVERIFY(table()->cell(0, 2).covered && table()->cell(1, 2).covered && !table()->cell(2, 2).covered);
        QCOMPARE(problem(), QString());
        ed->undo();
        ed->beginTextEdit(id, 0, 2, 1);
        w.act(QStringLiteral("tbl.insLeft"))->trigger();
        QCOMPARE(table()->cols, 4);
        QCOMPARE(table()->cell(0, 0).colSpan, 3);
        QVERIFY(table()->cell(0, 1).covered && table()->cell(1, 1).covered && !table()->cell(2, 1).covered);
        QCOMPARE(problem(), QString());
        ed->undo();
        QCOMPARE(problem(), QString());

        // Edits in any order (fixed seeds), undone and redone, never break the grid.
        const char *actions[] = {"tbl.merge", "tbl.split", "tbl.delRow", "tbl.delCol", "tbl.insAbove", "tbl.insBelow", "tbl.insLeft", "tbl.insRight", "edit.undo", "edit.redo"};
        int insertsInMerges = 0;   // inserts made while the table had merged cells
        for (const uint seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
            QRandomGenerator rng(seed);
            ed->undoStack()->clear();   // so Undo never goes back past this seed's first edit
            for (int step = 0; step < 150; ++step) {
                const jp::TableItem *t = table();
                const int roll = rng.bounded(100);
                QString what;
                if (roll < 40) {
                    ed->selectCells(id, rng.bounded(t->rows), rng.bounded(t->cols), rng.bounded(t->rows), rng.bounded(t->cols));
                    what = QStringLiteral("select");
                } else if (roll < 50) {
                    ed->beginTextEdit(id, 0, rng.bounded(t->rows), rng.bounded(t->cols));
                    what = QStringLiteral("cursor");
                } else {
                    const QString a = QString::fromLatin1(actions[rng.bounded(int(sizeof actions / sizeof *actions))]);
                    if (a.startsWith(QLatin1String("tbl.ins")) && (t->rows > 7 || t->cols > 7)) continue;   // keep it small
                    if (a == QLatin1String("edit.undo") && !ed->undoStack()->canUndo()) continue;
                    if (a.startsWith(QLatin1String("tbl.ins")) && std::any_of(t->cells.cbegin(), t->cells.cend(), [](const jp::TableCell &c) { return c.covered; })) ++insertsInMerges;
                    w.act(a)->trigger();
                    what = a;
                }
                QVERIFY2(problem().isEmpty(), qPrintable(QStringLiteral("seed %1, step %2 (%3): %4").arg(seed).arg(step).arg(what, problem())));
                if (ed->hasCellBlock()) {
                    const jp::CellRange b = ed->cellBlock().range;
                    QVERIFY2(b.r1 < table()->rows && b.c1 < table()->cols, qPrintable(QStringLiteral("seed %1, step %2 (%3): the selected cells are outside the table").arg(seed).arg(step).arg(what)));
                }
            }
        }
        QVERIFY2(insertsInMerges >= 30, qPrintable(QStringLiteral("only %1 inserts were made in a merged table").arg(insertsInMerges)));   // the seeds do reach the case
    }

    // Align Top, Middle, and Bottom set every cell when only the table is
    // selected (as Text Direction and Cell Margins do; they did nothing), and
    // just the selected cells otherwise.
    void tableVerticalAlignmentWithTheTableSelected()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 300, 150), 3, 3));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        ed->select(id);
        QVERIFY(!ed->hasCellBlock());
        w.act(QStringLiteral("valign.1"))->trigger();
        for (const auto &c : table()->cells) QCOMPARE(c.valign, jp::VAlign::Middle);
        ed->undo();
        for (const auto &c : table()->cells) QCOMPARE(c.valign, jp::VAlign::Top);
        ed->selectCells(id, 1, 1, 1, 2);
        w.act(QStringLiteral("valign.2"))->trigger();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) QCOMPARE(table()->cell(r, c).valign, r == 1 && c >= 1 ? jp::VAlign::Bottom : jp::VAlign::Top);
    }

    // Table Layout > Size has Height and Width boxes for the whole table (and
    // none for a row or a column), Grow to Fit Text as a check box, all with
    // KeyTips and names. Typing a width scales the columns in proportion,
    // typing a height scales the rows; with Lock aspect ratio the other
    // follows. Each is one undo step.
    void tableSizeBoxes()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 400, 400), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        auto near = [](const QVector<double> &got, const QVector<double> &want) {
            if (got.size() != want.size()) return false;
            for (int i = 0; i < got.size(); ++i)
                if (std::abs(got[i] - want[i]) > 1e-6) return false;
            return true;
        };
        ed->change(QStringLiteral("Sizes"), [&] {
            table()->colW = {100, 50, 150, 100};
            table()->rowH = {40, 120, 80, 160};
            table()->syncRect();
        });
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r && r->tab(QStringLiteral("Table Layout")));
        jp::RibbonGroup *size = nullptr;
        for (jp::RibbonGroup *g : r->tab(QStringLiteral("Table Layout"))->groups())
            if (g->title() == QStringLiteral("Size")) size = g;
        QVERIFY(size);
        // The table's own two boxes, and no box for a row's height or a column's width.
        QCOMPARE(r->tab(QStringLiteral("Table Layout"))->findChildren<jp::MeasureSpin *>().size(), 2);
        jp::MeasureSpin *widthBox = nullptr, *heightBox = nullptr;
        for (auto *s : size->findChildren<jp::MeasureSpin *>()) (s->toolTip() == QStringLiteral("Table Width") ? widthBox : heightBox) = s;
        QVERIFY(widthBox && heightBox);
        auto *grow = size->findChild<QCheckBox *>();
        QVERIFY(grow);
        QCOMPARE(grow->text(), QStringLiteral("Grow to Fit Text"));
        for (QWidget *c : {static_cast<QWidget *>(widthBox), static_cast<QWidget *>(heightBox), static_cast<QWidget *>(grow)})
            QVERIFY2(!jp::keytip(c).isEmpty(), qPrintable(c->toolTip()));
        QCOMPARE(widthBox->accessibleName(), QStringLiteral("Table Width"));
        QCOMPARE(heightBox->accessibleName(), QStringLiteral("Table Height"));

        ed->select(id);
        w.refreshUi();
        QCOMPARE(widthBox->value(), 400.0);
        QCOMPARE(heightBox->value(), 400.0);
        QVERIFY(grow->isChecked());

        // A width: the columns scale in proportion, the rows stay.
        int steps = ed->undoStack()->index();
        widthBox->setValue(200);
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QVERIFY(near(table()->colW, {50, 25, 75, 50}));
        QVERIFY(near(table()->rowH, {40, 120, 80, 160}));
        QCOMPARE(table()->rect.size(), QSizeF(200, 400));
        ed->undo();
        QVERIFY(near(table()->colW, {100, 50, 150, 100}));
        // A height: the rows scale; it works while typing in a cell, too.
        ed->beginTextEdit(id, 0, 2, 1);
        w.refreshUi();
        steps = ed->undoStack()->index();
        heightBox->setValue(240);
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QVERIFY(near(table()->rowH, {24, 72, 48, 96}));
        QVERIFY(near(table()->colW, {100, 50, 150, 100}));
        QCOMPARE(table()->rect.size(), QSizeF(400, 240));
        ed->undo();
        QVERIFY(near(table()->rowH, {40, 120, 80, 160}));
        // Locked, the other size follows, and the shown sizes follow the table.
        ed->change(QStringLiteral("Lock"), [&] { table()->lockSize = true; });
        ed->select(id);
        w.refreshUi();   // the boxes show the table again
        steps = ed->undoStack()->index();
        widthBox->setValue(200);
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QVERIFY(near(table()->colW, {50, 25, 75, 50}));
        QVERIFY(near(table()->rowH, {20, 60, 40, 80}));
        w.refreshUi();
        QCOMPARE(heightBox->value(), 200.0);
        // A row too short for its text grows (Grow to Fit Text), whatever was typed.
        ed->change(QStringLiteral("Text"), [&] {
            ed->doc()->storyDoc(table()->cell(0, 0).storyId)->setPlainText(QStringLiteral("one\ntwo\nthree\nfour"));
            table()->lockSize = false;
        });
        w.refreshUi();
        heightBox->setValue(40);
        QVERIFY2(table()->rowH[0] > 20.0, qPrintable(QString::number(table()->rowH[0])));   // 4 lines of text do not fit in 4 points
        // Grow to Fit Text is a check box that follows the table, and one step.
        ed->select(id);
        w.refreshUi();
        QVERIFY(grow->isChecked());
        steps = ed->undoStack()->index();
        grow->click();
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        QVERIFY(!table()->growToFit);
        QVERIFY(!grow->isChecked());
        ed->undo();
        w.refreshUi();
        QVERIFY(table()->growToFit);
        QVERIFY(grow->isChecked());
    }

    // Format Table: the Alignment group's launcher and the table's right-click
    // menu open it, with the shape tabs and Cell Properties (vertical
    // alignment, the four cell margins, and turning the text), which act on
    // the selected cells, the cell the cursor is in, or all of them. Lock
    // aspect ratio stays with the table.
    void tableFormatTable()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 400, 200), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        auto *r = w.findChild<jp::Ribbon *>();
        jp::RibbonGroup *align = nullptr;
        for (jp::RibbonGroup *g : r->tab(QStringLiteral("Table Layout"))->groups())
            if (g->title() == QStringLiteral("Alignment")) align = g;
        QVERIFY(align && align->launcher());
        QVERIFY(!jp::keytip(align->launcher()).isEmpty());
        ed->select(id);
        r->showTab(r->tab(QStringLiteral("Table Layout")));
        w.refreshUi();
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) w.grab(QRect(0, 0, 1400, 170)).save(qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/table-layout.png"));

        // The launcher: the dialog is Format Table, on Cell Properties.
        QString title, shown;
        QStringList tabNames;
        QTimer::singleShot(0, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!d) return;
            title = d->windowTitle();
            if (auto *tabs = d->findChild<QTabWidget *>()) {
                shown = tabs->tabText(tabs->currentIndex());
                for (int i = 0; i < tabs->count(); ++i) tabNames << tabs->tabText(i);
            }
            d->reject();
        });
        align->launcher()->click();
        QCOMPARE(title, QStringLiteral("Format Table"));
        QCOMPARE(shown, QStringLiteral("Cell Properties"));
        QCOMPARE(tabNames, (QStringList{"Colors and Lines", "Size", "Layout", "Cell Properties", "Alt Text"}));

        // Cell Properties on a block: it shows the first cell's, and changes the block's cells only.
        ed->change(QStringLiteral("Set up"), [&] {
            table()->cell(1, 1).valign = jp::VAlign::Bottom;
            table()->cell(1, 1).margins = QMarginsF(5, 6, 7, 8);
        });
        double firstTop = 0;
        int firstAlign = -1;
        auto setCells = [&](int valign, double left, double right, double top, double bottom, bool turned, bool lock, bool report) {
            QTimer::singleShot(0, [=, &firstTop, &firstAlign] {
                auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!d) return;
                auto *va = d->findChild<QComboBox *>(QStringLiteral("cellValign"));
                auto spin = [&](const char *name) { return d->findChild<jp::MeasureSpin *>(QString::fromLatin1(name)); };
                auto *rotate = d->findChild<QCheckBox *>(QStringLiteral("cellRotate"));
                auto *lockBox = d->findChild<QCheckBox *>(QStringLiteral("lockAspect"));
                if (!va || !spin("cellMarginLeft") || !spin("cellMarginRight") || !spin("cellMarginTop") || !spin("cellMarginBottom") || !rotate || !lockBox) {
                    d->reject();   // a missing control is a failure the checks below report
                    return;
                }
                if (report) {
                    firstTop = spin("cellMarginTop")->value();
                    firstAlign = va->currentIndex();
                }
                va->setCurrentIndex(valign);
                spin("cellMarginLeft")->setValue(left);
                spin("cellMarginRight")->setValue(right);
                spin("cellMarginTop")->setValue(top);
                spin("cellMarginBottom")->setValue(bottom);
                rotate->setChecked(turned);
                lockBox->setChecked(lock);
                d->accept();
            });
        };
        ed->selectCells(id, 1, 1, 2, 2);
        int steps = ed->undoStack()->index();
        setCells(1, 10, 11, 12, 13, true, true, true);
        w.act(QStringLiteral("tbl.format"))->trigger();
        QCOMPARE(firstTop, 6.0);
        QCOMPARE(firstAlign, 2);
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        for (int rr = 0; rr < 4; ++rr)
            for (int cc = 0; cc < 4; ++cc) {
                const jp::TableCell &c = table()->cell(rr, cc);
                const bool in = rr >= 1 && rr <= 2 && cc >= 1 && cc <= 2;
                QCOMPARE(c.margins == QMarginsF(10, 12, 11, 13), in);
                QCOMPARE(c.valign == jp::VAlign::Middle, in);
                QCOMPARE(c.vertical, in);
            }
        QVERIFY(table()->lockSize);
        QVERIFY(ed->hasCellBlock());   // the block stays selected
        ed->undo();
        QVERIFY(!table()->lockSize);
        QCOMPARE(table()->cell(2, 2).margins, QMarginsF(2.88, 2.88, 2.88, 2.88));
        // The cell the cursor is in; and with only the table selected, every cell.
        ed->beginTextEdit(id, 0, 3, 3);
        steps = ed->undoStack()->index();
        setCells(2, 3, 3, 3, 3, true, false, false);
        w.act(QStringLiteral("tbl.format"))->trigger();
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        for (int rr = 0; rr < 4; ++rr)
            for (int cc = 0; cc < 4; ++cc) QCOMPARE(table()->cell(rr, cc).valign == jp::VAlign::Bottom && table()->cell(rr, cc).vertical, rr == 3 && cc == 3);
        ed->undo();
        ed->select(id);
        setCells(1, 4, 4, 4, 4, true, false, false);
        w.act(QStringLiteral("tbl.format"))->trigger();
        for (const auto &c : table()->cells) QVERIFY(c.valign == jp::VAlign::Middle && c.vertical && c.margins == QMarginsF(4, 4, 4, 4));

        // The right-click menu offers Format Table for a table, selected or typed in; other objects keep Format Object.
        jp::Canvas *cv = w.canvas();
        auto menuFor = [&](const QPointF &pagePoint) {
            QStringList ids;
            QTimer::singleShot(100, [&] {
                if (auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                    for (QAction *a : m->actions()) ids << a->objectName();
                    m->close();
                }
            });
            const QPoint at = cv->pageToView(pagePoint).toPoint();
            QContextMenuEvent ce(QContextMenuEvent::Mouse, at, cv->viewport()->mapToGlobal(at));
            QApplication::sendEvent(cv->viewport(), &ce);
            return ids;
        };
        ed->select(id);
        QStringList items = menuFor(QPointF(100, 100));
        QVERIFY2(items.contains(QStringLiteral("tbl.format")) && !items.contains(QStringLiteral("obj.format")), qPrintable(items.join(QLatin1Char(' '))));
        ed->beginTextEdit(id, 0, 1, 1);
        items = menuFor(QPointF(100, 100));
        QVERIFY2(items.contains(QStringLiteral("tbl.format")) && !items.contains(QStringLiteral("obj.format")), qPrintable(items.join(QLatin1Char(' '))));
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 400, 200, 60), QStringLiteral("text")));
        ed->addItem(box);
        items = menuFor(QPointF(100, 420));
        QVERIFY2(items.contains(QStringLiteral("obj.format")) && !items.contains(QStringLiteral("tbl.format")), qPrintable(items.join(QLatin1Char(' '))));
    }

    // Lock aspect ratio (Format Table > Size) keeps the table's proportions
    // when a corner handle is dragged.
    void tableLockAspectRatioKeepsProportions()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 360, 160), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        jp::Canvas *cv = w.canvas();
        auto cornerDrag = [&](const QPoint &by) {
            ed->select(id);
            const QPoint from = cv->pageToView(table()->rect.bottomRight()).toPoint();
            QTest::mousePress(cv->viewport(), Qt::LeftButton, Qt::NoModifier, from);
            for (int k = 1; k <= 4; ++k) {
                const QPoint p = from + by * k / 4;
                QMouseEvent mv(QEvent::MouseMove, QPointF(p), cv->viewport()->mapToGlobal(QPointF(p)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(cv->viewport(), &mv);
            }
            QTest::mouseRelease(cv->viewport(), Qt::LeftButton, Qt::NoModifier, from + by);
        };
        const double ratio = 360.0 / 160.0;
        cornerDrag(QPoint(80, 10));
        QVERIFY2(std::abs(table()->rect.width() / table()->rect.height() - ratio) > 0.2, "unlocked, a corner drag changes the proportions");
        ed->undo();
        QCOMPARE(table()->rect.size(), QSizeF(360, 160));
        ed->change(QStringLiteral("Lock"), [&] { table()->lockSize = true; });
        cornerDrag(QPoint(80, 10));
        QVERIFY2(table()->rect.width() > 400, qPrintable(QString::number(table()->rect.width())));
        QVERIFY2(std::abs(table()->rect.width() / table()->rect.height() - ratio) < 0.01, qPrintable(QString::number(table()->rect.width() / table()->rect.height())));
        // The rows and columns scaled along.
        double total = 0;
        for (double c : table()->colW) total += c;
        QVERIFY(std::abs(total - table()->rect.width()) < 0.01);
    }

    // Text Direction, Hyphenation, and the alignment buttons' check marks work
    // on the selected cells, as the other cell commands do (they took the
    // cell the cursor was in, or every cell).
    void tableTextDirectionAndHyphenationFollowTheBlock()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto made = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 72, 400, 200), 4, 4));
        ed->addItem(made);
        const QString id = made->id;
        auto table = [&] { return static_cast<jp::TableItem *>(ed->doc()->item(id)); };
        auto inBlock = [](int r, int c) { return r >= 1 && r <= 2 && c >= 1 && c <= 2; };
        ed->selectCells(id, 1, 1, 2, 2);
        w.refreshUi();
        int steps = ed->undoStack()->index();
        w.act(QStringLiteral("tb.direction"))->trigger();
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) QCOMPARE(table()->cell(r, c).vertical, inBlock(r, c));
        QVERIFY(ed->hasCellBlock());
        w.refreshUi();
        QVERIFY(w.act(QStringLiteral("tb.direction"))->isChecked());   // every cell of the block is turned
        ed->selectCells(id, 0, 0, 1, 1);
        w.refreshUi();
        QVERIFY(!w.act(QStringLiteral("tb.direction"))->isChecked());   // this block is not
        ed->selectCells(id, 1, 1, 2, 2);
        w.act(QStringLiteral("tb.direction"))->trigger();   // turned back
        for (const auto &c : table()->cells) QVERIFY(!c.vertical);
        // Hyphenation: the dialog shows the block's first cell and sets the block's cells.
        ed->change(QStringLiteral("Set up"), [&] { table()->cell(1, 1).hyphenate = false; });
        bool shownOn = true;
        QTimer::singleShot(0, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!d) return;
            for (auto *box : d->findChildren<QCheckBox *>()) {
                shownOn = box->isChecked();
                box->setChecked(false);
                break;
            }
            d->accept();
        });
        ed->selectCells(id, 1, 1, 2, 2);
        steps = ed->undoStack()->index();
        w.act(QStringLiteral("rev.hyphenation"))->trigger();
        QVERIFY(!shownOn);   // (1,1) is not hyphenated, and the dialog showed it
        QCOMPARE(ed->undoStack()->index(), steps + 1);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) QCOMPARE(table()->cell(r, c).hyphenate, !inBlock(r, c));
        QVERIFY(ed->hasCellBlock());
        // The vertical-alignment buttons are checked for the selected cells.
        ed->change(QStringLiteral("Align"), [&] {
            for (int r = 1; r <= 2; ++r)
                for (int c = 1; c <= 2; ++c) table()->cell(r, c).valign = jp::VAlign::Middle;
        });
        ed->selectCells(id, 1, 1, 2, 2);
        w.refreshUi();
        QVERIFY(w.act(QStringLiteral("valign.1"))->isChecked() && !w.act(QStringLiteral("valign.0"))->isChecked() && !w.act(QStringLiteral("valign.2"))->isChecked());
        ed->selectCells(id, 0, 1, 2, 2);   // mixed: none is checked
        w.refreshUi();
        QVERIFY(!w.act(QStringLiteral("valign.0"))->isChecked() && !w.act(QStringLiteral("valign.1"))->isChecked() && !w.act(QStringLiteral("valign.2"))->isChecked());
    }

    // A picture dragged onto another swaps with it, as the scratch area's
    // message says (nothing did): the dragged frame goes back, and the two
    // pictures trade frames.
    void pictureDroppedOnPictureSwaps()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto picture = [&](const QColor &c, const QRectF &r) {
            QImage img(40, 40, QImage::Format_RGB32);
            img.fill(c);
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            auto p = std::make_shared<jp::PictureItem>();
            p->imageId = ed->doc()->addImage(png, "png");
            p->rect = r;
            p->imgRect = QRectF(QPointF(0, 0), r.size());
            ed->addItem(p);
            return p;
        };
        auto red = picture(Qt::red, QRectF(72, 72, 144, 144));
        auto blue = picture(Qt::blue, QRectF(300, 300, 144, 144));
        const QString redImage = red->imageId, blueImage = blue->imageId;
        jp::Canvas *cv = w.canvas();
        QTest::qWait(50);
        const QPoint a = cv->pageToView(red->rect.center()).toPoint(), b = cv->pageToView(blue->rect.center()).toPoint();
        QTest::mousePress(cv->viewport(), Qt::LeftButton, Qt::NoModifier, a);
        for (int k = 1; k <= 4; ++k) {
            const QPoint at = a + (b - a) * k / 4;
            QMouseEvent mv(QEvent::MouseMove, QPointF(at), cv->viewport()->mapToGlobal(QPointF(at)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(cv->viewport(), &mv);
        }
        QTest::mouseRelease(cv->viewport(), Qt::LeftButton, Qt::NoModifier, b);
        QCOMPARE(red->rect, QRectF(72, 72, 144, 144));   // back where it was
        QCOMPARE(red->imageId, blueImage);
        QCOMPARE(blue->imageId, redImage);
        ed->undo();   // undo makes the objects afresh
        QCOMPARE(static_cast<jp::PictureItem *>(ed->doc()->item(red->id))->imageId, redImage);
    }

    // Ctrl+drag copies a picture, and a copy let go over another picture stays
    // where it was dropped (it swapped, which put the copy back over the
    // original, and gave the other picture the same image).
    void pictureCopiedOntoPictureDoesNotSwap()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto picture = [&](const QColor &c, const QRectF &r) {
            QImage img(40, 40, QImage::Format_RGB32);
            img.fill(c);
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            auto p = std::make_shared<jp::PictureItem>();
            p->imageId = ed->doc()->addImage(png, "png");
            p->rect = r;
            p->imgRect = QRectF(QPointF(0, 0), r.size());
            ed->addItem(p);
            return p;
        };
        auto red = picture(Qt::red, QRectF(72, 72, 144, 144));
        auto blue = picture(Qt::blue, QRectF(300, 300, 144, 144));
        const QString redImage = red->imageId, blueImage = blue->imageId;
        jp::Canvas *cv = w.canvas();
        ed->select(red->id);
        QTest::qWait(50);
        const QPoint a = cv->pageToView(red->rect.center()).toPoint(), b = cv->pageToView(blue->rect.center()).toPoint();
        QTest::mousePress(cv->viewport(), Qt::LeftButton, Qt::ControlModifier, a);
        for (int k = 1; k <= 4; ++k) {
            const QPoint at = a + (b - a) * k / 4;
            QMouseEvent mv(QEvent::MouseMove, QPointF(at), cv->viewport()->mapToGlobal(QPointF(at)), Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
            QApplication::sendEvent(cv->viewport(), &mv);
        }
        QTest::mouseRelease(cv->viewport(), Qt::LeftButton, Qt::ControlModifier, b);
        QCOMPARE(red->rect, QRectF(72, 72, 144, 144));   // the original has not moved
        QCOMPARE(red->imageId, redImage);
        QCOMPARE(blue->imageId, blueImage);              // and the other picture is as it was
        QCOMPARE(int(ed->surfaceItems().size()), 3);
        auto *copy = dynamic_cast<jp::PictureItem *>(ed->single());
        QVERIFY(copy && copy != red.get() && copy != blue.get());
        QCOMPARE(copy->imageId, redImage);
        QVERIFY(copy->rect.center().x() > 250 && copy->rect.center().y() > 250);   // where it was let go
        ed->undo();   // one step takes the copy back out
        QCOMPARE(int(ed->surfaceItems().size()), 2);
    }

    // Text and files dropped on the page land at the pointer, as Extra Content
    // does (they landed about 27 points up and to the left of it, by the
    // width of the ruler).
    void textAndFileDropsLandAtThePointer()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        jp::Canvas *cv = w.canvas();
        const QPoint at = cv->pageToView(QPointF(300, 250)).toPoint();
        const QPointF want = cv->toPage(QPointF(at));
        auto dropOn = [&](QMimeData &md) {
            QDragEnterEvent enter(at, Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(cv->viewport(), &enter);
            QVERIFY(enter.isAccepted());
            QDropEvent drop(QPointF(at), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(cv->viewport(), &drop);
            QVERIFY(drop.isAccepted());
        };
        QMimeData text;
        text.setText(QStringLiteral("dropped words"));
        dropOn(text);
        auto *box = dynamic_cast<jp::TextItem *>(ed->surfaceItems().back().get());
        QVERIFY(box);
        QCOMPARE(box->rect.topLeft(), want);
        QSignalSpy wanted(cv, &jp::Canvas::insertFilesWanted);
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(QStringLiteral("/nowhere/a picture.png"))});
        dropOn(files);
        QCOMPARE(wanted.count(), 1);
        QCOMPARE(wanted.at(0).at(1).toPointF(), want);
    }

    // Format Page Numbers sets the numbers' style and the first page's number
    // (it opened Insert Page Number, which added another number box).
    void formatPageNumbers()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        ed->insertPages(0, 2, false, false);
        ed->setCurrentPage(0);
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 200, 40)));
        ed->addItem(box);
        ed->beginTextEdit(box->id, 0);
        ed->insertField(QStringLiteral("page"));
        ed->endTextEdit();
        const int itemsBefore = int(ed->doc()->pages[0]->items.size());
        jp::FrameSpec spec = jp::Renderer::frameSpec(*ed->doc(), *box, 1, jp::RenderOptions());
        const QString keyBefore = spec.ctx.key();
        QCOMPARE(spec.ctx.resolve(QStringLiteral("page")), QStringLiteral("1"));
        QTimer::singleShot(0, [] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!d) return;
            if (auto *f = d->findChild<QComboBox *>(QStringLiteral("format"))) f->setCurrentIndex(3);   // i, ii, iii
            if (auto *s = d->findChild<QSpinBox *>(QStringLiteral("start"))) s->setValue(4);
            d->accept();
        });
        w.act(QStringLiteral("ins.pageNumberFormat"))->trigger();
        QCOMPARE(int(ed->doc()->pages[0]->items.size()), itemsBefore);   // no new box
        spec = jp::Renderer::frameSpec(*ed->doc(), *box, 1, jp::RenderOptions());
        QCOMPARE(spec.ctx.resolve(QStringLiteral("page")), QStringLiteral("iv"));
        QCOMPARE(spec.ctx.resolve(QStringLiteral("page:ROMAN")), QStringLiteral("IV"));   // its own style stays
        QVERIFY(spec.ctx.key() != keyBefore);   // laid out again
        ed->doc()->setup.pageNumberFormat = QStringLiteral("ALPHA");
        spec.ctx.pageNumber = 26;   // number 29 (from 4): past Z the letters go round again, doubled
        QCOMPARE(spec.ctx.resolve(QStringLiteral("page")), QStringLiteral("CC"));
        // Kept in the file.
        const jp::PageSetup back = jp::PageSetup::fromJson(ed->doc()->setup.toJson());
        QCOMPARE(back.firstPageNumber, 4);
        QCOMPARE(back.pageNumberFormat, QStringLiteral("ALPHA"));
    }

    // Online Pictures searches Openverse and Wikimedia Commons (it was a list
    // of links), shows each picture's author and license, and inserts the one
    // chosen with its title as alt text and a credit under it. Offline: the
    // libraries' answers are canned, in the form their documentation gives.
    void onlinePicturesSearchAndInsert()
    {
        // (Built here rather than written out: moc misreads raw string literals.)
        auto ovResult = [](const char *id, const char *title, const char *creator, const char *lic, const char *ver, const char *url) {
            return QJsonObject{{"id", id}, {"title", title}, {"creator", creator}, {"license", lic}, {"license_version", ver},
                               {"license_url", QStringLiteral("https://creativecommons.org/licenses/%1/%2/").arg(QLatin1String(lic), QLatin1String(ver))},
                               {"foreign_landing_url", QStringLiteral("https://example.org/page/%1").arg(QLatin1String(id))}, {"url", url},
                               {"thumbnail", QStringLiteral("https://api.openverse.org/v1/images/%1/thumb/").arg(QLatin1String(id))}};
        };
        const QByteArray openverse = QJsonDocument(QJsonObject{{"result_count", 2}, {"results", QJsonArray{
            ovResult("a1", "Red barn", "Pat Lee", "by", "2.0", "https://example.org/barn.png"),
            ovResult("a2", "Blue sky", "", "cc0", "1.0", "https://example.org/sky.png")}}}).toJson();
        const QList<jp::online::Picture> ov = jp::online::parseOpenverse(openverse);
        QCOMPARE(ov.size(), 2);
        QCOMPARE(ov[0].license, QStringLiteral("CC BY 2.0"));
        QVERIFY(ov[0].needsCredit);
        QCOMPARE(ov[1].license, QStringLiteral("CC0 1.0"));
        QVERIFY(!ov[1].needsCredit);
        QCOMPARE(jp::online::credit(ov[0]), QStringLiteral("“Red barn” by Pat Lee, CC BY 2.0"));
        auto meta = [](const QJsonObject &fields) {
            QJsonObject o;
            for (auto it = fields.begin(); it != fields.end(); ++it) o[it.key()] = QJsonObject{{"value", it.value()}};
            return o;
        };
        auto page = [](int index, const char *title, const char *url, const QJsonObject &ext) {
            return QJsonObject{{"pageid", index * 10}, {"index", index}, {"title", title},
                               {"imageinfo", QJsonArray{QJsonObject{{"url", url}, {"descriptionurl", QStringLiteral("https://commons.wikimedia.org/wiki/%1").arg(QLatin1String(title))},
                                                                    {"thumburl", QStringLiteral("%1.thumb").arg(QLatin1String(url))}, {"extmetadata", ext}}}}};
        };
        const QByteArray commons = QJsonDocument(QJsonObject{{"query", QJsonObject{{"pages", QJsonObject{
            {"20", page(2, "File:Second.jpg", "https://upload.wikimedia.org/2.jpg", meta({{"LicenseShortName", "Public domain"}, {"AttributionRequired", "false"}}))},
            {"10", page(1, "File:First photo.jpg", "https://upload.wikimedia.org/1.jpg",
                        meta({{"LicenseShortName", "CC BY-SA 4.0"}, {"Artist", "<a href=\"//x\">Sam <b>Rivera</b></a>"}, {"AttributionRequired", "true"}}))}}}}}}).toJson();
        const QList<jp::online::Picture> wc = jp::online::parseCommons(commons);
        QCOMPARE(wc.size(), 2);
        QCOMPARE(wc[0].title, QStringLiteral("First photo"));   // the search's order, the name without "File:"
        QCOMPARE(wc[0].creator, QStringLiteral("Sam Rivera"));
        QVERIFY(wc[0].needsCredit);
        QVERIFY(!wc[1].needsCredit);
        QVERIFY(jp::online::searchUrl(jp::online::Openverse, QStringLiteral("red barn")).toString(QUrl::FullyEncoded).contains(QStringLiteral("q=red%20barn")));
        // The pane, with the libraries answered from here.
        QImage red(30, 20, QImage::Format_RGB32);
        red.fill(Qt::red);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        red.save(&buf, "PNG");
        QStringList asked;
        const auto keep = jp::online::fetch;
        jp::online::fetch = [&](const QUrl &u, QObject *, const jp::online::Done &done, qint64) {
            asked << u.toString();
            done(u.host() == QLatin1String("api.openverse.org") && u.path() == QLatin1String("/v1/images/") ? openverse : png, QString());
        };
        jp::MainWindow w;
        w.showTaskPane(QStringLiteral("online"));
        auto *pane = w.findChild<jp::OnlinePicturesPane *>();
        QVERIFY(pane);
        pane->search(QStringLiteral("barn"));
        QCOMPARE(pane->results()->count(), 2);
        QVERIFY(!pane->results()->item(0)->icon().isNull());   // its small copy came
        QVERIFY(pane->creditBox()->isChecked());
        pane->insertButton()->click();
        jp::online::fetch = keep;
        QVERIFY(asked.contains(QStringLiteral("https://example.org/barn.png")));
        auto *pic = dynamic_cast<jp::PictureItem *>(w.editor()->single());
        QVERIFY(pic);
        QCOMPARE(pic->altText, QStringLiteral("Red barn"));
        auto *cap = dynamic_cast<jp::TextItem *>(w.editor()->doc()->item(pic->caption));
        QVERIFY(cap);
        QCOMPARE(w.editor()->doc()->storyDoc(cap->storyId)->toPlainText(), QStringLiteral("“Red barn” by Pat Lee, CC BY 2.0"));
        QCOMPARE(w.editor()->doc()->imageSize(pic->imageId), QSize(30, 20));
    }

    // Clearing the search box and pressing Enter left the old search's small
    // copies on their way, and they were set on list items that had gone (a
    // crash). A new search, with words or without, now ends the old one's
    // requests and drops any answer that still comes.
    void onlinePicturesNewSearchEndsPendingAnswers()
    {
        QJsonArray found;
        for (int i = 0; i < 3; ++i)
            found.append(QJsonObject{{"title", QStringLiteral("Pic %1").arg(i)}, {"license", "by"}, {"license_version", "2.0"},
                                     {"url", QStringLiteral("https://example.org/%1.png").arg(i)}, {"thumbnail", QStringLiteral("https://example.org/%1.thumb").arg(i)}});
        const QByteArray openverse = QJsonDocument(QJsonObject{{"results", found}}).toJson();
        QImage red(30, 20, QImage::Format_RGB32);
        red.fill(Qt::red);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        red.save(&buf, "PNG");
        std::vector<jp::online::Done> waiting;   // small copies, not yet arrived
        QList<QPointer<QObject>> asking;         // who asked for them
        const auto keep = jp::online::fetch;
        const auto restore = qScopeGuard([&] { jp::online::fetch = keep; });
        jp::online::fetch = [&](const QUrl &u, QObject *context, const jp::online::Done &done, qint64) {
            if (u.host() == QLatin1String("api.openverse.org")) {
                done(openverse, QString());
                return;
            }
            waiting.push_back(done);
            asking << context;
        };
        jp::MainWindow w;
        w.showTaskPane(QStringLiteral("online"));
        auto *pane = w.findChild<jp::OnlinePicturesPane *>();
        QVERIFY(pane);
        pane->search(QStringLiteral("barn"));
        QCOMPARE(pane->results()->count(), 3);
        QCOMPARE(int(waiting.size()), 3);
        pane->search(QString());   // the box cleared, and Enter pressed
        QCOMPARE(pane->results()->count(), 0);
        for (const auto &who : asking) QVERIFY(who.isNull());   // their requests are ended
        for (const auto &done : waiting) done(png, QString());   // and any that still come are dropped
        QCOMPARE(pane->results()->count(), 0);
        // A new search with words does the same for the one before it.
        waiting.clear();
        asking.clear();
        pane->search(QStringLiteral("barn"));
        pane->search(QStringLiteral("sky"));
        QCOMPARE(pane->results()->count(), 3);
        QCOMPARE(int(waiting.size()), 6);
        for (int i = 0; i < 3; ++i) QVERIFY(asking[i].isNull());
        for (int i = 0; i < 3; ++i) waiting[i](png, QString());   // the first search's
        for (int i = 0; i < 3; ++i) QVERIFY(pane->results()->item(i)->icon().isNull());
        for (int i = 3; i < 6; ++i) waiting[i](png, QString());   // the second's
        for (int i = 0; i < 3; ++i) QVERIFY(!pane->results()->item(i)->icon().isNull());
    }

    // A library's answer can name a file on this computer, a program, or data
    // in the address, and the pane would fetch it or show a link to it. Only
    // https addresses are used: results with another kind are left out, and the
    // download itself refuses them.
    void onlinePicturesTrustOnlySecureAddresses()
    {
        const QString pic = QStringLiteral("https://example.org/a.png"), thumb = QStringLiteral("https://example.org/a.thumb");
        const QString license = QStringLiteral("https://creativecommons.org/licenses/by/2.0/"), landing = QStringLiteral("https://example.org/p");
        auto result = [](const char *title, const QString &url, const QString &thumbnail, const QString &licenseUrl, const QString &page) {
            return QJsonObject{{"title", title}, {"license", "by"}, {"license_version", "2.0"}, {"license_url", licenseUrl}, {"foreign_landing_url", page}, {"url", url}, {"thumbnail", thumbnail}};
        };
        const QByteArray openverse = QJsonDocument(QJsonObject{{"results", QJsonArray{
            result("good", pic, thumb, license, landing),
            result("file picture", QStringLiteral("file:///etc/hostname"), thumb, license, landing),
            result("data picture", QStringLiteral("data:text/plain;base64,SEVMTE8="), thumb, license, landing),
            result("plain http picture", QStringLiteral("http://example.org/b.png"), thumb, license, landing),
            result("file thumbnail", pic, QStringLiteral("file:///etc/hostname"), license, landing),
            result("program link", pic, thumb, QStringLiteral("file:///C:/Windows/System32/calc.exe"), landing),
            result("script link", pic, thumb, license, QStringLiteral("javascript:alert(1)")),
            result("no thumbnail", pic, QString(), license, landing)}}}).toJson();
        const QList<jp::online::Picture> list = jp::online::parseOpenverse(openverse);
        QStringList titles;
        for (const auto &p : list) titles << p.title;
        QCOMPARE(titles, (QStringList{QStringLiteral("good"), QStringLiteral("no thumbnail")}));
        QVERIFY(list[1].thumb.isEmpty());
        for (const auto &p : list) {
            QCOMPARE(p.full.scheme(), QStringLiteral("https"));
            QVERIFY(p.licenseUrl.startsWith(QStringLiteral("https://")));
        }
        // Commons: the same for each of its addresses.
        auto meta = [](const QString &licenseUrl) {
            return QJsonObject{{"LicenseShortName", QJsonObject{{"value", "CC BY 2.0"}}}, {"LicenseUrl", QJsonObject{{"value", licenseUrl}}}};
        };
        auto entry = [&](int index, const char *title, const QString &url, const QString &licenseUrl) {
            return QJsonObject{{"index", index}, {"title", title}, {"imageinfo", QJsonArray{QJsonObject{{"url", url}, {"thumburl", thumb}, {"descriptionurl", landing}, {"extmetadata", meta(licenseUrl)}}}}};
        };
        const QByteArray commons = QJsonDocument(QJsonObject{{"query", QJsonObject{{"pages", QJsonObject{
            {"1", entry(1, "File:Good.png", pic, license)}, {"2", entry(2, "File:Local.png", QStringLiteral("file:///etc/hostname"), license)},
            {"3", entry(3, "File:Program.png", pic, QStringLiteral("file:///C:/Windows/System32/calc.exe"))}, {"4", entry(4, "File:Plain.png", QStringLiteral("http://example.org/c.png"), license)}}}}}}).toJson();
        const QList<jp::online::Picture> fromCommons = jp::online::parseCommons(commons);
        QCOMPARE(fromCommons.size(), 1);
        QCOMPARE(fromCommons[0].title, QStringLiteral("Good"));
        // The pane shows no link to them either.
        {
            const auto keep = jp::online::fetch;
            const auto restore = qScopeGuard([&] { jp::online::fetch = keep; });
            jp::online::fetch = [&](const QUrl &, QObject *, const jp::online::Done &done, qint64) { done(openverse, QString()); };
            jp::MainWindow w;
            w.showTaskPane(QStringLiteral("online"));
            auto *pane = w.findChild<jp::OnlinePicturesPane *>();
            QVERIFY(pane);
            pane->search(QStringLiteral("x"));
            QCOMPARE(pane->results()->count(), 2);
            for (auto *l : pane->findChildren<QLabel *>()) QVERIFY(!l->text().contains(QStringLiteral("file:")) && !l->text().contains(QStringLiteral("javascript:")));
        }
        // The download refuses a file, data, and a plain web address without reading them.
        QTemporaryDir dir;
        QFile secret(dir.filePath(QStringLiteral("secret.txt")));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("SECRET-LOCAL-FILE");
        secret.close();
        QObject context;
        for (const QUrl &u : {QUrl::fromLocalFile(secret.fileName()), QUrl(QStringLiteral("data:text/plain;base64,SEVMTE8=")), QUrl(QStringLiteral("http://127.0.0.1:9/never"))}) {
            bool answered = false;
            QByteArray body;
            QString error;
            jp::online::fetch(u, &context, [&](const QByteArray &b, const QString &e) { body = b; error = e; answered = true; }, jp::online::kMaxSmallBytes);
            QTRY_VERIFY_WITH_TIMEOUT(answered, 5000);
            QVERIFY2(body.isEmpty() && !error.isEmpty(), qPrintable(u.toString()));
        }
    }

    // A library's answer is held to a size: no more results than were asked
    // for, no more bytes than a picture or a small copy should be (the download
    // stops when they pass it, and when it takes too long overall, not only when
    // it goes quiet), and a Commons picture wider than 1,920 pixels comes as a
    // copy that wide, not the original upload. (The downloads are from a server
    // in this program, on this computer; nothing goes online.)
    void onlinePicturesLimitWhatALibraryCanSend()
    {
        QCOMPARE(jp::online::kMaxResults, 24);
        QVERIFY(jp::online::searchUrl(jp::online::Openverse, QStringLiteral("x")).query().contains(QStringLiteral("page_size=24")));
        QVERIFY(jp::online::searchUrl(jp::online::Commons, QStringLiteral("x")).query().contains(QStringLiteral("gsrlimit=24")));
        QJsonArray many;
        QJsonObject pages;
        for (int i = 0; i < 100; ++i) {
            many.append(QJsonObject{{"title", QStringLiteral("Pic %1").arg(i)}, {"license", "by"}, {"license_version", "2.0"}, {"url", QStringLiteral("https://example.org/%1.png").arg(i)}});
            pages[QString::number(i)] = QJsonObject{{"index", i}, {"title", QStringLiteral("File:Pic %1.png").arg(i)},
                                                    {"imageinfo", QJsonArray{QJsonObject{{"url", QStringLiteral("https://example.org/%1.png").arg(i)}}}}};
        }
        const QList<jp::online::Picture> ov = jp::online::parseOpenverse(QJsonDocument(QJsonObject{{"results", many}}).toJson());
        QCOMPARE(ov.size(), 24);
        QCOMPARE(ov.last().title, QStringLiteral("Pic 23"));
        const QList<jp::online::Picture> wc = jp::online::parseCommons(QJsonDocument(QJsonObject{{"query", QJsonObject{{"pages", pages}}}}).toJson());
        QCOMPARE(wc.size(), 24);
        QCOMPARE(wc.last().title, QStringLiteral("Pic 23"));   // the first 24 in the search's order
        // A big original comes as a sized copy; a small one, or one of unknown width, as itself.
        auto commons = [](const char *file, int width) {
            const QString original = QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/a/ab/%1").arg(QLatin1String(file));
            QJsonObject info{{"url", original}, {"thumburl", QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/%1/240px-%1").arg(QLatin1String(file))}};
            if (width > 0) info["width"] = width;
            return jp::online::parseCommons(QJsonDocument(QJsonObject{{"query", QJsonObject{{"pages", QJsonObject{{"1", QJsonObject{{"index", 1}, {"title", "File:x"}, {"imageinfo", QJsonArray{info}}}}}}}}}).toJson());
        };
        QCOMPARE(commons("Big.jpg", 6000).value(0).full.toString(), QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/Big.jpg/1920px-Big.jpg"));
        QCOMPARE(commons("Big.jpg", 1920).value(0).full.toString(), QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/a/ab/Big.jpg"));
        QCOMPARE(commons("Big.jpg", 0).value(0).full.toString(), QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/a/ab/Big.jpg"));
        QVERIFY(jp::online::searchUrl(jp::online::Commons, QStringLiteral("x")).query().contains(QStringLiteral("size")));   // the width is asked for
        // The download.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        enum Reply { Small, Declared, Endless, Stalled, Elsewhere } reply = Small;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *s = server.nextPendingConnection();
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QTcpSocket::readyRead, s, [&reply, s] {
                s->readAll();
                switch (reply) {
                case Small: s->write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"); break;
                case Declared: s->write("HTTP/1.1 200 OK\r\nContent-Length: 3000000\r\nConnection: close\r\n\r\n" + QByteArray(10000, 'x')); break;
                case Stalled: s->write("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\nhello"); break;
                case Elsewhere: s->write("HTTP/1.1 302 Found\r\nLocation: file:///etc/hostname\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); break;
                case Endless: {
                    s->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                    auto pump = [s] {
                        while (s->state() == QAbstractSocket::ConnectedState && s->bytesToWrite() < 200000) s->write(QByteArray(65536, 'x'));
                    };
                    connect(s, &QTcpSocket::bytesWritten, s, pump);
                    pump();
                    break;
                }
                }
            });
        });
        QNetworkAccessManager nam;
        auto get = [&](Reply r, qint64 most, int totalMs, QByteArray *body, QString *error) {
            reply = r;
            bool answered = false;
            QObject context;
            jp::online::download(&nam, QUrl(QStringLiteral("http://127.0.0.1:%1/p").arg(server.serverPort())), &context, [&](const QByteArray &b, const QString &e) { *body = b; *error = e; answered = true; }, most, totalMs);
            QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
        };
        QByteArray body;
        QString error;
        get(Small, 100000, 5000, &body, &error);
        QCOMPARE(body, QByteArray("hello"));
        QVERIFY(error.isEmpty());
        get(Declared, 100000, 5000, &body, &error);   // says it is 3 MB: stopped as soon as it says so
        QVERIFY(body.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("too large")), qPrintable(error));
        get(Endless, 100000, 5000, &body, &error);    // never says, never ends: stopped when it passes the limit
        QVERIFY(body.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("too large")), qPrintable(error));
        get(Stalled, 100000, 300, &body, &error);     // sends a little and waits: stopped by the overall time
        QVERIFY(body.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("too long")), qPrintable(error));
        get(Elsewhere, 100000, 5000, &body, &error);  // sends the download to a file on this computer: not followed
        QVERIFY(body.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    // "C++" searched for "C" (a plus sign was sent as it is, and a server reads
    // it as a space); and a literal "%20" was read as a space.
    void onlinePicturesSearchWordsAreEncoded()
    {
        // What a server reads back from the address: "+" is a space, and %XX a byte.
        auto serverReads = [](const QUrl &u, const QString &key) {
            for (const QString &pair : QString::fromLatin1(u.toEncoded()).section(QLatin1Char('?'), 1).split(QLatin1Char('&')))
                if (pair.startsWith(key + QLatin1Char('='))) return QUrl::fromPercentEncoding(pair.mid(key.size() + 1).replace(QLatin1Char('+'), QLatin1Char(' ')).toLatin1());
            return QString();
        };
        for (const QString &words : {QStringLiteral("C++"), QStringLiteral("a&b=c#d+e%20f"), QStringLiteral("100% fun"), QStringLiteral("caf\u00e9 \"red barn\""), QStringLiteral("red barn")}) {
            QCOMPARE(serverReads(jp::online::searchUrl(jp::online::Openverse, words), QStringLiteral("q")), words);
            QCOMPARE(serverReads(jp::online::searchUrl(jp::online::Commons, words), QStringLiteral("gsrsearch")), words + QStringLiteral(" filetype:bitmap"));
        }
        QVERIFY(jp::online::searchUrl(jp::online::Openverse, QStringLiteral("red barn")).toString(QUrl::FullyEncoded).contains(QStringLiteral("q=red%20barn")));
    }

    // A picture that cannot be loaded (too large, too slow, cut short, or not a
    // picture) says so in the pane; it stayed at "Downloading...", and the
    // chosen picture of the person's own was not touched.
    void onlinePicturesSayWhenAPictureFails()
    {
        const QByteArray openverse = QJsonDocument(QJsonObject{{"results", QJsonArray{QJsonObject{{"title", "Red barn"}, {"creator", "Pat"}, {"license", "by"}, {"license_version", "2.0"},
                                                                                                   {"url", "https://example.org/barn.bmp"}, {"thumbnail", "https://example.org/barn.thumb"}}}}}).toJson();
        QByteArray cut("\x89PNG\r\n\x1a\n", 8);   // sniffs as a PNG, but is cut short
        QString pictureError;
        const auto keep = jp::online::fetch;
        const auto restore = qScopeGuard([&] { jp::online::fetch = keep; });
        jp::online::fetch = [&](const QUrl &u, QObject *, const jp::online::Done &done, qint64) {
            if (u.host() == QLatin1String("api.openverse.org")) done(openverse, QString());
            else if (u.path().endsWith(QLatin1String(".bmp"))) done(pictureError.isEmpty() ? cut : QByteArray(), pictureError);
            else done(QByteArray(), QStringLiteral("no thumbnail"));
        };
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        QImage green(40, 40, QImage::Format_RGB32);
        green.fill(Qt::green);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        green.save(&buf, "PNG");
        auto mine = std::make_shared<jp::PictureItem>();
        mine->imageId = ed->doc()->addImage(png, "png");
        mine->rect = QRectF(72, 72, 100, 100);
        mine->imgRect = QRectF(0, 0, 100, 100);
        mine->altText = QStringLiteral("My family photo");
        ed->addItem(mine);
        w.showTaskPane(QStringLiteral("online"));
        auto *pane = w.findChild<jp::OnlinePicturesPane *>();
        QVERIFY(pane);
        auto shown = [&] {
            QStringList s;
            for (auto *l : pane->findChildren<QLabel *>()) s << l->text();
            return s.join(QLatin1Char('\n'));
        };
        pane->search(QStringLiteral("barn"));
        QCOMPARE(pane->results()->count(), 1);
        const size_t before = ed->surfaceItems().size();
        pane->insertButton()->click();   // cut short
        QVERIFY2(shown().contains(QStringLiteral("couldn't be loaded")), qPrintable(shown()));
        QVERIFY(!shown().contains(QStringLiteral("Downloading")));
        QCOMPARE(ed->surfaceItems().size(), before);
        QCOMPARE(mine->altText, QStringLiteral("My family photo"));
        QVERIFY(mine->caption.isEmpty());
        QVERIFY(pane->insertButton()->isEnabled());
        pictureError = QStringLiteral("It is too large.");
        pane->insertButton()->click();
        QVERIFY2(shown().contains(QStringLiteral("couldn't be downloaded: It is too large.")), qPrintable(shown()));
        QVERIFY(!shown().contains(QStringLiteral("Downloading")));
    }

    // Save as Building Block's blocks show in Insert > Page Parts, under My
    // Building Blocks, and insert from there (they were saved where no
    // gallery looked).
    void savedBuildingBlocksInsert()
    {
        QStandardPaths::setTestModeEnabled(true);   // a test folder, not the real one
        QDir(jp::userBlocksDir()).removeRecursively();
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 200, 60), QStringLiteral("Block text")));
        ed->addItem(box);
        QTimer::singleShot(0, [] {
            if (auto *d = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                d->setTextValue(QStringLiteral("Pull quote"));
                d->accept();
            }
        });
        w.act(QStringLiteral("obj.saveBlock"))->trigger();
        QVERIFY(QFile::exists(QDir(jp::userBlocksDir()).filePath(QStringLiteral("Pull quote.json"))));
        jp::GalleryButton *pageParts = nullptr;
        for (auto *g : w.findChildren<jp::GalleryButton *>())
            if (g->text() == QLatin1String("Page Parts")) pageParts = g;
        QVERIFY(pageParts);
        QString id;
        for (const auto &it : pageParts->items())
            if (it.tip == QLatin1String("Pull quote") && it.group == QLatin1String("My Building Blocks")) id = it.id;
        QVERIFY(!id.isEmpty());
        const int before = int(ed->surfaceItems().size());
        Q_EMIT pageParts->activated(id);
        QCOMPARE(int(ed->surfaceItems().size()), before + 1);
        auto *made = dynamic_cast<jp::TextItem *>(ed->single());
        QVERIFY(made && made->id != box->id);
        QCOMPARE(ed->doc()->storyDoc(made->storyId)->toPlainText(), QStringLiteral("Block text"));
        QDir(jp::userBlocksDir()).removeRecursively();
        QStandardPaths::setTestModeEnabled(false);
    }

    // The Master Page tab as the other program has it: Show Header/Footer
    // goes to the master's header, then its footer; Insert Date and Insert
    // Time (Alt+Shift+D, Alt+Shift+T) add fields; the Mailings tab hides.
    // Select Recipients also offers contacts (vCard files). In master view the
    // Master Page tab comes first, right after File, and is the one showing;
    // the other contextual tabs stay where they were, and the KeyTips stay
    // unique.
    void masterPageTabAsTheOtherProgram()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        jp::Ribbon *r = w.ribbon();
        auto visible = [r](const QString &name) {
            for (int i = 0; i < r->tabCount(); ++i)
                if (r->tabName(i) == name) return r->tabVisible(i);
            return false;
        };
        auto shown = [r] {
            QStringList names;
            for (int i = 0; i < r->tabCount(); ++i)
                if (r->tabVisible(i)) names << r->tabName(i);
            return names;
        };
        const QStringList onThePage = shown();
        QCOMPARE(onThePage, (QStringList{"Home", "Insert", "Page Design", "Mailings", "Review", "View", "Help"}));
        QVERIFY(visible(QStringLiteral("Mailings")));
        w.act(QStringLiteral("mp.showHeaderFooter"))->trigger();
        QVERIFY(!ed->masterView().isEmpty());
        QVERIFY(visible(QStringLiteral("Master Page")));
        QVERIFY(!visible(QStringLiteral("Mailings")));
        QCOMPARE(shown(), (QStringList{"Master Page", "Home", "Insert", "Page Design", "Review", "View", "Help", "Text Box"}));
        QCOMPARE(r->current(), r->tab(QStringLiteral("Master Page")));
        // Screen readers go through the tab buttons in the order the parent holds them: the order shown.
        int last = -1;
        for (int i = 0; i < r->tabCount(); ++i) {
            if (!r->tabVisible(i)) continue;
            QWidget *b = r->tabButton(i);
            const int at = b->parentWidget()->children().indexOf(b);
            QVERIFY2(at > last, qPrintable(r->tabName(i)));
            last = at;
        }
        // Their KeyTips are still all different (Master Page is JM), and the first tab is the one reached by it.
        QStringList keys{r->fileKeytip()};
        for (int i = 0; i < r->tabCount(); ++i)
            if (r->tabVisible(i)) keys << r->tabKeytip(i);
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()).size(), keys.size());
        QCOMPARE(keys.mid(0, 3), (QStringList{"F", "JM", "H"}));
        QVERIFY(ed->isEditingText());
        const QString header = ed->textTarget().itemId;
        QCOMPARE(ed->doc()->item(header)->name, QStringLiteral("Header"));
        w.act(QStringLiteral("ins.date"))->trigger();
        w.act(QStringLiteral("ins.time"))->trigger();
        QStringList fields;
        for (QTextBlock b = ed->editDoc()->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
                if (const QString f = it.fragment().charFormat().stringProperty(jp::tp::Field); !f.isEmpty()) fields << f;
        QCOMPARE(fields, (QStringList{QStringLiteral("date"), QStringLiteral("time")}));
        w.act(QStringLiteral("mp.showHeaderFooter"))->trigger();
        QVERIFY(ed->isEditingText());
        QCOMPARE(ed->doc()->item(ed->textTarget().itemId)->name, QStringLiteral("Footer"));
        w.act(QStringLiteral("mp.showHeaderFooter"))->trigger();
        QCOMPARE(ed->textTarget().itemId, header);
        QCOMPARE(w.act(QStringLiteral("ins.date"))->shortcut(), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_D));
        w.act(QStringLiteral("mp.close"))->trigger();
        QVERIFY(visible(QStringLiteral("Mailings")));
        QVERIFY(w.act(QStringLiteral("mm.contacts")));
        // Closed, the tab goes and the others are as they were, Home showing.
        QCOMPARE(shown(), onThePage);
        QCOMPARE(r->current(), r->tab(QStringLiteral("Home")));
        // From another tab, opening master view (Ctrl+M) shows the Master Page tab; a tab picked afterward stays, even as the selection brings other tabs.
        r->showTab(r->tab(QStringLiteral("View")));
        w.act(QStringLiteral("view.master"))->trigger();
        QCOMPARE(shown(), (QStringList{"Master Page", "Home", "Insert", "Page Design", "Review", "View", "Help"}));
        QCOMPARE(r->current(), r->tab(QStringLiteral("Master Page")));
        r->showTab(r->tab(QStringLiteral("Insert")));
        auto note = ed->newTextBox(QRectF(72, 72, 200, 100), QStringLiteral("x"));
        ed->addItem(note);
        ed->select(QStringList{note->id});
        QCOMPARE(shown().first(), QStringLiteral("Master Page"));
        QVERIFY(visible(QStringLiteral("Text Box")));
        QCOMPARE(r->current(), r->tab(QStringLiteral("Insert")));
        w.act(QStringLiteral("view.master"))->trigger();
        QCOMPARE(shown(), onThePage);
        // The contextual tabs that come with the selection keep their place, after Help.
        auto box = ed->newTextBox(QRectF(72, 72, 200, 100), QStringLiteral("y"));
        ed->addItem(box);
        ed->select(QStringList{box->id});
        QCOMPARE(shown(), onThePage + QStringList{"Text Box"});
    }

    // Increase Indent on list items nests them a level (numbered a., b. under
    // 1., bulleted with a circle under a dot), and Decrease Indent brings them
    // back, continuing the outer numbers; saved and opened again the same.
    void multilevelLists()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 300, 200), QStringLiteral("one\ntwo\nthree\nfour")));
        ed->addItem(box);
        auto selectBlocks = [&](int from, int to) {
            ed->beginTextEdit(box->id);
            QTextCursor c(ed->editDoc()->findBlockByNumber(from));
            c.setPosition(ed->editDoc()->findBlockByNumber(to).position(), QTextCursor::KeepAnchor);
            ed->setCursor(c);
        };
        auto markers = [](QTextDocument *d) {
            QStringList m;
            for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) m << (b.textList() ? b.textList()->itemText(b) : QString());
            return m.join(QLatin1Char(' '));
        };
        selectBlocks(0, 3);
        ed->setList(2, 1);
        QCOMPARE(markers(ed->editDoc()), QStringLiteral("1. 2. 3. 4."));
        selectBlocks(1, 2);
        ed->changeIndent(1);
        QCOMPARE(markers(ed->editDoc()), QStringLiteral("1. a. b. 2."));
        selectBlocks(2, 2);
        ed->changeIndent(-1);
        QCOMPARE(markers(ed->editDoc()), QStringLiteral("1. a. 2. 3."));
        ed->endTextEdit();
        QTextDocument copy;
        jp::storyFromJson(&copy, jp::storyToJson(ed->doc()->storyDoc(box->storyId)));
        QCOMPARE(markers(&copy), QStringLiteral("1. a. 2. 3."));
        // Bullets: a circle a level in.
        selectBlocks(0, 3);
        ed->setList(1);
        selectBlocks(1, 1);
        ed->changeIndent(1);
        QCOMPARE(ed->editDoc()->findBlockByNumber(1).textList()->format().style(), QTextListFormat::ListCircle);
        ed->endTextEdit();
    }

    // A picture field shows each record's picture in the preview, printing,
    // PDF, and email (only merging to a new publication did).
    void pictureFieldsShowEachRecord()
    {
        QTemporaryDir dir;
        auto save = [&](const QString &name, const QColor &c) {
            QImage img(20, 20, QImage::Format_RGB32);
            img.fill(c);
            img.save(dir.filePath(name));
        };
        save(QStringLiteral("a.png"), Qt::red);
        save(QStringLiteral("b.png"), Qt::blue);
        auto doc = jp::Document::blank(QSizeF(200, 200));
        doc->merge.path = dir.filePath(QStringLiteral("list.csv"));
        doc->merge.fields = {QStringLiteral("Name"), QStringLiteral("Photo")};
        doc->merge.rows = {{QStringLiteral("Ann"), QStringLiteral("a.png")}, {QStringLiteral("Bo"), QStringLiteral("b.png")}};
        doc->merge.include = {true, true};
        auto pic = std::make_shared<jp::PictureItem>();
        pic->name = QStringLiteral("merge:Photo");
        pic->rect = QRectF(50, 50, 100, 100);
        doc->pages[0]->items.push_back(pic);
        auto centre = [&](int record) {
            jp::LayoutCache cache;
            jp::PaintContext ctx;
            ctx.doc = doc.get();
            ctx.cache = &cache;
            ctx.opt.output = true;
            ctx.opt.mergeRecord = record;
            QImage img(200, 200, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            jp::Renderer::paintPage(&p, ctx, 0);
            p.end();
            return img.pixelColor(100, 100);
        };
        QCOMPARE(centre(0), QColor(Qt::red));
        QCOMPARE(centre(1), QColor(Qt::blue));
        QCOMPARE(centre(-1), QColor(Qt::white));   // field codes shown: no record's picture
    }

    // A run with no letter spacing of its own takes its paragraph style's
    // (a sign's headline style tightens its letters 87.5% and 3 points; they
    // came in at normal spacing, 8% wider than Publisher drew them).
    void styleLetterSpacingFromPub()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        jp::TextStyle st;
        st.name = QStringLiteral("Tight Headline");
        st.chr.setProperty(jp::tp::Tracking, 87.5);
        st.chr.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        st.chr.setFontLetterSpacing(-3);
        doc->styles << st;
        auto box = std::make_shared<jp::TextItem>();
        box->rect = QRectF(72, 72, 400, 100);
        box->storyId = doc->createStory();
        QTextDocument *sd = doc->storyDoc(box->storyId);
        QTextCursor c(sd);
        QTextBlockFormat bf;
        bf.setProperty(jp::tp::StyleName, st.name);
        c.setBlockFormat(bf);
        QTextCharFormat cf;
        cf.setFontPointSize(40);
        c.insertText(QStringLiteral("Help Wanted"), cf);   // no spacing of its own
        doc->pages[0]->items.push_back(box);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("tight.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QTextDocument *story = nullptr;
        for (auto it = back->stories.cbegin(); it != back->stories.cend(); ++it)
            if ((*it)->doc->toPlainText() == QLatin1String("Help Wanted")) story = (*it)->doc.get();
        QVERIFY(story);
        QTextCursor r(story);
        r.movePosition(QTextCursor::NextCharacter);
        QCOMPARE(jp::tp::trackingOf(r.charFormat()), 87.5);
        QCOMPARE(jp::tp::kerningOf(r.charFormat()), -3.0);
    }

    // Tracking is measured in the original font's average letter width
    // (Publisher's PDFs: 125% on Gill Sans MT Ext Condensed Bold adds 0.056
    // em a letter, 87.5% on Century Schoolbook Bold takes 0.066); a squeezed
    // stand-in's own average spread the letters twice as far.
    void trackingUsesTheOriginalFontsAverage()
    {
        QVERIFY(std::abs(jp::originalAverageCharEm(QStringLiteral("Arial"), false, false) - 904.0 / 2048) < 1e-4);   // as the old table had it
        const double ext = jp::originalAverageCharEm(QStringLiteral("Gill Sans MT Ext Condensed Bold"), true, false);   // one face only
        QVERIFY2(std::abs(0.25 * ext - 0.056) < 0.003, qPrintable(QString::number(ext)));
        const double csb = jp::originalAverageCharEm(QStringLiteral("Century Schoolbook"), true, false);
        QVERIFY2(std::abs(0.125 * csb - 0.066) < 0.002, qPrintable(QString::number(csb)));
        QCOMPARE(jp::originalAverageCharEm(QStringLiteral("No Such Font"), false, false), -1.0);
    }

    // A missing stock font's letters take the original's widths (measured
    // in the reference VM), not its stand-in's, so lines break where they
    // break in Publisher (Gill Sans MT's "W" is 1.04 em; its stand-in's
    // is its own).
    void missingFontsKeepTheirLetterWidths()
    {
        const QString fam = QStringLiteral("Gill Sans MT");
        if (QFontDatabase::hasFamily(fam)) QSKIP("Gill Sans MT is installed here");
        const QHash<char16_t, double> *widths = jp::originalLetterWidths(fam, false, false);
        QVERIFY(widths);
        QVERIFY(std::abs(widths->value(u'W') - 1.042) < 0.001);
        QVERIFY(jp::originalLetterWidths(fam, true, false) != widths);
        QVERIFY(!jp::originalLetterWidths(QStringLiteral("No Such Font"), false, false));
        QTextDocument doc;
        QTextCursor c(&doc);
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{fam});
        cf.setFontPointSize(12);   // below automatic kerning
        const QString text = QStringLiteral("Wilhelmina 2047 ");
        c.insertText(text, cf);
        jp::FrameSpec fs;
        fs.size = QSizeF(1000, 100);
        fs.insets = QMarginsF(0, 0, 0, 0);
        fs.hyphenate = false;
        jp::StoryLayout lay;
        lay.build(&doc, {fs}, jp::LayoutEnv());
        auto x = [&](int pos) {
            int frame = -1;
            QRectF r;
            lay.caretRect(pos, &frame, &r);
            return r.x();
        };
        // Each letter is centered in the original's width, so words (and
        // the spaces after them) start and end where the original's do.
        double want = 0;
        for (int i = 0; i < text.size(); ++i) {
            want += widths->value(text[i].unicode()) * 12;
            if (i + 1 < text.size() && text[i + 1] != QLatin1Char(' ')) continue;
            const double got = x(i + 1) - x(0);
            QVERIFY2(std::abs(got - want) < 0.015, qPrintable(QStringLiteral("after '%1': %2, Gill Sans MT %3").arg(text[i]).arg(got).arg(want)));
        }
    }

    // From 14 pt up, a missing stock font's letters kern by the original's
    // pairs: Gill Sans MT Bold's "Yo" closes up by 0.14 em, as in a banner
    // of Publisher's. Smaller, they don't kern.
    void missingFontsKernAsTheOriginal()
    {
        const QString fam = QStringLiteral("Gill Sans MT");
        if (QFontDatabase::hasFamily(fam)) QSKIP("Gill Sans MT is installed here");
        const QHash<char16_t, double> *widths = jp::originalLetterWidths(fam, true, false);
        const QHash<quint32, double> *kerning = jp::originalKerning(fam, true, false);
        QVERIFY(widths && kerning);
        const double yo = kerning->value(quint32(u'Y') << 16 | u'o');
        QVERIFY2(std::abs(yo + 0.140) < 0.001, qPrintable(QString::number(yo)));
        for (double size : {100.0, 12.0}) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{fam});
            cf.setFontWeight(QFont::Bold);
            cf.setFontPointSize(size);
            c.insertText(QStringLiteral("Yo you"), cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(2000, 400);
            fs.insets = QMarginsF(0, 0, 0, 0);
            fs.hyphenate = false;
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            auto x = [&](int pos) {
                int frame = -1;
                QRectF r;
                lay.caretRect(pos, &frame, &r);
                return r.x();
            };
            const double want = (widths->value(u'Y') + widths->value(u'o') + (size >= 14 ? yo : 0)) * size;
            QVERIFY2(std::abs(x(2) - x(0) - want) < 0.01 * size / 12, qPrintable(QStringLiteral("%1 pt: %2, not %3").arg(size).arg(x(2) - x(0)).arg(want)));
        }
    }

    // A missing stock font's run is cut into pieces of their own letter
    // spacing, and a cut never parts characters that belong together: an
    // emoji's two UTF-16 halves (drawn as two replacement diamonds), a letter
    // and its combining accent (drawn beside it), or the letters of a joined
    // script (the last drawn in its isolated form). Only two characters the
    // original's table measures are ever cut apart.
    void letterWidthPiecesKeepCharactersTogether()
    {
        const QString fam = QStringLiteral("Gill Sans MT");
        if (QFontDatabase::hasFamily(fam)) QSKIP("Gill Sans MT is installed here");
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{fam});
        cf.setFontPointSize(24);
        const QTextCharFormat rf = jp::resolveCharFormat(cf, jp::LayoutEnv());
        // Where the second and later pieces of a run of `text` start.
        auto cuts = [&](const QString &text) {
            QStringList at;
            for (const auto &r : jp::applyLetterWidths({QTextLayout::FormatRange{0, int(text.size()), rf}}, text))
                if (r.start > 0) at << QString::number(r.start);
            return at;
        };
        auto cutAt = [&](const QString &text, int pos) { return cuts(text).contains(QString::number(pos)); };
        // Plain letters do get a piece each, or nearly, so the rest proves something.
        QVERIFY2(cuts(QStringLiteral("abcdefghij")).size() >= 3, qPrintable(cuts(QStringLiteral("abcdefghij")).join(',')));
        // "ab", an emoji (two units), "cd".
        const QString emoji = QStringLiteral("ab\U0001F600cd");
        for (int pos : {2, 3, 4}) QVERIFY2(!cutAt(emoji, pos), qPrintable(QStringLiteral("cut at %1: %2").arg(pos).arg(cuts(emoji).join(','))));
        // "re" and an accent, "sume" and an accent (decomposed).
        const QString accents = QStringLiteral("re\u0301sume\u0301");
        for (int pos : {2, 3, 7}) QVERIFY2(!cutAt(accents, pos), qPrintable(QStringLiteral("cut at %1: %2").arg(pos).arg(cuts(accents).join(','))));
        // Four Arabic letters and a digit: nothing but the digit is measured.
        const QString arabic = QStringLiteral("\u0645\u0643\u062A\u0628" "1");
        QVERIFY2(cuts(arabic).isEmpty(), qPrintable(cuts(arabic).join(',')));
        // A joiner, a non-joiner, and a variation selector hold letters together.
        for (const QString &s : {QStringLiteral("a\u200Db"), QStringLiteral("a\u200Cb"), QStringLiteral("a\uFE0Fb")})
            QVERIFY2(cuts(s).isEmpty(), qPrintable(cuts(s).join(',')));
    }

    // One run of true small capitals in a font mustn't change how that font's
    // other runs are measured: the stand-in's advances were remembered by font
    // alone, so every normal run after it was set by the small capitals'
    // narrower letters (Impact: "banana band nab" came to 601 px for 475).
    void smallCapitalsLeaveLaterRunsLetterWidths()
    {
        const QString fam = QStringLiteral("Rockwell Extra Bold");   // used by no other test, so this one measures it first
        if (QFontDatabase::hasFamily(fam)) QSKIP("Rockwell Extra Bold is installed here");
        const QHash<char16_t, double> *widths = jp::originalLetterWidths(fam, false, false);
        QVERIFY(widths);
        const QString text = QStringLiteral("banana band nab ");
        for (bool smallCaps : {true, false}) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{fam});
            cf.setFontPointSize(12);
            if (smallCaps) {
                cf.setFontCapitalization(QFont::SmallCaps);
                cf.setProperty(jp::tp::TrueSmallCaps, true);
            }
            c.insertText(text, cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(1000, 100);
            fs.insets = QMarginsF(0, 0, 0, 0);
            fs.hyphenate = false;
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            if (smallCaps) continue;   // laid out first only so that it is measured first
            auto x = [&](int pos) {
                int frame = -1;
                QRectF r;
                lay.caretRect(pos, &frame, &r);
                return r.x();
            };
            double want = 0;
            for (int i = 0; i < text.size(); ++i) {
                want += widths->value(text[i].unicode()) * 12;
                if (i + 1 < text.size() && text[i + 1] != QLatin1Char(' ')) continue;
                const double got = x(i + 1) - x(0);
                QVERIFY2(std::abs(got - want) < 0.1, qPrintable(QStringLiteral("after '%1': %2, Rockwell Extra Bold %3").arg(text[i]).arg(got).arg(want)));
            }
        }
    }

    // Book Antiqua's lines are as Publisher spaces them, by the font's
    // Windows metrics (1.2427 em; bold 1.2056, its extra space above the
    // letters): a garage sale sign's two best-fit lines filled the box
    // exactly there, and came out 11% closer here (typo metrics, 1.07 em).
    void bookAntiquaLineSpacing()
    {
        for (const auto &[bold, line, base] : {std::tuple{false, 1.2427, 0.9604}, std::tuple{true, 1.2056, 0.9404}}) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{QStringLiteral("Book Antiqua")});
            cf.setFontPointSize(100);
            if (bold) cf.setFontWeight(QFont::Bold);
            c.insertText(QStringLiteral("Garage"), cf);
            c.insertBlock();
            c.insertText(QStringLiteral("Sale"), cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(600, 400);
            fs.insets = QMarginsF(0, 0, 0, 0);
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            const auto lines = lay.lineInfo(0);
            QCOMPARE(lines.size(), 2);
            QVERIFY2(std::abs(lines[1].baseline - lines[0].baseline - line * 100) < 0.05, qPrintable(QString::number(lines[1].baseline - lines[0].baseline)));
            QVERIFY2(std::abs(lines[0].baseline - base * 100) < 0.05, qPrintable(QString::number(lines[0].baseline)));
        }
    }

    // An octagon with no proportion of its own is Publisher's regular one,
    // its corners cut at 29.3% of each side (6326 of 21600, as its PDFs
    // draw a stop sign); libmspub had 5000, a squatter octagon.
    void octagonAsPublisherDrawsIt()
    {
        const libmspub::CustomShape *octagon = libmspub::getCustomShape(libmspub::OCTAGON);
        QVERIFY(octagon && octagon->m_numDefaultAdjustValues > 0);
        QCOMPARE(octagon->mp_defaultAdjustValues[0], 6326);
    }

    // A centered line is centered on its letters, a right-aligned one ends
    // at its last letter: Publisher lets the spaces a line ends with hang
    // past them. Qt counted them whenever they fit, half a space off.
    void centeredLinesHangTrailingSpaces()
    {
        auto layout = [](Qt::Alignment al, double *left, double *right) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextBlockFormat bf;
            bf.setAlignment(al);
            c.setBlockFormat(bf);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{QStringLiteral("Arial")});
            cf.setFontPointSize(24);
            c.insertText(QStringLiteral("Heading   "), cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(300, 100);
            fs.insets = QMarginsF(0, 0, 0, 0);
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            int frame = -1;
            QRectF r;
            lay.caretRect(0, &frame, &r);
            *left = r.x();
            lay.caretRect(7, &frame, &r);
            *right = r.x();
        };
        double l = 0, r = 0;
        layout(Qt::AlignHCenter, &l, &r);
        QVERIFY2(std::abs(l - (300 - r)) < 0.01, qPrintable(QStringLiteral("%1 .. %2").arg(l).arg(r)));
        layout(Qt::AlignRight, &l, &r);
        QVERIFY2(std::abs(r - 300) < 0.01, qPrintable(QString::number(r)));
        layout(Qt::AlignLeft, &l, &r);
        QVERIFY(std::abs(l) < 0.01);
    }

    // The same in a right-to-left paragraph, where the spaces a line ends with
    // sit on the left of its letters: aligned to the end (the left), the
    // letters start at the margin and the spaces hang outside it; centered,
    // the letters are centered; aligned to the start, they end at the right
    // margin as they did. The spaces had pushed a line aligned to the end
    // 33 points in.
    void rightToLeftLinesHangTrailingSpaces()
    {
        // Where the Hebrew word's letters lie: x of the caret before the
        // spaces (the letters' left end), and of the one before the letters.
        auto layout = [](Qt::Alignment al, double *left, double *right) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextBlockFormat bf;
            bf.setLayoutDirection(Qt::RightToLeft);
            bf.setAlignment(al);
            c.setBlockFormat(bf);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{QStringLiteral("Arial")});
            cf.setFontPointSize(24);
            c.insertText(QStringLiteral("\u05E9\u05DC\u05D5\u05DD     "), cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(300, 100);
            fs.insets = QMarginsF(0, 0, 0, 0);
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            int frame = -1;
            QRectF r;
            lay.caretRect(4, &frame, &r);
            *left = r.x();
            lay.caretRect(0, &frame, &r);
            *right = r.x();
        };
        double l = 0, r = 0;
        layout(Qt::AlignRight, &l, &r);   // the end of a right-to-left paragraph
        QVERIFY2(std::abs(l) < 0.01, qPrintable(QStringLiteral("%1 .. %2").arg(l).arg(r)));
        QVERIFY(r > 20 && r < 150);
        layout(Qt::AlignHCenter, &l, &r);
        QVERIFY2(std::abs(l - (300 - r)) < 0.01, qPrintable(QStringLiteral("%1 .. %2").arg(l).arg(r)));
        layout(Qt::AlignLeft, &l, &r);    // the start
        QVERIFY2(std::abs(r - 300) < 0.01, qPrintable(QStringLiteral("%1 .. %2").arg(l).arg(r)));
    }

    // Wave underlines, and the double and thick ones, are drawn under Hebrew
    // and Arabic text too: a right-to-left run's two ends came the wrong way
    // round, so the wave had no length and nothing was drawn.
    void waveUnderlinesDrawUnderRightToLeftText()
    {
        // How many pixels below the baseline are not white: only the underline gets there.
        auto inkBelow = [](const QString &text, Qt::LayoutDirection dir, QTextCharFormat::UnderlineStyle style, int kind) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextBlockFormat bf;
            bf.setLayoutDirection(dir);
            c.setBlockFormat(bf);
            QTextCharFormat cf;
            cf.setFontFamilies(QStringList{QStringLiteral("Arial")});
            cf.setFontPointSize(24);
            if (style != QTextCharFormat::NoUnderline) {
                cf.setUnderlineStyle(style);
                cf.setProperty(jp::tp::UnderlineKind, kind);
            }
            c.insertText(text, cf);
            jp::FrameSpec fs;
            fs.size = QSizeF(200, 60);
            fs.insets = QMarginsF(0, 0, 0, 0);
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            QImage img(800, 240, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.scale(4, 4);
            lay.paint(&p, 0, jp::PaintOptions());
            p.end();
            int ink = 0;
            for (int y = int((lay.lineInfo(0).first().baseline + 1) * 4); y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qGray(img.pixel(x, y)) < 200) ++ink;
            return ink;
        };
        const QString hebrew = QStringLiteral("\u05E9\u05DC\u05D5\u05DD"), latin = QStringLiteral("wave");
        for (Qt::LayoutDirection dir : {Qt::LeftToRight, Qt::RightToLeft}) {
            QCOMPARE(inkBelow(hebrew, dir, QTextCharFormat::NoUnderline, 0), 0);   // nothing else reaches below the baseline
            for (int kind : {1, 2, 4}) {
                const int latinInk = inkBelow(latin, dir, QTextCharFormat::WaveUnderline, kind);
                QVERIFY(latinInk > 100);
                const int hebrewInk = inkBelow(hebrew, dir, QTextCharFormat::WaveUnderline, kind);
                QVERIFY2(hebrewInk > 100, qPrintable(QStringLiteral("kind %1, direction %2: %3 pixels").arg(kind).arg(int(dir)).arg(hebrewInk)));
            }
        }
    }

    // A dashed underline is one line under the pieces a missing font's run is
    // cut into, its dashes even from end to end: the pattern started over in
    // each piece ("=====" on Impact came out ragged).
    void dashedUnderlinesRunOnAcrossPieces()
    {
        const QString fam = QStringLiteral("Gill Sans MT");
        if (QFontDatabase::hasFamily(fam)) QSKIP("Gill Sans MT is installed here");
        QTextDocument doc;
        QTextCursor c(&doc);
        QTextCharFormat cf;
        cf.setFontFamilies(QStringList{fam});
        cf.setFontPointSize(24);
        cf.setUnderlineStyle(QTextCharFormat::DashUnderline);
        cf.setProperty(jp::tp::UnderlineKind, 2);
        c.insertText(QStringLiteral("Wilhelminas"), cf);
        jp::FrameSpec fs;
        fs.size = QSizeF(400, 60);
        fs.insets = QMarginsF(0, 0, 0, 0);
        fs.hyphenate = false;
        jp::StoryLayout lay;
        lay.build(&doc, {fs}, jp::LayoutEnv());
        QImage img(3200, 480, QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        p.scale(8, 8);
        lay.paint(&p, 0, jp::PaintOptions());
        p.end();
        // The row under the baseline with the most ink is the underline.
        int row = -1, best = 0;
        for (int y = int((lay.lineInfo(0).first().baseline + 1) * 8); y < img.height(); ++y) {
            int ink = 0;
            for (int x = 0; x < img.width(); ++x) ink += qGray(img.pixel(x, y)) < 128;
            if (ink > best) { best = ink; row = y; }
        }
        QVERIFY(row > 0);
        // The dashes along it, their lengths and the gaps between.
        QVector<int> dash, gap;
        int runStart = -1, lastEnd = -1;
        for (int x = 0; x <= img.width(); ++x) {
            const bool on = x < img.width() && qGray(img.pixel(x, row)) < 128;
            if (on && runStart < 0) {
                runStart = x;
                if (lastEnd >= 0) gap << x - lastEnd;
            } else if (!on && runStart >= 0) {
                dash << x - runStart;
                lastEnd = x;
                runStart = -1;
            }
        }
        QVERIFY2(dash.size() >= 8, qPrintable(QString::number(dash.size())));
        // (The first and last dashes may be cut short by the line's ends.)
        const auto [dashLo, dashHi] = std::minmax_element(dash.cbegin() + 1, dash.cend() - 1);
        QVERIFY2(*dashHi - *dashLo <= 2, qPrintable(QStringLiteral("dashes %1..%2 px").arg(*dashLo).arg(*dashHi)));
        const auto [gapLo, gapHi] = std::minmax_element(gap.cbegin(), gap.cend());
        QVERIFY2(*gapHi - *gapLo <= 2, qPrintable(QStringLiteral("gaps %1..%2 px").arg(*gapLo).arg(*gapHi)));
    }

    // A line break (Shift+Enter) goes into a .pub as \n, as Publisher's own
    // files hold it; JeffPub wrote \v, which Publisher 2021 draws as a box
    // ("BASIC INCIDENT□COMMAND" on a cover). Opened again, it is a line break.
    void lineBreaksSavedAsPublishersOwn()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto box = std::make_shared<jp::TextItem>();
        box->rect = QRectF(72, 72, 300, 100);
        box->storyId = doc->createStory();
        QTextCursor(doc->storyDoc(box->storyId)).insertText(QStringLiteral("Basic Incident") + QChar(QChar::LineSeparator) + QStringLiteral("Command System"));
        doc->pages[0]->items.push_back(box);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("breaks.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        const QByteArray quill = jp::cfb::readStream(path, QStringLiteral("Quill/QuillSub/CONTENTS"), &err);
        QVERIFY2(!quill.isEmpty(), qPrintable(err));
        auto utf16 = [](const QString &s) { return QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2); };
        QVERIFY(quill.contains(utf16(QStringLiteral("Incident\nCommand"))));
        QVERIFY(!quill.contains(utf16(QStringLiteral("Incident\vCommand"))));
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        bool found = false;   // (toRawText: toPlainText shows a line break as \n)
        for (auto it = back->stories.cbegin(); it != back->stories.cend(); ++it)
            found |= (*it)->doc->toRawText().contains(QStringLiteral("Incident") + QChar(QChar::LineSeparator) + QStringLiteral("Command"));
        QVERIFY(found);
    }

    // Line spacing in points with neither flag bit set (12 pt as 1219200,
    // eighths of an EMU) is Publisher's too: a booklet's list double-spaced
    // in Publisher came in at its style's 1.19 lines, the value dropped.
    void unflaggedLineSpacingInPoints()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto box = std::make_shared<jp::TextItem>();
        box->rect = QRectF(72, 72, 300, 100);
        box->storyId = doc->createStory();
        QTextCursor c(doc->storyDoc(box->storyId));
        QTextBlockFormat bf;
        bf.setLineHeight(12, QTextBlockFormat::FixedHeight);
        c.setBlockFormat(bf);
        c.insertText(QStringLiteral("Purpose/Essential Task:"));
        doc->pages[0]->items.push_back(box);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("spacing.pub"));
        QString err;
        QVERIFY2(jp::exportPublisher(*doc, path, &err), qPrintable(err));
        // JeffPub writes the points kind (1219200 | 1); make it the unflagged kind.
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        jp::cfb::File file;
        QVERIFY(jp::cfb::read(f.readAll(), &file, &err));
        f.close();
        QByteArray quill = file.stream(QStringLiteral("Quill/QuillSub/CONTENTS"));
        QByteArray flagged("\x34\x22", 2), plain = flagged;
        for (quint32 v : {1219201u, 1219200u}) {
            QByteArray &b = v == 1219201u ? flagged : plain;
            for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff));
        }
        QVERIFY(quill.contains(flagged));
        quill.replace(flagged, plain);
        QVERIFY(file.setStream(QStringLiteral("Quill/QuillSub/CONTENTS"), quill));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(jp::cfb::write(file));
        out.close();
        auto back = jp::importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        bool found = false;
        for (auto it = back->stories.cbegin(); it != back->stories.cend(); ++it) {
            const QTextBlock b = (*it)->doc->begin();
            if (!b.text().startsWith(QLatin1String("Purpose"))) continue;
            found = true;
            QCOMPARE(b.blockFormat().lineHeightType(), int(QTextBlockFormat::FixedHeight));
            QCOMPARE(b.blockFormat().lineHeight(), 12.0);
        }
        QVERIFY(found);
    }

    // Save as E-book can make a fixed-layout EPUB: each page as designed (an
    // SVG drawing) at the page's size, its words underneath in reading order,
    // the contents from Heading 1-3 at the pages they're on.
    void fixedLayoutEbook()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        ed->insertPages(0, 1, false, false);
        auto heading = std::make_shared<jp::TextItem>();
        heading->rect = QRectF(72, 72, 400, 60);
        heading->storyId = ed->doc()->createStory();
        QTextCursor c(ed->doc()->storyDoc(heading->storyId));
        QTextBlockFormat hf;
        hf.setProperty(jp::tp::StyleName, QStringLiteral("Heading 1"));
        c.setBlockFormat(hf);
        c.insertText(QStringLiteral("Chapter One"));
        c.insertBlock(QTextBlockFormat());
        c.insertText(QStringLiteral("The story begins."));
        ed->doc()->pages[0]->items.push_back(heading);
        auto second = std::make_shared<jp::TextItem>();
        second->rect = QRectF(72, 72, 400, 60);
        second->storyId = ed->doc()->createStory();
        QTextCursor(ed->doc()->storyDoc(second->storyId)).insertText(QStringLiteral("And it ends."));
        ed->doc()->pages[1]->items.push_back(second);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("book.epub"));
        QString err;
        QVERIFY2(w.exportFixedEpubTo(path, QStringLiteral("A Book"), QStringLiteral("Pat"), &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QMap<QString, QByteArray> zip;
        QVERIFY(jp::readZip(f.readAll(), zip, &err));
        const QString opf = QString::fromUtf8(zip.value(QStringLiteral("OEBPS/content.opf")));
        QVERIFY(opf.contains(QStringLiteral("<meta property=\"rendition:layout\">pre-paginated</meta>")));
        QVERIFY(opf.contains(QStringLiteral("<itemref idref=\"p1\"/>")) && opf.contains(QStringLiteral("<itemref idref=\"p2\"/>")));
        const QString p1 = QString::fromUtf8(zip.value(QStringLiteral("OEBPS/page1.xhtml")));
        QVERIFY(p1.contains(QStringLiteral("content=\"width=816, height=1056\"")));   // a letter page in CSS pixels
        QVERIFY(p1.contains(QStringLiteral("<p>Chapter One</p>")) && p1.contains(QStringLiteral("<p>The story begins.</p>")));
        QVERIFY(QString::fromUtf8(zip.value(QStringLiteral("OEBPS/page2.xhtml"))).contains(QStringLiteral("<p>And it ends.</p>")));
        QVERIFY(zip.value(QStringLiteral("OEBPS/pages/page1.svg")).startsWith("<?xml") || zip.value(QStringLiteral("OEBPS/pages/page1.svg")).contains("<svg"));
        const QString nav = QString::fromUtf8(zip.value(QStringLiteral("OEBPS/nav.xhtml")));
        QVERIFY2(nav.contains(QStringLiteral("<a href=\"page1.xhtml\">Chapter One</a>")), qPrintable(nav));
        QVERIFY(nav.contains(QStringLiteral("epub:type=\"page-list\"")));
        QCOMPARE(zip.value(QStringLiteral("mimetype")), QByteArray("application/epub+zip"));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) QFile::copy(path, qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/fixed.epub"));
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

    // Page Design > Change Template applies another design to the open
    // publication: each story goes into the new design's box of the same
    // role, the pictures replace its placeholder pictures in reading order,
    // and what has no place waits in Extra Content. One undo step undoes it.
    void changeTemplate()
    {
        auto png = [](const QColor &c) {
            QImage im(60, 40, QImage::Format_RGB32);
            im.fill(c);
            QByteArray bytes;
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            im.save(&buf, "PNG");
            return bytes;
        };
        auto textOf = [](const jp::Document &d, const jp::Item *it) {
            if (it->type() == jp::ItemType::TextArt) return static_cast<const jp::TextArtItem *>(it)->text;
            return d.storyDoc(static_cast<const jp::TextItem *>(it)->storyId)->toPlainText();
        };
        // The boxes, TextArt, and pictures of a role, in the order they were made.
        auto withRole = [](const jp::Document &d, const QString &role) {
            QVector<jp::Item *> out;
            for (const auto &pg : d.pages)
                for (const auto &it : pg->items)
                    if (it->role == role) out << it.get();
            return out;
        };
        auto typeOver = [](jp::Document &d, jp::Item *it, const QString &text) {
            QTextCursor c(d.storyDoc(static_cast<jp::TextItem *>(it)->storyId));
            c.select(QTextCursor::Document);
            c.insertText(text);
        };
        auto noImages = [](QJsonObject o) {   // pictures stay in the file after an undo, like any edit's
            o.remove(QStringLiteral("images"));
            return o;
        };

        // Every built-in design tags its text boxes, TextArt, and pictures.
        for (const jp::TemplateInfo &t : jp::templates()) {
            auto built = t.build(jp::TemplateOptions());
            for (const auto &pg : built->pages)
                for (const auto &it : pg->items)
                    if (it->type() == jp::ItemType::Text || it->type() == jp::ItemType::TextArt || it->type() == jp::ItemType::Picture)
                        QVERIFY2(!it->role.isEmpty(), qPrintable(t.id));
        }

        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        jp::Editor *ed = w.editor();
        auto first = jp::findTemplate(QStringLiteral("flyer-announce"))->build(jp::TemplateOptions());
        first->templateId = QStringLiteral("flyer-announce");
        ed->setDocument(std::move(first));
        jp::Document *d = ed->doc();
        const QString scheme = d->colors.name, fonts = d->fonts.name;
        // Typed over: the headline and the story. Added: a date, a note, and
        // a second picture. The footer stays as the design made it.
        typeOver(*d, withRole(*d, QStringLiteral("title")).first(), QStringLiteral("Grand Opening Weekend"));
        typeOver(*d, withRole(*d, QStringLiteral("body")).first(), QStringLiteral("Tulips bloom along the whole river walk."));
        ed->addItem(ed->newTextBox(QRectF(72, 700, 200, 24), QStringLiteral("Friday, March 13")));
        ed->addItem(ed->newTextBox(QRectF(72, 730, 200, 24), QStringLiteral("Bring a lawn chair")));
        static_cast<jp::PictureItem *>(withRole(*d, QStringLiteral("picture")).first())->imageId = d->addImage(png(Qt::red), "png");
        auto blue = std::make_shared<jp::PictureItem>();
        blue->rect = QRectF(400, 40, 120, 80);
        blue->imageId = d->addImage(png(Qt::blue), "png");
        blue->imgRect = QRectF(QPointF(), blue->rect.size());
        ed->addItem(blue);

        const jp::TemplateInfo *event = jp::findTemplate(QStringLiteral("flyer-event"));
        auto reference = event->build(jp::optionsForChange(*d, jp::TemplateOptions()));
        auto refBox = [&](const QString &role) {
            const auto found = withRole(*reference, role);
            return found.size() == 1 ? found.first() : nullptr;
        };
        QVERIFY(refBox(QStringLiteral("title")) && refBox(QStringLiteral("body")) && refBox(QStringLiteral("date")) && refBox(QStringLiteral("picture")));

        const QJsonObject before = d->toJson();
        auto design = event->build(jp::optionsForChange(*d, jp::TemplateOptions()));
        design->templateId = event->id;
        const int steps = ed->undoStack()->count();
        jp::ChangeReport rep;
        ed->applyTemplate(std::move(design), &rep);
        QCOMPARE(ed->undoStack()->count(), steps + 1);
        d = ed->doc();
        QCOMPARE(d->templateId, event->id);
        QCOMPARE(d->colors.name, scheme);   // only the gallery's choices replace the schemes
        QCOMPARE(d->fonts.name, fonts);
        // The headline went into the new design's TextArt title, the story and the date
        // into their boxes, and the red picture into the placeholder.
        const auto title = withRole(*d, QStringLiteral("title"));
        QCOMPARE(title.size(), 1);
        QCOMPARE(title.first()->type(), jp::ItemType::TextArt);
        QCOMPARE(textOf(*d, title.first()), QStringLiteral("Grand Opening Weekend"));
        QCOMPARE(title.first()->rect, refBox(QStringLiteral("title"))->rect);
        const auto body = withRole(*d, QStringLiteral("body"));
        QCOMPARE(body.size(), 1);
        QCOMPARE(textOf(*d, body.first()), QStringLiteral("Tulips bloom along the whole river walk."));
        QCOMPARE(body.first()->rect, refBox(QStringLiteral("body"))->rect);
        QCOMPARE(static_cast<jp::TextItem *>(body.first())->columns, 2);   // the new design's box
        const auto date = withRole(*d, QStringLiteral("date"));
        QCOMPARE(date.size(), 1);
        QCOMPARE(textOf(*d, date.first()), QStringLiteral("Friday, March 13"));
        QCOMPARE(date.first()->rect, refBox(QStringLiteral("date"))->rect);
        // Text the design keeps is its own: nothing of the old footer came across.
        const auto footers = withRole(*d, QStringLiteral("address"));
        QCOMPARE(footers.size(), withRole(*reference, QStringLiteral("address")).size());
        QCOMPARE(textOf(*d, footers.first()), textOf(*reference, withRole(*reference, QStringLiteral("address")).first()));
        const auto pics = withRole(*d, QStringLiteral("picture"));
        QCOMPARE(pics.size(), 1);
        auto *placed = static_cast<jp::PictureItem *>(pics.first());
        QCOMPARE(placed->rect, refBox(QStringLiteral("picture"))->rect);
        QCOMPARE(d->image(placed->imageId).pixelColor(30, 20), QColor(Qt::red));
        // The note and the second picture have no place: they are in Extra Content.
        QCOMPARE(rep.stories, 3);
        QCOMPARE(rep.pictures, 1);
        QCOMPARE(rep.extraStories, 1);
        QCOMPARE(rep.extraPictures, 1);
        QCOMPARE(int(d->extra.size()), 2);
        QCOMPARE(d->extra[0]->type(), jp::ItemType::Text);
        QCOMPARE(textOf(*d, d->extra[0].get()), QStringLiteral("Bring a lawn chair"));
        QCOMPARE(d->extra[1]->type(), jp::ItemType::Picture);
        QCOMPARE(d->image(static_cast<jp::PictureItem *>(d->extra[1].get())->imageId).pixelColor(30, 20), QColor(Qt::blue));
        // They stay with the publication when it is saved and opened again.
        {
            QString err;
            auto back = jp::publicationFromBytes(jp::publicationBytes(*d, QImage()), &err);
            QVERIFY2(back, qPrintable(err));
            QCOMPARE(int(back->extra.size()), 2);
            QCOMPARE(textOf(*back, back->extra[0].get()), QStringLiteral("Bring a lawn chair"));
            QCOMPARE(back->image(static_cast<jp::PictureItem *>(back->extra[1].get())->imageId).pixelColor(30, 20), QColor(Qt::blue));
        }

        // One undo step brings the first design back, and redo applies it again.
        ed->undo();
        QCOMPARE(noImages(ed->doc()->toJson()), noImages(before));
        QVERIFY(ed->doc()->extra.empty());
        ed->redo();
        QCOMPARE(int(ed->doc()->extra.size()), 2);
        d = ed->doc();

        // Extra Content: the pane lists what is left, each can be placed on the page or discarded.
        QTRY_VERIFY(w.act(QStringLiteral("pd.extraContent"))->isEnabled());   // the window updates its commands soon after a change
        w.showTaskPane(QStringLiteral("extra"));
        QCOMPARE(w.currentTaskPane(), QStringLiteral("extra"));
        auto *pane = w.findChild<jp::TaskPane *>();
        QVERIFY(pane);
        auto *list = pane->findChild<QListWidget *>(QStringLiteral("extraContentList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        auto button = [&](const QString &text) -> QPushButton * {
            for (QPushButton *b : pane->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QVERIFY(button(QStringLiteral("Place on Page")) && button(QStringLiteral("Discard")));
        {   // What a drag from the list carries: the ids of the items.
            std::unique_ptr<QMimeData> drag(list->model()->mimeData({list->model()->index(1, 0)}));
            QVERIFY(drag && drag->hasFormat(QString::fromLatin1(jp::kExtraContentMime)));
            QCOMPARE(QString::fromUtf8(drag->data(QString::fromLatin1(jp::kExtraContentMime))), d->extra[1]->id);
        }
        const int onPage = int(d->pages[ed->currentPage()]->items.size());
        list->setCurrentRow(1);   // the blue picture
        button(QStringLiteral("Place on Page"))->click();
        QCOMPARE(list->count(), 1);
        QCOMPARE(int(d->extra.size()), 1);
        QCOMPARE(int(d->pages[ed->currentPage()]->items.size()), onPage + 1);
        auto *landed = dynamic_cast<jp::PictureItem *>(d->pages[ed->currentPage()]->items.back().get());
        QVERIFY(landed);
        QCOMPARE(d->image(landed->imageId).pixelColor(30, 20), QColor(Qt::blue));
        QVERIFY(QRectF(QPointF(0, 0), d->pageSize()).contains(landed->rect));
        // A story placed becomes a text box big enough for its text.
        ed->placeExtra(d->extra[0]->id, QPointF(100, 600));
        auto *note = dynamic_cast<jp::TextItem *>(d->pages[ed->currentPage()]->items.back().get());
        QVERIFY(note);
        QCOMPARE(textOf(*d, note), QStringLiteral("Bring a lawn chair"));
        QCOMPARE(note->rect.topLeft(), QPointF(100, 600));
        QVERIFY(d->extra.empty());
        w.refreshUi();   // the pane follows the publication when the window updates, and only while it shows
        QCOMPARE(list->count(), 0);
        ed->undo();   // back in Extra Content
        QCOMPARE(int(d->extra.size()), 1);
        // Dragged from the pane onto the page, it lands where it is dropped.
        QMimeData dragged;
        dragged.setData(QString::fromLatin1(jp::kExtraContentMime), d->extra[0]->id.toUtf8());
        QDragEnterEvent enter(QPoint(300, 300), Qt::CopyAction, &dragged, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(w.canvas()->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(300, 300), Qt::CopyAction, &dragged, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(w.canvas()->viewport(), &drop);
        QVERIFY(d->extra.empty());
        auto *dropped = dynamic_cast<jp::TextItem *>(d->pages[ed->currentPage()]->items.back().get());
        QVERIFY(dropped);
        QCOMPARE(dropped->rect.topLeft(), w.canvas()->toPage(QPointF(300, 300)));
        ed->undo();
        QCOMPARE(int(d->extra.size()), 1);
        ed->discardExtra(d->extra[0]->id);
        QVERIFY(d->extra.empty());
        QTRY_VERIFY(!w.act(QStringLiteral("pd.extraContent"))->isEnabled());
        ed->undo();
        QCOMPARE(int(d->extra.size()), 1);

        // "Create a new publication": the gallery's color scheme applies, the new
        // design's page size, and this publication stays as it is.
        const QJsonObject mid = noImages(d->toJson());
        const jp::TemplateInfo *invite = jp::findTemplate(QStringLiteral("invitation-evening"));
        jp::TemplateOptions pick;
        pick.colorScheme = QStringLiteral("Cherry");
        auto second = invite->build(jp::optionsForChange(*d, pick));
        second->templateId = invite->id;
        jp::MainWindow *other = w.changeTemplate(std::move(second), true);
        QVERIFY(other && other != &w);
        QCOMPARE(noImages(ed->doc()->toJson()), mid);
        const jp::Document *nd = other->editor()->doc();
        QCOMPARE(nd->pageSize(), QSizeF(5 * 72, 7 * 72));
        QCOMPARE(nd->colors.name, QStringLiteral("Cherry"));
        QCOMPARE(nd->fonts.name, fonts);
        QCOMPARE(nd->templateId, invite->id);
        const auto headline = withRole(*nd, QStringLiteral("title"));   // TextArt text into a text box
        QCOMPARE(headline.size(), 1);
        QCOMPARE(headline.first()->type(), jp::ItemType::Text);
        QCOMPARE(textOf(*nd, headline.first()), QStringLiteral("Grand Opening Weekend"));
        QVERIFY(!nd->extra.empty());   // the story and the note have no box in an invitation
        delete other;

        // Pictures take placeholders in reading order, page by page, top to
        // bottom, left to right; stories of one role take the boxes the same way.
        ed->setDocument(jp::Document::blank(QSizeF(612, 792)));
        d = ed->doc();
        auto addPicture = [&](const QColor &c, const QRectF &r) {
            auto p = std::make_shared<jp::PictureItem>();
            p->rect = r;
            p->imageId = d->addImage(png(c), "png");
            p->imgRect = QRectF(QPointF(), r.size());
            ed->addItem(p);
        };
        addPicture(Qt::red, QRectF(300, 100, 120, 80));
        addPicture(Qt::green, QRectF(50, 104, 120, 80));   // in red's row, to its left
        addPicture(Qt::blue, QRectF(50, 400, 120, 80));
        ed->addItem(ed->newTextBox(QRectF(50, 600, 300, 60), QStringLiteral("Lower story, written first, long enough to count as running text.")));
        ed->addItem(ed->newTextBox(QRectF(50, 300, 300, 60), QStringLiteral("Upper story, written second, long enough to count as running text.")));
        auto catalog = jp::findTemplate(QStringLiteral("catalog-fall"))->build(jp::optionsForChange(*d, jp::TemplateOptions()));
        ed->applyTemplate(std::move(catalog));
        d = ed->doc();
        auto reading = [&](int page, jp::ItemType type) {
            QVector<jp::Item *> out;
            for (const auto &it : d->pages[page]->items)
                if (it->type() == type && it->role != QLatin1String("logo")) out << it.get();
            std::stable_sort(out.begin(), out.end(), [](const jp::Item *a, const jp::Item *b) {
                return a->rect.top() != b->rect.top() ? a->rect.top() < b->rect.top() : a->rect.left() < b->rect.left();
            });
            return out;
        };
        auto colorOf = [&](const jp::Item *it) { return d->image(static_cast<const jp::PictureItem *>(it)->imageId).pixelColor(30, 20); };
        QCOMPARE(colorOf(reading(0, jp::ItemType::Picture).first()), QColor(Qt::green));
        const auto grid = reading(1, jp::ItemType::Picture);
        QCOMPARE(colorOf(grid[0]), QColor(Qt::red));
        QCOMPARE(colorOf(grid[1]), QColor(Qt::blue));
        QVERIFY(colorOf(grid[2]) != QColor(Qt::blue) && colorOf(grid[2]) != QColor(Qt::red));   // the design's own picture
        const auto boxes = reading(1, jp::ItemType::Text);
        QVERIFY(textOf(*d, boxes[0]).startsWith(QLatin1String("Upper story")));
        QVERIFY(textOf(*d, boxes[1]).startsWith(QLatin1String("Lower story")));
        QVERIFY(d->extra.empty());

        // Nothing the user made is lost. A table, shape, line, group, or Text Art
        // stays behind only when it is the old design's own and unchanged; any
        // other goes to Extra Content, whole, and can be placed back.
        auto extrasOf = [&](jp::ItemType t) {
            int n = 0;
            for (const auto &e : d->extra) n += e->type() == t;
            return n;
        };
        auto firstOf = [&](jp::ItemType t) -> jp::Item * {
            for (const auto &it : d->pages[0]->items)
                if (it->type() == t) return it.get();
            return nullptr;
        };
        auto start = [&](const QString &id) {
            auto made = jp::findTemplate(id)->build(jp::TemplateOptions());
            made->templateId = id;
            ed->setDocument(std::move(made));
            d = ed->doc();
        };
        auto change = [&](const QString &id, jp::ChangeReport *r) {
            const jp::TemplateInfo *to = jp::findTemplate(id);
            auto design = to->build(jp::optionsForChange(*d, jp::TemplateOptions()));
            design->templateId = id;
            ed->applyTemplate(std::move(design), r);
            d = ed->doc();
        };
        auto editStory = [&](const QString &storyId, const QString &text) {
            QTextCursor c(d->storyDoc(storyId));
            c.select(QTextCursor::Document);
            c.insertText(text);
        };
        // Untouched, a design's own shapes and lines stay with it.
        start(QStringLiteral("flyer-announce"));
        jp::ChangeReport kept;
        change(QStringLiteral("flyer-event"), &kept);
        QVERIFY(d->extra.empty());
        QCOMPARE(kept.extraObjects, 0);
        // Edited, moved, or drawn by the user, they go to Extra Content.
        start(QStringLiteral("flyer-announce"));
        auto *sidebar = static_cast<jp::ShapeItem *>(firstOf(jp::ItemType::Shape));
        QVERIFY(sidebar && !sidebar->storyId.isEmpty());
        editStory(sidebar->storyId, QStringLiteral("Doors open at six"));
        static_cast<jp::LineItem *>(firstOf(jp::ItemType::Line))->moveBy(0, 10);
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(300, 600, 80, 40);
        ed->addItem(box);
        auto stroke = std::make_shared<jp::LineItem>();
        stroke->p1 = QPointF(40, 600);
        stroke->p2 = QPointF(240, 640);
        stroke->syncRect();
        ed->addItem(stroke);
        auto wordArt = std::make_shared<jp::TextArtItem>();
        wordArt->text = QStringLiteral("Drawn WordArt");
        wordArt->rect = QRectF(72, 640, 200, 60);
        ed->addItem(wordArt);
        auto table = std::static_pointer_cast<jp::TableItem>(ed->newTable(QRectF(72, 500, 240, 60), 2, 3));
        editStory(table->cell(0, 0).storyId, QStringLiteral("Cell A"));
        ed->addItem(table);
        auto inner = std::make_shared<jp::ShapeItem>();
        inner->rect = QRectF(420, 640, 50, 50);
        auto rule = std::make_shared<jp::LineItem>();
        rule->p1 = QPointF(420, 700);
        rule->p2 = QPointF(520, 700);
        rule->syncRect();
        auto group = std::make_shared<jp::GroupItem>();
        group->children = {inner, rule};
        group->syncRect();
        ed->addItem(group);
        jp::ChangeReport lost;
        change(QStringLiteral("flyer-event"), &lost);
        QCOMPARE(extrasOf(jp::ItemType::Shape), 2);   // the sidebar with its new words, and the drawn one
        QCOMPARE(extrasOf(jp::ItemType::Line), 2);    // the divider that was moved, and the drawn one
        QCOMPARE(extrasOf(jp::ItemType::Table), 1);
        QCOMPARE(extrasOf(jp::ItemType::Group), 1);
        QCOMPARE(extrasOf(jp::ItemType::TextArt), 1);
        QCOMPARE(lost.extraObjects, 6);
        QCOMPARE(lost.extraStories, 1);   // the Text Art is a story with no title box to take it
        // Placed back, a table is whole: its size, and the words in its cells.
        jp::Item *extraTable = nullptr, *extraGroup = nullptr;
        for (const auto &e : d->extra) {
            if (e->type() == jp::ItemType::Table) extraTable = e.get();
            if (e->type() == jp::ItemType::Group) extraGroup = e.get();
        }
        QVERIFY(extraTable && extraGroup);
        ed->placeExtra(extraTable->id);
        auto *back = dynamic_cast<jp::TableItem *>(d->pages[ed->currentPage()]->items.back().get());
        QVERIFY(back);
        QCOMPARE(back->rows, 2);
        QCOMPARE(back->cols, 3);
        QCOMPARE(d->storyDoc(back->cell(0, 0).storyId)->toPlainText(), QStringLiteral("Cell A"));
        ed->placeExtra(extraGroup->id);
        auto *regrouped = dynamic_cast<jp::GroupItem *>(d->pages[ed->currentPage()]->items.back().get());
        QVERIFY(regrouped);
        QCOMPARE(int(regrouped->children.size()), 2);
        // A design's own table stays behind unchanged; once a cell is edited, it goes to Extra Content.
        start(QStringLiteral("newsletter-classic"));
        change(QStringLiteral("letterhead"), &kept);
        QVERIFY(d->extra.empty());
        start(QStringLiteral("newsletter-classic"));
        ed->setCurrentPage(1);
        auto *calendar = static_cast<jp::TableItem *>(d->pages[1]->items.back().get());
        QCOMPARE(calendar->type(), jp::ItemType::Table);
        editStory(calendar->cell(1, 1).storyId, QStringLiteral("Opening night: the tulip show"));
        change(QStringLiteral("letterhead"), &lost);
        QCOMPARE(extrasOf(jp::ItemType::Table), 1);
        ed->placeExtra(d->extra.back()->id);
        auto *calendarBack = dynamic_cast<jp::TableItem *>(d->pages[ed->currentPage()]->items.back().get());
        QVERIFY(calendarBack);
        QCOMPARE(calendarBack->rows, 5);
        QCOMPARE(d->storyDoc(calendarBack->cell(1, 1).storyId)->toPlainText(), QStringLiteral("Opening night: the tulip show"));
        QCOMPARE(d->storyDoc(calendarBack->cell(2, 1).storyId)->toPlainText(), QStringLiteral("Open house at the new studio"));
        // Built with its logo (which moves a line), and saved and opened again, a design's own parts are still its own.
        jp::TemplateOptions withLogo;
        withLogo.options["logo"] = true;
        auto card = jp::findTemplate(QStringLiteral("bizcard-classic"))->build(withLogo);
        card->templateId = QStringLiteral("bizcard-classic");
        QString err;
        auto reopened = jp::publicationFromBytes(jp::publicationBytes(*card, QImage()), &err);
        QVERIFY2(reopened, qPrintable(err));
        ed->setDocument(std::move(reopened));
        d = ed->doc();
        change(QStringLiteral("bizcard-band"), &kept);
        QVERIFY(d->extra.empty());
    }

    // The gallery Change Template opens is the New page's, in a mode of its
    // own: its title and button say so, it offers designs and no blank
    // sizes, its schemes default to the publication's own, and choosing a
    // design asks whether to apply it here or to make a new publication.
    void changeTemplateGallery()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        jp::Editor *ed = w.editor();
        auto first = jp::findTemplate(QStringLiteral("flyer-announce"))->build(jp::TemplateOptions());
        first->templateId = QStringLiteral("flyer-announce");
        ed->setDocument(std::move(first));
        for (const auto &it : ed->doc()->pages[0]->items)
            if (it->role == QLatin1String("body")) {
                QTextCursor c(ed->doc()->storyDoc(static_cast<jp::TextItem *>(it.get())->storyId));
                c.select(QTextCursor::Document);
                c.insertText(QStringLiteral("Tulips bloom along the whole river walk."));
            }
        auto gallery = [&]() -> QWidget * {
            for (QWidget *c : w.findChildren<QWidget *>())
                if (c->inherits("jp::Backstage")) return c;
            return nullptr;
        };
        // What shows: the File view keeps its other pages, hidden.
        auto label = [](QWidget *in, const QString &text) {
            for (QLabel *l : in->findChildren<QLabel *>())
                if (l->isVisible() && l->text() == text) return true;
            return false;
        };
        auto button = [](QWidget *in, const QString &text) -> QAbstractButton * {
            for (QAbstractButton *b : in->findChildren<QAbstractButton *>())
                if (b->isVisible() && b->text() == text) return b;
            return nullptr;
        };
        // The New page is as it was.
        w.act(QStringLiteral("file.new"))->trigger();
        QVERIFY(gallery() && gallery()->isVisible());
        QVERIFY(label(gallery(), QStringLiteral("New Publication")));
        QVERIFY(button(gallery(), QStringLiteral("Create")));
        bool blankSizes = false;
        for (QListWidget *l : gallery()->findChildren<QListWidget *>())
            for (int i = 0; i < l->count(); ++i) blankSizes |= l->isVisible() && l->item(i)->text() == QLatin1String("Blank Sizes");
        QVERIFY(blankSizes);
        w.hideBackstage();

        w.act(QStringLiteral("pd.changeTemplate"))->trigger();
        QVERIFY(gallery()->isVisible());
        QVERIFY(label(gallery(), QStringLiteral("Change Template")));
        QVERIFY(!label(gallery(), QStringLiteral("New Publication")));
        QAbstractButton *go = button(gallery(), QStringLiteral("Change Template"));
        QVERIFY(go && !button(gallery(), QStringLiteral("Create")));
        QListWidget *designs = nullptr;
        blankSizes = false;
        for (QListWidget *l : gallery()->findChildren<QListWidget *>()) {
            if (!l->isVisible()) continue;
            if (l->count() && l->item(0)->data(Qt::UserRole).toString().startsWith(QLatin1String("tpl:"))) designs = l;
            for (int i = 0; i < l->count(); ++i) blankSizes |= l->item(i)->text() == QLatin1String("Blank Sizes");
        }
        QVERIFY(designs && !blankSizes);
        int keeping = 0;
        for (QComboBox *c : gallery()->findChildren<QComboBox *>()) keeping += c->isVisible() && c->itemText(0) == QLatin1String("(keep current)");
        QCOMPARE(keeping, 2);   // the color scheme and the font scheme
        for (int i = 0; i < designs->count(); ++i)
            if (designs->item(i)->data(Qt::UserRole).toString() == QLatin1String("tpl:flyer-event")) designs->setCurrentRow(i);
        QCOMPARE(designs->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("tpl:flyer-event"));
        // The dialog gives the two ways; answer it the way it starts, applying here.
        QStringList choices;
        QTimer answer;
        answer.setInterval(20);
        connect(&answer, &QTimer::timeout, &answer, [&] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dlg) return;
            for (QAbstractButton *b : dlg->findChildren<QAbstractButton *>())
                if (b->inherits("QRadioButton")) choices << b->text() + (b->isChecked() ? QStringLiteral(" (chosen)") : QString());
            answer.stop();
            dlg->accept();
        });
        answer.start();
        const int steps = ed->undoStack()->count();
        go->click();
        answer.stop();
        QCOMPARE(choices, (QStringList{QStringLiteral("Apply template to the current publication (chosen)"),
                                       QStringLiteral("Create a new publication with my text and graphics")}));
        QCOMPARE(ed->doc()->templateId, QStringLiteral("flyer-event"));
        QCOMPARE(ed->undoStack()->count(), steps + 1);
        QVERIFY(!gallery()->isVisible());
        bool landed = false;
        for (const auto &it : ed->doc()->pages[0]->items)
            if (it->type() == jp::ItemType::Text && ed->doc()->storyDoc(static_cast<jp::TextItem *>(it.get())->storyId)->toPlainText() == QLatin1String("Tulips bloom along the whole river walk."))
                landed = it->role == QLatin1String("body");
        QVERIFY(landed);
        // Cancelling the question changes nothing.
        w.act(QStringLiteral("pd.changeTemplate"))->trigger();
        go = button(gallery(), QStringLiteral("Change Template"));
        QTimer cancel;
        cancel.setInterval(20);
        connect(&cancel, &QTimer::timeout, &cancel, [&] {
            if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                cancel.stop();
                dlg->reject();
            }
        });
        cancel.start();
        go->click();
        cancel.stop();
        QCOMPARE(ed->undoStack()->count(), steps + 1);
        QVERIFY(gallery()->isVisible());   // still choosing
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
        const QString text = QStringLiteral("Defense Force volunteers serve their state");
        // The phrase's width in Gill Sans MT (ems), regular and bold.
        for (const auto &[bold, ems] : {std::pair{false, 17.388}, std::pair{true, 19.855}}) {
            QTextDocument doc;
            QTextCursor c(&doc);
            QTextCharFormat f;
            f.setFontFamilies(QStringList{QStringLiteral("Gill Sans MT")});
            f.setFontPointSize(10);
            if (bold) f.setFontWeight(QFont::Bold);
            c.insertText(text, f);
            jp::FrameSpec fs;
            fs.size = QSizeF(10000, 100);
            fs.insets = QMarginsF(0, 0, 0, 0);
            fs.hyphenate = false;
            jp::StoryLayout lay;
            lay.build(&doc, {fs}, jp::LayoutEnv());
            auto x = [&](int pos) {
                int frame = -1;
                QRectF r;
                lay.caretRect(pos, &frame, &r);
                return r.x();
            };
            const double width = x(text.size()) - x(0);
            QVERIFY2(std::abs(width - ems * 10) < ems * 10 * 0.002, qPrintable(QStringLiteral("%1: %2 pt, not %3").arg(bold ? "bold" : "regular").arg(width).arg(ems * 10)));
        }
    }

    // A font's stand-in is looked up once, not on every layout of every run
    // (asking the font database costs 0.2 ms here), and is looked up again
    // when fonts are added or removed.
    void substituteForIsRemembered()
    {
        const QString fam = QStringLiteral("Gill Sans MT");
        if (QFontDatabase::hasFamily(fam)) QSKIP("Gill Sans MT is installed here");
        QVERIFY(!jp::substituteFor(fam).isEmpty());
        QElapsedTimer timer;
        timer.start();
        int found = 0;
        for (int i = 0; i < 1000; ++i) found += !jp::substituteFor(fam).isEmpty();
        QCOMPARE(found, 1000);
        QVERIFY2(timer.elapsed() < 50, qPrintable(QStringLiteral("1,000 lookups took %1 ms").arg(timer.elapsed())));
        // Fonts coming and going (a bundled font renamed "Arial" stands in for an installed Arial).
        if (QFontDatabase::hasFamily(QStringLiteral("Arial"))) QSKIP("Arial is installed here");
        QVERIFY(!jp::substituteFor(QStringLiteral("Arial")).isEmpty());
        QFile f(QStringLiteral(JP_TEST_DATA "/../../resources/fonts/Arimo-Regular.ttf"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray font = f.readAll();
        font.replace(QByteArray("\x00" "A\x00r\x00i\x00m\x00o", 10), QByteArray("\x00" "A\x00r\x00i\x00" "a\x00l", 10));
        font.replace(QByteArray("Arimo"), QByteArray("Arial"));
        const int id = QFontDatabase::addApplicationFontFromData(font);
        QVERIFY(id >= 0);
        const bool installed = jp::substituteFor(QStringLiteral("Arial")).isEmpty();
        QFontDatabase::removeApplicationFont(id);
        QVERIFY(installed);
        QVERIFY(!jp::substituteFor(QStringLiteral("Arial")).isEmpty());
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

    // The ribbon as built, one control per line. JP_RIBBON_DUMP=file writes
    // it, to compare a rebuilt ribbon with the one before; with
    // JP_RIBBON_DUMP_KEYTIPS=file it carries the KeyTips as well.
    void ribbonDescription()
    {
        jp::MainWindow w;
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r);
        // ribbon.json is built into the program; every command, widget and menu it names must exist.
        QVERIFY2(w.ribbonError().isEmpty(), qPrintable(w.ribbonError()));
        const QString d = r->describe();
        QVERIFY2(!d.contains(QStringLiteral("(none)")), "a ribbon button has no command");
        for (const char *tab : {"Home", "Insert", "Page Design", "Mailings", "Review", "View", "Text Box", "Table Layout"})
            QVERIFY2(d.contains(QStringLiteral("\ntab %1").arg(QLatin1String(tab))), tab);
        QVERIFY(d.contains(QStringLiteral("QToolButton edit.paste")));
        // Commands that only open a menu carry ids like every other command.
        QVERIFY(d.contains(QStringLiteral("QToolButton ribbon.changeCase")));
        // The only buttons without a command are the Table button, Line Weight and the bare drop-down arrows.
        for (const QString &line : d.split(QLatin1Char('\n')))
            if (line.trimmed().startsWith(QStringLiteral("QToolButton \"")))
                QVERIFY2(line.contains(QStringLiteral("\"Table\"")) || line.contains(QStringLiteral("\"Line Weight\"")) || line.contains(QStringLiteral("QToolButton \"\"")), qPrintable(line));
        const QString withKeytips = r->describe(true);
        QVERIFY(withKeytips.contains(QStringLiteral("tab Home keytip=H")));
        QVERIFY(withKeytips.contains(QStringLiteral("QToolButton edit.paste style=under icon=30 popup=split")));
        QVERIFY(withKeytips.contains(QStringLiteral(" keytip=V\n")));
        if (const QByteArray out = qgetenv("JP_RIBBON_DUMP"); !out.isEmpty()) {
            QFile f(QString::fromLocal8Bit(out));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(d.toUtf8());
        }
        if (const QByteArray out = qgetenv("JP_RIBBON_DUMP_KEYTIPS"); !out.isEmpty()) {
            QFile f(QString::fromLocal8Bit(out));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(withKeytips.toUtf8());
        }
    }

    // Every KeyTip on the ribbon is one or two of A-Z and 0-9; the keys that
    // can be typed first (File, Quick Access, the tabs) are unique, and in each
    // tab no KeyTip is another's beginning, so typing one never leaves doubt.
    void ribbonKeytips()
    {
        jp::MainWindow w;
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r);
        QVERIFY2(w.ribbonError().isEmpty(), qPrintable(w.ribbonError()));
        static const QRegularExpression valid(QStringLiteral("^[A-Z0-9]{1,2}$"));
        auto check = [&](const QStringList &keys, const QString &where) {
            for (int i = 0; i < keys.size(); ++i) {
                QVERIFY2(valid.match(keys[i]).hasMatch(), qPrintable(QStringLiteral("%1: \"%2\" isn't a KeyTip").arg(where, keys[i])));
                for (int j = 0; j < keys.size(); ++j)
                    if (i != j) QVERIFY2(!keys[j].startsWith(keys[i]), qPrintable(QStringLiteral("%1: %2 and %3 can't both be KeyTips").arg(where, keys[i], keys[j])));
            }
        };

        QStringList top{r->fileKeytip()};
        QCOMPARE(r->fileKeytip(), QStringLiteral("F"));
        for (const QToolButton *b : r->quickAccessButtons()) top << jp::keytip(b);
        QCOMPARE(top.mid(1), (QStringList{"1", "2", "3", "4"}));
        // The tabs always there have one letter; the contextual ones are J and another.
        const QStringList contextual{"Master Page", "Text Box", "Shape Format", "Picture Format", "Table Design", "Table Layout", "Text Art Format"};
        for (int i = 0; i < r->tabCount(); ++i) {
            top << r->tabKeytip(i);
            if (contextual.contains(r->tabName(i))) QVERIFY2(r->tabKeytip(i).size() == 2 && r->tabKeytip(i)[0] == QLatin1Char('J'), qPrintable(r->tabName(i)));
            else QVERIFY2(r->tabKeytip(i).size() == 1, qPrintable(r->tabName(i)));
        }
        check(top, QStringLiteral("File, Quick Access and tabs"));
        QCOMPARE(r->tabCount(), 7 + contextual.size());   // Home, Insert, Page Design, Mailings, Review, View, Help
        QCOMPARE(r->tabKeytip(0), QStringLiteral("H"));
        QCOMPARE(r->tabKeytip(1), QStringLiteral("N"));

        int controls = 0;
        for (int i = 0; i < r->tabCount(); ++i) {
            QStringList keys;
            const QString where = r->tabName(i);
            const QString need = QStringLiteral("%1: %2 has no KeyTip");
            for (jp::RibbonGroup *g : r->tabAt(i)->groups()) {
                QVERIFY2(!jp::keytip(g).isEmpty(), qPrintable(need.arg(where, g->title())));
                QVERIFY2(jp::keytip(g).startsWith(QLatin1Char('Z')), qPrintable(where + ": a collapsed group's KeyTip is Z and a letter"));
                keys << jp::keytip(g);
                if (g->launcher()) {
                    QVERIFY2(!jp::keytip(g->launcher()).isEmpty(), qPrintable(need.arg(where, g->title() + " launcher")));
                    keys << jp::keytip(g->launcher());
                }
                for (QWidget *c : g->controls()) {
                    if (qobject_cast<QLabel *>(c)) continue;   // plain text
                    ++controls;
                    QVERIFY2(!jp::keytip(c).isEmpty(), qPrintable(need.arg(where, QString::fromLatin1(c->metaObject()->className()) + " in " + g->title())));
                    keys << jp::keytip(c);
                }
            }
            check(keys, where);
            // Only a group's own KeyTip may start with Z, so a control's never hides one.
            for (const QString &k : keys)
                if (k.startsWith(QLatin1Char('Z'))) QVERIFY2(k.size() == 2, qPrintable(where + ": " + k));
        }
        QVERIFY2(controls > 250, qPrintable(QString::number(controls)));
    }

    // jp::buildRibbon reports every unknown name at once, with where it was used.
    void ribbonBuilderReportsProblems()
    {
        jp::Ribbon r;
        QAction known(QStringLiteral("Known"));
        jp::RibbonParts parts;
        parts.action = [&](const QString &id) { return id == QLatin1String("known") ? &known : nullptr; };
        const QByteArray json = R"({"tabs": [{"name": "T", "keytip": "T", "groups": [
            {"name": "G", "keytip": "ZG", "launcher": "noLauncher", "items": [
              {"large": "missing.one"}, {"widget": "noWidget"}, {"large": "known", "menu": "@noMenu"},
              {"small": "known", "typo": 1}, {"row": [{"icon": "missing.two"}]},
              {"large": {"id": "ribbon.x", "text": "X", "icon": "no-such-icon"}}, {"large": "known", "keytip": "abc"}]},
            {"use": "noTemplate"}]},
            {"name": "C", "context": "Ctx", "color": "noColor", "keytip": "JC", "groups": []}]})";
        QString err;
        QVERIFY(!jp::buildRibbon(&r, json, parts, &r, &err));
        for (const char *needle : {"missing.one", "missing.two", "noWidget", "noMenu", "noLauncher", "noTemplate", "typo", "no-such-icon", "abc", "noColor", "(in T / G)"})
            QVERIFY2(err.contains(QLatin1String(needle)), qPrintable(QStringLiteral("%1 not reported in:\n%2").arg(QLatin1String(needle), err)));
        QVERIFY(!jp::buildRibbon(&r, "{not json", parts, &r, &err));
        QVERIFY(err.contains(QStringLiteral("JSON")));
    }

    // The loader's pieces together: a template used twice, a command that only
    // opens a menu (made once, shared by its uses, doing what its trigger says),
    // sections and submenus, a launcher, KeyTips, and translation.
    void ribbonBuilderBuilds()
    {
        struct Upper : QTranslator {
            bool isEmpty() const override { return false; }
            QString translate(const char *context, const char *source, const char *, int) const override
            {
                return QLatin1String(context) == QLatin1String("Ribbon") ? QString::fromUtf8(source).toUpper() : QString();
            }
        };
        QAction known(QStringLiteral("Known"));
        known.setObjectName(QStringLiteral("known"));
        int launched = 0, triggered = 0, boxes = 0;
        jp::RibbonParts parts;
        parts.action = [&](const QString &id) { return id == QLatin1String("known") ? &known : nullptr; };
        parts.widgets[QStringLiteral("box")] = [&]() -> QWidget * { ++boxes; return new QComboBox(); };
        parts.menus[QStringLiteral("dyn")] = [&](QWidget *owner) { auto *m = new QMenu(owner); m->addAction(&known); return m; };
        parts.launchers[QStringLiteral("dlg")] = [&] { ++launched; };
        parts.triggers[QStringLiteral("ribbon.menuOnly")] = [&] { ++triggered; };
        parts.colors[QStringLiteral("blue")] = QColor(Qt::blue);
        const QByteArray json = R"({
          "fileKeytip": "F",
          "quickAccess": [{"cmd": "known", "keytip": "1"}],
          "templates": {"Shared": {"name": "Shared", "keytip": "ZS", "launcher": "dlg", "launcherTip": "Shared Settings", "launcherKeytip": "SS", "items": [
            {"large": {"id": "ribbon.menuOnly", "text": "Menu Only", "icon": "table"}, "keytip": "MO", "menu": [
              "known", "-", {"section": "Heading"}, {"submenu": "Sub", "icon": "square", "menu": ["known"]}]},
            {"row": [{"label": "Label"}, {"widget": "box", "keytip": "BX"}, {"dropdown": {"icon": "chevron-down", "tip": "More"}, "menu": "@dyn", "keytip": "MR"}]}]}},
          "tabs": [
            {"name": "One", "keytip": "O", "groups": [{"use": "Shared"},
              {"name": "Own", "keytip": "ZO", "items": [{"small": "ribbon.menuOnly", "keytip": "OM"}, {"small": "known", "split": false, "keytip": "KN"}]}]},
            {"name": "Two", "keytip": "JT", "context": "Ctx", "color": "blue", "groups": [{"use": "Shared", "keytip": "ZX"}]}]})";
        Upper upper;
        QCoreApplication::installTranslator(&upper);
        jp::Ribbon r;
        QString err;
        const bool ok = jp::buildRibbon(&r, json, parts, &r, &err);
        QCoreApplication::removeTranslator(&upper);
        QVERIFY2(ok, qPrintable(err));

        QCOMPARE(r.tabCount(), 2);
        QCOMPARE(r.tabName(0), QStringLiteral("One"));   // how code finds the tab doesn't change
        QCOMPARE(r.tabTitle(0), QStringLiteral("ONE"));   // what the header shows does
        QCOMPARE(r.tabKeytip(1), QStringLiteral("JT"));
        QCOMPARE(r.fileKeytip(), QStringLiteral("F"));
        QCOMPARE(jp::keytip(r.quickAccessButtons().value(0)), QStringLiteral("1"));
        QCOMPARE(boxes, 2);   // the template's widget, once per use

        const auto groups = r.tabAt(0)->groups();
        QCOMPARE(groups.size(), 2);
        QCOMPARE(groups[0]->title(), QStringLiteral("SHARED"));
        QCOMPARE(jp::keytip(groups[0]), QStringLiteral("ZS"));
        QCOMPARE(jp::keytip(r.tabAt(1)->groups().value(0)), QStringLiteral("ZX"));   // a use may rekey its template
        QVERIFY(groups[0]->launcher());
        QCOMPARE(groups[0]->launcher()->toolTip(), QStringLiteral("SHARED SETTINGS"));
        QCOMPARE(jp::keytip(groups[0]->launcher()), QStringLiteral("SS"));
        groups[0]->launcher()->click();
        QCOMPARE(launched, 1);

        const auto controls = groups[0]->controls();   // the big button, the label, the box, the dropdown
        QCOMPARE(controls.size(), 4);
        auto *big = qobject_cast<QToolButton *>(controls[0]);
        QVERIFY(big);
        QCOMPARE(jp::keytip(big), QStringLiteral("MO"));
        QVERIFY(big->defaultAction());
        QCOMPARE(big->defaultAction()->objectName(), QStringLiteral("ribbon.menuOnly"));
        QCOMPARE(big->defaultAction()->text(), QStringLiteral("MENU ONLY"));
        QVERIFY(!big->defaultAction()->icon().isNull());
        QCOMPARE(big->popupMode(), QToolButton::InstantPopup);
        QVERIFY(big->menu());
        const QList<QAction *> items = big->menu()->actions();
        QCOMPARE(items.size(), 4);
        QCOMPARE(items[0], &known);
        QVERIFY(items[1]->isSeparator());
        QVERIFY(items[2]->isSeparator());   // a section is a separator with a heading
        QCOMPARE(items[2]->text(), QStringLiteral("HEADING"));
        QVERIFY(items[3]->menu());
        QCOMPARE(items[3]->text(), QStringLiteral("SUB"));
        QCOMPARE(items[3]->menu()->actions(), QList<QAction *>{&known});
        QCOMPARE(qobject_cast<QLabel *>(controls[1])->text(), QStringLiteral("LABEL"));
        QCOMPARE(jp::keytip(controls[2]), QStringLiteral("BX"));
        QVERIFY(jp::keytip(controls[1]).isEmpty());
        auto *more = qobject_cast<QToolButton *>(controls[3]);
        QVERIFY(more && more->menu());
        QCOMPARE(more->menu()->actions(), QList<QAction *>{&known});
        QCOMPARE(more->toolTip(), QStringLiteral("MORE"));

        // The command made for the first use is the one every later use shares.
        const auto own = groups[1]->controls();
        QCOMPARE(own.size(), 2);
        QCOMPARE(qobject_cast<QToolButton *>(own[0])->defaultAction(), big->defaultAction());
        QCOMPARE(qobject_cast<QToolButton *>(own[1])->defaultAction(), &known);
        QCOMPARE(qobject_cast<QToolButton *>(r.tabAt(1)->groups()[0]->controls()[0])->defaultAction(), big->defaultAction());
        big->defaultAction()->trigger();
        QCOMPARE(triggered, 1);

        // The contextual tab, and the plain description that tests compare.
        const QString d = r.describe(true);
        QVERIFY2(d.contains(QStringLiteral("tab Two (Ctx, #0000ff) keytip=JT")), qPrintable(d));
        QVERIFY2(d.contains(QStringLiteral("group SHARED launcher=\"SHARED SETTINGS\" keytip=ZS launcher-keytip=SS")), qPrintable(d));
        QVERIFY(!r.describe().contains(QStringLiteral("keytip")));
    }

    // Setting a group's launcher again cut every connection from its button,
    // including the one that tells Qt's screen reader support the button is
    // gone: its stale entry was later handed to a new control at the same
    // address, and asking its name crashed (only where a screen reader or
    // other accessibility client is active, as on the Mac and Windows
    // runners).
    void launcherKeepsAccessibilityInTouch()
    {
        auto *g = new jp::RibbonGroup(QStringLiteral("Font"));
        g->setLauncher([] {}, QStringLiteral("Font"));
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(g->launcher());
        QVERIFY(iface);
        const QAccessible::Id id = QAccessible::uniqueId(iface);
        g->setLauncher([] {}, QStringLiteral("Font Settings"));
        delete g;
        QVERIFY(!QAccessible::accessibleInterface(id));   // gone with its button
    }

    // Every ribbon control has a name screen readers read, and the keyboard
    // reaches it (the ribbon's buttons, galleries and color buttons refused
    // the focus; its tabs and File button were only drawn).
    void ribbonControlsAreNamedAndReachable()
    {
        jp::MainWindow w;
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r);
        QStringList unnamed, unreachable, undescribed;
        int checked = 0;
        for (QWidget *c : r->findChildren<QWidget *>()) {
            QWidget *pw = c->parentWidget();
            // A box one types in hands the focus to its typing field, which
            // Windows' screen readers name on its own.
            const bool typingField = c->inherits("QLineEdit") && pw && pw->inherits("QComboBox");
            const bool control = qobject_cast<QAbstractButton *>(c) || qobject_cast<QComboBox *>(c) || qobject_cast<QAbstractSpinBox *>(c)
                                 || qobject_cast<QAbstractItemView *>(c) || qobject_cast<QSlider *>(c) || typingField;
            if (!control || !pw || qobject_cast<QAbstractItemView *>(pw) || pw->inherits("QComboBoxPrivateContainer")) continue;   // a combo's own list
            ++checked;
            QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(c);
            QVERIFY(iface);
            const QString name = iface->text(QAccessible::Name).trimmed();
            const auto *group = qobject_cast<jp::RibbonGroup *>(c->parentWidget());
            const QString where = QStringLiteral("%1 \"%2\" in %3").arg(QString::fromLatin1(c->metaObject()->className()), c->toolTip(),
                                                                          group ? group->title() : c->parentWidget() ? QString::fromLatin1(c->parentWidget()->metaObject()->className()) : QString());
            // Boxes, spin boxes and lists need a name of their own: some
            // systems read their current text instead, others nothing.
            if (name.isEmpty() || (!qobject_cast<QAbstractButton *>(c) && c->accessibleName().isEmpty())) unnamed << where;
            if (typingField) {
                // What the reader says once the box's letters put the keyboard there.
                if (iface->text(QAccessible::Help).trimmed().isEmpty()) undescribed << where + QStringLiteral(" [typing field]");
                continue;
            }
            if (!(c->focusPolicy() & Qt::TabFocus) && c->objectName() != QLatin1String("jpRibbonTab")) unreachable << where + QStringLiteral(" [") + name + QLatin1Char(']');
            // Windows' screen readers read Qt's help text; some read nothing
            // else of a control's description.
            if (iface->text(QAccessible::Help).trimmed().isEmpty()) undescribed << where + QStringLiteral(" [") + name + QLatin1Char(']');
        }
        QVERIFY2(checked > 300, qPrintable(QString::number(checked)));
        QVERIFY2(unnamed.isEmpty(), qPrintable(QStringLiteral("no name: ") + unnamed.join(QStringLiteral("; "))));
        QVERIFY2(unreachable.isEmpty(), qPrintable(QStringLiteral("no keyboard focus: ") + unreachable.join(QStringLiteral("; "))));
        QVERIFY2(undescribed.isEmpty(), qPrintable(QStringLiteral("no description: ") + undescribed.join(QStringLiteral("; "))));
    }

    // Outside the ribbon too: everything the keyboard stops on has a name,
    // and nothing that only holds controls takes a stop (Windows' screen
    // readers found the page, its thumbnails, the zoom slider, and the
    // font boxes' typing fields unnamed, and the ribbon's scroller empty).
    void windowControlsAreNamed()
    {
        jp::MainWindow w;
        QStringList unnamed;
        int checked = 0;
        for (QWidget *c : w.findChildren<QWidget *>()) {
            if (!(c->focusPolicy() & Qt::TabFocus) || c->focusProxy()) continue;
            bool inBackstage = false;
            for (QWidget *a = c; a; a = a->parentWidget()) inBackstage |= a->inherits("jp::Backstage") || a->inherits("QMenu");
            if (inBackstage) continue;
            QWidget *pw = c->parentWidget();
            if (pw && (qobject_cast<QAbstractItemView *>(pw) || pw->inherits("QComboBoxPrivateContainer"))) continue;   // a list's own parts
            ++checked;
            QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(c);
            QVERIFY(iface);
            if (iface->text(QAccessible::Name).trimmed().isEmpty())
                unnamed << QStringLiteral("%1 %2 in %3").arg(QString::fromLatin1(c->metaObject()->className()), c->objectName(),
                                                            pw ? QString::fromLatin1(pw->metaObject()->className()) : QString());
        }
        QVERIFY2(checked > 300, qPrintable(QString::number(checked)));
        QVERIFY2(unnamed.isEmpty(), qPrintable(QStringLiteral("no name: ") + unnamed.join(QStringLiteral("; "))));
        // The page list's entries say which page.
        auto *pages = w.findChild<jp::PagesPane *>();
        QVERIFY(pages && pages->count() > 0);
        QCOMPARE(pages->item(0)->data(Qt::AccessibleTextRole).toString(), QStringLiteral("Page 1"));
    }

    // No two commands share keys (Ctrl+M was both Increase Indent and
    // Master Page, so neither worked), and the keys the other program gives
    // these commands do the same here: Ctrl+M the master page, Ctrl+Shift+C
    // and Ctrl+Shift+V copy and paste formatting.
    void shortcutsAreOneEach()
    {
        jp::MainWindow w;
        QHash<QString, QStringList> byKeys;
        for (const QString &id : w.actionIds())
            for (const QKeySequence &k : w.act(id)->shortcuts()) byKeys[k.toString(QKeySequence::PortableText)] << id;
        QStringList shared;
        for (auto it = byKeys.begin(); it != byKeys.end(); ++it)
            if (it.value().size() > 1) shared << it.key() + QStringLiteral(": ") + it.value().join(QStringLiteral(", "));
        QVERIFY2(shared.isEmpty(), qPrintable(shared.join(QStringLiteral("; "))));
        QCOMPARE(byKeys.value(QStringLiteral("Ctrl+M")), QStringList{QStringLiteral("view.master")});
        QCOMPARE(byKeys.value(QStringLiteral("Ctrl+Shift+C")), QStringList{QStringLiteral("edit.copyFormat")});
        QCOMPARE(byKeys.value(QStringLiteral("Ctrl+Shift+V")), QStringList{QStringLiteral("edit.pasteFormat")});
        // Copy one box's formatting, paste it on another: its fill and its text's.
        jp::Editor *ed = w.editor();
        auto a = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 200, 60), QStringLiteral("Bold one")));
        auto b = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 200, 200, 60), QStringLiteral("Plain one")));
        ed->addItem(a);
        ed->addItem(b);
        ed->select(a->id);
        ed->toggleBold();
        ed->forEachSelected(QStringLiteral("Fill"), [](jp::Item *it) { it->fill = jp::Fill::solid(jp::ColorRef::fromString(QStringLiteral("#ff0000"))); });
        w.act(QStringLiteral("edit.copyFormat"))->trigger();
        QVERIFY(ed->hasCopiedFormatting());
        QCOMPARE(ed->tool(), jp::Tool::Select);   // no painter pointer
        ed->select(b->id);
        w.act(QStringLiteral("edit.pasteFormat"))->trigger();
        QTextCursor c(ed->doc()->storyDoc(b->storyId));
        c.movePosition(QTextCursor::NextCharacter);
        QCOMPARE(c.charFormat().fontWeight(), int(QFont::Bold));
        QCOMPARE(b->fill.toJson(), a->fill.toJson());
        ed->undo();
        c = QTextCursor(ed->doc()->storyDoc(b->storyId));
        c.movePosition(QTextCursor::NextCharacter);
        QVERIFY(c.charFormat().fontWeight() != int(QFont::Bold));
    }

    // Help's topics are all built in, each has a title and search words,
    // every link and F1 context leads to a topic, the contents lists them
    // all, and none names another company's products.
    void helpTopicsAreComplete()
    {
        const QString src = QStringLiteral(JP_TEST_DATA "/../../resources/help");
        QStringList onDisk, built;
        for (QDirIterator it(src, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) onDisk << QDir(src).relativeFilePath(it.next());
        for (QDirIterator it(QStringLiteral(":/help"), QDir::Files, QDirIterator::Subdirectories); it.hasNext();) built << it.next().mid(7);
        onDisk.sort();
        built.sort();
        QCOMPARE(built, onDisk);   // resources.qrc lists every file in resources/help
        const QStringList ids = jp::help::topicIds();
        QVERIFY2(ids.size() >= 45, qPrintable(QString::number(ids.size())));
        static const QRegularExpression link(QStringLiteral("\\]\\(([^)]+)\\)"));
        static const QRegularExpression banned(QStringLiteral("\\b(Microsoft|Publisher|Office)\\b"));
        const jp::help::Topic contents = jp::help::topic(QStringLiteral("index"));
        QStringList problems;
        for (const QString &id : ids) {
            const jp::help::Topic t = jp::help::topic(id);
            if (t.title.isEmpty()) problems << id + QStringLiteral(": no title");
            if (t.keywords.isEmpty()) problems << id + QStringLiteral(": no keywords");
            if (const auto m = banned.match(t.markdown); m.hasMatch()) problems << id + QStringLiteral(": says ") + m.captured(1);
            for (auto m = link.globalMatch(t.markdown); m.hasNext();) {
                const QString target = m.next().captured(1);
                if (!target.startsWith(QLatin1String("https://")) && !ids.contains(target)) problems << id + QStringLiteral(": link to ") + target;
            }
            if (id != QLatin1String("index") && !contents.markdown.contains(QStringLiteral("](%1)").arg(id))) problems << id + QStringLiteral(": not in the contents");
            QTextDocument doc;
            doc.setMarkdown(jp::help::displayMarkdown(t, nullptr), QTextDocument::MarkdownDialectGitHub);
            if (doc.toPlainText().trimmed().size() < 100) problems << id + QStringLiteral(": nearly empty");
        }
        QFile cf(src + QStringLiteral("/context.json"));
        QVERIFY(cf.open(QIODevice::ReadOnly));
        const QJsonObject context = QJsonDocument::fromJson(cf.readAll()).object();
        for (auto it = context.begin(); it != context.end(); ++it)
            if (!ids.contains(it.value().toString())) problems << QStringLiteral("context %1: no topic %2").arg(it.key(), it.value().toString());
        jp::MainWindow w;
        for (int i = 0; i < w.ribbon()->tabCount(); ++i)
            if (jp::help::contextTopic(QStringLiteral("tab:") + w.ribbon()->tabName(i)).isEmpty()) problems << QStringLiteral("no topic for the tab ") + w.ribbon()->tabName(i);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
        // The keyboard topic's table comes from the commands themselves.
        const QString keys = jp::help::displayMarkdown(jp::help::topic(QStringLiteral("keyboard")), &w);
        QVERIFY(!keys.contains(QLatin1String("<!--")));
        const QString save = QKeySequence(QKeySequence::Save).toString(QKeySequence::NativeText);
        QVERIFY2(keys.contains(QStringLiteral("| Save | %1 | Quick Access Toolbar |").arg(save)), qPrintable(keys.right(3000)));
        QVERIFY(keys.contains(QStringLiteral("| Collapse the Ribbon | %1 |").arg(QKeySequence(Qt::CTRL | Qt::Key_F1).toString(QKeySequence::NativeText))));
    }

    // F1 opens help on what is being done: the contents from the page, a
    // tab's topic from the ribbon, an object's topic when one is selected,
    // and a window of its own over the File page and over dialogs.
    void helpFollowsWhatYouAreDoing()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        w.canvas()->setFocus();
        QTest::keyClick(w.canvas(), Qt::Key_F1);
        jp::HelpView *v = w.helpView();
        QVERIFY(v && v->isVisible());
        QCOMPARE(w.currentTaskPane(), QStringLiteral("help"));
        QCOMPARE(v->currentTopic(), QStringLiteral("index"));
        QVERIFY(v->browser()->toPlainText().contains(QLatin1String("Getting started")));
        // From the ribbon: the tab's topic.
        jp::Ribbon *r = w.ribbon();
        r->showTab(r->tab(QStringLiteral("Insert")));
        r->focusCurrentTab();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F1);
        QCOMPARE(v->currentTopic(), QStringLiteral("tab-insert"));
        // A selected text box: text boxes.
        jp::Editor *ed = w.editor();
        ed->addItem(ed->newTextBox(QRectF(72, 72, 300, 100), QStringLiteral("Hello")));
        QVERIFY(ed->single());
        w.canvas()->setFocus();
        QTest::keyClick(w.canvas(), Qt::Key_F1);
        QCOMPARE(v->currentTopic(), QStringLiteral("text-boxes"));
        // The "?" beside the collapse chevron opens it too.
        w.hideTaskPane();
        auto *qmark = r->findChild<QAbstractButton *>(QStringLiteral("jpRibbonHelp"));
        QVERIFY(qmark && qmark->isVisible());
        QCOMPARE(qmark->accessibleName(), QStringLiteral("Help"));
        qmark->click();
        QCOMPARE(w.currentTaskPane(), QStringLiteral("help"));
        // Over the File page, whose print page covers the pane: a window.
        w.showBackstage(QStringLiteral("print"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F1);
        auto *hw = w.findChild<jp::HelpWindow *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(hw && hw->isVisible());
        QCOMPARE(hw->view()->currentTopic(), QStringLiteral("printing"));
        hw->close();
        QTRY_VERIFY(!w.findChild<jp::HelpWindow *>());   // closing deletes it
        w.hideBackstage();
        // Over a dialog: a window of the dialog's own, on its topic. (Dialogs
        // are modal, as exec() makes them, which keeps the window's own F1 off.)
        QDialog dlg(&w);
        dlg.setWindowModality(Qt::ApplicationModal);
        dlg.setWindowTitle(QStringLiteral("Mail Merge"));
        auto *field = new QLineEdit(&dlg);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowActive(&dlg));
        field->setFocus();
        QTest::keyClick(field, Qt::Key_F1);
        auto *over = dlg.findChild<jp::HelpWindow *>();
        QVERIFY(over);
        QCOMPARE(over->view()->currentTopic(), QStringLiteral("mail-merge"));
        // There it finds topics only: commands can't run under a dialog.
        over->view()->search(QStringLiteral("text box"));
        bool command = false;
        for (int i = 0; i < over->view()->results()->count(); ++i) command |= over->view()->results()->item(i)->text() == QLatin1String("Commands");
        QVERIFY(!command);
    }

    // The search box finds topics (ignoring "how do I") and commands, and
    // runs a command; links move between topics, Back and Forward return,
    // and web links go to the browser.
    void helpSearchFindsTopicsAndCommands()
    {
        jp::MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        w.showHelp();
        jp::HelpView *v = w.helpView();
        QVERIFY(v);
        auto texts = [v] {
            QStringList t;
            for (int i = 0; i < v->results()->count(); ++i) t << v->results()->item(i)->text();
            return t;
        };
        QCOMPARE(jp::help::searchTopics(QStringLiteral("how do I print")).value(0), QStringLiteral("printing"));
        QCOMPARE(jp::help::searchTopics(QStringLiteral("textbox")).value(0), QStringLiteral("text-boxes"));
        QVERIFY(jp::help::searchTopics(QStringLiteral("zzqxv")).isEmpty());
        v->search(QStringLiteral("text box"));
        QVERIFY(v->results()->isVisible() && !v->browser()->isVisible());
        const QStringList found = texts();
        QVERIFY2(found.contains(QStringLiteral("Help topics")) && found.contains(QStringLiteral("Commands")), qPrintable(found.join(QStringLiteral(" / "))));
        QListWidgetItem *draw = nullptr;
        for (int i = 0; i < v->results()->count(); ++i)
            if (v->results()->item(i)->text().startsWith(QLatin1String("Draw Text Box  (Home > "))) draw = v->results()->item(i);
        QVERIFY2(draw, qPrintable(found.join(QStringLiteral(" / "))));
        QSignalSpy ran(w.act(QStringLiteral("ins.textbox")), &QAction::triggered);
        QTest::keyClick(v->searchBox(), Qt::Key_Down);
        QCOMPARE(QApplication::focusWidget(), v->results());
        v->results()->setCurrentItem(draw);
        QTest::keyClick(v->results(), Qt::Key_Return);
        QCOMPARE(ran.count(), 1);
        v->search(QStringLiteral("zzqxv"));
        QCOMPARE(texts(), QStringList{QStringLiteral("Nothing found. Try other words, or open the contents.")});
        // Enter in the search box opens the first topic.
        v->search(QStringLiteral("mail merge"));
        QTest::keyClick(v->searchBox(), Qt::Key_Return);
        QCOMPARE(v->currentTopic(), QStringLiteral("mail-merge"));
        QVERIFY(v->browser()->isVisible() && v->searchBox()->text().isEmpty());
        // Links: a topic, Back, Forward, and a web page.
        Q_EMIT v->browser()->anchorClicked(QUrl(QStringLiteral("linked-text")));
        QCOMPARE(v->currentTopic(), QStringLiteral("linked-text"));
        auto button = [v](const QString &name) {
            for (auto *b : v->findChildren<QToolButton *>())
                if (b->accessibleName() == name) return b;
            return static_cast<QToolButton *>(nullptr);
        };
        QVERIFY(button(QStringLiteral("Back"))->isEnabled());
        button(QStringLiteral("Back"))->click();
        QCOMPARE(v->currentTopic(), QStringLiteral("mail-merge"));
        button(QStringLiteral("Forward"))->click();
        QCOMPARE(v->currentTopic(), QStringLiteral("linked-text"));
        button(QStringLiteral("Contents"))->click();
        QCOMPARE(v->currentTopic(), QStringLiteral("index"));
        QList<QUrl> opened;
        const auto keep = jp::help::openUrl;
        jp::help::openUrl = [&opened](const QUrl &u) { opened << u; return true; };
        Q_EMIT v->browser()->anchorClicked(QUrl(QStringLiteral("https://github.com/JeffOffice/jeffpub")));
        QCOMPARE(v->currentTopic(), QStringLiteral("index"));
        // The Help tab's web pages: a problem report and an idea carry the
        // version; What's New is this version's release.
        w.act(QStringLiteral("help.support"))->trigger();
        w.act(QStringLiteral("help.feedback"))->trigger();
        w.act(QStringLiteral("help.whatsNew"))->trigger();
        jp::help::openUrl = keep;
        QCOMPARE(opened.size(), 4);
        QCOMPARE(opened[1].toString(QUrl::RemoveQuery), QStringLiteral("https://github.com/JeffOffice/jeffpub/issues/new"));
        QVERIFY(QUrlQuery(opened[1]).queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded).contains(QStringLiteral("JeffPub " JP_VERSION " on ")));
        QCOMPARE(QUrlQuery(opened[2]).queryItemValue(QStringLiteral("labels")), QStringLiteral("enhancement"));
        QCOMPARE(opened[3].toString(), QStringLiteral("https://github.com/JeffOffice/jeffpub/releases/tag/v" JP_VERSION));
        // Ctrl+F1 collapses the ribbon and brings it back, as there.
        QVERIFY(!w.ribbon()->isMinimized());
        QTest::keyClick(&w, Qt::Key_F1, Qt::ControlModifier);
        QVERIFY(w.ribbon()->isMinimized());
        QTest::keyClick(&w, Qt::Key_F1, Qt::ControlModifier);
        QVERIFY(!w.ribbon()->isMinimized());
    }

    // Screen readers see the page's objects: each is a child of the page,
    // named by its kind and its text or alt text; the selected one is the
    // focus (so Tab is read out object by object); while typing, its text,
    // cursor, and selection can be read. (The page was one item, "Page".)
    void pageObjectsForScreenReaders()
    {
        jp::MainWindow w;
        w.resize(1200, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        jp::Editor *ed = w.editor();
        jp::Canvas *cv = w.canvas();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 300, 60), QStringLiteral("Spring sale")));
        ed->addItem(box);
        auto pic = std::make_shared<jp::PictureItem>();
        pic->rect = QRectF(72, 200, 144, 144);
        pic->altText = QStringLiteral("A red barn");
        ed->addItem(pic);
        auto bare = std::make_shared<jp::PictureItem>();
        bare->rect = QRectF(300, 200, 72, 72);
        ed->addItem(bare);
        cv->setFocus();
        QAccessibleInterface *page = QAccessible::queryAccessibleInterface(cv);
        QVERIFY(page);
        QCOMPARE(page->text(QAccessible::Name), QStringLiteral("Page"));
        QStringList names;
        for (int i = 0; i < page->childCount(); ++i)
            if (QAccessibleInterface *c = page->child(i); c && c->object() == nullptr) names << c->text(QAccessible::Name);
        QCOMPARE(names, (QStringList{QStringLiteral("Text box: Spring sale"), QStringLiteral("Picture: A red barn"), QStringLiteral("Picture, no alt text")}));
        // Tab selects each object, and the selected one is the focus.
        ed->clearSelection();
        QVERIFY(!page->focusChild());
        QTest::keyClick(cv, Qt::Key_Tab);
        QAccessibleInterface *f = page->focusChild();
        QVERIFY(f);
        QCOMPARE(f->text(QAccessible::Name), QStringLiteral("Text box: Spring sale"));
        QCOMPARE(f->role(), QAccessible::EditableText);
        QVERIFY(f->state().focused && f->state().selected);
        QVERIFY(f->text(QAccessible::Description).contains(QStringLiteral("from the left")));
        QCOMPARE(page->indexOfChild(f), 0);
        QVERIFY(f->rect().width() > 0 && cv->rect().translated(cv->mapToGlobal(QPoint(0, 0))).intersects(f->rect()));
        QTest::keyClick(cv, Qt::Key_Tab);
        QCOMPARE(page->focusChild()->text(QAccessible::Name), QStringLiteral("Picture: A red barn"));
        QCOMPARE(page->focusChild()->role(), QAccessible::Graphic);
        QVERIFY(!f->state().focused);
        // Typing: the text, the cursor, and the selection.
        ed->select(box->id);
        QTest::keyClick(cv, Qt::Key_Return);
        QVERIFY(ed->isEditingText());
        f = page->focusChild();
        QAccessibleTextInterface *t = f->textInterface();
        QVERIFY(t);
        QCOMPARE(t->text(0, t->characterCount()), QStringLiteral("Spring sale"));
        t->setCursorPosition(7);
        QCOMPARE(ed->cursor().position(), 7);
        QCOMPARE(t->cursorPosition(), 7);
        t->setSelection(0, 0, 6);
        QCOMPARE(ed->cursor().selectedText(), QStringLiteral("Spring"));
        int a = -1, b = -1;
        t->selection(0, &a, &b);
        QCOMPARE(a, 0);
        QCOMPARE(b, 6);
        QVERIFY(t->characterRect(3).isValid());
        QTest::keyClick(cv, Qt::Key_End);
        QTest::keyClicks(cv, QStringLiteral("!"));
        QCOMPARE(t->text(0, t->characterCount()), QStringLiteral("Spring sale!"));
        // An object deleted goes from the list.
        QTest::keyClick(cv, Qt::Key_Escape);
        ed->select(bare->id);
        ed->deleteSelection();
        names.clear();
        for (int i = 0; i < page->childCount(); ++i)
            if (QAccessibleInterface *c = page->child(i); c && c->object() == nullptr) names << c->text(QAccessible::Name);
        QCOMPARE(names.size(), 2);
    }

    // A screen reader walks the page child by child; the page's list of
    // objects is kept until the publication changes (each child() call made the
    // whole list and looked every object up in the document: 4.9 seconds for
    // 1,500 objects), and still follows additions, deletions, undo, and pages.
    void pageObjectListIsKeptBetweenChanges()
    {
        jp::MainWindow w;
        w.resize(1200, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        jp::Canvas *cv = w.canvas();
        QAccessibleInterface *page = QAccessible::queryAccessibleInterface(cv);
        QVERIFY(page);
        const int own = page->childCount();   // the page's own controls
        for (int i = 0; i < 1500; ++i) {
            auto pic = std::make_shared<jp::PictureItem>();
            pic->rect = QRectF(10 + i % 40, 10 + i / 40, 20, 20);
            pic->altText = QStringLiteral("Picture %1").arg(i);
            ed->surfaceItems().push_back(pic);
        }
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < page->childCount(); ++i) QVERIFY(page->child(i));
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QStringLiteral("walking 1,500 objects took %1 ms").arg(timer.elapsed())));
        QCOMPARE(page->childCount(), own + 1500);
        QCOMPARE(page->child(1499)->text(QAccessible::Name), QStringLiteral("Picture: Picture 1499"));
        // The list follows the publication.
        auto extra = std::make_shared<jp::PictureItem>();
        extra->rect = QRectF(300, 300, 20, 20);
        extra->altText = QStringLiteral("Added");
        ed->addItem(extra);
        QCOMPARE(page->childCount(), own + 1501);
        QCOMPARE(page->child(1500)->text(QAccessible::Name), QStringLiteral("Picture: Added"));
        ed->undo();
        QCOMPARE(page->childCount(), own + 1500);
        ed->redo();
        QCOMPARE(page->childCount(), own + 1501);
        ed->select(extra->id);
        ed->deleteSelection();
        QCOMPARE(page->childCount(), own + 1500);
        ed->insertPages(1, 1, false, false);
        ed->setCurrentPage(1);
        QCOMPARE(page->childCount(), own);
        ed->setCurrentPage(0);
        QCOMPARE(page->childCount(), own + 1500);
    }

    // Every task pane's controls have names too (the panes are made when
    // first opened, so windowControlsAreNamed doesn't see them).
    void taskPaneControlsAreNamed()
    {
        jp::MainWindow w;
        QStringList unnamed;
        for (const char *pane : {"help", "online", "find", "designchecker", "mailmerge", "graphics", "research", "catalog"}) {
            w.showTaskPane(QString::fromLatin1(pane));
            auto *tp = w.findChild<jp::TaskPane *>();
            QVERIFY(tp);
            for (QWidget *c : tp->findChildren<QWidget *>()) {
                if (!(c->focusPolicy() & Qt::TabFocus) || c->focusProxy() || !c->isVisibleTo(tp)) continue;
                QWidget *pw = c->parentWidget();
                if (pw && (qobject_cast<QAbstractItemView *>(pw) || pw->inherits("QComboBoxPrivateContainer"))) continue;
                QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(c);
                // A box that chooses needs a name of its own: some systems
                // read its current choice instead, Windows nothing.
                const bool ownName = qobject_cast<QComboBox *>(c) && c->accessibleName().isEmpty();
                if (!iface || iface->text(QAccessible::Name).trimmed().isEmpty() || ownName)
                    unnamed << QStringLiteral("%1: %2 %3").arg(QLatin1String(pane), QString::fromLatin1(c->metaObject()->className()), c->objectName());
            }
        }
        QVERIFY2(unnamed.isEmpty(), qPrintable(unnamed.join(QStringLiteral("; "))));
    }

    // KeyTips: Alt shows letters on the top row, a tab's letter opens it and
    // shows its controls' letters, a control's letters use it; Escape steps
    // back; Alt held while typing goes straight there; a click ends it.
    void keyTipsUseTheRibbon()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *r = w.findChild<jp::Ribbon *>();
        jp::KeyTips *kt = w.keyTips();
        QVERIFY(kt);
        auto keys = [&] {
            QStringList k;
            for (const auto &p : kt->shown()) k << p.first;
            return k;
        };
        QTest::keyPress(&w, Qt::Key_Alt);
        QTest::keyRelease(&w, Qt::Key_Alt);
        QCOMPARE(kt->level(), jp::KeyTips::Top);
        // The other program's letters (its 2021 version): File F, Home H,
        // Insert N, Page Design P, Mailings M, Review R, View W, Help Y,
        // and the Quick Access Toolbar's buttons numbered.
        QStringList top = keys();
        top.sort();
        QCOMPARE(top.join(QLatin1Char(' ')), QStringLiteral("1 2 3 4 F H M N P R W Y"));
        QTest::keyClick(&w, Qt::Key_H);
        QCOMPARE(kt->level(), jp::KeyTips::InTab);
        QCOMPARE(r->current(), r->tab(QStringLiteral("Home")));
        QVERIFY(keys().contains(QStringLiteral("V")));   // Paste
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) w.grab(QRect(0, 0, 1400, 170)).save(qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/keytips-home.png"));
        for (const auto &p : kt->shown()) QVERIFY(r->current()->isAncestorOf(p.second));
        // Two letters on another tab: View > Show > Rulers (W, then S R).
        QTest::keyClick(&w, Qt::Key_Escape);
        QTest::keyClick(&w, Qt::Key_W);
        QCOMPARE(r->current(), r->tab(QStringLiteral("View")));
        QSignalSpy rulers(w.act(QStringLiteral("view.rulers")), &QAction::triggered);
        QTest::keyClick(&w, Qt::Key_S);
        QCOMPARE(rulers.count(), 0);   // "S" starts several; they wait for the second letter
        QVERIFY(!keys().contains(QStringLiteral("NM")));
        QTest::keyClick(&w, Qt::Key_R);
        QCOMPARE(rulers.count(), 1);
        QCOMPARE(kt->level(), jp::KeyTips::Off);
        // Escape steps back a level at a time.
        QTest::keyPress(&w, Qt::Key_Alt);
        QTest::keyRelease(&w, Qt::Key_Alt);
        QTest::keyClick(&w, Qt::Key_N);
        QCOMPARE(r->current(), r->tab(QStringLiteral("Insert")));
        QTest::keyClick(&w, Qt::Key_Escape);
        QCOMPARE(kt->level(), jp::KeyTips::Top);
        QTest::keyClick(&w, Qt::Key_Escape);
        QCOMPARE(kt->level(), jp::KeyTips::Off);
        // Alt held while typing: straight to the tab.
        QTest::keyPress(&w, Qt::Key_Alt);
        QTest::keyClick(&w, Qt::Key_P, Qt::AltModifier);
        QTest::keyRelease(&w, Qt::Key_Alt);
        QCOMPARE(r->current(), r->tab(QStringLiteral("Page Design")));
        QCOMPARE(kt->level(), jp::KeyTips::InTab);
        // A click puts the letters away.
        QTest::mouseClick(w.findChild<jp::Canvas *>(), Qt::LeftButton);
        QCOMPARE(kt->level(), jp::KeyTips::Off);
        // A box's letters put the keyboard in it, ready to type (Home > Font:
        // F F); while it can't be used (no text selected), the letters stay
        // up and nothing moves.
        QWidget *before = QApplication::focusWidget();
        QTest::keyPress(&w, Qt::Key_Alt);
        QTest::keyRelease(&w, Qt::Key_Alt);
        QTest::keyClick(&w, Qt::Key_H);
        QTest::keyClick(&w, Qt::Key_F);
        QTest::keyClick(&w, Qt::Key_F);
        QCOMPARE(kt->level(), jp::KeyTips::InTab);
        QCOMPARE(QApplication::focusWidget(), before);
        QVERIFY(keys().contains(QStringLiteral("FF")));
        QTest::keyClick(&w, Qt::Key_Escape);
        QTest::keyClick(&w, Qt::Key_Escape);
        QCOMPARE(kt->level(), jp::KeyTips::Off);
        jp::Editor *ed = w.editor();
        ed->addItem(ed->newTextBox(QRectF(72, 72, 200, 100)));
        QTRY_VERIFY(w.findChildren<jp::FontCombo *>().constFirst()->isEnabled());
        QTest::keyPress(&w, Qt::Key_Alt);
        QTest::keyRelease(&w, Qt::Key_Alt);
        QTest::keyClick(&w, Qt::Key_H);
        QTest::keyClick(&w, Qt::Key_F);
        QTest::keyClick(&w, Qt::Key_F);
        QCOMPARE(kt->level(), jp::KeyTips::Off);
        QWidget *typing = QApplication::focusWidget();
        QVERIFY2(typing && (typing->inherits("jp::FontCombo") || (typing->parentWidget() && typing->parentWidget()->inherits("jp::FontCombo"))),
                 typing ? typing->metaObject()->className() : "nothing");
    }

    // Tab belongs to the page: while typing it types a tab, otherwise it
    // selects the next object (Shift+Tab the one before), and the keyboard
    // stays on the page; F6 is the way to the ribbon. Once the ribbon's
    // controls took the keyboard, Tab left the page for the File button.
    void tabStaysOnThePage()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        jp::Editor *ed = w.editor();
        ed->addItem(ed->newTextBox(QRectF(72, 72, 200, 100)));
        ed->addItem(ed->newTextBox(QRectF(72, 300, 200, 100)));
        const auto &items = ed->surfaceItems();
        QCOMPARE(int(items.size()), 2);
        const QString first = items[0]->id, second = items[1]->id;
        auto *canvas = w.findChild<jp::Canvas *>();
        canvas->setFocus();
        ed->select(QStringList{first});
        QTest::keyClick(canvas, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), canvas);
        QCOMPARE(ed->selection(), QStringList{second});
        QTest::keyClick(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(ed->selection(), QStringList{first});
        ed->beginTextEdit(first);
        QTest::keyClicks(canvas, QStringLiteral("a"));
        QTest::keyClick(canvas, Qt::Key_Tab);
        QTest::keyClicks(canvas, QStringLiteral("b"));
        QCOMPARE(QApplication::focusWidget(), canvas);
        QVERIFY2(ed->editDoc() && ed->editDoc()->toPlainText().contains(QStringLiteral("a\tb")), qPrintable(ed->editDoc() ? ed->editDoc()->toPlainText() : QString()));
    }

    // Using the ribbon with the keyboard alone: F6 enters it on the current
    // tab, Right switches to the next tab, Down goes into its controls, the
    // arrow keys move among them, and Escape goes back to the page.
    void ribbonByKeyboard()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *r = w.findChild<jp::Ribbon *>();
        auto *canvas = w.findChild<jp::Canvas *>();
        jp::installFocusRing(qApp);
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, QPoint(5, 5));   // the mouse in use: no ring
        canvas->setFocus();
        QTest::keyClick(&w, Qt::Key_F6);
        QWidget *f = QApplication::focusWidget();
        QVERIFY(f && f->objectName() == QLatin1String("jpRibbonTab"));
        // The first key, though a shortcut, shows where the keyboard went.
        auto *ring = w.findChild<QWidget *>(QStringLiteral("jpFocusRing"));
        QVERIFY(ring && ring->isVisible());
        QVERIFY(ring->geometry().contains(QRect(f->mapTo(&w, QPoint(0, 0)), f->size())));
        QCOMPARE(f->accessibleName(), QStringLiteral("Home"));
        QTest::keyClick(f, Qt::Key_Right);
        QCOMPARE(r->current(), r->tab(QStringLiteral("Insert")));
        f = QApplication::focusWidget();
        QCOMPARE(f->accessibleName(), QStringLiteral("Insert"));
        QTest::keyClick(f, Qt::Key_Down);
        f = QApplication::focusWidget();
        QVERIFY(f && r->current()->isAncestorOf(f));
        QTest::keyClick(f, Qt::Key_Right);
        QWidget *g = QApplication::focusWidget();
        QVERIFY(g && g != f && r->current()->isAncestorOf(g));
        QTest::keyClick(g, Qt::Key_Escape);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(canvas));
    }

    // The keyboard reaches the shared controls: arrow keys move among
    // colors and table cells, Enter picks, and a ring shows the focus while
    // the keyboard is in use (the ribbon's controls drew none and refused
    // the focus).
    void keyboardReachesSharedControls()
    {
        // The nearest control in each direction, in a 3 x 3 grid.
        QWidget grid;
        auto *gl = new QGridLayout(&grid);
        QToolButton *b[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                b[r][c] = new QToolButton(&grid);
                b[r][c]->setFixedSize(30, 30);
                b[r][c]->setFocusPolicy(Qt::TabFocus);
                gl->addWidget(b[r][c], r, c);
            }
        grid.show();
        QVERIFY(QTest::qWaitForWindowExposed(&grid));
        QList<QWidget *> all;
        for (auto &row : b) for (QToolButton *x : row) all << x;
        QCOMPARE(jp::nearestInDirection(b[1][1], Qt::Key_Right, all), static_cast<QWidget *>(b[1][2]));
        QCOMPARE(jp::nearestInDirection(b[1][1], Qt::Key_Left, all), static_cast<QWidget *>(b[1][0]));
        QCOMPARE(jp::nearestInDirection(b[1][1], Qt::Key_Up, all), static_cast<QWidget *>(b[0][1]));
        QCOMPARE(jp::nearestInDirection(b[1][1], Qt::Key_Down, all), static_cast<QWidget *>(b[2][1]));
        QCOMPARE(jp::nearestInDirection(b[1][2], Qt::Key_Right, all), static_cast<QWidget *>(nullptr));

        // Colors: the first has the focus; Right moves; Enter picks.
        {
            auto *pop = new jp::ColorPopup(jp::ColorScheme(), true, QStringLiteral("No Color"));
            jp::ColorRef got;
            bool picked = false;
            connect(pop, &jp::ColorPopup::picked, &grid, [&](const jp::ColorRef &c) { got = c; picked = true; });
            pop->showBelow(&grid);
            QVERIFY(QTest::qWaitForWindowExposed(pop));
            QWidget *first = QApplication::focusWidget();
            QVERIFY(first && first->objectName() == QLatin1String("swatch"));
            QVERIFY(!first->accessibleName().isEmpty());
            QTest::keyClick(first, Qt::Key_Right);
            QWidget *second = QApplication::focusWidget();
            QVERIFY(second && second != first && second->objectName() == QLatin1String("swatch"));
            QTest::keyClick(second, Qt::Key_Return);
            QVERIFY(picked);
            QCOMPARE(got.kind(), jp::ColorRef::Scheme);
        }

        // Table size: arrows choose, Enter inserts.
        {
            jp::TableGrid tg;
            int rows = 0, cols = 0;
            connect(&tg, &jp::TableGrid::picked, &tg, [&](int r, int c) { rows = r; cols = c; });
            tg.show();
            QVERIFY(QTest::qWaitForWindowExposed(&tg));
            for (int k : {Qt::Key_Right, Qt::Key_Right, Qt::Key_Right, Qt::Key_Down}) QTest::keyClick(&tg, Qt::Key(k));
            QTest::keyClick(&tg, Qt::Key_Return);
            QCOMPARE(rows, 2);
            QCOMPARE(cols, 3);
        }

        // A gallery: Enter applies the tile with the focus.
        {
            jp::Gallery g(QSize(20, 20), 3);
            g.setItems({jp::GalleryItem{QStringLiteral("a"), QStringLiteral("First"), QIcon(), QString()},
                        jp::GalleryItem{QStringLiteral("b"), QStringLiteral("Second"), QIcon(), QString()}});
            g.setTitle(QStringLiteral("Styles"));
            QString chosen;
            connect(&g, &jp::Gallery::activated, &g, [&](const QString &id) { chosen = id; });
            g.show();
            QVERIFY(QTest::qWaitForWindowExposed(&g));
            auto *list = g.findChild<QListWidget *>();
            QVERIFY(list && (list->focusPolicy() & Qt::TabFocus));
            QCOMPARE(list->accessibleName(), QStringLiteral("Styles"));
            QCOMPARE(list->item(1)->data(Qt::AccessibleTextRole).toString(), QStringLiteral("Second"));
            list->setFocus();
            list->setCurrentRow(1);
            QTest::keyClick(list, Qt::Key_Return);
            QCOMPARE(chosen, QStringLiteral("b"));
        }

        // The ring: shown around the focused control after a key, gone after a click.
        jp::installFocusRing(qApp);
        grid.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&grid));
        b[2][2]->setFocus(Qt::TabFocusReason);
        QTest::keyClick(b[0][0], Qt::Key_Shift);
        b[0][0]->setFocus(Qt::TabFocusReason);
        QCoreApplication::processEvents();
        auto *ring = grid.findChild<QWidget *>(QStringLiteral("jpFocusRing"));
        QVERIFY(ring && ring->isVisible());
        QVERIFY(ring->geometry().contains(b[0][0]->geometry()));
        QTest::mouseClick(b[2][2], Qt::LeftButton);
        QVERIFY(!ring->isVisible());
    }

    // The Styles gallery's samples are the text as it prints: in a dark
    // interface or high contrast they sit on paper (black samples on a dark
    // tile couldn't be read in Windows' high contrast).
    void styleSamplesReadInDark()
    {
        jp::MainWindow w;
        QListWidgetItem *normal = nullptr;
        QTRY_VERIFY([&] {
            for (auto *g : w.findChildren<jp::Gallery *>())
                for (auto *list : g->findChildren<QListWidget *>())
                    for (int i = 0; i < list->count(); ++i)
                        if (list->item(i)->data(Qt::UserRole).toString() == QLatin1String("Normal")) normal = list->item(i);
            return normal != nullptr;
        }());
        const bool wasDark = jp::uiDark();
        auto paper = [&] {
            const QImage img = normal->icon().pixmap(QSize(76, 46)).toImage().convertToFormat(QImage::Format_ARGB32);
            int white = 0, total = 0;
            for (int y = 2; y < img.height() / 2; ++y)
                for (int x = 4; x < img.width() - 4; ++x, ++total) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.alpha() > 200 && c.lightness() > 230) ++white;
                }
            return double(white) / total;
        };
        jp::setUiDark(true);
        const double dark = paper();
        jp::setUiDark(wasDark);
        QVERIFY2(dark > 0.4, qPrintable(QString::number(dark)));
    }

    // Switching light or dark in Options applies at once: it half-applied
    // (the palette and the parts with colors of their own stayed) until a
    // restart.
    void themeSwitchAppliesNow()
    {
        const QPalette pal = qApp->palette();
        const QString sheet = qApp->styleSheet();
        const bool wasDark = jp::uiDark();
        jp::MainWindow w;
        auto *side = w.findChild<QFrame *>(QStringLiteral("jpSide"));
        auto *pages = w.findChild<jp::PagesPane *>();
        QVERIFY(side && pages);
        for (int choice : {2, 1}) {
            const bool dark = choice == 2;
            jp::applyUiTheme(choice);
            QCOMPARE(jp::uiDark(), dark);
            QVERIFY2((qApp->palette().color(QPalette::Window).lightness() < 128) == dark, qPrintable(qApp->palette().color(QPalette::Window).name()));
            QVERIFY(qApp->styleSheet().contains(dark ? QStringLiteral("#22262d") : QStringLiteral("#ffffff")));
            QVERIFY2(side->styleSheet().contains(dark ? QStringLiteral("#171a1f") : QStringLiteral("#f1f2f5")), qPrintable(side->styleSheet()));
            QVERIFY(pages->styleSheet().contains(dark ? QStringLiteral("#171a1f") : QStringLiteral("#f1f2f5")));
            auto *ribbon = w.findChild<jp::Ribbon *>();
            QVERIFY2((ribbon->palette().color(QPalette::Window).lightness() < 128) == dark, qPrintable(ribbon->palette().color(QPalette::Window).name()));
            if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
                w.resize(1200, 800);
                w.show();
                QTest::qWait(200);
                w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/theme-%1.png").arg(dark ? "dark" : "light"));
            }
        }
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
        jp::setUiDark(wasDark);
        qApp->setPalette(pal);
        qApp->setStyleSheet(sheet);
    }

    // Windows' high contrast: the system's colors everywhere (here the
    // stand-in black theme JP_HIGH_CONTRAST=1 selects), none of JeffPub's.
    void highContrastUsesSystemColors()
    {
        const QPalette pal = qApp->palette();
        const QString sheet = qApp->styleSheet();
        const bool wasDark = jp::uiDark();
        jp::MainWindow w;
        qputenv("JP_HIGH_CONTRAST", "1");
        jp::applyUiTheme(1);
        QVERIFY(jp::uiHighContrast());
        QVERIFY(qApp->styleSheet().isEmpty());
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor(Qt::black));
        QCOMPARE(w.findChild<jp::Ribbon *>()->palette().color(QPalette::Window), QColor(Qt::black));
        QCOMPARE(jp::focusRingColor(), qApp->palette().color(QPalette::Highlight));
        QCOMPARE(jp::uiLine(), QColor(Qt::white));   // borders as strong as the text
        QVERIFY(w.findChild<QFrame *>(QStringLiteral("jpSide"))->styleSheet().contains(QStringLiteral("#000000")));
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) {
            w.resize(1200, 800);
            w.show();
            QTest::qWait(200);
            w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + QStringLiteral("/theme-contrast.png"));
        }
        qputenv("JP_HIGH_CONTRAST", "0");
        jp::applyUiTheme(1);
        QVERIFY(!jp::uiHighContrast());
        QVERIFY(!qApp->styleSheet().isEmpty());
        qunsetenv("JP_HIGH_CONTRAST");
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
        jp::setUiDark(wasDark);
        qApp->setPalette(pal);
        qApp->setStyleSheet(sheet);
    }

    // English plurals come from a small translation of their own: without
    // it, "Added %n page(s)" reads "page(s)". Every plural in it has its
    // forms filled in (lupdate adds new ones empty).
    void englishPlurals()
    {
        QTranslator en;
        QVERIFY(en.load(QStringLiteral(":/i18n/jeffpub_en.qm")));
        QCoreApplication::installTranslator(&en);
        QCOMPARE(QCoreApplication::translate("Dialogs", "Added %n page(s) for the rest of the text.", "", 1), QStringLiteral("Added 1 page for the rest of the text."));
        QCOMPARE(QCoreApplication::translate("Dialogs", "Added %n page(s) for the rest of the text.", "", 3), QStringLiteral("Added 3 pages for the rest of the text."));
        QCoreApplication::removeTranslator(&en);
        QVERIFY(!jp::availableUiLanguages().contains(QStringLiteral("en")));
        QFile ts(QStringLiteral(JP_TEST_DATA "/../../translations/jeffpub_en.ts"));
        QVERIFY(ts.open(QIODevice::ReadOnly));
        const QByteArray xml = ts.readAll();
        QVERIFY2(!xml.contains("type=\"unfinished\"") && !xml.contains("<numerusform></numerusform>"),
                 "translations/jeffpub_en.ts has a plural without its English forms");
    }

    // The ribbon's text, in resources/ribbon.json where lupdate can't see
    // it, is listed for translators in src/app/ribbon_strings.cpp; this
    // keeps that list current (run tools/ribbon_strings.py).
    void ribbonStringsListed()
    {
        QFile json(QStringLiteral(":/ribbon.json"));
        QVERIFY(json.open(QIODevice::ReadOnly));
        QFile listed(QStringLiteral(JP_TEST_DATA "/../../src/app/ribbon_strings.cpp"));
        QVERIFY(listed.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString have = QString::fromUtf8(listed.readAll());
        QStringList missing;
        const QStringList keys = {"name", "text", "launcherTip", "tip", "label", "section", "submenu", "context"};
        std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &v) {
            if (v.isObject()) {
                const QJsonObject o = v.toObject();
                for (auto it = o.begin(); it != o.end(); ++it) {
                    if (keys.contains(it.key()) && it.value().isString() && !it.value().toString().isEmpty()) {
                        const QString lit = QStringLiteral("QT_TRANSLATE_NOOP(\"Ribbon\", \"%1\")").arg(it.value().toString().replace(QLatin1Char('"'), QStringLiteral("\\\"")));
                        if (!have.contains(lit)) missing << it.value().toString();
                    }
                    walk(it.value());
                }
            } else if (v.isArray()) {
                for (const QJsonValue &x : v.toArray()) walk(x);
            }
        };
        walk(QJsonDocument::fromJson(json.readAll()).object());
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("not in ribbon_strings.cpp (run tools/ribbon_strings.py): ") + missing.join(QStringLiteral(", "))));
    }

    // AutoRecover keeps one copy per document and run: two "Cover.pub"
    // files in different folders overwrote each other's copy, copies were
    // never offered after a crash, and never removed.
    // A page fitted to the window at the size where a scroll bar is just
    // needed: the bar came, the page area shrank, the page refitted smaller,
    // the bar went, and so on, a whole processor busy while JeffPub sat idle
    // (Windows' usual window size hit it). Each window size must settle.
    void fittedPageSettles()
    {
        QString err;
        auto doc = importPublisherFile(QStringLiteral(JP_TEST_DATA "/pub/poi-Sample2.pub"), &err);
        QVERIFY2(doc, qPrintable(err));
        jp::MainWindow w;
        w.editor()->setDocument(std::move(doc));
        w.show();
        auto *canvas = w.findChild<jp::Canvas *>();
        QVERIFY(canvas);
        struct Resizes : QObject {
            jp::Canvas *canvas = nullptr;
            int n = 0;
            bool runaway = false;
            bool eventFilter(QObject *, QEvent *e) override
            {
                if (e->type() == QEvent::Resize && ++n > 50 && !runaway) {
                    runaway = true;
                    canvas->zoomToFit(jp::Canvas::Fit::None);   // ends the loop, so the test fails rather than hangs
                }
                return false;
            }
        } resizes;
        resizes.canvas = canvas;
        canvas->viewport()->installEventFilter(&resizes);
        // Heights on both sides of where a bar is just needed, whatever the
        // system's ribbon and font sizes.
        for (int h = 560; h <= 800; ++h) {
            canvas->zoomToFit(jp::Canvas::Fit::WholePage);
            resizes.n = 0;
            w.resize(1000, h);
            for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
            QVERIFY2(!resizes.runaway, qPrintable(QStringLiteral("the page area kept resizing at 1000x%1").arg(h)));
        }
    }

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

        // Recovered work opens under its file's name, marked as recovered
        // in the window only: the publication's own title (saved in it and
        // in its PDFs) became "Cover (Recovered)".
        {
            auto titled = jp::Document::blank(QSizeF(612, 792));
            titled->props.title = QStringLiteral("Spring Newsletter");
            const QString lost = dir.filePath(QStringLiteral("AutoRecover/lost"));
            QDir().mkpath(lost);
            QVERIFY(jp::recovery::write(*titled, lost + QStringLiteral("/Cover 0123abcd.jpub"), QStringLiteral("/docs/Cover.jpub"), QStringLiteral("Cover")));
            const QVector<jp::recovery::Recovered> left = jp::recovery::orphans();
            QCOMPARE(left.size(), 2);   // and the loose Publication1 copy
            const auto it = std::find_if(left.begin(), left.end(), [](const auto &r) { return r.title == QLatin1String("Cover"); });
            QVERIFY(it != left.end());
            jp::MainWindow w;
            QCOMPARE(w.openRecovered(*it), &w);
            QCOMPARE(w.editor()->doc()->props.title, QStringLiteral("Spring Newsletter"));
            QCOMPARE(w.editor()->displayName(), QStringLiteral("Cover"));
            QVERIFY2(w.windowTitle().startsWith(QLatin1String("Cover (Recovered)")), qPrintable(w.windowTitle()));
            QVERIFY(w.editor()->isModified());
            QVERIFY(QFileInfo(w.recoveryCopy()).fileName().startsWith(QLatin1String("Cover ")));   // a second crash still says Cover
            QVERIFY(!QDir(lost).exists());   // the old copy goes once this run has its own
            QVERIFY(w.saveTo(dir.filePath(QStringLiteral("Cover.jpub"))));
            QVERIFY2(!w.windowTitle().contains(QLatin1String("Recovered")), qPrintable(w.windowTitle()));
            QCOMPARE(w.editor()->doc()->props.title, QStringLiteral("Spring Newsletter"));
        }

        // Long names are cut between characters, never inside one.
        const QString emoji = QString(59, QLatin1Char('a')) + QString::fromUtf8("\xF0\x9F\x98\x80");
        QVERIFY(QFileInfo(jp::recovery::copyPath(QString(), emoji, 1)).fileName().isValidUtf16());
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

    // ---- Pictures linked to their files ----

    // A picture file for the linked-picture tests: its left half is `color`
    // and its right half the opposite color; `noisy` makes it large to store.
    static QString linkPicture(const QString &path, const QSize &size, const QColor &color, bool noisy = false)
    {
        QImage img(size, QImage::Format_RGB32);
        img.fill(color);
        QPainter p(&img);
        p.fillRect(QRect(size.width() / 2, 0, size.width() - size.width() / 2, size.height()), QColor(255 - color.red(), 255 - color.green(), 255 - color.blue()));
        p.end();
        if (noisy) {
            QRandomGenerator rng(7);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) img.setPixel(x, y, rng.generate() | 0xff000000);
        }
        img.save(path);
        return path;
    }

    // The page as it prints, picture and all.
    static QImage linkRender(jp::Document *doc)
    {
        jp::LayoutCache cache;
        jp::PaintContext ctx;
        ctx.doc = doc;
        ctx.cache = &cache;
        ctx.opt.output = true;
        return jp::Renderer::renderToImage(ctx, 0, 1.0);
    }

    static bool linkColorsClose(const QColor &a, const QColor &b)
    {
        return std::abs(a.red() - b.red()) < 14 && std::abs(a.green() - b.green()) < 14 && std::abs(a.blue() - b.blue()) < 14;
    }

    static jp::PictureItem *linkFirstPicture(jp::Document *doc)
    {
        for (const auto &it : doc->pages[0]->items)
            if (auto *p = dynamic_cast<jp::PictureItem *>(it.get())) return p;
        return nullptr;
    }

    static QByteArray linkFileBytes(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    // Link to File keeps the file's path and a preview of at most 512 pixels,
    // not the picture; the picture draws from the file, whole, when the
    // publication opens again.
    void linkedPictureDrawsFromFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(1000, 800), QColor(200, 30, 30), true);
        const QByteArray fileBytes = linkFileBytes(file);
        QVERIFY(!fileBytes.isEmpty());
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        jp::Document *doc = w.editor()->doc();
        jp::PictureItem *pic = linkFirstPicture(doc);
        QVERIFY(pic);
        const QString id = pic->imageId;
        const jp::ImageData &img = doc->images[id];
        QVERIFY(img.linked);
        QVERIFY(!img.keepsCopy);
        QCOMPARE(doc->linkStatus(id), jp::LinkStatus::Linked);
        QCOMPARE(img.sourcePath, QFileInfo(file).absoluteFilePath());
        const QImage preview = QImage::fromData(img.preview);
        QVERIFY(!preview.isNull());
        QCOMPARE(std::max(preview.width(), preview.height()), 512);

        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(w.saveTo(path));
        QMap<QString, QByteArray> entries;
        QVERIFY(jp::readZip(linkFileBytes(path), entries));
        QStringList stored;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) stored << it.key();
        QVERIFY2(std::none_of(stored.cbegin(), stored.cend(), [](const QString &k) { return k.startsWith(QLatin1String("images/")); }), qPrintable(stored.join(' ')));
        QVERIFY2(std::any_of(stored.cbegin(), stored.cend(), [&](const QString &k) { return k.startsWith(QLatin1String("previews/") + id + QLatin1Char('.')); }), qPrintable(stored.join(' ')));
        QVERIFY(QFileInfo(path).size() < QFileInfo(file).size() / 4);   // the picture itself isn't in it

        QString err;
        auto back = jp::loadPublication(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->linkStatus(id), jp::LinkStatus::Linked);
        QCOMPARE(back->images[id].sourcePath, QFileInfo(file).absoluteFilePath());
        QCOMPARE(back->images[id].bytes, fileBytes);   // read from the file, not the preview
        QCOMPARE(back->image(id).size(), QSize(1000, 800));
        QCOMPARE(back->imageSize(id), QSize(1000, 800));

        // Drawn from the file: left half red, right half the opposite.
        auto flat = jp::Document::blank(QSizeF(612, 792));
        const QString flatFile = linkPicture(dir.filePath(QStringLiteral("flat.png")), QSize(400, 200), QColor(200, 30, 30));
        auto item = std::make_shared<jp::PictureItem>();
        item->rect = QRectF(100, 100, 400, 200);
        item->imgRect = QRectF(0, 0, 400, 200);
        item->imageId = flat->addLinkedImage(linkFileBytes(flatFile), QStringLiteral("png"), flatFile, false);
        flat->pages[0]->items.push_back(item);
        const QString flatPath = dir.filePath(QStringLiteral("flat.jpub"));
        QVERIFY(jp::savePublication(*flat, flatPath, QImage(), &err));
        auto flatBack = jp::loadPublication(flatPath, &err);
        QVERIFY2(flatBack, qPrintable(err));
        const QImage page = linkRender(flatBack.get());
        QVERIFY(linkColorsClose(QColor(page.pixel(200, 200)), QColor(200, 30, 30)));
        QVERIFY(linkColorsClose(QColor(page.pixel(400, 200)), QColor(55, 225, 225)));
    }

    // Insert and Link keeps the full picture and the link: the picture stays
    // when the file goes, and a changed file replaces the stored copy when
    // the publication opens.
    void insertAndLinkRefreshesStoredCopy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(800, 600), QColor(20, 120, 200));
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::EmbedAndLink);
        const QString id = linkFirstPicture(w.editor()->doc())->imageId;
        QVERIFY(w.editor()->doc()->images[id].linked);
        QVERIFY(w.editor()->doc()->images[id].keepsCopy);
        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(w.saveTo(path));
        QMap<QString, QByteArray> entries;
        QVERIFY(jp::readZip(linkFileBytes(path), entries));
        QStringList keys = entries.keys();
        QVERIFY2(keys.filter(QRegularExpression(QStringLiteral("^images/"))).size() == 1, qPrintable(keys.join(' ')));
        QVERIFY2(keys.filter(QRegularExpression(QStringLiteral("^previews/"))).isEmpty(), qPrintable(keys.join(' ')));

        // The file changes: the stored copy follows when the publication opens.
        const QString oldId = id;
        linkPicture(file, QSize(500, 500), QColor(20, 200, 60));
        QString err;
        auto changed = jp::loadPublication(path, &err);
        QVERIFY2(changed, qPrintable(err));
        QCOMPARE(changed->linkStatus(oldId), jp::LinkStatus::Linked);   // refreshed, so no longer Modified
        QCOMPARE(changed->images[oldId].bytes, linkFileBytes(file));
        QCOMPARE(changed->image(oldId).size(), QSize(500, 500));
        const jp::PictureItem *pic = linkFirstPicture(changed.get());
        QVERIFY(std::abs(pic->imgRect.width() - pic->imgRect.height()) < 0.5);   // not stretched to the old shape

        // The file goes: the stored picture is all there is, and all that's needed.
        QVERIFY(w.saveTo(path));
        QVERIFY(QFile::remove(file));
        auto gone = jp::loadPublication(path, &err);
        QVERIFY2(gone, qPrintable(err));
        QCOMPARE(gone->linkStatus(oldId), jp::LinkStatus::Missing);
        QCOMPARE(gone->image(oldId).size(), QSize(800, 600));
        jp::MainWindow w2;
        w2.show();
        w2.editor()->setDocument(std::move(gone), path);
        w2.showTaskPane(QStringLiteral("designchecker"));
        w2.refreshUi();
        QStringList found;
        for (auto *l : w2.findChildren<QListWidget *>())
            for (int i = 0; i < l->count(); ++i) found << l->item(i)->text();
        QVERIFY2(!found.filter(QStringLiteral("(Page 1)")).isEmpty(), qPrintable(found.join('|')));   // the checker did list things
        QVERIFY2(!found.join('|').contains(QStringLiteral("Linked picture is missing")), qPrintable(found.join('|')));
        QVERIFY2(!found.join('|').contains(QStringLiteral("linked, not embedded")), qPrintable(found.join('|')));
    }

    // The path relative to the publication wins when the publication was
    // moved together with its pictures, even where the old path still exists.
    void linkedPictureMovedWithPublication()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("a/pictures")));
        const QString file = linkPicture(root.filePath(QStringLiteral("a/pictures/p.png")), QSize(400, 300), QColor(200, 30, 30));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto item = std::make_shared<jp::PictureItem>();
        item->rect = QRectF(100, 100, 400, 300);
        item->imgRect = QRectF(0, 0, 400, 300);
        item->imageId = doc->addLinkedImage(linkFileBytes(file), QStringLiteral("png"), file, false);
        const QString id = item->imageId;
        doc->pages[0]->items.push_back(item);
        QString err;
        QVERIFY(jp::savePublication(*doc, root.filePath(QStringLiteral("a/book.jpub")), QImage(), &err));

        // Moved together: the copy in b has other contents, and is the one found.
        QVERIFY(root.mkpath(QStringLiteral("b/pictures")));
        QVERIFY(QFile::copy(root.filePath(QStringLiteral("a/book.jpub")), root.filePath(QStringLiteral("b/book.jpub"))));
        const QString moved = linkPicture(root.filePath(QStringLiteral("b/pictures/p.png")), QSize(400, 300), QColor(30, 30, 200));
        auto back = jp::loadPublication(root.filePath(QStringLiteral("b/book.jpub")), &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->images[id].sourcePath, QFileInfo(moved).absoluteFilePath());
        QCOMPARE(back->images[id].bytes, linkFileBytes(moved));
        QCOMPARE(back->linkStatus(id), jp::LinkStatus::Modified);   // not the file it was made from

        // Not moved: the full path is still the link, but the file is outside the folder
        // the publication is in now, so it is not looked at (see linksOutsideTheFolderAreNotFollowed).
        QVERIFY(root.mkpath(QStringLiteral("c")));
        QVERIFY(QFile::copy(root.filePath(QStringLiteral("a/book.jpub")), root.filePath(QStringLiteral("c/book.jpub"))));
        auto stayed = jp::loadPublication(root.filePath(QStringLiteral("c/book.jpub")), &err);
        QVERIFY2(stayed, qPrintable(err));
        QCOMPARE(stayed->images[id].sourcePath, QFileInfo(file).absoluteFilePath());
        QCOMPARE(stayed->linkStatus(id), jp::LinkStatus::NotUpdated);

        // Saved somewhere else, the link is kept relative to the new place.
        QVERIFY(jp::savePublication(*stayed, root.filePath(QStringLiteral("c/again.jpub")), QImage(), &err));
        QMap<QString, QByteArray> entries;
        QVERIFY(jp::readZip(linkFileBytes(root.filePath(QStringLiteral("c/again.jpub"))), entries));
        const QJsonObject json = QJsonDocument::fromJson(entries["document.json"]).object();
        QCOMPARE(json["images"].toArray()[0].toObject()["relative"].toString(), QStringLiteral("../a/pictures/p.png"));
    }

    // A file that has gone leaves the preview on the page; the picture is
    // Missing, its size is still the file's, and the Design Checker says so.
    void linkedPictureMissingShowsPreview()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(1200, 900), QColor(200, 30, 30));
        jp::MainWindow w;
        w.show();
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(w.saveTo(path));
        const QString id = linkFirstPicture(w.editor()->doc())->imageId;
        QVERIFY(QFile::remove(file));
        QCOMPARE(w.editor()->doc()->linkStatus(id), jp::LinkStatus::Missing);   // gone while the publication is open

        QString err;
        auto back = jp::loadPublication(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->linkStatus(id), jp::LinkStatus::Missing);
        QCOMPARE(back->imageSize(id), QSize(1200, 900));
        const QImage shown = back->image(id);
        QVERIFY(!shown.isNull());
        QCOMPARE(std::max(shown.width(), shown.height()), 512);   // the preview
        // ... and it is what the page shows.
        jp::PictureItem *pic = linkFirstPicture(back.get());
        const QImage page = linkRender(back.get());
        const QPoint left = (pic->rect.topLeft() + QPointF(pic->rect.width() * 0.25, pic->rect.height() * 0.5)).toPoint();
        const QPoint right = (pic->rect.topLeft() + QPointF(pic->rect.width() * 0.75, pic->rect.height() * 0.5)).toPoint();
        QVERIFY(linkColorsClose(QColor(page.pixel(left)), QColor(200, 30, 30)));
        QVERIFY(linkColorsClose(QColor(page.pixel(right)), QColor(55, 225, 225)));

        // The Design Checker names both problems.
        w.editor()->setDocument(std::move(back), path);
        w.showTaskPane(QStringLiteral("designchecker"));
        w.refreshUi();
        QStringList found;
        for (auto *l : w.findChildren<QListWidget *>())
            for (int i = 0; i < l->count(); ++i) found << l->item(i)->text();
        QVERIFY2(found.contains(QStringLiteral("Linked picture is missing (Page 1)")), qPrintable(found.join('|')));
        QVERIFY2(found.contains(QStringLiteral("Picture is linked, not embedded (Page 1)")), qPrintable(found.join('|')));
    }

    // A changed file shows as Modified, in the Graphics Manager too, until
    // Update Link reads it again; Update Link can be undone.
    void linkedPictureModifiedUpdates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(800, 600), QColor(200, 30, 30));
        jp::MainWindow w;
        w.show();
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        jp::Document *doc = w.editor()->doc();
        jp::PictureItem *pic = linkFirstPicture(doc);
        const QString oldId = pic->imageId;
        QCOMPARE(doc->linkStatus(oldId), jp::LinkStatus::Linked);

        // Changed while open: Modified, still drawn as it was, then updated.
        linkPicture(file, QSize(600, 300), QColor(30, 200, 30));
        QCOMPARE(doc->linkStatus(oldId), jp::LinkStatus::Modified);
        QCOMPARE(doc->image(oldId).size(), QSize(800, 600));
        w.editor()->select(pic->id);
        w.showTaskPane(QStringLiteral("graphics"));
        w.refreshUi();
        QListWidget *list = nullptr;
        for (auto *l : w.findChildren<QListWidget *>())
            if (l->accessibleName() == QLatin1String("Pictures")) list = l;
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QVERIFY2(list->item(0)->text().contains(QStringLiteral("Modified")), qPrintable(list->item(0)->text()));
        list->setCurrentRow(0);
        QPushButton *update = nullptr;
        for (auto *b : w.findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Update Link")) update = b;
        QVERIFY(update);
        QVERIFY(update->isEnabled());
        update->click();
        w.refreshUi();
        pic = linkFirstPicture(doc);
        QVERIFY(pic->imageId != oldId);
        QCOMPARE(doc->linkStatus(pic->imageId), jp::LinkStatus::Linked);
        QCOMPARE(doc->image(pic->imageId).size(), QSize(600, 300));
        QVERIFY(std::abs(pic->imgRect.width() / pic->imgRect.height() - 2.0) < 0.02);   // the new shape, not the old one
        QVERIFY2(list->item(0)->text().contains(QStringLiteral("Linked")), qPrintable(list->item(0)->text()));
        QCOMPARE(doc->images[pic->imageId].preview.isEmpty(), false);
        QCOMPARE(QImage::fromData(doc->images[pic->imageId].preview).size(), QSize(512, 256));   // the preview follows the file

        w.editor()->undo();
        QCOMPARE(linkFirstPicture(doc)->imageId, oldId);
        QCOMPARE(doc->linkStatus(oldId), jp::LinkStatus::Modified);

        // Changed while closed: the picture draws from the file, Modified until updated.
        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(w.saveTo(path));
        linkPicture(file, QSize(300, 600), QColor(30, 30, 200));
        QString err;
        auto back = jp::loadPublication(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->linkStatus(oldId), jp::LinkStatus::Modified);
        QCOMPARE(back->image(oldId).size(), QSize(300, 600));
    }

    // Embed Picture turns a link into a stored copy that outlives the file;
    // Change Link points the picture at another file.
    void embedAndChangeLinkedPicture()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(800, 600), QColor(200, 30, 30), true);
        const QString other = linkPicture(dir.filePath(QStringLiteral("other.png")), QSize(400, 400), QColor(30, 30, 200));
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        jp::Document *doc = w.editor()->doc();
        const QString picId = linkFirstPicture(doc)->id;
        const QString linkedId = linkFirstPicture(doc)->imageId;

        QVERIFY(w.editor()->changeLink(picId, other));
        QCOMPARE(doc->images[linkFirstPicture(doc)->imageId].sourcePath, QFileInfo(other).absoluteFilePath());
        QCOMPARE(doc->image(linkFirstPicture(doc)->imageId).size(), QSize(400, 400));
        QVERIFY(!w.editor()->changeLink(picId, dir.filePath(QStringLiteral("nothing.png"))));   // no such file: nothing changes
        QCOMPARE(doc->images[linkFirstPicture(doc)->imageId].sourcePath, QFileInfo(other).absoluteFilePath());
        w.editor()->undo();
        QCOMPARE(linkFirstPicture(doc)->imageId, linkedId);

        QVERIFY(w.editor()->embedPicture(picId));
        const QString embeddedId = linkFirstPicture(doc)->imageId;
        QVERIFY(embeddedId != linkedId);
        QVERIFY(!doc->images[embeddedId].linked);
        QCOMPARE(doc->linkStatus(embeddedId), jp::LinkStatus::Embedded);
        QCOMPARE(doc->images[embeddedId].bytes, linkFileBytes(file));
        QVERIFY(!w.editor()->embedPicture(picId));   // nothing left to embed

        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(w.saveTo(path));
        QVERIFY(QFile::remove(file));
        QString err;
        auto back = jp::loadPublication(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->linkStatus(embeddedId), jp::LinkStatus::Embedded);
        QCOMPARE(back->image(embeddedId).size(), QSize(800, 600));

        w.editor()->undo();   // back to the link
        QCOMPARE(linkFirstPicture(doc)->imageId, linkedId);
        QCOMPARE(doc->linkStatus(linkedId), jp::LinkStatus::Missing);
    }

    // Saving to .pub embeds the full picture from the file; when the file is
    // gone, the preview.
    void pubExportEmbedsLinkedPictures()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(1000, 800), QColor(200, 30, 30), true);
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(w.saveTo(path));

        QString err;
        auto exportedSize = [&](jp::Document *doc, const QString &name) {
            const QString pub = dir.filePath(name);
            if (!jp::exportPublisher(*doc, pub, &err)) return QSize();
            auto back = jp::importPublisherFile(pub, &err);
            if (!back) return QSize();
            for (const auto &it : back->pages[0]->items)
                if (auto *p = dynamic_cast<jp::PictureItem *>(it.get())) return back->image(p->imageId).size();
            return QSize();
        };
        auto present = jp::loadPublication(path, &err);
        QVERIFY2(present, qPrintable(err));
        QCOMPARE(exportedSize(present.get(), QStringLiteral("full.pub")), QSize(1000, 800));

        QVERIFY(QFile::remove(file));
        auto gone = jp::loadPublication(path, &err);
        QVERIFY2(gone, qPrintable(err));
        const QSize small = exportedSize(gone.get(), QStringLiteral("preview.pub"));
        QVERIFY2(!small.isEmpty() && std::max(small.width(), small.height()) == 512, qPrintable(err));
    }

    // Pack and Go and e-mail send the pictures themselves, not links to files
    // the other computer doesn't have.
    void sharedPublicationsEmbedLinkedPictures()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(900, 700), QColor(200, 30, 30));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto item = std::make_shared<jp::PictureItem>();
        item->rect = QRectF(100, 100, 400, 300);
        item->imgRect = QRectF(0, 0, 400, 300);
        item->imageId = doc->addLinkedImage(linkFileBytes(file), QStringLiteral("png"), file, false);
        const QString id = item->imageId;
        doc->pages[0]->items.push_back(item);
        QString err;
        const QByteArray sent = jp::publicationBytes(*doc, QImage(), QString(), true);
        QVERIFY(QFile::remove(file));
        auto there = jp::publicationFromBytes(sent, &err);
        QVERIFY2(there, qPrintable(err));
        QCOMPARE(there->linkStatus(id), jp::LinkStatus::NotUpdated);   // opened with no folder of its own: no file is looked at
        QCOMPARE(there->image(id).size(), QSize(900, 700));   // whole, not the preview
    }

    // Files from before pictures could be linked have the same pictures,
    // embedded, and a picture that isn't linked writes nothing about links.
    void embeddedPicturesWriteNoLinkFields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(300, 200), QColor(200, 30, 30));
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1));
        const QString id = linkFirstPicture(w.editor()->doc())->imageId;
        const QJsonObject io = w.editor()->doc()->toJson()["images"].toArray()[0].toObject();
        QStringList keys = io.keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{"format", "h", "id", "linked", "source", "w"}));
        QString err;
        auto back = jp::publicationFromBytes(jp::publicationBytes(*w.editor()->doc(), QImage()), &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->linkStatus(id), jp::LinkStatus::Embedded);
        QVERIFY(!back->images[id].linked);
        QCOMPARE(back->images[id].bytes, linkFileBytes(file));
        // An embedded copy of a file that is also linked stays a separate picture.
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        w.insertFiles({file}, QPointF(-1, -1));
        QSet<QString> ids;
        for (const auto &it : w.editor()->doc()->pages[0]->items)
            if (auto *p = dynamic_cast<jp::PictureItem *>(it.get())) ids.insert(p->imageId);
        QCOMPARE(ids.size(), 2);
    }

    // Insert Picture's file dialog has an "Insert as" choice with a name, and
    // the choice decides how the picture joins the publication.
    void insertPictureDialogOffersLinking()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(300, 200), QColor(200, 30, 30));
        jp::Settings::get().setValue(QStringLiteral("dirs/pictures"), dir.path());
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QString name;
        QStringList choices;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            auto *dlg = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dlg) return;
            poll.stop();
            if (auto *how = dlg->findChild<QComboBox *>(QStringLiteral("insertAs"))) {
                name = how->accessibleName();
                for (int i = 0; i < how->count(); ++i) choices << how->itemText(i);
                how->setCurrentIndex(2);
            }
            if (auto *typed = dlg->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) typed->setText(file);
            QMetaObject::invokeMethod(dlg, "accept");
        });
        poll.start();
        w.insertPictureFromFile();
        poll.stop();
        QCOMPARE(name, QStringLiteral("Insert as"));
        QCOMPARE(choices, (QStringList{"Insert", "Link to File", "Insert and Link"}));
        jp::PictureItem *pic = linkFirstPicture(w.editor()->doc());
        QVERIFY(pic);
        const jp::ImageData &img = w.editor()->doc()->images[pic->imageId];
        QVERIFY(img.linked);
        QVERIFY(img.keepsCopy);
    }

    // A linked picture that is copied and pasted stays linked to its file.
    void copiedLinkedPictureStaysLinked()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = linkPicture(dir.filePath(QStringLiteral("photo.png")), QSize(300, 200), QColor(200, 30, 30));
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        jp::Document *doc = w.editor()->doc();
        const QString id = linkFirstPicture(doc)->imageId;
        w.editor()->select(linkFirstPicture(doc)->id);
        w.editor()->copy();
        w.editor()->paste();
        QCOMPARE(int(doc->pages[0]->items.size()), 2);
        for (const auto &it : doc->pages[0]->items) {
            auto *p = dynamic_cast<jp::PictureItem *>(it.get());
            QVERIFY(p);
            QVERIFY(doc->images[p->imageId].linked);
            QCOMPARE(p->imageId, id);
        }
    }

    // A picture file is read only when it is a regular file of at most
    // kMaxPictureFile bytes: not a pipe or a device, not an empty file, not a
    // huge one. Every way in (open, Insert, Update Link, Change Link) uses it.
    void pictureFilesAreReadSafely()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString good = linkPicture(dir.filePath(QStringLiteral("good.png")), QSize(40, 30), QColor(200, 30, 30));
        auto read = [](const QString &path) {
            QByteArray bytes;
            QString format;
            QSize px;
            return jp::readPictureFile(path, 0, &bytes, &format, &px);
        };
        QVERIFY(read(good));
        QVERIFY(!read(dir.filePath(QStringLiteral("nothing.png"))));
        QVERIFY(!read(dir.path()));   // a folder
        QFile empty(dir.filePath(QStringLiteral("empty.png")));
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close();
        QVERIFY(!read(empty.fileName()));
        // A real picture followed by zeros, one byte past the limit: too big, though it would decode.
        QByteArray padded = linkFileBytes(good);
        QFile big(dir.filePath(QStringLiteral("big.png")));
        QVERIFY(big.open(QIODevice::WriteOnly));
        QVERIFY(big.write(padded) == padded.size());
        QVERIFY(big.resize(jp::kMaxPictureFile + 1));
        big.close();
        QVERIFY(!read(big.fileName()));
        QVERIFY(big.resize(jp::kMaxPictureFile));   // exactly the limit is still fine
        QVERIFY(read(big.fileName()));
        QVERIFY(big.remove());
#ifdef Q_OS_UNIX
        // A named pipe would block the first open of it forever, and a file the
        // system reports as empty can be endless.
        const QString fifo = dir.filePath(QStringLiteral("pipe.png"));
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QVERIFY(!read(fifo));
        QVERIFY(!read(QStringLiteral("/dev/zero")));
        QVERIFY(!read(QStringLiteral("/dev/null")));
        if (QFileInfo::exists(QStringLiteral("/proc/self/maps"))) QVERIFY(!read(QStringLiteral("/proc/self/maps")));   // a file of size 0

        // Insert, Change Link, and Update Link on it do nothing, and don't wait.
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({fifo}, QPointF(-1, -1), jp::PictureInsert::Embed);
        QVERIFY(!linkFirstPicture(w.editor()->doc()));
        w.insertFiles({good}, QPointF(-1, -1), jp::PictureInsert::Link);
        jp::PictureItem *pic = linkFirstPicture(w.editor()->doc());
        QVERIFY(pic);
        const QString picId = pic->id, imageId = pic->imageId;
        QVERIFY(!w.editor()->changeLink(picId, fifo));
        QVERIFY(QFile::remove(good));
        QVERIFY(::mkfifo(QFile::encodeName(good).constData(), 0600) == 0);   // the linked file becomes a pipe
        QCOMPARE(w.editor()->doc()->linkStatus(imageId), jp::LinkStatus::Missing);
        QVERIFY(!w.editor()->updateLink(picId));
        QCOMPARE(linkFirstPicture(w.editor()->doc())->imageId, imageId);
#endif
    }

    // A .jpub with its image entries changed by `edit` (a crafted file).
    static QByteArray rewriteImageEntries(const QByteArray &jpub, const std::function<void(QJsonArray &)> &edit)
    {
        QMap<QString, QByteArray> entries;
        if (!jp::readZip(jpub, entries)) return QByteArray();
        QJsonObject json = QJsonDocument::fromJson(entries["document.json"]).object();
        QJsonArray images = json["images"].toArray();
        edit(images);
        json["images"] = images;
        entries["document.json"] = QJsonDocument(json).toJson(QJsonDocument::Compact);
        jp::ZipWriter z;
        z.add(QStringLiteral("mimetype"), entries.take(QStringLiteral("mimetype")));
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) z.add(it.key(), it.value());
        return z.finish();
    }

    // A picture as file bytes: the left half `color`, the right half the opposite.
    static QByteArray linkPictureBytes(const QSize &size, const QColor &color)
    {
        QTemporaryDir tmp;
        const QString path = linkPicture(tmp.filePath(QStringLiteral("p.png")), size, color);
        return linkFileBytes(path);
    }

    static void linkWrite(const QString &path, const QByteArray &bytes, qint64 msecs)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write(bytes) == bytes.size());
        QVERIFY(f.flush());   // closing the file later must not touch its date again
        QVERIFY(f.setFileTime(QDateTime::fromMSecsSinceEpoch(msecs), QFileDevice::FileModificationTime));
    }

    // A linked picture is Modified when the file's contents differ, not when
    // its size and date do: two different files can share both (written in
    // one clock tick), and a file saved again unchanged differs in neither
    // way that matters.
    void linkedPictureModifiedByContent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray red = linkPictureBytes(QSize(100, 80), QColor(200, 30, 30));
        QByteArray blue = linkPictureBytes(QSize(100, 80), QColor(30, 30, 200));
        // Zeros after the picture's end change nothing in it: the two files get one size.
        const qsizetype size = std::max(red.size(), blue.size());
        red.append(QByteArray(size - red.size(), '\0'));
        blue.append(QByteArray(size - blue.size(), '\0'));
        QVERIFY(red != blue && red.size() == blue.size());
        const qint64 then = 1700000000000;
        const QString file = dir.filePath(QStringLiteral("photo.png"));
        linkWrite(file, red, then);

        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto item = std::make_shared<jp::PictureItem>();
        item->rect = QRectF(100, 100, 200, 160);
        item->imgRect = QRectF(0, 0, 200, 160);
        item->imageId = doc->addLinkedImage(red, QStringLiteral("png"), file, false);
        const QString id = item->imageId;
        doc->pages[0]->items.push_back(item);
        QCOMPARE(doc->images[id].fileHash, QCryptographicHash::hash(red, QCryptographicHash::Sha1).toHex());
        QString err;
        const QString path = dir.filePath(QStringLiteral("book.jpub"));
        QVERIFY(jp::savePublication(*doc, path, QImage(), &err));
        QMap<QString, QByteArray> entries;
        QVERIFY(jp::readZip(linkFileBytes(path), entries));
        QCOMPARE(QJsonDocument::fromJson(entries["document.json"]).object()["images"].toArray()[0].toObject()["fileHash"].toString().toLatin1(),
                 QCryptographicHash::hash(red, QCryptographicHash::Sha1).toHex());

        auto same = jp::loadPublication(path, &err);
        QVERIFY2(same, qPrintable(err));
        QCOMPARE(same->linkStatus(id), jp::LinkStatus::Linked);

        // Another picture, the same size, the same date.
        linkWrite(file, blue, then);
        QCOMPARE(QFileInfo(file).size(), qint64(size));
        auto other = jp::loadPublication(path, &err);
        QVERIFY2(other, qPrintable(err));
        QCOMPARE(other->linkStatus(id), jp::LinkStatus::Modified);
        QCOMPARE(other->images[id].bytes, blue);   // drawn from the file until updated
        const jp::PictureItem *shown = linkFirstPicture(other.get());
        const QImage page = linkRender(other.get());
        const QPoint left = (shown->rect.topLeft() + QPointF(shown->rect.width() * 0.25, shown->rect.height() * 0.5)).toPoint();
        QVERIFY(linkColorsClose(QColor(page.pixel(left)), QColor(30, 30, 200)));

        // The same picture, saved again a minute later: still the one that was linked.
        linkWrite(file, red, then + 60000);
        auto touched = jp::loadPublication(path, &err);
        QVERIFY2(touched, qPrintable(err));
        QCOMPARE(touched->linkStatus(id), jp::LinkStatus::Linked);

        // A file from before links had a hash has only the size and date to go by.
        const QByteArray old = rewriteImageEntries(linkFileBytes(path), [](QJsonArray &images) {
            QJsonObject io = images[0].toObject();
            io.remove(QStringLiteral("fileHash"));
            images[0] = io;
        });
        QVERIFY(!old.isEmpty());
        linkWrite(file, blue, then);
        auto before = jp::publicationFromBytes(old, &err, dir.path());
        QVERIFY2(before, qPrintable(err));
        QCOMPARE(before->linkStatus(id), jp::LinkStatus::Linked);   // the same size and date: no way to tell
        linkWrite(file, blue + QByteArray(10, '\0'), then);
        auto longer = jp::publicationFromBytes(old, &err, dir.path());
        QVERIFY2(longer, qPrintable(err));
        QCOMPARE(longer->linkStatus(id), jp::LinkStatus::Modified);

        // Update Link keeps the hash of what it read.
        jp::MainWindow w;
        w.editor()->setDocument(std::move(other), path);
        const QString picId = linkFirstPicture(w.editor()->doc())->id;
        linkWrite(file, blue, then);
        QVERIFY(w.editor()->updateLink(picId));
        const jp::ImageData &updated = w.editor()->doc()->images[linkFirstPicture(w.editor()->doc())->imageId];
        QCOMPARE(updated.fileHash, QCryptographicHash::hash(blue, QCryptographicHash::Sha1).toHex());
    }

    // A publication that arrives from someone else must not make JeffPub
    // reach for files of the computer it opens on. A link to a file in the
    // publication's own folder or below is followed when it opens; any other
    // (elsewhere, up a `..`, over a symbolic link, a network or device path)
    // is not touched at all, not even to ask if it is there: the page shows
    // the stored copy or preview, and the status is Not updated.
    void linksOutsideTheFolderAreNotFollowed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("pub/sub")) && root.mkpath(QStringLiteral("pub2")) && root.mkpath(QStringLiteral("outside")));
        const QString pub = root.filePath(QStringLiteral("pub"));
        const QSize big(1000, 800);
        const QString inside = linkPicture(pub + QStringLiteral("/sub/inside.png"), big, QColor(200, 30, 30));
        const QString secret = linkPicture(root.filePath(QStringLiteral("outside/secret.png")), big, QColor(30, 30, 200));
        const QString sibling = linkPicture(root.filePath(QStringLiteral("pub2/near.png")), big, QColor(30, 200, 30));   // "pub" is the start of "pub2"
        auto doc = jp::Document::blank(QSizeF(612, 792));
        int row = 0;
        auto link = [&](const QString &file) {
            auto item = std::make_shared<jp::PictureItem>();
            item->rect = QRectF(50, 50 + 100 * row++, 100, 80);
            item->imgRect = QRectF(0, 0, 100, 80);
            item->imageId = doc->addLinkedImage(linkFileBytes(file), QStringLiteral("png"), file, false);
            doc->pages[0]->items.push_back(item);
            return item->imageId;
        };
        const QString idInside = link(inside), idSecret = link(secret), idSibling = link(sibling);
        QString err;
        const QString path = pub + QStringLiteral("/book.jpub");
        QVERIFY(jp::savePublication(*doc, path, QImage(), &err));
        const QByteArray saved = linkFileBytes(path);

        // Each file changes after the link was made, so that a file that is read shows.
        for (const QString &f : {inside, secret, sibling}) linkPicture(f, big, QColor(250, 250, 10));
        auto isStored = [&](const jp::Document &d, const QString &id) {   // the preview, not the file
            return d.images[id].bytes != linkFileBytes(inside) && d.images[id].bytes != linkFileBytes(secret) && d.images[id].bytes != linkFileBytes(sibling) &&
                   std::max(d.image(id).width(), d.image(id).height()) == 512;
        };
        auto loaded = jp::publicationFromBytes(saved, &err, pub);
        QVERIFY2(loaded, qPrintable(err));
        QCOMPARE(loaded->linkStatus(idInside), jp::LinkStatus::Modified);   // in the folder: read, and found changed
        QCOMPARE(loaded->images[idInside].bytes, linkFileBytes(inside));
        QCOMPARE(loaded->linkStatus(idSecret), jp::LinkStatus::NotUpdated);
        QVERIFY(isStored(*loaded, idSecret));
        QCOMPARE(loaded->linkStatus(idSibling), jp::LinkStatus::NotUpdated);
        QVERIFY(isStored(*loaded, idSibling));
        QCOMPARE(loaded->imageSize(idSecret), big);   // the file's size is still known
        // The page shows the preview of the outside picture (the second): its left half is blue, not the yellow of the file.
        const QImage page = linkRender(loaded.get());
        QVERIFY(linkColorsClose(QColor(page.pixel(75, 190)), QColor(30, 30, 200)));

        // Without a folder to be in (a publication from a message, say), no link is followed.
        auto nowhere = jp::publicationFromBytes(saved, &err);
        QVERIFY2(nowhere, qPrintable(err));
        QCOMPARE(nowhere->linkStatus(idInside), jp::LinkStatus::NotUpdated);
        QVERIFY(isStored(*nowhere, idInside));

        // Paths a file can name that reach other computers and devices, and
        // `..`, absolute, and bad relative ones. None of them is there, and
        // none is looked for: Not updated, never Missing.
        const QStringList sources{QStringLiteral("\\\\server\\share\\x.png"), QStringLiteral("//server/share/x.png"), QStringLiteral("\\\\?\\C:\\x.png"),
                                  QStringLiteral("\\\\?\\UNC\\server\\share\\x.png"), QStringLiteral("\\\\.\\pipe\\x"), QStringLiteral("\\\\.\\C:"),
                                  pub + QStringLiteral("/../outside/secret.png"), pub + QStringLiteral("/sub/../../outside/nothing.png"),
                                  QStringLiteral("pictures/x.png"), QStringLiteral("/nonexistent-folder/x.png"), QStringLiteral("/dev/zero"),
                                  QStringLiteral("/proc/self/pagemap"), QString()};
        for (const QString &source : sources) {
            const QByteArray crafted = rewriteImageEntries(saved, [&](QJsonArray &images) {
                for (int i = 0; i < images.size(); ++i) {
                    QJsonObject io = images[i].toObject();
                    if (io["id"].toString() != idSecret) continue;
                    io["source"] = source;
                    io.remove(QStringLiteral("relative"));
                    images[i] = io;
                }
            });
            auto d = jp::publicationFromBytes(crafted, &err, pub);
            QVERIFY2(d, qPrintable(err));
            QVERIFY2(d->linkStatus(idSecret) == jp::LinkStatus::NotUpdated, qPrintable(source));
            QVERIFY2(isStored(*d, idSecret), qPrintable(source));
        }
        // A `relative` that leaves the folder, or is absolute, is not followed either.
        for (const QString &relative : {QStringLiteral("../outside/secret.png"), root.filePath(QStringLiteral("outside/secret.png")), QStringLiteral("\\\\server\\share\\x.png"),
                                        QStringLiteral("//server/share/x.png"), QStringLiteral("sub/../../outside/secret.png"), QStringLiteral("C:\\x.png")}) {
            const QByteArray crafted = rewriteImageEntries(saved, [&](QJsonArray &images) {
                for (int i = 0; i < images.size(); ++i) {
                    QJsonObject io = images[i].toObject();
                    if (io["id"].toString() != idSecret) continue;
                    io["source"] = QString();
                    io["relative"] = relative;
                    images[i] = io;
                }
            });
            auto d = jp::publicationFromBytes(crafted, &err, pub);
            QVERIFY2(d, qPrintable(err));
            QVERIFY2(d->linkStatus(idSecret) == jp::LinkStatus::NotUpdated, qPrintable(relative));
            QVERIFY2(isStored(*d, idSecret), qPrintable(relative));
        }
        // A relative path inside the folder still finds the file when the full path is stale.
        const QByteArray moved = rewriteImageEntries(saved, [&](QJsonArray &images) {
            for (int i = 0; i < images.size(); ++i) {
                QJsonObject io = images[i].toObject();
                if (io["id"].toString() != idInside) continue;
                io["source"] = QStringLiteral("/somewhere/else/inside.png");
                io["relative"] = QStringLiteral("sub/inside.png");
                images[i] = io;
            }
        });
        auto found = jp::publicationFromBytes(moved, &err, pub);
        QVERIFY2(found, qPrintable(err));
        QCOMPARE(found->images[idInside].sourcePath, QFileInfo(inside).absoluteFilePath());
        QCOMPARE(found->linkStatus(idInside), jp::LinkStatus::Modified);
        // A `..` that comes back down stays inside.
        const QByteArray round = rewriteImageEntries(saved, [&](QJsonArray &images) {
            for (int i = 0; i < images.size(); ++i) {
                QJsonObject io = images[i].toObject();
                if (io["id"].toString() != idInside) continue;
                io["source"] = pub + QStringLiteral("/sub/../sub/inside.png");
                io.remove(QStringLiteral("relative"));
                images[i] = io;
            }
        });
        auto roundDown = jp::publicationFromBytes(round, &err, pub);
        QVERIFY2(roundDown, qPrintable(err));
        QCOMPARE(roundDown->linkStatus(idInside), jp::LinkStatus::Modified);
        QCOMPARE(roundDown->images[idInside].bytes, linkFileBytes(inside));

#ifndef Q_OS_WIN
        // A symbolic link in the folder that leads out of it is outside; one that stays in is not.
        QVERIFY(QFile::link(secret, pub + QStringLiteral("/escape.png")));
        QVERIFY(QFile::link(root.filePath(QStringLiteral("outside")), pub + QStringLiteral("/escapedir")));
        QVERIFY(QFile::link(inside, pub + QStringLiteral("/alias.png")));
        QVERIFY(QFile::link(pub + QStringLiteral("/sub"), pub + QStringLiteral("/aliasdir")));
        struct Case { QString source; jp::LinkStatus status; };
        for (const Case &c : {Case{pub + QStringLiteral("/escape.png"), jp::LinkStatus::NotUpdated}, Case{pub + QStringLiteral("/escapedir/secret.png"), jp::LinkStatus::NotUpdated},
                              Case{pub + QStringLiteral("/alias.png"), jp::LinkStatus::Modified}, Case{pub + QStringLiteral("/aliasdir/inside.png"), jp::LinkStatus::Modified}}) {
            const QByteArray crafted = rewriteImageEntries(saved, [&](QJsonArray &images) {
                for (int i = 0; i < images.size(); ++i) {
                    QJsonObject io = images[i].toObject();
                    if (io["id"].toString() != idSecret) continue;
                    io["source"] = c.source;
                    io.remove(QStringLiteral("relative"));
                    images[i] = io;
                }
            });
            auto d = jp::publicationFromBytes(crafted, &err, pub);
            QVERIFY2(d, qPrintable(err));
            QVERIFY2(d->linkStatus(idSecret) == c.status, qPrintable(c.source));
            QCOMPARE(d->images[idSecret].bytes == linkFileBytes(inside), c.status == jp::LinkStatus::Modified);
            QVERIFY2(c.status == jp::LinkStatus::Modified || isStored(*d, idSecret), qPrintable(c.source));
        }
#endif
    }

    // The one way to follow a link to a file outside the folder is Update
    // Link (or Change Link) on that picture in the Graphics Manager; it holds
    // for the session, and saving keeps the path as it was.
    void outsideLinkIsFollowedByUpdateLinkOnly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("pub")) && root.mkpath(QStringLiteral("outside")));
        const QString one = linkPicture(root.filePath(QStringLiteral("outside/one.png")), QSize(800, 600), QColor(200, 30, 30));
        const QString two = linkPicture(root.filePath(QStringLiteral("outside/two.png")), QSize(800, 600), QColor(30, 200, 30));
        const QString other = linkPicture(root.filePath(QStringLiteral("outside/other.png")), QSize(400, 400), QColor(30, 30, 200));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        QStringList imageIds, pictureIds;
        for (const QString &file : {one, two}) {
            auto item = std::make_shared<jp::PictureItem>();
            item->rect = QRectF(50, 50 + 200 * imageIds.size(), 160, 120);
            item->imgRect = QRectF(0, 0, 160, 120);
            item->imageId = doc->addLinkedImage(linkFileBytes(file), QStringLiteral("png"), file, false);
            imageIds << item->imageId;
            pictureIds << item->id;
            doc->pages[0]->items.push_back(item);
        }
        QString err;
        const QString path = root.filePath(QStringLiteral("pub/book.jpub"));
        QVERIFY(jp::savePublication(*doc, path, QImage(), &err));
        const QByteArray fileOne = linkFileBytes(one);
        linkPicture(one, QSize(800, 600), QColor(250, 250, 10));   // changed since
        linkPicture(two, QSize(800, 600), QColor(250, 250, 10));

        auto loaded = jp::loadPublication(path, &err);
        QVERIFY2(loaded, qPrintable(err));
        QCOMPARE(loaded->linkStatus(imageIds[0]), jp::LinkStatus::NotUpdated);
        QCOMPARE(loaded->linkStatus(imageIds[1]), jp::LinkStatus::NotUpdated);
        jp::MainWindow w;
        w.show();
        w.editor()->setDocument(std::move(loaded), path);
        jp::Document *d = w.editor()->doc();
        w.editor()->select(pictureIds[0]);
        w.showTaskPane(QStringLiteral("graphics"));
        w.refreshUi();
        QListWidget *list = nullptr;
        for (auto *l : w.findChildren<QListWidget *>())
            if (l->accessibleName() == QLatin1String("Pictures")) list = l;
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        for (int i = 0; i < 2; ++i) QVERIFY2(list->item(i)->text().contains(QStringLiteral("Not updated")), qPrintable(list->item(i)->text()));
        list->setCurrentRow(0);
        QString details;
        for (auto *l : w.findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Status:"))) details = l->text();
        QVERIFY2(details.contains(QStringLiteral("Not updated")) && details.contains(QStringLiteral("outside the publication's folder")), qPrintable(details));

        QPushButton *update = nullptr;
        for (auto *b : w.findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Update Link")) update = b;
        QVERIFY(update && update->isEnabled());
        update->click();
        w.refreshUi();
        jp::PictureItem *first = dynamic_cast<jp::PictureItem *>(d->item(pictureIds[0]));
        jp::PictureItem *second = dynamic_cast<jp::PictureItem *>(d->item(pictureIds[1]));
        QVERIFY(first && second);
        QCOMPARE(d->linkStatus(first->imageId), jp::LinkStatus::Linked);
        QCOMPARE(d->images[first->imageId].bytes, linkFileBytes(one));
        QVERIFY(d->images[first->imageId].bytes != fileOne);
        QCOMPARE(d->linkStatus(second->imageId), jp::LinkStatus::NotUpdated);   // not followed with it
        QVERIFY(d->images[second->imageId].bytes != linkFileBytes(two));

        // Saved, the link is the one it was; opened again, it is outside again.
        QVERIFY(w.saveTo(path));
        QMap<QString, QByteArray> entries;
        QVERIFY(jp::readZip(linkFileBytes(path), entries));
        QJsonObject io;
        for (const auto &v : QJsonDocument::fromJson(entries["document.json"]).object()["images"].toArray())
            if (v.toObject()["source"].toString() == QFileInfo(one).absoluteFilePath()) io = v.toObject();
        QVERIFY(!io.isEmpty());
        QCOMPARE(io["relative"].toString(), QStringLiteral("../outside/one.png"));
        auto again = jp::loadPublication(path, &err);
        QVERIFY2(again, qPrintable(err));
        QCOMPARE(again->linkStatus(first->imageId), jp::LinkStatus::NotUpdated);

        w.editor()->undo();   // Update Link can be undone
        QCOMPARE(d->linkStatus(dynamic_cast<jp::PictureItem *>(d->item(pictureIds[0]))->imageId), jp::LinkStatus::NotUpdated);

        // Change Link points the other picture at a file, and follows it.
        QVERIFY(w.editor()->changeLink(pictureIds[1], other));
        const QString changed = dynamic_cast<jp::PictureItem *>(d->item(pictureIds[1]))->imageId;
        QCOMPARE(d->linkStatus(changed), jp::LinkStatus::Linked);
        QCOMPARE(d->image(changed).size(), QSize(400, 400));
    }

    // The mail merge's picture files follow the same rule: those in the
    // publication's folder (or below) show, others are not looked for. A file
    // that is not there is not asked after at every repaint.
    void mergePicturesStayInTheFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("pub/faces")) && root.mkpath(QStringLiteral("outside")));
        linkPicture(root.filePath(QStringLiteral("pub/faces/in.png")), QSize(200, 160), QColor(200, 30, 30));
        linkPicture(root.filePath(QStringLiteral("outside/out.png")), QSize(200, 160), QColor(30, 30, 200));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto frame = std::make_shared<jp::PictureItem>();
        frame->rect = QRectF(100, 100, 200, 160);
        frame->imgRect = QRectF(0, 0, 200, 160);
        frame->name = QStringLiteral("merge:Photo");
        doc->pages[0]->items.push_back(frame);
        doc->merge.path = root.filePath(QStringLiteral("pub/list.csv"));
        doc->merge.fields = {QStringLiteral("Photo")};
        doc->merge.rows = {{QStringLiteral("faces/in.png")}, {QStringLiteral("../outside/out.png")}, {root.filePath(QStringLiteral("outside/out.png"))},
                           {QStringLiteral("\\\\server\\share\\x.png")}, {QStringLiteral("later.png")}};
        doc->merge.include = {true, true, true, true, true};
        auto shows = [&](jp::Document *d, int record) {
            jp::LayoutCache cache;
            jp::PaintContext ctx;
            ctx.doc = d;
            ctx.cache = &cache;
            ctx.opt.output = true;
            ctx.opt.mergeRecord = record;
            const QImage page = jp::Renderer::renderToImage(ctx, 0, 1.0);
            return QColor(page.pixel(150, 180));   // the left half of the frame
        };
        // A publication made in this session has no folder it must keep to.
        QVERIFY(linkColorsClose(shows(doc.get(), 0), QColor(200, 30, 30)));
        QVERIFY(linkColorsClose(shows(doc.get(), 1), QColor(30, 30, 200)));

        QString err;
        const QString path = root.filePath(QStringLiteral("pub/book.jpub"));
        QVERIFY(jp::savePublication(*doc, path, QImage(), &err));
        auto loaded = jp::loadPublication(path, &err);
        QVERIFY2(loaded, qPrintable(err));
        QVERIFY(linkColorsClose(shows(loaded.get(), 0), QColor(200, 30, 30)));
        for (int record : {1, 2, 3}) QVERIFY2(linkColorsClose(shows(loaded.get(), record), QColor(255, 255, 255)), qPrintable(QString::number(record)));

        // A picture that is not there yet: not there at the next repaint either, though it has come.
        QVERIFY(linkColorsClose(shows(loaded.get(), 4), QColor(255, 255, 255)));
        linkPicture(root.filePath(QStringLiteral("pub/later.png")), QSize(200, 160), QColor(30, 200, 30));
        QVERIFY(linkColorsClose(shows(loaded.get(), 4), QColor(255, 255, 255)));
    }

    // A linked picture copied from one publication into one that came from a
    // file, whose file is outside that publication's folder, arrives without
    // the link: the file is not looked at.
    void pastedOutsideLinkComesWithoutTheLink()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("pub")) && root.mkpath(QStringLiteral("outside")));
        const QString file = linkPicture(root.filePath(QStringLiteral("outside/photo.png")), QSize(300, 200), QColor(200, 30, 30));
        jp::MainWindow w;
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.insertFiles({file}, QPointF(-1, -1), jp::PictureInsert::Link);
        w.editor()->select(linkFirstPicture(w.editor()->doc())->id);
        w.editor()->copy();

        QString err;
        const QString path = root.filePath(QStringLiteral("pub/book.jpub"));
        QVERIFY(jp::savePublication(*jp::Document::blank(QSizeF(612, 792)), path, QImage(), &err));
        auto opened = jp::loadPublication(path, &err);
        QVERIFY2(opened, qPrintable(err));
        jp::MainWindow v;
        v.editor()->setDocument(std::move(opened), path);
        v.editor()->paste();
        jp::PictureItem *pic = linkFirstPicture(v.editor()->doc());
        QVERIFY(pic);
        QVERIFY(!v.editor()->doc()->images[pic->imageId].linked);
        QCOMPARE(v.editor()->doc()->image(pic->imageId).size(), QSize(300, 200));

        // In the publication that has the file in its folder, it stays a link.
        w.editor()->paste();
        int linked = 0;
        for (const auto &it : w.editor()->doc()->pages[0]->items)
            if (auto *p = dynamic_cast<jp::PictureItem *>(it.get())) linked += w.editor()->doc()->images[p->imageId].linked;
        QCOMPARE(linked, 2);
    }

    // Embedding for e-mail, Pack and Go, Save as Template, and .pub files
    // writes what the publication holds: a link not followed brings its
    // preview, and the outside file is not read for it.
    void embeddingNeverReadsAnOutsideLink()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("pub")) && root.mkpath(QStringLiteral("outside")));
        const QString file = linkPicture(root.filePath(QStringLiteral("outside/photo.png")), QSize(1000, 800), QColor(200, 30, 30));
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto item = std::make_shared<jp::PictureItem>();
        item->rect = QRectF(100, 100, 250, 200);
        item->imgRect = QRectF(0, 0, 250, 200);
        item->imageId = doc->addLinkedImage(linkFileBytes(file), QStringLiteral("png"), file, false);
        const QString id = item->imageId;
        doc->pages[0]->items.push_back(item);
        QString err;
        const QString path = root.filePath(QStringLiteral("pub/book.jpub"));
        QVERIFY(jp::savePublication(*doc, path, QImage(), &err));
        linkPicture(file, QSize(1000, 800), QColor(250, 250, 10));
        auto loaded = jp::loadPublication(path, &err);
        QVERIFY2(loaded, qPrintable(err));
        QCOMPARE(loaded->linkStatus(id), jp::LinkStatus::NotUpdated);

        auto there = jp::publicationFromBytes(jp::publicationBytes(*loaded, QImage(), QString(), true), &err);
        QVERIFY2(there, qPrintable(err));
        QCOMPARE(std::max(there->image(id).width(), there->image(id).height()), 512);   // the preview, not the 1000-pixel file
        QCOMPARE(there->imageSize(id), QSize(1000, 800));
        const QString pub = root.filePath(QStringLiteral("embedded.pub"));
        QVERIFY2(jp::exportPublisher(*loaded, pub, &err), qPrintable(err));
        auto back = jp::importPublisherFile(pub, &err);
        QVERIFY2(back, qPrintable(err));
        for (const auto &it : back->pages[0]->items)
            if (auto *p = dynamic_cast<jp::PictureItem *>(it.get())) QCOMPARE(std::max(back->image(p->imageId).width(), back->image(p->imageId).height()), 512);
    }

    // Extra Content in a damaged file: a text box with no story to hold its
    // text, a table whose cell has none, are dropped when the file loads; a
    // shape without text stays. (Found by a crafted file.)
    void danglingExtraContentIsDroppedOnLoad()
    {
        QJsonObject json = jp::Document::blank(QSizeF(612, 792))->toJson();
        json["extra"] = QJsonArray{
            QJsonObject{{"id", "x1"}, {"type", "text"}, {"rect", QJsonArray{0, 0, 100, 50}}, {"story", "no-such-story"}},
            QJsonObject{{"id", "x2"}, {"type", "shape"}, {"rect", QJsonArray{0, 0, 100, 50}}, {"shape", "rect"}},
            QJsonObject{{"id", "x3"}, {"type", "table"}, {"rect", QJsonArray{0, 0, 100, 50}}, {"rows", 1}, {"cols", 1}, {"colW", QJsonArray{100}},
                        {"rowH", QJsonArray{50}}, {"cells", QJsonArray{QJsonObject{{"story", "nope"}}}}},
            QJsonObject{{"id", "x4"}, {"type", "shape"}, {"rect", QJsonArray{0, 0, 100, 50}}, {"shape", "rect"}, {"story", "gone"}}};
        jp::Document doc;
        doc.fromJson(json);
        QCOMPARE(int(doc.extra.size()), 1);
        QCOMPARE(doc.extra[0]->id, QStringLiteral("x2"));

        // One that has its story is kept.
        auto good = jp::Document::blank(QSizeF(612, 792));
        auto box = std::make_shared<jp::TextItem>();
        box->rect = QRectF(0, 0, 100, 50);
        box->storyId = good->createStory(QStringLiteral("Kept"));
        good->extra.push_back(box);
        jp::Document back;
        back.fromJson(good->toJson());
        QCOMPARE(int(back.extra.size()), 1);
    }

    // The Extra Content pane lists whatever is there, with words from the
    // story when it has one: an object whose story is missing (it can't come
    // from a file, but nothing may crash on it) is listed by its kind.
    void extraContentPaneSurvivesMissingStories()
    {
        jp::MainWindow w;
        w.show();
        jp::Document *d = w.editor()->doc();
        auto box = std::make_shared<jp::TextItem>();
        box->rect = QRectF(0, 0, 100, 50);
        box->storyId = QStringLiteral("ghost");
        auto shape = std::make_shared<jp::ShapeItem>();
        shape->rect = QRectF(0, 0, 100, 50);
        shape->storyId = QStringLiteral("ghost");
        auto table = std::make_shared<jp::TableItem>();
        table->rows = table->cols = 1;
        table->colW = {100};
        table->rowH = {50};
        table->cells.resize(1);
        table->cells[0].storyId = QStringLiteral("ghost");
        auto plain = std::make_shared<jp::ShapeItem>();
        plain->rect = QRectF(0, 0, 100, 50);
        d->extra = {box, shape, table, plain};
        w.showTaskPane(QStringLiteral("extra"));
        auto *list = w.findChild<jp::TaskPane *>()->findChild<QListWidget *>(QStringLiteral("extraContentList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 4);
    }

    // A pane that is not showing does nothing when the publication changes:
    // the Extra Content pane rescales every picture in it each time it
    // refreshes, so hidden it must not refresh at all.
    void hiddenExtraContentPaneDoesNotRefresh()
    {
        jp::MainWindow w;
        w.show();
        jp::Editor *ed = w.editor();
        QImage big(1600, 1200, QImage::Format_RGB32);
        big.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        big.save(&buffer, "PNG");
        for (int i = 0; i < 3; ++i) {
            auto pic = std::make_shared<jp::PictureItem>();
            pic->rect = QRectF(0, 0, 100, 80);
            pic->imageId = ed->doc()->addImage(png, QStringLiteral("png"));
            ed->doc()->extra.push_back(pic);
        }
        w.showTaskPane(QStringLiteral("extra"));
        auto *list = w.findChild<jp::TaskPane *>()->findChild<QListWidget *>(QStringLiteral("extraContentList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 3);
        w.showTaskPane(QStringLiteral("graphics"));   // another pane in front: this one is hidden
        QVERIFY(!list->isVisible());
        QSignalSpy rebuilt(list->model(), &QAbstractItemModel::rowsInserted);
        for (int i = 0; i < 4; ++i) {
            ed->change(QStringLiteral("Test"), [&] {
                auto box = ed->newTextBox(QRectF(50, 50 + 20 * i, 100, 40));
                ed->surfaceItems().push_back(box);
            });
            w.refreshUi();
        }
        QCOMPARE(rebuilt.count(), 0);
        // Showing it brings it up to date again, and a change while it shows refreshes it.
        ed->doc()->extra.pop_back();
        w.showTaskPane(QStringLiteral("extra"));
        QCOMPARE(list->count(), 2);
        ed->doc()->extra.pop_back();
        ed->change(QStringLiteral("Test"), [&] {});
        w.refreshUi();
        QCOMPARE(list->count(), 1);
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

    // Clipping inside an SVG picture (clip-path, which Qt's SVG renderer
    // leaves out) shows on screen, in print and PDF, and in Save as Picture:
    // the big square is cut to the circle, and what is drawn before and
    // after it keeps its place in the stacking order.
    void svgPictureClips()
    {
        using namespace jp;
        const QByteArray plain =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\">"
            "<defs><clipPath id=\"c\"><circle cx=\"50\" cy=\"25\" r=\"20\"/></clipPath></defs>"
            "<rect x=\"60\" y=\"20\" width=\"20\" height=\"10\" fill=\"#ffff00\"/>"
            "<rect width=\"100\" height=\"50\" fill=\"#ff0000\" clip-path=\"url(#c)\"/>"
            "<rect width=\"10\" height=\"10\" fill=\"#0000ff\"/></svg>";
        // The clip is in the box's own units, and moves with the group's transform.
        const QByteArray boxed =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\">"
            "<defs><clipPath id=\"b\" clipPathUnits=\"objectBoundingBox\"><circle cx=\".5\" cy=\".5\" r=\".5\"/></clipPath></defs>"
            "<g transform=\"translate(50 0)\"><rect width=\"50\" height=\"50\" fill=\"#00ff00\" style=\"clip-path:url(#b)\"/></g></svg>";
        auto empty = [](QRgb c) { return qAlpha(c) < 10 || (qRed(c) > 245 && qGreen(c) > 245 && qBlue(c) > 245); };
        auto is = [](QRgb c, QColor want) {
            return std::abs(qRed(c) - want.red()) < 12 && std::abs(qGreen(c) - want.green()) < 12 && std::abs(qBlue(c) - want.blue()) < 12 && qAlpha(c) > 245;
        };
        // The picture is 200 by 100 points at (100, 100): two points to a unit.
        auto place = [](double x, double y) { return QPoint(int(100 + 2 * x), int(100 + 2 * y)); };
        auto check = [&](const QImage &page, bool first, const QString &how) {
            auto px = [&](double x, double y) { return page.pixel(place(x, y)); };
            const QByteArray msg = how.toUtf8();
            if (first) {
                QVERIFY2(is(px(50, 25), Qt::red), msg.constData());                 // inside the circle
                QVERIFY2(is(px(62, 37), Qt::red), msg.constData());
                QVERIFY2(is(px(65, 25), Qt::red), msg.constData());                 // the yellow under it is covered
                QVERIFY2(is(px(75, 25), Qt::yellow), msg.constData());              // and shows beyond the circle
                QVERIFY2(empty(px(30, 5)), msg.constData());                        // the square's corners are cut off
                QVERIFY2(empty(px(67, 42)), msg.constData());
                QVERIFY2(empty(px(95, 45)), msg.constData());
                QVERIFY2(is(px(5, 5), Qt::blue), msg.constData());                  // drawn after, outside the clip
            } else {
                QVERIFY2(is(px(75, 25), Qt::green), msg.constData());
                QVERIFY2(is(px(60, 40), Qt::green), msg.constData());
                QVERIFY2(empty(px(52, 2)), msg.constData());
                QVERIFY2(empty(px(98, 48)), msg.constData());
                QVERIFY2(empty(px(25, 25)), msg.constData());
            }
        };
        for (const bool first : {true, false}) {
            auto doc = Document::blank(QSizeF(612, 792));
            auto pic = std::make_shared<PictureItem>();
            pic->imageId = doc->addImage(first ? plain : boxed, QStringLiteral("svg"));
            pic->rect = QRectF(100, 100, 200, 100);
            pic->imgRect = QRectF(0, 0, 200, 100);
            doc->pages[0]->items.push_back(pic);
            MainWindow w;
            w.editor()->setDocument(std::move(doc));
            Editor *ed = w.editor();
            PaintContext ctx;
            ctx.doc = ed->doc();
            ctx.cache = &ed->cache();
            for (const bool output : {false, true}) {
                ctx.opt.output = output;
                check(Renderer::renderToImage(ctx, 0, 1.0, true), first, output ? QStringLiteral("print path") : QStringLiteral("screen path"));
                if (QTest::currentTestFailed()) return;
            }
            QTemporaryDir dir;
            const QString path = dir.filePath(QStringLiteral("clip.pdf"));
            QVERIFY(w.exportPdfTo(path, MainWindow::PdfSettings()));
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const PdfDocument pdf(f.readAll());
            QVERIFY(pdf.isValid());
            check(pdf.render(0, QSize(612, 792)), first, QStringLiteral("PDF"));
            if (QTest::currentTestFailed()) return;
            // Save as Picture (SVG) writes the cut as a clip path: round,
            // not the picture's frame.
            ctx.opt.output = true;
            const QString svgPage = QString::fromUtf8(pageSvg(ctx, 0, QStringLiteral("clip")));
            const QRegularExpression clipPath(QStringLiteral("<clipPath[^>]*>\\s*<path[^>]* d=\"([^\"]*)\""));
            int cuts = 0;
            for (auto m = clipPath.globalMatch(svgPage); m.hasNext();)
                cuts += m.next().captured(1).count(QLatin1Char('L')) > 20;
            QVERIFY(cuts > 0);
        }
        // Convert to Shapes cuts the square to the circle too: 40 units, 80 points, across.
        PictureItem pic;
        pic.rect = QRectF(100, 100, 200, 100);
        pic.imgRect = QRectF(0, 0, 200, 100);
        bool partial = false;
        const ItemPtr made = svg::pictureShapes(plain, pic, &partial);
        QVERIFY(made && made->type() == ItemType::Group);
        auto *parts = static_cast<GroupItem *>(made.get());
        QCOMPARE(parts->children.size(), size_t(3));
        const QRectF cut = parts->children[1]->rect;
        QVERIFY2(std::abs(cut.width() - 80) < 1 && std::abs(cut.height() - 80) < 1, qPrintable(QStringLiteral("%1 x %2").arg(cut.width()).arg(cut.height())));
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
    // The Print page's Pages box takes a list such as "1-3, 5", as its
    // example says (it printed 1-3 and dropped the rest).
    void printPageList()
    {
        QCOMPARE(jp::parsePageList(QStringLiteral("1-3, 5"), 10), (QVector<int>{0, 1, 2, 4}));
        QCOMPARE(jp::parsePageList(QStringLiteral("5-3;9"), 10), (QVector<int>{2, 3, 4, 8}));
        QCOMPARE(jp::parsePageList(QStringLiteral("2, 2, 8-20"), 10), (QVector<int>{1, 7, 8, 9}));
        QVERIFY(jp::parsePageList(QStringLiteral("0, 11, x, 1-2-3"), 10).isEmpty());
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        ed->insertPages(0, 4, false, false);
        QCOMPARE(ed->doc()->pages.size(), qsizetype(5));
        QTemporaryDir dir;
        auto pagesPrinted = [&](const QJsonObject &opts) {
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(dir.filePath("p.pdf"));
            printer.setResolution(72);
            printer.setFullPage(true);
            jp::printDocument(ed, &printer, opts);
            QFile f(dir.filePath("p.pdf"));
            if (!f.open(QIODevice::ReadOnly)) return -1;
            const QByteArray pdf = f.readAll();
            return int(pdf.count("/Type /Page\n") + pdf.count("/Type /Page\r") + pdf.count("/Type /Page ") + pdf.count("/Type /Page/"));
        };
        QCOMPARE(pagesPrinted({{"layout", "one"}, {"pages", "1-2, 5"}}), 3);
        QCOMPARE(pagesPrinted({{"layout", "one"}}), 5);
    }

    // Pack and Go for a printer makes the commercial press PDF its card
    // describes (it made a plain one, with no marks).
    void packForPrinterHasMarks()
    {
        jp::MainWindow w;
        QTemporaryDir dir;
        jp::packForPrinter(&w, dir.path());
        QFile f(QDir(dir.path()).filePath(w.editor()->displayName() + QStringLiteral(".pdf")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray pdf = f.readAll();
        static const QRegularExpression media(QStringLiteral("/MediaBox \\[\\s*0 0 ([0-9.]+) ([0-9.]+)"));
        const auto m = media.match(QString::fromLatin1(pdf));
        QVERIFY(m.hasMatch());
        QVERIFY2(m.captured(1).toDouble() > w.editor()->doc()->pageSize().width() + 36, qPrintable(m.captured(0)));   // room for the marks
        QVERIFY(QFile::exists(QDir(dir.path()).filePath(w.editor()->displayName() + QStringLiteral(".jpub"))));
    }

    // The Design Checker's low-resolution check is a final publishing check:
    // it runs with only those checks on (it needed the general ones too).
    void designCheckerFinalChecksAlone()
    {
        jp::MainWindow w;
        w.show();
        QImage tiny(20, 20, QImage::Format_RGB32);
        tiny.fill(Qt::red);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        tiny.save(&buf, "PNG");
        auto pic = std::make_shared<jp::PictureItem>();
        pic->imageId = w.editor()->doc()->addImage(png, "png");
        pic->rect = QRectF(72, 72, 288, 288);
        pic->imgRect = QRectF(0, 0, 288, 288);   // 20 pixels across 4 inches: 5 ppi
        w.editor()->addItem(pic);
        w.showTaskPane(QStringLiteral("designchecker"));
        QCheckBox *general = nullptr;
        for (auto *c : w.findChildren<QCheckBox *>())
            if (c->text() == QLatin1String("Run general design checks")) general = c;
        QVERIFY(general);
        general->setChecked(false);
        QStringList found;
        for (auto *l : w.findChildren<QListWidget *>())
            for (int i = 0; i < l->count(); ++i) found << l->item(i)->text();
        QVERIFY2(found.contains(QStringLiteral("Picture has low resolution (5 ppi) (Page 1)")), qPrintable(found.join(QStringLiteral(" / "))));
    }

    // File > Options holds the statistics choice the first start points to,
    // and a new AutoRecover interval reaches windows already open.
    void optionsApplyToOpenWindows()
    {
        jp::MainWindow w;
        const bool wasOn = telemetry::enabled();
        const QVariant minutes = Settings::get().value(QStringLiteral("save/autoRecoverMinutes"));
        telemetry::setEnabled(false);
        int statsShown = -1;
        QTimer::singleShot(0, [&statsShown] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!d) return;
            auto *stats = d->findChild<QCheckBox *>(QStringLiteral("stats"));
            auto *spin = d->findChild<QSpinBox *>(QStringLiteral("autoRecoverMinutes"));
            if (stats && spin) {
                statsShown = stats->isChecked();
                stats->setChecked(true);
                spin->setValue(3);
            }
            d->accept();   // whatever was found, so a failure can't leave it open
        });
        w.act(QStringLiteral("file.options"))->trigger();
        QCOMPARE(statsShown, 0);
        QVERIFY(telemetry::enabled());
        QCOMPARE(w.autoRecoverInterval(), 3 * 60 * 1000);
        telemetry::setEnabled(wasOn);
        Settings::get().setValue(QStringLiteral("save/autoRecoverMinutes"), minutes);
    }

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

    // The first page number and the number style are kept in a .pub file: the
    // DOCUMENT chunk names (field 2f) a section chunk (type 75) with an entry
    // for the first page holding its start number (02) and, unless it is 1 2
    // 3, its style (03: 1 I II III, 2 i ii iii, 3 A B C, 4 a b c). A
    // publication numbered 1 2 3 has no such chunk, as in Publisher's files.
    void pubFirstPageNumberAndStyle()
    {
        using namespace jp;
        struct Case { int first; QString style; };
        const QStringList styles = {QString(), QStringLiteral("ROMAN"), QStringLiteral("roman"), QStringLiteral("ALPHA"), QStringLiteral("alpha")};
        auto le16 = [](int v) {
            QByteArray b;
            b.append(char(v & 0xff));
            b.append(char(v >> 8));
            return b;
        };
        QTemporaryDir dir;
        int n = 0;
        for (const Case &c : {Case{5, QStringLiteral("roman")}, Case{1, QStringLiteral("ROMAN")}, Case{12, QStringLiteral("alpha")},
                              Case{3, QStringLiteral("ALPHA")}, Case{7, QString()}, Case{1000, QStringLiteral("roman")}, Case{1, QString()}}) {
            auto doc = Document::blank(QSizeF(612, 792), QStringLiteral("Letter"), 3);
            doc->setup.firstPageNumber = c.first;
            doc->setup.pageNumberFormat = c.style;
            const QString path = dir.filePath(QStringLiteral("num%1.pub").arg(n++));
            QString err;
            QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
            auto back = importPublisherFile(path, &err);
            QVERIFY2(back, qPrintable(err));
            QCOMPARE(back->pages.size(), 3);
            QCOMPARE(back->setup.firstPageNumber, c.first);
            QCOMPARE(back->setup.pageNumberFormat, c.style);
            // The entry: first page (01 = 1), start number, and style.
            const QByteArray contents = cfb::readStream(path, QStringLiteral("Contents"));
            const QByteArray first = QByteArray::fromHex("012001000000") + QByteArray::fromHex("0218");
            if (c.first == 1 && c.style.isEmpty()) {
                QVERIFY(!contents.contains(first));
            } else {
                QByteArray entry = first + le16(c.first);
                if (!c.style.isEmpty()) entry += QByteArray::fromHex("0318") + le16(int(styles.indexOf(c.style)));
                QVERIFY(contents.contains(entry));
            }
        }
        // Publisher won't open a file whose numbering starts past 1000.
        auto doc = Document::blank(QSizeF(612, 792));
        doc->setup.firstPageNumber = 5000;
        const QString path = dir.filePath(QStringLiteral("big.pub"));
        QString err;
        QVERIFY2(exportPublisher(*doc, path, &err), qPrintable(err));
        auto back = importPublisherFile(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->setup.firstPageNumber, 1000);
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

    // A footnote too tall for a whole column is cut where its column ends and
    // goes on at the bottom of the next column, above that column's own
    // notes, under a rule as wide as the column. The line with the number
    // stays where the note starts, no line of the note is hidden, and they
    // keep their order.
    void footnoteTallerThanColumnGoesOnInNextColumn()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto a = std::make_shared<jp::TextItem>();
        a->rect = QRectF(72, 72, 300, 200);
        a->columns = 2;
        a->storyId = doc->createStory();
        doc->pages[0]->items.push_back(a);
        QTextDocument *sd = doc->storyDoc(a->storyId);
        {
            QTextCursor c(sd);
            for (int i = 0; i < 4; ++i) {
                if (i) c.insertBlock();
                c.insertText(QStringLiteral("Paragraph %1.").arg(i + 1));
            }
        }
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        auto at = [&](int block) {
            ed->beginTextEdit(a->id);
            QTextCursor c(sd->findBlockByNumber(block));
            c.movePosition(QTextCursor::EndOfBlock);
            ed->setCursor(c);
        };
        auto words = [](int n) {
            QStringList l;
            for (int i = 1; i <= n; ++i) l << QStringLiteral("word%1").arg(i);
            return l.join(QLatin1Char(' '));
        };
        at(0);
        const QString big = jp::addNote(ed, false, words(10));
        at(3);
        const QString small = jp::addNote(ed, false, QStringLiteral("The second note."));
        QVERIFY(!big.isEmpty() && !small.isEmpty());
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        auto lay = [&] {
            cache.clear();
            return cache.textFrame(*ed->doc(), *a, 1, opt);
        };
        // Words are added to the first note until it no longer fits a column
        // with its line (whatever the fonts), then a few lines more.
        auto fl = lay();
        int count = 10;
        while (fl.layout->notes()[0].more.isEmpty() && count < 1000) {
            count += 5;
            jp::setNoteText(ed, big, words(count));
            fl = lay();
        }
        QVERIFY(count < 1000);
        count += 15;
        jp::setNoteText(ed, big, words(count));
        fl = lay();
        const auto &notes = fl.layout->notes();
        QCOMPARE(notes.size(), 2);
        const auto &n = notes[0];
        QCOMPARE(n.frame, 0);
        QCOMPARE(n.column, 0);
        QCOMPARE(n.more.size(), 1);
        QCOMPARE(n.more[0].frame, 0);
        QCOMPARE(n.more[0].column, 1);
        QVERIFY(!fl.layout->overflow());
        // Each part is at the bottom of its own column; the second note
        // follows the rest of the first in the second column.
        const double colW = (300 - a->insets.left() - a->insets.right() - a->columnGap) / 2;
        const double col1Left = a->insets.left() + colW + a->columnGap;
        const double bottom = 200 - a->insets.bottom();
        QVERIFY(std::abs(n.rect.left() - a->insets.left()) < 0.01);
        QVERIFY(std::abs(n.more[0].rect.left() - col1Left) < 0.01);
        QVERIFY2(std::abs(n.rect.bottom() + 2 - bottom) < 1.5, qPrintable(QString::number(n.rect.bottom())));
        QCOMPARE(notes[1].frame, 0);
        QCOMPARE(notes[1].column, 1);
        QVERIFY(n.more[0].rect.bottom() <= notes[1].rect.top() + 0.01);
        QVERIFY(std::abs(notes[1].rect.bottom() + 2 - bottom) < 1.5);
        QVERIFY(n.rect.top() >= a->insets.top());
        // The line with the number is the first in the first column, above its note;
        // no line of text runs into a note.
        const auto info = fl.layout->lineInfo(0);
        QVERIFY(info.first().rect.left() < col1Left - 1);
        QVERIFY(info.first().text.contains(QStringLiteral(".1")));
        for (const QRectF &r : fl.layout->lineRects(0))
            QVERIFY(r.bottom() <= (r.left() < col1Left - 1 ? n.rect.top() : n.more[0].rect.top()) + 0.01);
        // The note's lines, in order, none hidden.
        auto shown = [](const jp::StoryLayout *l) {
            QString s;
            for (const auto &li : l->lineInfo(0)) s += li.text;
            return s;
        };
        QVERIFY(!shown(n.layout.get()).isEmpty());
        QVERIFY(!shown(n.more[0].layout.get()).isEmpty());
        QString all = shown(n.layout.get()) + shown(n.more[0].layout.get());
        all.remove(QChar(0x00AD));
        QCOMPARE(all.simplified(), words(count));
        // A short rule over the first column's notes, one as wide as the column over the second's.
        QVector<QLineF> rules;
        for (const auto &r : fl.layout->noteRules())
            if (r.frame == 0) rules << r.line;
        QCOMPARE(rules.size(), 2);
        std::sort(rules.begin(), rules.end(), [](const QLineF &x, const QLineF &y) { return x.x1() < y.x1(); });
        QVERIFY(std::abs(rules[0].length() - colW / 3) < 0.01);
        QVERIFY(std::abs(rules[1].x1() - col1Left) < 0.01);
        QVERIFY2(std::abs(rules[1].length() - colW) < 0.01, qPrintable(QString::number(rules[1].length())));
        QVERIFY(rules[0].y1() < n.rect.top() && rules[1].y1() < n.more[0].rect.top());
        // Drawn: ink where the rest of the note is, none above the box.
        const QRectF firstRect = n.rect, restRect = n.more[0].rect;
        jp::PaintContext pc;
        pc.doc = ed->doc();
        pc.cache = &cache;
        pc.opt.output = true;
        const QImage img = jp::Renderer::renderToImage(pc, 0, 1.0);
        auto ink = [&](const QRectF &r) {
            int dark = 0;
            for (int y = std::max(0, int(r.top())); y < std::min(img.height(), int(std::ceil(r.bottom()))); ++y)
                for (int x = std::max(0, int(r.left())); x < std::min(img.width(), int(std::ceil(r.right()))); ++x) dark += qGray(img.pixel(x, y)) < 128;
            return dark;
        };
        QVERIFY(ink(restRect.translated(a->rect.topLeft())) > 20);
        QVERIFY(ink(firstRect.translated(a->rect.topLeft())) > 20);
        QCOMPARE(ink(QRectF(0, 0, 612, a->rect.top())), 0);
    }

    // The rest of a footnote goes on in the next linked text box, on whatever
    // page it is and however wide its columns are.
    void footnoteTallerThanBoxGoesOnInNextBox()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto a = std::make_shared<jp::TextItem>();
        a->rect = QRectF(72, 72, 300, 200);
        a->storyId = doc->createStory();
        auto b2 = std::make_shared<jp::TextItem>();
        b2->rect = QRectF(100, 72, 240, 200);
        b2->storyId = a->storyId;
        a->nextId = b2->id;
        doc->pages[0]->items.push_back(a);
        doc->addPage()->items.push_back(b2);
        QTextDocument *sd = doc->storyDoc(a->storyId);
        {
            QTextCursor c(sd);
            for (int i = 0; i < 6; ++i) {
                if (i) c.insertBlock();
                c.insertText(QStringLiteral("Paragraph %1.").arg(i + 1));
            }
        }
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        ed->beginTextEdit(a->id);
        QTextCursor cur(sd->findBlockByNumber(0));
        cur.movePosition(QTextCursor::EndOfBlock);
        ed->setCursor(cur);
        auto words = [](int n) {
            QStringList l;
            for (int i = 1; i <= n; ++i) l << QStringLiteral("word%1").arg(i);
            return l.join(QLatin1Char(' '));
        };
        const QString big = jp::addNote(ed, false, words(10));
        QVERIFY(!big.isEmpty());
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        auto lay = [&] {
            cache.clear();
            return cache.textFrame(*ed->doc(), *a, 1, opt);
        };
        auto fl = lay();
        int count = 10;
        while (fl.layout->notes()[0].more.isEmpty() && count < 1000) {
            count += 5;
            jp::setNoteText(ed, big, words(count));
            fl = lay();
        }
        QVERIFY(count < 1000);
        count += 25;
        jp::setNoteText(ed, big, words(count));
        fl = lay();
        QCOMPARE(fl.layout->frameCount(), 2);
        const auto &n = fl.layout->notes()[0];
        QCOMPARE(n.frame, 0);
        QCOMPARE(n.more.size(), 1);
        QCOMPARE(n.more[0].frame, 1);
        QCOMPARE(n.more[0].column, 0);
        QVERIFY(!fl.layout->overflow());
        // The rest is laid out for the second box's column and sits at its bottom.
        const double colW2 = 240 - b2->insets.left() - b2->insets.right();
        QVERIFY(std::abs(n.more[0].rect.width() - colW2) < 0.01);
        QVERIFY(std::abs(n.more[0].rect.left() - b2->insets.left()) < 0.01);
        QVERIFY2(std::abs(n.more[0].rect.bottom() + 2 - (200 - b2->insets.bottom())) < 1.5, qPrintable(QString::number(n.more[0].rect.bottom())));
        // The text of the story goes on above it in the second box.
        const auto second = fl.layout->lineInfo(1);
        QVERIFY(!second.isEmpty());
        for (const QRectF &r : fl.layout->lineRects(1)) QVERIFY(r.bottom() <= n.more[0].rect.top() + 0.01);
        for (const QRectF &r : fl.layout->lineRects(0)) QVERIFY(r.bottom() <= n.rect.top() + 0.01);
        QVERIFY(fl.layout->lineInfo(0).first().text.contains(QStringLiteral(".1")));
        // Every line of the note, in order.
        auto shown = [](const jp::StoryLayout *l) {
            QString s;
            for (const auto &li : l->lineInfo(0)) s += li.text;
            return s;
        };
        QString all = shown(n.layout.get()) + shown(n.more[0].layout.get());
        all.remove(QChar(0x00AD));
        QCOMPARE(all.simplified(), words(count));
        // A short rule in the first box, one as wide as the column in the second.
        QLineF first, rest;
        for (const auto &r : fl.layout->noteRules()) (r.frame == 0 ? first : rest) = r.line;
        QVERIFY(std::abs(first.length() - (300 - a->insets.left() - a->insets.right()) / 3) < 0.01);
        QVERIFY2(std::abs(rest.length() - colW2) < 0.01, qPrintable(QString::number(rest.length())));
        // Drawn on the second box's page.
        const QRectF restRect = n.more[0].rect;
        jp::PaintContext pc;
        pc.doc = ed->doc();
        pc.cache = &cache;
        pc.opt.output = true;
        const QImage img = jp::Renderer::renderToImage(pc, 1, 1.0);
        int dark = 0;
        const QRectF r = restRect.translated(b2->rect.topLeft());
        for (int y = int(r.top()); y < int(std::ceil(r.bottom())); ++y)
            for (int x = int(r.left()); x < int(std::ceil(r.right())); ++x) dark += qGray(img.pixel(x, y)) < 128;
        QVERIFY(dark > 20);
    }

    // A very long footnote (of several paragraphs) goes on through as many
    // text boxes as it needs, a whole box where it takes one; with no box
    // left, the story overflows as for any other text.
    void footnoteGoesOnThroughManyBoxes()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        std::shared_ptr<jp::TextItem> boxes[3];
        for (int i = 0; i < 3; ++i) {
            boxes[i] = std::make_shared<jp::TextItem>();
            boxes[i]->rect = QRectF(72, 72 + 230 * i, 300, 200);
            doc->pages[0]->items.push_back(boxes[i]);
        }
        auto a = boxes[0];
        a->storyId = doc->createStory();
        for (int i = 1; i < 3; ++i) {
            boxes[i]->storyId = a->storyId;
            boxes[i - 1]->nextId = boxes[i]->id;
        }
        QTextDocument *sd = doc->storyDoc(a->storyId);
        QTextCursor(sd).insertText(QStringLiteral("Paragraph 1."));
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        ed->beginTextEdit(a->id);
        QTextCursor cur(sd->findBlockByNumber(0));
        cur.movePosition(QTextCursor::EndOfBlock);
        ed->setCursor(cur);
        // Paragraphs of twelve words each, the last shorter.
        auto words = [](int n) {
            QStringList paragraphs, l;
            for (int i = 1; i <= n; ++i) {
                l << QStringLiteral("word%1").arg(i);
                if (l.size() == 12 || i == n) {
                    paragraphs << l.join(QLatin1Char(' '));
                    l.clear();
                }
            }
            return paragraphs.join(QLatin1Char('\n'));
        };
        const QString big = jp::addNote(ed, false, words(10));
        QVERIFY(!big.isEmpty());
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        auto lay = [&] {
            cache.clear();
            return cache.textFrame(*ed->doc(), *a, 1, opt);
        };
        auto fl = lay();
        int count = 10;
        while (fl.layout->notes()[0].more.size() < 2 && count < 2000) {
            count += 10;
            jp::setNoteText(ed, big, words(count));
            fl = lay();
        }
        QVERIFY(count < 2000);
        const auto &n = fl.layout->notes()[0];
        QCOMPARE(n.more.size(), 2);
        QVERIFY(!fl.layout->overflow());
        QCOMPARE(n.frame, 0);
        QCOMPARE(n.more[0].frame, 1);
        QCOMPARE(n.more[1].frame, 2);
        // The second box holds nothing but the note, from its rule to its
        // bottom, though the text ended in the first.
        QVERIFY(fl.layout->lineRects(1).isEmpty());
        QVERIFY(n.more[0].rect.top() < 40);
        QVERIFY(std::abs(n.more[0].rect.bottom() + 2 - (200 - a->insets.bottom())) < 1.5);
        QVERIFY(std::abs(n.more[1].rect.bottom() + 2 - (200 - a->insets.bottom())) < 1.5);
        QVector<QLineF> rules(3);
        for (const auto &r : fl.layout->noteRules()) rules[r.frame] = r.line;
        QVERIFY(std::abs(rules[0].length() - (300 - a->insets.left() - a->insets.right()) / 3) < 0.01);
        QVERIFY(std::abs(rules[1].length() - (300 - a->insets.left() - a->insets.right())) < 0.01);
        QVERIFY(std::abs(rules[2].length() - (300 - a->insets.left() - a->insets.right())) < 0.01);
        auto shown = [](const jp::StoryLayout *l) {
            QString s;
            for (const auto &li : l->lineInfo(0)) s += li.text + QLatin1Char(' ');
            return s;
        };
        QString all = shown(n.layout.get()) + shown(n.more[0].layout.get()) + shown(n.more[1].layout.get());
        all.remove(QChar(0x00AD));
        QCOMPARE(all.simplified(), words(count).simplified());
        // The last box taken away: the rest of the note has nowhere to go.
        boxes[1]->nextId.clear();
        fl = lay();
        QVERIFY(fl.layout->overflow());
        QCOMPARE(fl.layout->notes()[0].more.size(), 1);
        QCOMPARE(fl.layout->notes()[0].more[0].frame, 1);
    }

    // A note that fits a column is never cut: at the bottom of the column
    // under the short rule, or, when it doesn't fit under its line, on with
    // the line in the next column.
    void shortFootnoteIsNotCut()
    {
        jp::MainWindow w;
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto a = std::make_shared<jp::TextItem>();
        a->rect = QRectF(72, 72, 300, 200);
        a->columns = 2;
        a->storyId = doc->createStory();
        doc->pages[0]->items.push_back(a);
        QTextDocument *sd = doc->storyDoc(a->storyId);
        {
            QTextCursor c(sd);
            for (int i = 0; i < 9; ++i) {
                if (i) c.insertBlock();
                c.insertText(QStringLiteral("Paragraph %1.").arg(i + 1));
            }
        }
        w.editor()->setDocument(std::move(doc));
        jp::Editor *ed = w.editor();
        auto at = [&](int block) {
            ed->beginTextEdit(a->id);
            QTextCursor c(sd->findBlockByNumber(block));
            c.movePosition(QTextCursor::EndOfBlock);
            ed->setCursor(c);
        };
        at(0);
        QVERIFY(!jp::addNote(ed, false, QStringLiteral("The first note.")).isEmpty());
        jp::LayoutCache cache;
        jp::RenderOptions opt;
        auto fl = cache.textFrame(*ed->doc(), *a, 1, opt);
        const double colW = (300 - a->insets.left() - a->insets.right() - a->columnGap) / 2;
        const double col1Left = a->insets.left() + colW + a->columnGap;
        auto n = fl.layout->notes()[0];
        QCOMPARE(n.frame, 0);
        QCOMPARE(n.column, 0);
        QVERIFY(n.more.isEmpty());
        QVERIFY(std::abs(n.rect.bottom() + 2 - (200 - a->insets.bottom())) < 1.5);
        QVERIFY(std::abs(n.rect.width() - colW) < 0.01);
        QCOMPARE(fl.layout->noteRules().size(), 1);
        QVERIFY(std::abs(fl.layout->noteRules()[0].line.length() - colW / 3) < 0.01);
        QVERIFY(!fl.layout->overflow());
        for (const QRectF &r : fl.layout->lineRects(0)) QVERIFY(r.left() > col1Left - 1 || r.bottom() <= n.rect.top() + 0.01);
        // A note that fits an empty column but not the room left under its
        // line: lines are added to it until it no longer fits the first
        // column; both go to the next column, the note whole.
        at(6);
        const QString mid = jp::addNote(ed, false, QStringLiteral("Note line 1"));
        QVERIFY(!mid.isEmpty());
        QStringList lines{QStringLiteral("Note line 1")};
        cache.clear();
        fl = cache.textFrame(*ed->doc(), *a, 1, opt);
        QCOMPARE(fl.layout->notes().size(), 2);
        QCOMPARE(fl.layout->notes()[1].column, 0);
        while (fl.layout->notes()[1].column == 0 && lines.size() < 40) {
            lines << QStringLiteral("Note line %1").arg(lines.size() + 1);
            jp::setNoteText(ed, mid, lines.join(QLatin1Char('\n')));
            cache.clear();
            fl = cache.textFrame(*ed->doc(), *a, 1, opt);
        }
        n = fl.layout->notes()[1];
        QCOMPARE(n.frame, 0);
        QCOMPARE(n.column, 1);
        QVERIFY(n.more.isEmpty());
        QVERIFY(!fl.layout->overflow());
        bool refInSecond = false;
        for (const auto &li : fl.layout->lineInfo(0)) refInSecond |= li.rect.left() > col1Left - 1 && li.text.contains(QStringLiteral("7.2"));
        QVERIFY(refInSecond);
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
        for (const QString &name : {QStringLiteral("test10-table.pub"), QStringLiteral("test11-table-text.pub")}) {
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

    // The rulers' zero point can move. Shift and the right button dragging on
    // a ruler sets that ruler's zero where you let go; dragging from the box
    // where the two rulers meet sets both. A double-click on a ruler puts
    // that ruler's zero back at the page's corner, and one on the box puts
    // both back. Only the rulers' numbers change: guides are still placed from
    // the page's corner.
    void rulerZeroPoint()
    {
        jp::Settings &st = jp::Settings::get();
        const jp::Unit was = st.unit();
        st.setUnit(jp::Unit::Inch);
        jp::MainWindow w;
        w.resize(1200, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(50);   // the whole page fits the window
        jp::Canvas *c = w.canvas();
        c->setRulersVisible(true);
        jp::Ruler *h = c->hRuler(), *v = c->vRuler();
        QWidget *corner = c->rulerCorner();
        QVERIFY(corner);
        // A place on the page, as a place on a ruler or on the box where they meet.
        auto view = [&](QWidget *on, QPointF page) { return on->mapFromGlobal(c->viewport()->mapToGlobal(c->pageToView(page).toPoint())); };
        auto onH = [&](double x) { return QPoint(view(h, QPointF(x, 0)).x(), 5); };
        auto onV = [&](double y) { return QPoint(5, view(v, QPointF(0, y)).y()); };
        auto send = [](QWidget *on, QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held, Qt::KeyboardModifiers mods, QPoint pos) {
            QMouseEvent e(type, QPointF(pos), QPointF(on->mapToGlobal(pos)), button, held, mods);
            QApplication::sendEvent(on, &e);
        };
        auto drag = [&](QWidget *on, Qt::MouseButton button, Qt::KeyboardModifiers mods, QPoint from, QPoint to) {
            send(on, QEvent::MouseButtonPress, button, button, mods, from);
            send(on, QEvent::MouseMove, Qt::NoButton, button, mods, (from + to) / 2);
            send(on, QEvent::MouseMove, Qt::NoButton, button, mods, to);
            send(on, QEvent::MouseButtonRelease, button, Qt::NoButton, mods, to);
        };
        const double pixel = 2 / c->ppp();   // a pixel or two, in points
        auto closeTo = [&](double a, double b) { return qAbs(a - b) <= pixel; };
        auto look = [](jp::Ruler *r) {
            r->setMouse(-1e9);
            return r->grab().toImage();
        };

        // At first both count from the page's corner.
        const QImage plainH = look(h), plainV = look(v);
        QCOMPARE(h->zero(), 0.0);
        QCOMPARE(v->zero(), 0.0);
        QCOMPARE(h->valueAt(144), 2.0);
        QCOMPARE(v->valueAt(216), 3.0);

        // Shift and the right button on the top ruler: its zero goes to 2 in. The side ruler stays.
        drag(h, Qt::RightButton, Qt::ShiftModifier, onH(72), onH(144));
        QVERIFY2(closeTo(h->zero(), 144), qPrintable(QString::number(h->zero())));
        QCOMPARE(v->zero(), 0.0);
        const double hz = h->zero();
        QVERIFY(qAbs(h->valueAt(hz)) < 1e-9);
        QVERIFY(qAbs(h->valueAt(hz + 72) - 1.0) < 1e-9);
        QVERIFY(qAbs(h->valueAt(hz - 36) + 0.5) < 1e-9);
        QVERIFY(look(h) != plainH);
        QCOMPARE(look(v), plainV);
        // And the same on the side ruler.
        drag(v, Qt::RightButton, Qt::ShiftModifier, onV(300), onV(216));
        QVERIFY2(closeTo(v->zero(), 216), qPrintable(QString::number(v->zero())));
        QCOMPARE(h->zero(), hz);
        QVERIFY(qAbs(v->valueAt(v->zero() + 72) - 1.0) < 1e-9);
        // Other buttons, and the right button without Shift, leave the zero alone.
        drag(h, Qt::RightButton, Qt::NoModifier, onH(300), onH(400));
        drag(h, Qt::MiddleButton, Qt::ShiftModifier, onH(300), onH(400));
        QCOMPARE(h->zero(), hz);
        // Shift and the right button do not bring up the page's pop-up menu.
        QSignalSpy menu(c, &jp::Canvas::contextMenuWanted);
        QContextMenuEvent ce(QContextMenuEvent::Mouse, onH(144), h->mapToGlobal(onH(144)), Qt::ShiftModifier);
        QApplication::sendEvent(h, &ce);
        QCOMPARE(menu.count(), 0);

        // The box where they meet moves both; a click on it moves nothing.
        drag(corner, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10), view(corner, QPointF(72, 108)));
        QVERIFY2(closeTo(h->zero(), 72), qPrintable(QString::number(h->zero())));
        QVERIFY2(closeTo(v->zero(), 108), qPrintable(QString::number(v->zero())));
        const double hz2 = h->zero(), vz2 = v->zero();
        if (!qEnvironmentVariableIsEmpty("JP_SHOT_DIR")) w.grab().save(qEnvironmentVariable("JP_SHOT_DIR") + "/ruler-zero.png");
        QTest::mouseClick(corner, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QCOMPARE(h->zero(), hz2);
        QCOMPARE(v->zero(), vz2);

        // Guides are placed from the page's corner all the same.
        drag(h, Qt::LeftButton, Qt::NoModifier, onH(300), QPoint(onH(300).x(), view(h, QPointF(0, 250)).y()));
        const QVector<double> &guides = w.editor()->surface()->guides.h;
        QCOMPARE(guides.size(), 1);
        QVERIFY2(closeTo(guides[0], 250), qPrintable(QString::number(guides[0])));
        QCOMPARE(h->zero(), hz2);

        // A double-click anywhere on a ruler puts that ruler's zero back, and only that one's; no guide comes of the clicks.
        QTest::mouseDClick(h, Qt::LeftButton, Qt::NoModifier, onH(300));
        QCOMPARE(h->zero(), 0.0);
        QCOMPARE(v->zero(), vz2);
        QTest::mouseDClick(v, Qt::LeftButton, Qt::NoModifier, onV(400));
        QCOMPARE(v->zero(), 0.0);
        QCOMPARE(guides.size(), 1);
        QCOMPARE(w.editor()->surface()->guides.v.size(), 0);

        // A double-click on the box puts both back, and the rulers look as they did.
        drag(corner, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10), view(corner, QPointF(72, 108)));
        QVERIFY(h->zero() > 1 && v->zero() > 1);
        QTest::mouseDClick(corner, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QCOMPARE(h->zero(), 0.0);
        QCOMPARE(v->zero(), 0.0);
        QCOMPARE(look(h), plainH);
        QCOMPARE(look(v), plainV);

        // A new publication starts with the rulers at the corner.
        drag(corner, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10), view(corner, QPointF(72, 108)));
        QVERIFY(h->zero() > 1 && v->zero() > 1);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QCOMPARE(h->zero(), 0.0);
        QCOMPARE(v->zero(), 0.0);
        st.setUnit(was);
    }

    // The moved zero is saved with the publication in a .jpub, and comes back
    // when it is opened; a file with the zero at the corner has no mention of
    // it. Moving it is one undo step, and changes the publication.
    void rulerZeroSaved()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        ed->setDocument(jp::Document::blank(QSizeF(612, 792)));
        jp::Ruler *h = w.canvas()->hRuler(), *v = w.canvas()->vRuler();
        QVERIFY(!ed->isModified());
        QVERIFY(!jp::publicationBytes(*ed->doc(), QImage()).contains("rulerZero"));
        h->setZero(216);
        v->setZero(72);
        QVERIFY(ed->isModified());
        QCOMPARE(ed->doc()->rulerZero, QPointF(216, 72));
        QVERIFY(jp::publicationBytes(*ed->doc(), QImage()).contains("\"rulerZero\":[216,72]"));
        // Each move is a step: undo and redo take it back and bring it again, the other ruler's unmoved.
        ed->undoStack()->undo();
        QCOMPARE(v->zero(), 0.0);
        QCOMPARE(h->zero(), 216.0);
        ed->undoStack()->undo();
        QCOMPARE(h->zero(), 0.0);
        QVERIFY(!ed->isModified());
        ed->undoStack()->redo();
        ed->undoStack()->redo();
        QCOMPARE(ed->doc()->rulerZero, QPointF(216, 72));

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zero.jpub"));
        QString err;
        QVERIFY2(jp::savePublication(*ed->doc(), path, QImage(), &err), qPrintable(err));
        jp::MainWindow other;
        auto back = jp::loadPublication(path, &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->rulerZero, QPointF(216, 72));
        other.editor()->setDocument(std::move(back), path);
        QCOMPARE(other.canvas()->hRuler()->zero(), 216.0);
        QCOMPARE(other.canvas()->vRuler()->zero(), 72.0);
        // Put back at the corner and saved again, the file says nothing of it.
        other.canvas()->hRuler()->setZero(0);
        other.canvas()->vRuler()->setZero(0);
        QVERIFY(!jp::publicationBytes(*other.editor()->doc(), QImage()).contains("rulerZero"));
        // A new publication starts at the corner.
        h->setZero(100);
        ed->setDocument(jp::Document::blank(QSizeF(612, 792)));
        QCOMPARE(h->zero(), 0.0);
        QCOMPARE(v->zero(), 0.0);
    }

    // The status bar's position counts from the rulers' zero, with a minus
    // sign before it; the sizes are as ever.
    void rulerZeroInStatusBar()
    {
        jp::Settings &st = jp::Settings::get();
        const jp::Unit was = st.unit();
        st.setUnit(jp::Unit::Inch);
        jp::MainWindow w;
        w.resize(1200, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        jp::Canvas *c = w.canvas();
        auto box = std::make_shared<jp::ShapeItem>();
        box->rect = QRectF(72, 90, 144, 36);
        ed->addItem(box);
        ed->select(QStringList{box->id});
        auto position = [&] {
            for (QLabel *l : w.statusBar()->findChildren<QLabel *>())
                if (l->text().contains(QLatin1String(", "))) return l->text();
            return QString();
        };
        auto size = [&] {
            for (QLabel *l : w.statusBar()->findChildren<QLabel *>())
                if (l->text().contains(QChar(0x00D7))) return l->text();
            return QString();
        };
        QTRY_COMPARE(position(), QStringLiteral("1\", 1.25\""));
        const QString sized = size();
        QVERIFY(!sized.isEmpty());
        // With the zero at 3 in across and 1 in down, the shape (1 in, 1.25 in) reads -2 in, 0.25 in.
        c->hRuler()->setZero(216);
        c->vRuler()->setZero(72);
        QTRY_COMPARE(position(), QStringLiteral("-2\", 0.25\""));
        QCOMPARE(size(), sized);
        // So does the pointer's place: the page's 4 in across and 5 in down reads 1 in, 4 in.
        QWidget *vp = c->viewport();
        QSignalSpy moved(c, &jp::Canvas::mouseMovedPage);
        const QPoint at = c->pageToView(QPointF(288, 360)).toPoint();
        QMouseEvent mv(QEvent::MouseMove, QPointF(at), QPointF(vp->mapToGlobal(at)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &mv);
        QVERIFY(moved.count() > 0);
        const QPointF page = moved.last().first().toPointF();
        QVERIFY(qAbs(page.x() - 288) < 3 && qAbs(page.y() - 360) < 3);
        QCOMPARE(position(), QStringLiteral("%1, %2").arg(st.format(page.x() - 216), st.format(page.y() - 72)));
        // The Position boxes and the file still measure from the page's corner.
        QCOMPARE(box->rect.topLeft(), QPointF(72, 90));
        st.setUnit(was);
    }

    // The text ruler (indents and tabs) is measured from the text box, so it
    // works as ever with the zero moved.
    void rulerZeroLeavesTextRuler()
    {
        jp::MainWindow w;
        w.resize(1200, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(50);
        jp::Editor *ed = w.editor();
        jp::Canvas *c = w.canvas();
        c->setRulersVisible(true);
        jp::Ruler *h = c->hRuler();
        auto box = std::static_pointer_cast<jp::TextItem>(ed->newTextBox(QRectF(72, 72, 300, 200), QStringLiteral("one two three")));
        ed->addItem(box);
        ed->beginTextEdit(box->id);
        const double left = box->rect.left() + box->insets.left();   // where the text starts
        auto onH = [&](double x) {
            const QPoint p = h->mapFromGlobal(c->viewport()->mapToGlobal(c->pageToView(QPointF(x, 0)).toPoint()));
            return QPoint(p.x(), 5);
        };
        auto send = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held, QPoint pos) {
            QMouseEvent e(type, QPointF(pos), QPointF(h->mapToGlobal(pos)), button, held, Qt::NoModifier);
            QApplication::sendEvent(h, &e);
        };
        const double pixel = 2 / c->ppp();
        h->setZero(200);
        // Drag the first-line marker half an inch in.
        send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, onH(left));
        send(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, onH(left + 18));
        send(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, onH(left + 36));
        send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, onH(left + 36));
        const QTextBlockFormat bf = ed->cursor().blockFormat();
        QVERIFY2(qAbs(bf.textIndent() - 36) <= pixel, qPrintable(QString::number(bf.textIndent())));
        // A click inside the text area sets a tab stop that far from the text, wherever the zero is.
        send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, onH(left + 100));
        send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, onH(left + 100));
        const auto tabs = ed->cursor().blockFormat().tabPositions();
        QCOMPARE(tabs.size(), 1);
        QVERIFY2(qAbs(tabs[0].position - 100) <= pixel, qPrintable(QString::number(tabs[0].position)));
        QCOMPARE(h->zero(), 200.0);
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

    // A table cell's Text Direction and hyphenation are saved with the table;
    // a cell that keeps the defaults saves nothing more than it did.
    void tableCellDirectionAndHyphenationSave()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TableItem>();
        t->rows = 1;
        t->cols = 3;
        t->colW = {60, 60, 60};
        t->rowH = {120};
        t->rect = QRectF(72, 72, 0, 0);
        t->syncRect();
        t->cells.resize(3);
        for (auto &c : t->cells) c.storyId = doc->createStory(QStringLiteral("cell"));
        t->cell(0, 1).vertical = true;
        t->cell(0, 2).hyphenate = false;
        t->cell(0, 2).hyphenZone = 30;
        doc->pages[0]->items.push_back(t);
        QString err;
        auto again = jp::publicationFromBytes(jp::publicationBytes(*doc, QImage()), &err);
        QVERIFY2(again, qPrintable(err));
        const auto *back = static_cast<const jp::TableItem *>(again->pages[0]->items[0].get());
        QCOMPARE(int(back->cells.size()), 3);
        QVERIFY(!back->cell(0, 0).vertical && back->cell(0, 0).hyphenate && back->cell(0, 0).hyphenZone == 18.0);
        QVERIFY(back->cell(0, 1).vertical && back->cell(0, 1).hyphenate);
        QVERIFY(!back->cell(0, 2).vertical && !back->cell(0, 2).hyphenate && back->cell(0, 2).hyphenZone == 30.0);
        const QJsonObject plain = t->toJson()["cells"].toArray()[0].toObject();
        for (const char *key : {"vertical", "hyph", "hyphZone"}) QVERIFY2(!plain.contains(QLatin1String(key)), key);
    }

    // A cell's Text Direction turns its text 90 degrees as a text box's does:
    // the lines run down a tall, narrow cell, and it is drawn just as the
    // vertical text box is.
    void tableCellTextDirectionLaysOutAndDraws()
    {
        const QString text = QStringLiteral("Region name and notes");
        auto tableDoc = [&](bool vertical) {
            auto doc = jp::Document::blank(QSizeF(612, 792));
            auto t = std::make_shared<jp::TableItem>();
            t->rows = 1;
            t->cols = 1;
            t->colW = {40};
            t->rowH = {200};
            t->rect = QRectF(72, 72, 0, 0);
            t->syncRect();
            t->stroke = jp::Stroke::none();
            t->cells.resize(1);
            t->cells[0].storyId = doc->createStory(text);
            t->cells[0].vertical = vertical;
            doc->pages[0]->items.push_back(t);
            return doc;
        };
        auto lines = [](jp::Document &doc) {
            jp::LayoutCache cache;
            jp::PaintContext ctx;
            ctx.doc = &doc;
            ctx.cache = &cache;
            QPointF origin;
            const auto *lay = jp::Renderer::cellLayout(ctx, *static_cast<jp::TableItem *>(doc.pages[0]->items[0].get()), 0, 0, &origin);
            return lay ? int(lay->lineInfo(0).size()) : -1;
        };
        auto across = tableDoc(false), down = tableDoc(true);
        QVERIFY2(lines(*across) > 1, "a 40-point cell wraps the text");
        QCOMPARE(lines(*down), 1);   // the same cell turned: 200 points to run along
        auto box = jp::Document::blank(QSizeF(612, 792));
        auto spine = std::make_shared<jp::TextItem>();
        spine->rect = QRectF(72, 72, 40, 200);
        spine->vertical = true;
        spine->stroke = jp::Stroke::none();
        spine->storyId = box->createStory(text);
        box->pages[0]->items.push_back(spine);
        const QImage cell = jp::renderPlate(*down, 0, 3, 72), asBox = jp::renderPlate(*box, 0, 3, 72);
        QRect ink;
        for (int y = 0; y < cell.height(); ++y)
            for (int x = 0; x < cell.width(); ++x)
                if (qGray(cell.pixel(x, y)) < 128) ink |= QRect(x, y, 1, 1);
        QVERIFY2(QRect(72, 72, 40, 200).contains(ink) && ink.height() > 3 * ink.width(), qPrintable(QStringLiteral("%1,%2 %3x%4").arg(ink.x()).arg(ink.y()).arg(ink.width()).arg(ink.height())));
        QVERIFY2(cell == asBox, "a turned cell draws as a turned text box");
    }

    // A cell hyphenates, or not, as its setting says: in the layout (a word
    // breaks at the end of the first line, with its hyphen) and in the
    // drawing; the zone is the cell's too.
    void tableCellHyphenationLaysOutAndDraws()
    {
        auto doc = jp::Document::blank(QSizeF(612, 792));
        auto t = std::make_shared<jp::TableItem>();
        t->rows = 1;
        t->cols = 1;
        t->colW = {110};
        t->rowH = {100};
        t->rect = QRectF(72, 72, 0, 0);
        t->syncRect();
        t->stroke = jp::Stroke::none();
        t->cells.resize(1);
        t->cells[0].storyId = doc->createStory(QStringLiteral("We met at the international conference today."));
        doc->pages[0]->items.push_back(t);
        auto firstLine = [&](bool hyphenate, double zone) {
            t->cells[0].hyphenate = hyphenate;
            t->cells[0].hyphenZone = zone;
            jp::LayoutCache cache;
            jp::PaintContext ctx;
            ctx.doc = doc.get();
            ctx.cache = &cache;
            QPointF origin;
            const auto *lay = jp::Renderer::cellLayout(ctx, *t, 0, 0, &origin);
            return lay ? lay->lineInfo(0).value(0).text : QString();
        };
        QString on = firstLine(true, 0), off = firstLine(false, 0), far = firstLine(true, 200);
        QVERIFY2(on.endsWith(QChar(0x00AD)), qPrintable(on));   // the line ends in a hyphen
        for (QString *line : {&on, &off, &far}) line->remove(QChar(0x00AD));
        QVERIFY2(on.contains(QStringLiteral("inter")), qPrintable(on));
        QVERIFY2(!off.contains(QStringLiteral("inter")), qPrintable(off));
        QVERIFY2(!far.contains(QStringLiteral("inter")), qPrintable(far));   // moving the word whole leaves less than the zone
        // Drawn: the first line holds the word's start and its hyphen.
        auto topInk = [&](bool hyphenate) {
            t->cells[0].hyphenate = hyphenate;
            t->cells[0].hyphenZone = 0;
            const QImage k = jp::renderPlate(*doc, 0, 3, 72);
            int n = 0;
            for (int y = 72; y < 90; ++y)
                for (int x = 72; x < 182; ++x) n += qGray(k.pixel(x, y)) < 128;
            return n;
        };
        const int hyphenated = topInk(true), whole = topInk(false);
        QVERIFY2(hyphenated > whole, qPrintable(QStringLiteral("%1 vs %2").arg(hyphenated).arg(whole)));
    }

    // Typing in a cell turned on its side: the caret lies across the lines,
    // and a click at a letter puts the caret there.
    void tableCellEditsOnItsSide()
    {
        jp::MainWindow w;
        w.resize(1400, 900);
        w.editor()->setDocument(jp::Document::blank(QSizeF(612, 792)));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        jp::Editor *ed = w.editor();
        auto t = std::make_shared<jp::TableItem>();
        t->rows = 1;
        t->cols = 2;
        t->colW = {40, 120};
        t->rowH = {200};
        t->rect = QRectF(72, 72, 0, 0);
        t->syncRect();
        t->stroke = jp::Stroke::none();
        t->cells.resize(2);
        for (auto &c : t->cells) c.storyId = ed->doc()->createStory(QStringLiteral("Region name"));
        t->cell(0, 0).vertical = true;
        ed->addItem(t);
        jp::Canvas *cv = w.canvas();
        QTest::qWait(50);
        ed->beginTextEdit(t->id, 3, 0, 0);
        QRectF caret = cv->caretViewRect();
        QVERIFY2(caret.width() > caret.height(), "the caret in turned text is a level stroke");
        ed->beginTextEdit(t->id, 3, 0, 1);
        caret = cv->caretViewRect();
        QVERIFY2(caret.height() > caret.width(), "and an upright one in plain text");
        // Each place between letters is found again from where it is drawn:
        // the text reads down the cell, from its top.
        jp::PaintContext ctx;
        ctx.doc = ed->doc();
        ctx.cache = &ed->cache();
        QPointF origin;
        const auto *lay = jp::Renderer::cellLayout(ctx, *t, 0, 0, &origin);
        QVERIFY(lay);
        double lastY = -1;
        for (int pos : {0, 4, 7, 11}) {
            int frame;
            QRectF r;
            QVERIFY(lay->caretRect(pos, &frame, &r));
            const QPointF page = t->rect.topLeft() + QPointF(t->cellRect(0, 0).width() - r.center().y(), r.x());
            QCOMPARE(cv->textPosAt(t->id, page, 0, 0), pos);
            QVERIFY(page.y() > lastY);
            lastY = page.y();
        }
    }

    // Table Layout > Alignment has Text Direction, for the cell the cursor is
    // in or, with the table selected, every cell; new rows follow their
    // neighbors.
    void tableTextDirectionButton()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto made = ed->newTable(QRectF(72, 72, 288, 72), 2, 2);
        ed->addItem(made);
        const QString id = made->id;
        auto turned = [&] {
            QString s;
            for (const auto &c : static_cast<jp::TableItem *>(ed->doc()->item(id))->cells) s += c.vertical ? 'v' : '-';
            return s;
        };
        auto direction = [&] { return w.act(QStringLiteral("tb.direction")); };
        ed->beginTextEdit(id, 0, 1, 0);
        direction()->trigger();
        QCOMPARE(turned(), QStringLiteral("--v-"));   // row by row: the cell the cursor is in
        QTRY_VERIFY(direction()->isChecked());   // the ribbon catches up on its next refresh
        ed->beginTextEdit(id, 0, 0, 1);
        QTRY_VERIFY2(!direction()->isChecked(), "the button follows the cell");
        direction()->trigger();
        QCOMPARE(turned(), QStringLiteral("-vv-"));
        direction()->trigger();
        QCOMPARE(turned(), QStringLiteral("--v-"));
        // A row added next to a turned cell is turned too.
        ed->beginTextEdit(id, 0, 1, 0);
        w.act(QStringLiteral("tbl.insBelow"))->trigger();
        QCOMPARE(turned(), QStringLiteral("--v-v-"));
        ed->undo();
        QCOMPARE(turned(), QStringLiteral("--v-"));
        // The whole table: every cell turns, and turns back when all are turned.
        ed->endTextEdit();
        ed->select(id);
        direction()->trigger();
        QCOMPARE(turned(), QStringLiteral("vvvv"));
        QTRY_VERIFY(direction()->isChecked());   // the ribbon catches up on its next refresh
        direction()->trigger();
        QCOMPARE(turned(), QStringLiteral("----"));
        ed->undo();
        QCOMPARE(turned(), QStringLiteral("vvvv"));
        // Its place on the ribbon: Table Layout > Alignment, with KeyTips.
        auto *r = w.findChild<jp::Ribbon *>();
        QVERIFY(r);
        const QString d = r->describe(true);
        const QString layout = d.mid(d.indexOf(QStringLiteral("\ntab Table Layout")) + 1).section(QStringLiteral("\ntab "), 0, 0);
        QVERIFY(!layout.isEmpty());
        bool direct = false, hyphen = false;
        for (const QString &line : layout.split(QLatin1Char('\n'))) {
            direct = direct || (line.contains(QStringLiteral("tb.direction")) && line.endsWith(QStringLiteral(" keytip=TD")));
            hyphen = hyphen || (line.contains(QStringLiteral("ribbon.hyphenation")) && line.endsWith(QStringLiteral(" keytip=HY")));
        }
        QVERIFY2(direct && hyphen, qPrintable(layout));
    }

    // The Hyphenation command works in a table: on the cell the cursor is in
    // or, with the table selected, on every cell.
    void tableHyphenationCommand()
    {
        jp::MainWindow w;
        jp::Editor *ed = w.editor();
        auto made = ed->newTable(QRectF(72, 72, 288, 72), 2, 2);
        ed->addItem(made);
        const QString id = made->id;
        auto *t = static_cast<jp::TableItem *>(ed->doc()->item(id));
        for (auto &c : t->cells) ed->doc()->storyDoc(c.storyId)->setPlainText(QStringLiteral("international conference"));
        auto settings = [&] {
            QString s;
            for (const auto &c : static_cast<jp::TableItem *>(ed->doc()->item(id))->cells) s += c.hyphenate ? 'h' : '-';
            return s;
        };
        auto hyphenated = [&] {
            QString s;
            for (const auto &c : static_cast<jp::TableItem *>(ed->doc()->item(id))->cells)
                s += ed->doc()->storyDoc(c.storyId)->toPlainText().contains(QChar(0x00AD)) ? 'o' : '-';
            return s;
        };
        // The dialog: automatic hyphenation on or off, and optional hyphens added.
        auto command = [&](bool automatic, bool addHyphens) {
            QTimer::singleShot(0, &w, [&, automatic, addHyphens] {
                auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dlg) return;
                auto *check = dlg->findChild<QCheckBox *>();
                if (!check) {
                    dlg->reject();   // the "click in a text box" notice
                    return;
                }
                check->setChecked(automatic);
                if (addHyphens)
                    for (auto *b : dlg->findChildren<QPushButton *>())
                        if (b->text() == QLatin1String("Add Optional Hyphens")) b->click();
                dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            w.act(QStringLiteral("rev.hyphenation"))->trigger();
        };
        QCOMPARE(settings(), QStringLiteral("hhhh"));
        ed->beginTextEdit(id, 0, 1, 1);
        command(false, true);
        QCOMPARE(settings(), QStringLiteral("hhh-"));   // the cell the cursor is in
        QCOMPARE(hyphenated(), QStringLiteral("---o"));
        ed->undo();
        ed->undo();
        QCOMPARE(settings(), QStringLiteral("hhhh"));
        ed->endTextEdit();
        ed->select(id);
        command(false, true);
        QCOMPARE(settings(), QStringLiteral("----"));   // the whole table
        QCOMPARE(hyphenated(), QStringLiteral("oooo"));
        command(true, false);
        QCOMPARE(settings(), QStringLiteral("hhhh"));
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

    // Changing the page size, in the Size gallery or the Page Setup window,
    // scales nothing: every object moves by half the change on each axis and
    // keeps its distance from the page center, as in Publisher (Letter to A4:
    // 8.36 points left, 24.94 down). Ruler guides keep their share of the
    // page. Undo puts everything back in one step.
    void pageSizeKeepsDistanceFromCenter()
    {
        using namespace jp;
        const QSizeF a4(595.2756, 841.8898);
        auto make = [] {
            auto doc = Document::blank(QSizeF(612, 792), QStringLiteral("Letter"), 2);
            auto shape = [](const QRectF &r) {
                auto s = std::make_shared<ShapeItem>();
                s->rect = r;
                return s;
            };
            auto text = std::make_shared<TextItem>();
            text->rect = QRectF(72, 300, 200, 60);
            text->storyId = doc->createStory(QStringLiteral("Hello"));
            QTextCursor tc(doc->storyDoc(text->storyId));
            tc.select(QTextCursor::Document);
            QTextCharFormat big;
            big.setFontPointSize(18);
            tc.mergeCharFormat(big);
            auto turned = shape(QRectF(300, 400, 80, 40));
            turned->rotation = 30;
            auto line = std::make_shared<LineItem>();
            line->p1 = QPointF(50, 500);
            line->p2 = QPointF(200, 520);
            line->syncRect();
            auto group = std::make_shared<GroupItem>();
            group->children = {shape(QRectF(400, 100, 30, 30)), shape(QRectF(440, 100, 30, 30))};
            group->syncRect();
            doc->pages[0]->items = {shape(QRectF(72, 72, 100, 50)), text, turned, line, group};
            doc->pages[0]->guides.v = {200};
            doc->pages[0]->guides.h = {300};
            doc->pages[1]->items = {shape(QRectF(100, 200, 80, 40))};
            doc->masters[0]->items = {shape(QRectF(36, 36, 50, 50))};
            doc->masters[0]->guides.v = {100};
            doc->scratch = {shape(QRectF(-200, 100, 50, 50))};
            return doc;
        };
        const double dx = (a4.width() - 612) / 2, dy = (a4.height() - 792) / 2;
        QCOMPARE(QString::number(dx, 'f', 2), QStringLiteral("-8.36"));
        QCOMPARE(QString::number(dy, 'f', 2), QStringLiteral("24.94"));
        for (const bool gallery : {true, false}) {
            Editor ed;
            ed.setDocument(make());
            Document *d = ed.doc();
            const QByteArray before = QJsonDocument(d->toJson()).toJson();
            const QRectF first = d->pages[0]->items[0]->rect, turned = d->pages[0]->items[2]->rect;
            const QPointF p1 = static_cast<LineItem *>(d->pages[0]->items[3].get())->p1;
            const QRectF member = static_cast<GroupItem *>(d->pages[0]->items[4].get())->children[1]->rect;
            const QRectF masterItem = d->masters[0]->items[0]->rect, scratchItem = d->scratch[0]->rect;
            if (gallery) {
                applyPageSize(&ed, a4, QStringLiteral("A4"));
            } else {
                PageSetup ns = d->setup;
                ns.size = ns.sheet = a4;
                applyPageSetup(&ed, ns, QStringLiteral("A4"), QStringLiteral("Page Setup"));
            }
            QCOMPARE(d->setup.size, a4);
            QCOMPARE(d->setup.sizeName, QStringLiteral("A4"));
            // Nothing is scaled: sizes stay, positions shift.
            QCOMPARE(d->pages[0]->items[0]->rect, first.translated(dx, dy));
            QCOMPARE(d->pages[0]->items[2]->rect, turned.translated(dx, dy));
            QCOMPARE(d->pages[0]->items[2]->rotation, 30.0);
            QCOMPARE(static_cast<LineItem *>(d->pages[0]->items[3].get())->p1, p1 + QPointF(dx, dy));
            QCOMPARE(static_cast<GroupItem *>(d->pages[0]->items[4].get())->children[1]->rect, member.translated(dx, dy));
            QCOMPARE(d->pages[1]->items[0]->rect, QRectF(100 + dx, 200 + dy, 80, 40));
            QCOMPARE(d->masters[0]->items[0]->rect, masterItem.translated(dx, dy));
            QCOMPARE(d->scratch[0]->rect, scratchItem);
            auto *t = static_cast<TextItem *>(d->pages[0]->items[1].get());
            QCOMPARE(t->rect.size(), QSizeF(200, 60));
            QCOMPARE(QTextCursor(d->storyDoc(t->storyId)->begin()).charFormat().fontPointSize(), 18.0);
            QCOMPARE(d->setup.margins, QMarginsF(36, 36, 36, 36));
            // Ruler guides keep their share of the page.
            QCOMPARE(d->pages[0]->guides.v[0], 200 * a4.width() / 612);
            QCOMPARE(d->pages[0]->guides.h[0], 300 * a4.height() / 792);
            QCOMPARE(d->masters[0]->guides.v[0], 100 * a4.width() / 612);
            QCOMPARE(d->scratch.size(), size_t(1));
            // One undo step restores all of it.
            QCOMPARE(ed.undoStack()->count(), 1);
            ed.undo();
            QCOMPARE(QJsonDocument(d->toJson()).toJson(), before);
        }
    }

    // What a smaller page leaves wholly off it goes to the scratch area, where
    // the shift put it, and the scratch area itself doesn't move. On a
    // two-page master each page's objects keep their places across and move
    // half the change down.
    void pageSizeSendsOffPageObjectsToScratch()
    {
        using namespace jp;
        auto doc = Document::blank(QSizeF(612, 792), QStringLiteral("Letter"), 2);
        auto shape = [](const QRectF &r) {
            auto s = std::make_shared<ShapeItem>();
            s->rect = r;
            return s;
        };
        auto corner = shape(QRectF(72, 72, 100, 50)), far = shape(QRectF(500, 700, 60, 60)), mid = shape(QRectF(300, 500, 60, 60));
        auto onSecond = shape(QRectF(100, 200, 80, 40)), masterItem = shape(QRectF(36, 36, 50, 50)), kept = shape(QRectF(-200, 100, 50, 50));
        doc->pages[0]->items = {corner, far, mid};
        doc->pages[1]->items = {onSecond};
        doc->masters[0]->items = {masterItem};
        doc->scratch = {kept};
        auto spread = std::make_shared<MasterPage>();
        spread->id = QStringLiteral("S");
        spread->twoPage = true;
        auto leftOff = shape(QRectF(100, 100, 40, 40)), rightOff = shape(QRectF(700, 100, 40, 40));
        auto leftOn = shape(QRectF(100, 400, 40, 40)), rightOn = shape(QRectF(700, 400, 40, 40));
        spread->items = {leftOff, rightOff, leftOn, rightOn};
        doc->masters.push_back(spread);
        Editor ed;
        ed.setDocument(std::move(doc));
        Document *d = ed.doc();
        applyPageSize(&ed, QSizeF(300, 300), QStringLiteral("Small"));
        // Half of 312 across and 492 down.
        QCOMPARE(d->pages[0]->items.size(), size_t(1));
        QCOMPARE(mid->rect, QRectF(144, 254, 60, 60));
        QVERIFY(d->pages[1]->items.empty());
        QVERIFY(d->masters[0]->items.empty());
        QCOMPARE(d->scratch.size(), size_t(7));
        QVERIFY(d->scratch[0] == kept);
        QCOMPARE(kept->rect, QRectF(-200, 100, 50, 50));
        QVERIFY(d->scratch[1] == corner);
        QCOMPARE(corner->rect, QRectF(-84, -174, 100, 50));
        QVERIFY(d->scratch[2] == far);
        QCOMPARE(far->rect, QRectF(344, 454, 60, 60));
        QVERIFY(d->scratch[3] == onSecond);
        QCOMPARE(onSecond->rect, QRectF(-56, -46, 80, 40));
        QVERIFY(d->scratch[4] == masterItem);
        QCOMPARE(masterItem->rect, QRectF(-120, -210, 50, 50));
        // Two-page master: the right page's objects follow the page's edge
        // (now at 300), and what is wholly off goes to the scratch area at
        // its place on its own page.
        QVERIFY(d->scratch[5] == leftOff);
        QCOMPARE(leftOff->rect, QRectF(100, -146, 40, 40));
        QVERIFY(d->scratch[6] == rightOff);
        QCOMPARE(rightOff->rect, QRectF(88, -146, 40, 40));
        QCOMPARE(d->masters[1]->items.size(), size_t(2));
        QCOMPARE(leftOn->rect, QRectF(100, 154, 40, 40));
        QCOMPARE(rightOn->rect, QRectF(388, 154, 40, 40));
        // A later change leaves the scratch area where it is.
        applyPageSize(&ed, QSizeF(400, 400), QStringLiteral("Square"));
        QCOMPARE(far->rect, QRectF(344, 454, 60, 60));
        QCOMPARE(mid->rect, QRectF(194, 304, 60, 60));
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
        for (const QString &f : {QStringLiteral("/Templates/club.jpub"), QStringLiteral("/BuildingBlocks/logo.json")}) {
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

    // Metafiles made here play the same on every system: boxes and ovals,
    // text, a bitmap, and text in the symbol fonts (Wingdings, Symbol) that
    // Windows has and Linux lacks, which shows as the same pictures from Unicode.
    void metafileDrawnFromScratch()
    {
        auto le16 = [](QByteArray &b, int v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); };
        auto le32 = [](QByteArray &b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF)); };
        auto words = [&](std::initializer_list<int> v) { QByteArray b; for (int w : v) le16(b, w); return b; };
        auto ints = [&](std::initializer_list<qint32> v) { QByteArray b; for (qint32 w : v) le32(b, quint32(w)); return b; };
        // A record: a metafile's has its length in words and a function, an
        // enhanced metafile's a type and its length in bytes.
        auto wmfRecord = [&](int fn, const QByteArray &params) {
            QByteArray b;
            le32(b, quint32((6 + params.size() + (params.size() & 1)) / 2));
            le16(b, fn);
            b += params;
            if (params.size() & 1) b.append('\0');
            return b;
        };
        auto emfRecord = [&](quint32 type, const QByteArray &payload) {
            QByteArray pad = payload;
            while (pad.size() % 4) pad.append('\0');
            QByteArray b;
            le32(b, type);
            le32(b, quint32(8 + pad.size()));
            return b + pad;
        };
        // A 400 by 300 drawing of a blue box (20 to 120 across), a red oval
        // (280 to 380), and a line of text in `face` at (x, y), `em` tall;
        // `more` is records after them.
        auto wmf = [&](const QString &face, int charset, const QByteArray &text, int x, int y, int em, const QByteArray &more = QByteArray()) {
            QByteArray body = wmfRecord(0x020B, words({0, 0})) + wmfRecord(0x020C, words({300, 400})) + wmfRecord(0x0102, words({1}));   // window, transparent text
            QByteArray blue = words({0}), red = words({0}), pen = words({5, 0, 0});
            le32(blue, 0xFF0000);
            le16(blue, 0);
            le32(red, 0x0000FF);
            le16(red, 0);
            le32(pen, 0);
            body += wmfRecord(0x02FC, blue) + wmfRecord(0x02FC, red) + wmfRecord(0x02FA, pen);   // objects 0, 1, 2
            body += wmfRecord(0x012D, words({2})) + wmfRecord(0x012D, words({0})) + wmfRecord(0x041B, words({80, 120, 20, 20}));
            body += wmfRecord(0x012D, words({1})) + wmfRecord(0x0418, words({80, 380, 20, 280}));
            QByteArray font = words({-em, 0, 0, 0, 400});
            font.append(char(0)).append(char(0)).append(char(0)).append(char(charset));
            font += QByteArray(4, '\0') + face.toLatin1() + '\0';
            body += wmfRecord(0x02FB, font) + wmfRecord(0x012D, words({3}));
            QByteArray line = words({int(text.size())}) + text;
            if (text.size() & 1) line.append('\0');
            le16(line, y);
            le16(line, x);
            body += wmfRecord(0x0209, QByteArray(4, '\0')) + wmfRecord(0x0521, line) + more + wmfRecord(0x0000, QByteArray());
            QByteArray head, standard;
            le32(head, 0x9AC6CDD7u);
            le16(head, 0);
            for (int v : {0, 0, 400, 300, 72}) le16(head, v);   // box, units to the inch
            le32(head, 0);
            quint16 sum = 0;
            for (int i = 0; i < 20; i += 2) sum ^= quint16(quint8(head[i]) | (quint8(head[i + 1]) << 8));
            le16(head, sum);
            for (int v : {1, 9, 0x300}) le16(standard, v);
            le32(standard, quint32((18 + body.size()) / 2));
            le16(standard, 4);
            le32(standard, 60);
            le16(standard, 0);
            return head + standard + body;
        };
        // The same drawing as an enhanced metafile, its text in UTF-16.
        auto emf = [&](const QString &face, int charset, const QString &text, int x, int y, int em, const QByteArray &more = QByteArray()) {
            QByteArray body = emfRecord(18, ints({1}));                           // transparent text
            body += emfRecord(39, ints({1, 0, 0xFF0000, 0})) + emfRecord(39, ints({2, 0, 0x0000FF, 0})) + emfRecord(37, ints({qint32(0x80000008u)}));   // blue, red, no pen
            body += emfRecord(37, ints({1})) + emfRecord(43, ints({20, 20, 120, 80})) + emfRecord(37, ints({2})) + emfRecord(42, ints({280, 20, 380, 80}));
            QByteArray lf = ints({3, -em, 0, 0, 0, 400});
            lf.append(char(0)).append(char(0)).append(char(0)).append(char(charset));
            lf += QByteArray(4, '\0');
            QByteArray name(64, '\0');
            for (int i = 0; i < face.size() && i < 31; ++i) { name[2 * i] = char(face[i].unicode() & 0xFF); name[2 * i + 1] = char(face[i].unicode() >> 8); }
            body += emfRecord(82, lf + name) + emfRecord(37, ints({3})) + emfRecord(24, ints({0}));
            QByteArray chars;
            for (QChar c : text) le16(chars, c.unicode());
            while (chars.size() % 4) chars.append('\0');
            const int fixed = 8 + 16 + 12 + 40;   // record header, bounds, scales, and the text's own fields
            QByteArray spacing;
            for (int i = 0; i < text.size(); ++i) le32(spacing, 13);
            const QByteArray run = ints({0, 0, 0, 0, 1}) + QByteArray(8, '\0') + ints({x, y, int(text.size()), fixed, 0, 0, 0, 0, 0, fixed + int(chars.size())}) + chars + spacing;
            body += emfRecord(84, run) + more + emfRecord(14, ints({0, 16, 20}));
            // Bounds, a frame of 400 by 300 pixels (in hundredths of a millimeter on a 3,840 pixel screen 1,016 millimeters wide), and the counts.
            const QByteArray head = ints({0, 0, 399, 299, 0, 0, 10583, 7937, qint32(0x464D4520u), 0x10000, 0, 1, 1, 0, 0, 0, 3840, 2880, 1016, 762});
            QByteArray all = emfRecord(1, head) + body;
            qToLittleEndian<quint32>(quint32(all.size()), all.data() + 48);
            return all;
        };
        auto picture = [](const QByteArray &data) {
            Metafile m;
            if (!m.load(data)) return QImage();
            return m.toImage(400);
        };
        // The dark pixels in an area, and the box they fill.
        auto ink = [](const QImage &img, const QRect &area, QRect *box = nullptr) {
            int n = 0, x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QRgb c = img.pixel(x, y);
                    if (qAlpha(c) > 128 && qRed(c) < 100 && qGreen(c) < 100 && qBlue(c) < 100) {
                        ++n;
                        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
                }
            if (box) *box = n ? QRect(QPoint(x0, y0), QPoint(x1, y1)) : QRect();
            return n;
        };
        const QByteArray wmfBytes = wmf(QStringLiteral("Arial"), 0, "Arial Hello", 20, 120, 28), emfBytes = emf(QStringLiteral("Arial"), 0, QStringLiteral("Arial Hello"), 20, 120, 28);
        for (const QImage &img : {picture(wmfBytes), picture(emfBytes)}) {
            QCOMPARE(img.size(), QSize(400, 300));
            QCOMPARE(QColor(img.pixel(70, 50)), QColor(0, 0, 255));       // the box
            QCOMPARE(QColor(img.pixel(330, 50)), QColor(255, 0, 0));      // the oval
            QCOMPARE(qAlpha(img.pixel(200, 50)), 0);                      // nothing between them
            QVERIFY(ink(img, QRect(0, 110, 400, 70)) > 150);              // the words
        }
        // Wingdings' round bullet is a solid disc, with the font or without
        // it: not an empty box for a missing letter, nor a thin "l".
        for (const QImage &img : {picture(wmf(QStringLiteral("Wingdings"), 2, "l", 120, 150, 120)), picture(emf(QStringLiteral("Wingdings"), 2, QString(QChar(0xF06C)), 120, 150, 120))}) {
            QRect box;
            const int dark = ink(img, QRect(0, 120, 400, 180), &box);
            QVERIFY2(!box.isEmpty() && dark > box.width() * box.height() * 0.6, qPrintable(QStringLiteral("%1 dark in %2 x %3").arg(dark).arg(box.width()).arg(box.height())));
        }
        // Symbol's Greek letters show too (Linux calls another font in for Symbol).
        for (const QImage &img : {picture(wmf(QStringLiteral("Symbol"), 2, "a", 120, 150, 120)), picture(emf(QStringLiteral("Symbol"), 2, QString(QChar(0xF061)), 120, 150, 120))})
            QVERIFY(ink(img, QRect(0, 120, 400, 180)) > 300);
        auto near = [](QRgb c, QColor want) { return qAlpha(c) == 255 && std::abs(qRed(c) - want.red()) < 12 && std::abs(qGreen(c) - want.green()) < 12 && std::abs(qBlue(c) - want.blue()) < 12; };
        // A bitmap inside one (2 by 2 pixels over 150 to 250 across and 100
        // to 200 down: blue and white above red and green) is decoded and drawn.
        QByteArray dib;
        for (quint32 v : {40u, 2u, 2u}) le32(dib, v);
        le16(dib, 1);
        le16(dib, 24);
        for (quint32 v : {0u, 16u, 0u, 0u, 0u, 0u}) le32(dib, v);
        dib += QByteArray::fromHex("0000FF00FF000000") + QByteArray::fromHex("FF0000FFFFFF0000");   // bottom row first, blue-green-red, each row padded to 4 bytes
        QByteArray stretchWmf, stretchEmf;
        le32(stretchWmf, 0x00CC0020);   // copy
        for (int v : {0, 2, 2, 0, 0, 100, 100, 100, 150}) le16(stretchWmf, v);   // usage; source height, width, y, x; destination height, width, y, x
        for (quint32 v : {0u, 0u, 0u, 0u, 150u, 100u, 0u, 0u, 2u, 2u, 80u, 40u, 120u, 16u, 0u, 0x00CC0020u, 100u, 100u}) le32(stretchEmf, v);   // bounds, destination, source, where the bitmap and its bits are
        stretchWmf += dib;
        stretchEmf += dib;
        for (const QImage &img : {picture(wmf(QStringLiteral("Arial"), 0, "", 0, 0, 12, wmfRecord(0x0F43, stretchWmf))),
                                  picture(emf(QStringLiteral("Arial"), 0, QString(), 0, 0, 12, emfRecord(81, stretchEmf)))}) {
            QVERIFY(near(img.pixel(175, 125), QColor(0, 0, 255)));
            QVERIFY(near(img.pixel(225, 125), QColor(255, 255, 255)));
            QVERIFY(near(img.pixel(175, 175), QColor(255, 0, 0)));
            QVERIFY(near(img.pixel(225, 175), QColor(0, 255, 0)));
        }
        // Insert Picture takes both from files, whatever the case of the
        // extension, and the page shows the picture on screen and in print.
        QTemporaryDir dir;
        MainWindow w;
        const QStringList names = {QStringLiteral("clip.WMF"), QStringLiteral("clip.emf")};
        for (int i = 0; i < 2; ++i) {
            QFile f(dir.filePath(names[i]));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(i ? emfBytes : wmfBytes);
            f.close();
            w.insertFiles({f.fileName()}, QPointF(20, 20 + 360 * i));
        }
        Editor *ed = w.editor();
        QCOMPARE(ed->doc()->pages[0]->items.size(), size_t(2));
        PaintContext ctx;
        ctx.doc = ed->doc();
        ctx.cache = &ed->cache();
        for (int i = 0; i < 2; ++i) {
            const auto *pic = static_cast<PictureItem *>(ed->doc()->pages[0]->items[i].get());
            QCOMPARE(ed->doc()->images.value(pic->imageId).format, i ? QStringLiteral("emf") : QStringLiteral("wmf"));
            for (const bool output : {false, true}) {
                ctx.opt.output = output;
                const QImage page = Renderer::renderToImage(ctx, 0, 1.0);
                // The blue box, 70 of the picture's 400 units across and 50 of its 300 down.
                const QPoint box(int(pic->rect.x() + pic->rect.width() * 70 / 400), int(pic->rect.y() + pic->rect.height() * 50 / 300));
                QVERIFY2(QColor(page.pixel(box)) == QColor(0, 0, 255), qPrintable(QStringLiteral("picture %1, output %2").arg(i).arg(output)));
            }
        }
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
