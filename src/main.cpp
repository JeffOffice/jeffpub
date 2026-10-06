// JeffPub 79 entry point.
//
//   JeffPub79 [file ...]                  open publications (.jpub, .pub, ...)
//   JeffPub79 --template <id>             start from a built-in template
//   JeffPub79 --backstage <page>          open on a File page (new, open, print, ...)
//   JeffPub79 --screenshot <png> [--size WxH]
//                                         render the window to a PNG and quit
//                                         (works with QT_QPA_PLATFORM=offscreen)

#include "app/editor.h"
#include "app/mainwindow.h"
#include "app/ribbon.h"
#include "app/updater.h"
#include "app/settings.h"
#include "app/icons.h"
#include "core/fonts.h"
#include "templates/templates.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif


namespace {

void applyTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const int theme = jp::Settings::get().value(QStringLiteral("ui/theme"), 0).toInt();
    bool dark = false;
    if (theme == 2) dark = true;
    else if (theme == 0) dark = app.styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    if (theme == 1) app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    jp::setUiDark(dark);   // not every platform honors setColorScheme, so record the choice
    if (!dark) return;
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    QPalette p;
    const QColor base(0x26, 0x26, 0x26), window(0x2f, 0x2f, 0x2f), text(0xe8, 0xe8, 0xe8);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, QColor(0xC2, 0x4A, 0x8A));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0xE0, 0x7A, 0xB0));
    p.setColor(QPalette::PlaceholderText, QColor(0x9a, 0x9a, 0x9a));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x80, 0x80, 0x80));
    app.setPalette(p);
}

} // namespace

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(resources);   // the icons live in the static jpcore library
    QApplication::setApplicationName(QStringLiteral("JeffPub 79"));
    QApplication::setOrganizationName(QStringLiteral("JeffPub"));
    QApplication::setApplicationVersion(QStringLiteral(JP_VERSION));
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app.png")));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("JeffPub 79 desktop publishing"));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the window and quit."), QStringLiteral("png"));
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size for --screenshot."), QStringLiteral("WxH"), QStringLiteral("1440x900"));
    QCommandLineOption templOpt(QStringLiteral("template"), QStringLiteral("Start from a built-in template."), QStringLiteral("id"));
    QCommandLineOption pdfOpt(QStringLiteral("export-pdf"), QStringLiteral("Export the opened publication to a PDF and quit."), QStringLiteral("pdf"));
    QCommandLineOption pdfaOpt(QStringLiteral("pdfa"), QStringLiteral("With --export-pdf: write PDF/A-1b for archiving."));
    QCommandLineOption tabOpt(QStringLiteral("tab"), QStringLiteral("Open on a ribbon tab (for screenshots)."), QStringLiteral("name"));
    QCommandLineOption stageOpt(QStringLiteral("backstage"), QStringLiteral("Open on a File page."), QStringLiteral("page"));
    cli.addOptions({shotOpt, sizeOpt, templOpt, stageOpt, pdfOpt, pdfaOpt, tabOpt});
    cli.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Publications to open."), QStringLiteral("[files...]"));
    cli.process(app);

    jp::initCore();
    applyTheme(app);
    app.setStyleSheet(jp::modernStyleSheet());
    jp::installUiPolish();

    auto *w = new jp::MainWindow();
    w->setAttribute(Qt::WA_DeleteOnClose);

    jp::installFileOpenHandler(&app);
    const QStringList files = cli.positionalArguments();
    if (!files.isEmpty()) {
        w->openFile(files.first());
        for (int i = 1; i < files.size(); ++i) {
            auto *other = new jp::MainWindow();
            other->setAttribute(Qt::WA_DeleteOnClose);
            other->openFile(files[i]);
            other->show();
        }
    } else if (cli.isSet(templOpt)) {
        if (const jp::TemplateInfo *t = jp::findTemplate(cli.value(templOpt))) {
            auto doc = t->build(jp::TemplateOptions());
            doc->templateId = t->id;
            w->editor()->setDocument(std::move(doc));
        }
    }

    if (cli.isSet(pdfOpt)) {
        w->exportPdf(cli.value(pdfOpt), false, cli.isSet(pdfaOpt));
        delete w;
        return 0;
    }

    if (cli.isSet(shotOpt)) {
        const QStringList wh = cli.value(sizeOpt).split('x');
        w->resize(wh.value(0).toInt() > 0 ? wh.value(0).toInt() : 1440, wh.value(1).toInt() > 0 ? wh.value(1).toInt() : 900);
        w->show();
        if (cli.isSet(stageOpt)) w->showBackstage(cli.value(stageOpt));
        if (cli.isSet(tabOpt))
            if (auto *r = w->findChild<jp::Ribbon *>())
                if (jp::RibbonTab *t = r->tab(cli.value(tabOpt))) r->showTab(t);
        const QString out = cli.value(shotOpt);
        // Let layouts, fonts and the first paint settle before grabbing.
        QTimer::singleShot(600, w, [w, out] {
            w->screenshotTo(out);
            QApplication::exit(0);
        });
        return app.exec();
    }

#ifdef Q_OS_WIN
    // Lets the installer see that JeffPub 79 is running and ask to close it first.
    CreateMutexW(nullptr, FALSE, L"JeffPub79Running");
#endif
    w->show();
    auto *up = new jp::Updater(w);
    jp::setUpdater(up);
    up->checkOnStartup();
    if (cli.isSet(stageOpt)) w->showBackstage(cli.value(stageOpt));
    else if (files.isEmpty() && !cli.isSet(templOpt) && jp::Settings::get().value(QStringLiteral("ui/startBackstage"), true).toBool())
        w->showBackstage(QStringLiteral("new"));
    return app.exec();
}
