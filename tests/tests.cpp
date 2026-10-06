#include "core/document.h"
#include "io/cfb.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "core/fonts.h"
#include "render/metafile.h"
#include "text/storyio.h"
#include "text/textengine.h"
#include "text/textprops.h"
#include "render/renderer.h"
#include "render/shapes.h"
#include "render/textart.h"
#include "templates/templates.h"
#include "app/appfuncs.h"
#include "io/importers.h"
#include <QPrinter>
#include "app/icons.h"
#include <QTabBar>
#include <QTabWidget>
#include "app/dialogs.h"
#include "app/editor.h"
#include "app/mainwindow.h"
#include "app/ribbon.h"
#include "app/updater.h"
#include "app/widgets.h"
#include "canvas/canvas.h"
#include <QTemporaryDir>
#include <QLineEdit>
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <QBuffer>
#include <QAbstractItemView>
#include <QPainter>

#include <QTextCursor>
#include <QTextDocument>
#include <QDirIterator>
#include <QtTest>

using namespace jp;

class Tests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
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
    void templatesFitTheirText()
    {
        QStringList problems;
        for (const jp::TemplateInfo &t : jp::templates()) {
            std::unique_ptr<jp::Document> doc = t.build(jp::TemplateOptions());
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
        const QSet<QString> skip = {"file.exit", "win.new", "win.arrange", "win.cascade"};
        jp::MainWindow w;
        w.resize(1400, 900);
        w.show();
        QStringList ran;
        for (const QString &id : w.actionIds()) {
            if (skip.contains(id)) continue;
            QAction *a = w.act(id);
            if (!a) continue;
            current = id;
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
            if (gp.isEmpty()) gp.addRect(QRectF(QPointF(), match->rect.size()));
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
        // The rectangle comes back turned; the importer draws other shapes
        // as outlines, so the oval is checked by where it lands.
        for (const jp::ShapeItem *s : shapes) {
            const QString got = QStringLiteral("rot %1 %2,%3 %4x%5").arg(s->rotation).arg(s->rect.x()).arg(s->rect.y()).arg(s->rect.width()).arg(s->rect.height());
            if (s->customPath.isEmpty()) {
                QVERIFY2(std::abs(s->rotation - 30) < 0.5, qPrintable(got));
                QVERIFY2(QLineF(s->rect.center(), rect->rect.center()).length() < 1 && std::abs(s->rect.width() - 200) < 1 &&
                             std::abs(s->rect.height() - 80) < 1, qPrintable(got));
            } else {
                const QRectF want = oval->bounds(), b = s->bounds();
                QVERIFY2(std::abs(b.left() - want.left()) < 1 && std::abs(b.top() - want.top()) < 1 && std::abs(b.width() - want.width()) < 1 &&
                             std::abs(b.height() - want.height()) < 1, qPrintable(got));
            }
        }
        QVERIFY(bp);
        QVERIFY(std::abs(bp->rect.left() - 100) < 1 && std::abs(bp->rect.width() - 160) < 1 && std::abs(bp->rect.height() - 80) < 1);
        const QImage got = back->image(bp->imageId);
        QCOMPARE(got.size(), QSize(40, 20));
        QCOMPARE(got.pixelColor(5, 5), QColor(220, 30, 30));
        QCOMPARE(got.pixelColor(35, 5), QColor(30, 30, 220));
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
        QTest::qWait(80);
        jp::SizeCombo *box = nullptr;
        for (jp::SizeCombo *s : w.findChildren<jp::SizeCombo *>())
            if (s->isVisible()) { box = s; break; }
        QVERIFY(box);
        QCOMPARE(box->currentText(), QStringLiteral("24"));
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
    void updateVersionOrder()
    {
        QVERIFY(jp::Updater::isNewer("v0.1.6", "0.1.0"));
        QVERIFY(jp::Updater::isNewer("v0.1.6", "v0.1.0-preview5"));
        QVERIFY(jp::Updater::isNewer("v0.1.0-preview5", "v0.1.0-preview4"));
        QVERIFY(jp::Updater::isNewer("0.1.10", "0.1.9"));
        QVERIFY(!jp::Updater::isNewer("v0.1.6", "0.1.6"));
        QVERIFY(!jp::Updater::isNewer("v0.1.0-preview5", "0.1.6"));
        QVERIFY(jp::Updater::isNewer("v0.1.0", "v0.1.0-preview5"));
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
