// Command-line helper for testing: import a .pub (or open a .jpub) and render
// every page to PNG, printing what was found.
//   jpubtool render <file> <outdir> [dpi]
//   jpubtool convert <file.pub> <file.jpub>
//   jpubtool template <id|all> <outdir> [dpi]   render built-in templates

#include "core/fonts.h"
#include "io/cfb.h"
#include "io/importers.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "render/renderer.h"
#include "templates/templates.h"

#include <QTextBlock>
#include <QTextDocument>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
        out << "usage: jpubtool render <file> <outdir> [dpi] | convert <in> <out.jpub> | repack <in.pub> <out.pub> | topub <in> <out.pub> | layout <file> x\n";
        return 2;
    }
    const QString cmd = args[1], in = args[2];
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
            std::unique_ptr<Document> doc = t.build(TemplateOptions());
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
        << " chains=" << rep.linkedChains << " ms=" << t.elapsed() << "\n";
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
                    out << "    line y=" << li.rect.y() << " h=" << li.rect.height() << " x=" << li.rect.x() << " w=" << li.rect.width()
                        << "\t" << li.family << "\t" << li.pointSize << "\tlh=" << bf.lineHeightType() << ":" << bf.lineHeight()
                        << "\tpara=" << li.docStart << "\t" << li.text << "\n";
                }
            });
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
    QDir().mkpath(args[3]);
    LayoutCache cache;
    PaintContext ctx;
    ctx.doc = doc.get();
    ctx.cache = &cache;
    ctx.opt.output = true;
    const QString base = QFileInfo(in).completeBaseName();
    for (int i = 0; i < doc->pages.size(); ++i) {
        const QImage img = Renderer::renderToImage(ctx, i, dpi / 72.0);
        img.save(QDir(args[3]).filePath(QStringLiteral("%1-%2.png").arg(base).arg(i + 1)));
    }
    return 0;
}
