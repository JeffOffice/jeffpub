// Command-line helper for testing: import a .pub (or open a .jpub) and render
// every page to PNG, printing what was found.
//   jpubtool render <file> <outdir> [dpi]
//   jpubtool convert <file.pub> <file.jpub>
//   jpubtool template <id|all> <outdir> [dpi]   render built-in templates
//   jpubtool pdfcheck <file.pdf> <outdir> [pages] compare a PDF's pages drawn as
//                                                 vectors with PDFium's raster

#include "core/fonts.h"
#include "io/cfb.h"
#include "io/importers.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "render/pdfpage.h"
#include "render/renderer.h"
#include "templates/templates.h"
#include "text/textengine.h"
#include "text/textprops.h"
#include "io/pubshapes.h"
#include "render/shapes.h"
#include <QPainter>

#include <QTextBlock>
#include <QTextDocument>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QGuiApplication>
#include <QTextStream>

using namespace jp;

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    initCore();
    const QStringList args = app.arguments();
    QTextStream out(stdout);
    if (args.size() < 4) {
        out << "usage: jpubtool render <file> <outdir> [dpi [pages]] | convert <in> <out.jpub> | repack <in.pub> <out.pub> | topub <in> <out.pub> | layout <file> x\n";
        return 2;
    }
    const QString cmd = args[1], in = args[2];
    if (cmd == "lineem") {
        // lineem <family>[,<family>...] x: the single line height (ems) and
        // the font drawn, regular and bold, for each family.
        for (const QString &fam : in.split(QLatin1Char(','))) {
            for (bool bold : {false, true}) {
                QFont f(fam);
                f.setFamilies({fam});
                f.setBold(bold);
                f.setPointSizeF(100);
                out << fam << (bold ? " bold" : "") << "\t" << naturalLineEm(f, fam) << "\t" << QFontInfo(f).family() << "\n";
            }
        }
        return 0;
    }
    if (cmd == "pdfcheck") {
        QFile f(in);
        if (!f.open(QIODevice::ReadOnly)) return 1;
        const PdfDocument doc(f.readAll());
        if (!doc.isValid()) {
            out << "not a PDF\n";
            return 1;
        }
        QDir().mkpath(args[3]);
        const int pages = std::min(doc.pageCount(), args.size() > 4 ? args[4].toInt() : 5);
        for (int pg = 0; pg < pages; ++pg) {
            const QSizeF pt = doc.pageSize(pg);
            const QSize px = (pt * 100.0 / 72.0).toSize();
            QElapsedTimer t;
            t.start();
            const QImage ref = doc.render(pg, px).convertToFormat(QImage::Format_ARGB32);
            const qint64 rasterMs = t.restart();
            QImage played(px, QImage::Format_ARGB32);
            played.fill(Qt::transparent);
            {
                QPainter p(&played);
                doc.play(&p, pg, QRectF(QPointF(0, 0), QSizeF(px)));
            }
            const qint64 playMs = t.elapsed();
            auto over = [](QRgb c) { const int a = qAlpha(c); return QColor(255 - a + qRed(c) * a / 255, 255 - a + qGreen(c) * a / 255, 255 - a + qBlue(c) * a / 255); };
            int differ = 0, inked = 0;
            QImage both(px.width() * 2, px.height(), QImage::Format_RGB32);
            both.fill(Qt::white);
            for (int y = 0; y < px.height(); ++y)
                for (int x = 0; x < px.width(); ++x) {
                    const QColor a = over(ref.pixel(x, y)), b = over(played.pixel(x, y));
                    if (a != Qt::white) ++inked;
                    if (std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) > 120) ++differ;
                    both.setPixelColor(x, y, a);
                    both.setPixelColor(px.width() + x, y, b);
                }
            t.restart();
            const double share = doc.rasterShare(pg);
            out << QStringLiteral("page %1: %2 of %3 inked pixels differ (%4%), raster %5 ms, vectors %6 ms; check %7 ms finds %8% for the raster\n")
                       .arg(pg + 1).arg(differ).arg(inked).arg(inked ? 100.0 * differ / inked : 0, 0, 'f', 2).arg(rasterMs).arg(playMs).arg(t.elapsed()).arg(100 * share, 0, 'f', 1);
            both.save(QStringLiteral("%1/page%2.png").arg(args[3]).arg(pg + 1));
        }
        return 0;
    }
    if (cmd == "shapecheck") {
        // shapecheck <tmpdir> <w>x<h> [preset...]: save each preset shape to
        // .pub, read it back, and score how well the outlines overlap.
        const QStringList wh = args[3].split('x');
        const double w = wh.value(0).toDouble(), h = wh.value(1).toDouble();
        QStringList ids = args.mid(4);
        if (ids.isEmpty())
            for (const ShapeDef &d : shapeLibrary()) ids << d.id;
        auto mask = [&](const QPainterPath &p, bool stroke) {
            QImage img(int(w) + 40, int(h) + 40, QImage::Format_Grayscale8);
            img.fill(0);
            QPainter g(&img);
            g.setRenderHint(QPainter::Antialiasing, false);
            g.translate(20, 20);
            if (stroke) g.strokePath(p, QPen(Qt::white, 3));
            else g.fillPath(p, Qt::white);
            return img;
        };
        for (const QString &id : ids) {
            const ShapeDef *def = shapeDef(id);
            if (!def) continue;
            auto doc = Document::blank(QSizeF(612, 792));
            auto s = std::make_shared<ShapeItem>();
            s->shape = id;
            s->rect = QRectF(100, 100, w, h);
            s->fill = Fill::solid(ColorRef::rgb(QColor(30, 60, 200)));
            s->stroke = def->open ? Stroke::line(ColorRef::rgb(Qt::black), 1) : Stroke::none();
            s->rotation = qEnvironmentVariableIntValue("JP_SHAPECHECK_ROT");
            s->flipH = qEnvironmentVariableIsSet("JP_SHAPECHECK_FLIPH");
            doc->pages[0]->items.push_back(s);
            const QString path = in + "/" + id + ".pub";
            QString err;
            exportPublisher(*doc, path, &err);
            auto back = importPublisherFile(path, &err);
            QPainterPath got;
            int n = 0;
            if (back)
                walkItems(back->pages[0]->items, [&](const ItemPtr &it) {
                    if (it->type() == ItemType::Shape || it->type() == ItemType::Line) {
                        const Item *bi = it.get();
                        QPainterPath p;
                        if (it->type() == ItemType::Line) {
                            auto *l = static_cast<const LineItem *>(bi);
                            p.moveTo(l->p1);
                            p.lineTo(l->p2);
                        } else {
                            auto *bs = static_cast<const ShapeItem *>(bi);
                            p = bs->customPath.isEmpty() ? QPainterPath() : bs->customPath;
                            if (p.isEmpty()) p.addRect(QRectF(QPointF(), bs->rect.size()));
                            p = bi->transform().map(p);
                        }
                        got.addPath(p.translated(-100, -100));
                        ++n;
                    }
                });
            const QImage a = mask(s->transform().map(shapePath(id, QSizeF(w, h))).translated(-100, -100), def->open), b = mask(got, def->open);
            if (qEnvironmentVariableIsSet("JP_SHAPECHECK_DUMP")) {
                a.save(in + "/" + id + "-want.png");
                b.save(in + "/" + id + "-got.png");
            }
            qint64 both = 0, any = 0;
            for (int y = 0; y < a.height(); ++y) {
                const uchar *ra = a.constScanLine(y), *rb = b.constScanLine(y);
                for (int x = 0; x < a.width(); ++x) {
                    both += ra[x] && rb[x];
                    any += ra[x] || rb[x];
                }
            }
            out << id << "\t" << pubShapeType(id) << "\t" << (any ? QString::number(100.0 * both / any, 'f', 1) : QStringLiteral("-")) << "%\t" << n
                << " item(s)\n";
            out.flush();
        }
        return 0;
    }
    if (cmd == "setstream") {
        // setstream <base.pub> <out.pub> <stream> <file>: replace one stream's bytes.
        if (args.size() < 6) { out << "usage: jpubtool setstream <base> <out> <stream> <file>\n"; return 2; }
        cfb::File base;
        QString err;
        QFile bf(in), rf(args[5]);
        if (!bf.open(QIODevice::ReadOnly) || !cfb::read(bf.readAll(), &base, &err)) { out << "bad base: " << err << "\n"; return 1; }
        if (!rf.open(QIODevice::ReadOnly)) { out << "can't read " << args[5] << "\n"; return 1; }
        const QString sp = QString(args[4]).replace(QStringLiteral("\\x01"), QStringLiteral("\x01")).replace(QStringLiteral("\\x05"), QStringLiteral("\x05"));
        if (!base.setStream(sp, rf.readAll())) { out << "no stream " << sp << "\n"; return 1; }
        QFile o(args[3]);
        if (!o.open(QIODevice::WriteOnly)) { out << "can't write\n"; return 1; }
        o.write(cfb::write(base));
        out << "OK\t" << args[3] << "\n";
        return 0;
    }
    if (cmd == "graft") {
        // graft <base.pub> <donor.pub> <out.pub> <stream>...: the base file
        // with the named streams taken from the donor. Finds which stream a
        // strict reader objects to.
        if (args.size() < 6) { out << "usage: jpubtool graft <base> <donor> <out> <stream>...\n"; return 2; }
        cfb::File base, donor;
        QString err;
        QFile bf(in), df(args[3]);
        if (!bf.open(QIODevice::ReadOnly) || !cfb::read(bf.readAll(), &base, &err)) { out << "bad base: " << err << "\n"; return 1; }
        if (!df.open(QIODevice::ReadOnly) || !cfb::read(df.readAll(), &donor, &err)) { out << "bad donor: " << err << "\n"; return 1; }
        for (int i = 5; i < args.size(); ++i) {
            const QString sp = QString(args[i]).replace(QStringLiteral("\\x01"), QStringLiteral("\x01")).replace(QStringLiteral("\\x05"), QStringLiteral("\x05"));
            if (donor.find(sp) < 0 || !base.setStream(sp, donor.stream(sp))) { out << "no stream " << sp << "\n"; return 1; }
        }
        QFile o(args[4]);
        if (!o.open(QIODevice::WriteOnly)) { out << "can't write\n"; return 1; }
        o.write(cfb::write(base));
        out << "OK\t" << args[4] << "\n";
        return 0;
    }
    if (cmd == "repack") {
        // Rewrite a compound file with JeffPub's container writer, every
        // stream unchanged: tests the container layer against other readers.
        QFile f(in);
        if (!f.open(QIODevice::ReadOnly)) { out << "can't read " << in << "\n"; return 1; }
        cfb::File c;
        QString err;
        if (!cfb::read(f.readAll(), &c, &err)) { out << "not a compound file: " << err << "\n"; return 1; }
        QFile o(args[3]);
        if (!o.open(QIODevice::WriteOnly)) { out << "can't write " << args[3] << "\n"; return 1; }
        o.write(cfb::write(c));
        out << "OK\t" << args[3] << "\n";
        return 0;
    }
    if (cmd == "template") {
        const double dpi = args.size() > 4 ? args[4].toDouble() : 60;
        QDir().mkpath(args[3]);
        for (const TemplateInfo &t : templates()) {
            if (in != "all" && in != t.id) continue;
            TemplateOptions topt;
            // JP_TEMPLATE_LOGO=<picture file> turns on "Include logo" with that picture.
            if (qEnvironmentVariableIsSet("JP_TEMPLATE_LOGO")) {
                QFile lf(qEnvironmentVariable("JP_TEMPLATE_LOGO"));
                if (lf.open(QIODevice::ReadOnly)) {
                    topt.options["logo"] = true;
                    topt.logoBytes = lf.readAll();
                    topt.logoFormat = QFileInfo(lf.fileName()).suffix().toLower();
                }
            }
            std::unique_ptr<Document> doc = t.build(topt);
            LayoutCache cache;
            PaintContext ctx;
            ctx.doc = doc.get();
            ctx.cache = &cache;
            ctx.opt.output = true;
            for (int i = 0; i < doc->pages.size(); ++i)
                Renderer::renderToImage(ctx, i, dpi / 72.0).save(QDir(args[3]).filePath(QStringLiteral("%1-%2.png").arg(t.id).arg(i + 1)));
            out << t.id << "\tpages=" << doc->pages.size() << "\n";
        }
        return 0;
    }
    QString err;
    PubImportReport rep;
    QElapsedTimer t;
    t.start();
    std::unique_ptr<Document> doc = in.endsWith(".jpub", Qt::CaseInsensitive) ? loadPublication(in, &err) : importPublisherFile(in, &err, &rep);
    if (!doc) {
        out << "FAIL\t" << in << "\t" << err << "\n";
        return 1;
    }
    out << "OK\t" << in << "\tpages=" << doc->pages.size() << " size=" << doc->pageSize().width() << "x" << doc->pageSize().height()
        << " text=" << rep.textBoxes << " pictures=" << rep.pictures << " shapes=" << rep.shapes << " tables=" << rep.tables
        << " chains=" << rep.linkedChains << " attached=" << rep.attachedEnds << " inline=" << rep.inlineObjects << " masters=" << doc->masters.size() << " ms=" << t.elapsed() << "\n";
    if (!rep.fontsUsed.isEmpty()) out << "  fonts: " << rep.fontsUsed.join(", ") << "\n";
    for (const auto &w : rep.warnings) out << "  warning: " << w << "\n";
    if (cmd == "layout") {
        // Text boxes with the position of every laid-out line, in points.
        LayoutCache cache;
        RenderOptions opt;
        for (int pi = 0; pi < doc->pages.size(); ++pi)
            walkItems(doc->pages[pi]->items, [&](const ItemPtr &it) {
                auto *t = dynamic_cast<TextItem *>(it.get());
                if (!t) return;
                const auto fl = cache.textFrame(*doc, *t, pi + 1, opt);
                out << "p" << pi + 1 << " box " << t->rect.x() << "," << t->rect.y() << " " << t->rect.width() << "x" << t->rect.height()
                    << " autofit=" << t->autofit << " fit=" << fl.fitScale << " overflow=" << (fl.layout && fl.layout->overflow()) << "\n";
                for (const QPolygonF &ob : Renderer::wrapObstacles(*doc, *t)) {
                    const QRectF b = ob.boundingRect();
                    out << "    obstacle " << b.x() << "," << b.y() << " " << b.width() << "x" << b.height() << "\n";
                }
                if (!fl.layout) return;
                QTextDocument *sd = doc->storyDoc(t->storyId);
                for (const auto &li : fl.layout->lineInfo(fl.frame)) {
                    const QTextBlockFormat bf = sd ? sd->findBlock(li.docStart).blockFormat() : QTextBlockFormat();
                    out << "    line y=" << li.rect.y() << " h=" << li.rect.height() << " x=" << li.rect.x() << " w=" << li.rect.width() << " base=" << li.baseline
                        << "\t" << li.family << "\t" << li.pointSize << "\tlh=" << bf.lineHeightType() << ":" << bf.lineHeight()
                        << "\tpara=" << li.docStart << "\t" << li.text << "\n";
                }
            });
        return 0;
    }
    if (cmd == "spans") {
        // Every story's runs with their links, fields and objects in text,
        // in a fixed order (to compare two builds' reading of a file).
        QStringList stories;
        for (const auto &st : doc->stories) {
            QString dump;
            for (QTextBlock b = st->doc->begin(); b.isValid(); b = b.next()) {
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat cf = it.fragment().charFormat();
                    dump += QStringLiteral("[%1|%2|%3|%4]").arg(it.fragment().text(), cf.anchorHref(), cf.stringProperty(tp::Field), cf.stringProperty(tp::InlineObject).isEmpty() ? QString() : QStringLiteral("obj"));
                }
                dump += QLatin1Char('\n');
            }
            stories << dump;
        }
        std::sort(stories.begin(), stories.end());
        for (const QString &d : stories) out << d << "----\n";
        return 0;
    }
    if (cmd == "topub") {
        QString err;
        if (!exportPublisher(*doc, args[3], &err)) { out << "save failed: " << err << "\n"; return 1; }
        out << "OK\t" << args[3] << (err.isEmpty() ? QString() : QStringLiteral("\t") + err) << "\n";
        return 0;
    }
    if (cmd == "convert") {
        if (!savePublication(*doc, args[3], QImage(), &err)) { out << "save failed: " << err << "\n"; return 1; }
        return 0;
    }
    const double dpi = args.size() > 4 ? args[4].toDouble() : 60;
    // An optional page count draws only the first pages of a long book.
    const int pages = args.size() > 5 ? std::min(int(doc->pages.size()), std::max(1, args[5].toInt())) : int(doc->pages.size());
    QDir().mkpath(args[3]);
    LayoutCache cache;
    PaintContext ctx;
    ctx.doc = doc.get();
    ctx.cache = &cache;
    ctx.opt.output = true;
    const QString base = QFileInfo(in).completeBaseName();
    for (int i = 0; i < pages; ++i) {
        const QImage img = Renderer::renderToImage(ctx, i, dpi / 72.0);
        img.save(QDir(args[3]).filePath(QStringLiteral("%1-%2.png").arg(base).arg(i + 1)));
    }
    return 0;
}
