#include "app/mainwindow.h"
#include <QComboBox>
#include <QLocale>
#include <QDateTime>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>

#include "io/importers.h"
#include "app/appfuncs.h"
#include "app/backstage.h"
#include "app/dialogs.h"
#include "app/icons.h"
#include "app/pagespane.h"
#include "app/ribbon.h"
#include "app/settings.h"
#include "app/taskpane.h"
#include "app/widgets.h"
#include "canvas/canvas.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "render/metafile.h"
#include "text/storyio.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPdfWriter>
#include <QPrintDialog>
#include <QPrinter>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QBuffer>

namespace jp {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowIcon(drawnIcon([](QPainter *p, const QRectF &r) {
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0x9E, 0x1F, 0x63));
        p->drawRoundedRect(r.adjusted(1, 1, -1, -1), r.width() * 0.18, r.width() * 0.18);
        const QColor dots[4] = {QColor(0, 163, 224), QColor(229, 0, 126), QColor(255, 212, 0), QColor(255, 255, 255)};
        for (int i = 0; i < 4; ++i) {
            p->setBrush(dots[i]);
            p->drawEllipse(QPointF(r.left() + r.width() * (0.3 + 0.4 * (i % 2)), r.top() + r.height() * (0.3 + 0.4 * (i / 2))), r.width() * 0.12, r.width() * 0.12);
        }
    }));
    m_ed = new Editor(this);
    m_canvas = new Canvas(m_ed, this);
    m_ribbon = new Ribbon(this);
    m_pages = new PagesPane(m_ed, this);
    m_task = new TaskPane(this);
    m_task->hide();

    setMenuWidget(m_ribbon);
    m_split = new QSplitter(Qt::Horizontal, this);
    m_split->addWidget(m_pages);
    m_split->addWidget(m_canvas);
    m_split->addWidget(m_task);
    m_split->setStretchFactor(1, 1);
    m_split->setCollapsible(1, false);
    m_split->setSizes({150, 900, 280});
    setCentralWidget(m_split);

    createActions();
    buildRibbon();
    buildStatusBar();

    m_backstage = new Backstage(this);
    m_backstage->hide();
    connect(m_backstage, &Backstage::closeRequested, this, &MainWindow::hideBackstage);
    connect(m_ribbon, &Ribbon::fileClicked, this, [this] { showBackstage(); });
    connect(m_task, &TaskPane::closed, this, &MainWindow::hideTaskPane);

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(30);
    connect(&m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshUi);
    auto queue = [this] { m_refreshTimer.start(); };
    connect(m_ed, &Editor::changed, this, queue);
    connect(m_ed, &Editor::selectionChanged, this, [this, queue] { updateContextTabs(); queue(); });
    connect(m_ed, &Editor::textCursorChanged, this, queue);
    connect(m_ed, &Editor::pageChanged, this, [this, queue] { m_pages->refresh(); queue(); });
    connect(m_ed, &Editor::viewChanged, this, queue);
    connect(m_ed, &Editor::documentReplaced, this, [this] { m_pages->refresh(); updateTitle(); refreshUi(); });
    connect(m_ed, &Editor::modifiedChanged, this, &MainWindow::updateTitle);
    connect(m_ed, &Editor::status, this, [this](const QString &m) { statusBar()->showMessage(m, 6000); });
    connect(m_ed, &Editor::changed, m_pages, [this] { m_pages->refresh(true); });
    connect(m_canvas, &Canvas::contextMenuWanted, this, &MainWindow::contextMenu);
    connect(m_canvas, &Canvas::insertPictureWanted, this, [this](const QString &id) { insertPictureFromFile(id); });
    connect(m_canvas, &Canvas::editTextArtWanted, this, &MainWindow::editTextArt);
    connect(m_canvas, &Canvas::openFileWanted, this, [this](const QString &f) { if (maybeSave()) openFile(f); });
    connect(m_canvas, &Canvas::insertFilesWanted, this, &MainWindow::insertFiles);
    connect(m_canvas, &Canvas::pictureTabWanted, this, [this] { if (RibbonTab *t = m_ribbon->tab("Picture Format")) m_ribbon->showTab(t); });
    connect(m_canvas, &Canvas::mouseMovedPage, this, [this](const QPointF &p) {
        m_posLabel->setText(QStringLiteral("%1, %2").arg(Settings::get().format(p.x()), Settings::get().format(p.y())));
    });
    connect(m_canvas, &Canvas::zoomChanged, this, [this](double z) {
        m_zoomLabel->setText(QStringLiteral("%1%").arg(std::lround(z * 100)));
        const QSignalBlocker b(m_zoomSlider);
        m_zoomSlider->setValue(int(std::lround(std::log(z) / std::log(1.05))));
    });

    m_recoverTimer.setInterval(std::max(1, Settings::get().autoRecoverMinutes()) * 60 * 1000);
    connect(&m_recoverTimer, &QTimer::timeout, this, &MainWindow::autoRecover);
    m_recoverTimer.start();

    resize(1400, 900);
    updateTitle();
    m_pages->refresh();
    refreshUi();
    updateContextTabs();
    m_canvas->setFocus();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow() = default;

QAction *MainWindow::act(const QString &id) const
{
    QAction *a = m_actions.value(id);
    if (!a) qWarning("JeffPub: unknown action %s", qPrintable(id));
    return a;
}

QAction *MainWindow::mk(const QString &id, const QString &text, const QString &iconName, const QKeySequence &key,
                        const std::function<void()> &fn, bool checkable)
{
    auto *a = new QAction(text, this);
    if (!iconName.isEmpty()) a->setIcon(icon(iconName));
    if (!key.isEmpty()) {
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        a->setToolTip(QStringLiteral("%1 (%2)").arg(QString(text).remove('&'), key.toString(QKeySequence::NativeText)));
    }
    a->setCheckable(checkable);
    connect(a, &QAction::triggered, this, [fn] { fn(); });
    addAction(a);
    m_actions.insert(id, a);
    return a;
}

void MainWindow::updateTitle()
{
    setWindowTitle(QStringLiteral("%1%2 - JeffPub 79").arg(m_ed->displayName(), m_ed->isModified() ? QStringLiteral("*") : QString()));
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    if (m_backstage) m_backstage->setGeometry(rect());
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    // Escape closes the backstage from anywhere.
    if (e->type() == QEvent::KeyPress && m_backstage && m_backstage->isVisible()) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape) { hideBackstage(); return true; }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::showBackstage(const QString &page)
{
    m_ed->endTextEdit();
    m_backstage->setGeometry(rect());
    m_backstage->showPage(page.isEmpty() ? QStringLiteral("info") : page);
    m_backstage->show();
    m_backstage->raise();
    m_backstage->setFocus();
}

void MainWindow::hideBackstage()
{
    m_backstage->hide();
    m_canvas->setFocus();
}

void MainWindow::showTaskPane(const QString &name)
{
    m_task->open(name);
    m_task->show();
    QList<int> sz = m_split->sizes();
    if (sz.value(2) < 200) {
        sz[2] = 300;
        m_split->setSizes(sz);
    }
}

void MainWindow::hideTaskPane() { m_task->hide(); }
QString MainWindow::currentTaskPane() const { return m_task->isVisible() ? m_task->current() : QString(); }

// ---------------- files ----------------
void MainWindow::newPublication(std::unique_ptr<Document> doc)
{
    m_ed->setDocument(std::move(doc));
    hideBackstage();
}

bool MainWindow::openFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Open"), QStringLiteral("JeffPub 79 can't open \"%1\".\n%2").arg(QFileInfo(path).fileName(), f.errorString()));
        return false;
    }
    const QByteArray bytes = f.readAll();
    QString err;
    std::unique_ptr<Document> doc;
    bool fromPub = false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    PubImportReport rep;
    if (isPublisherFile(bytes)) {
        doc = importPublisher(bytes, &err, &rep);
        fromPub = true;
    } else {
        doc = publicationFromBytes(bytes, &err);
    }
    QApplication::restoreOverrideCursor();
    if (!doc) {
        QMessageBox::warning(this, QStringLiteral("Open"), QStringLiteral("JeffPub 79 can't open \"%1\".\n%2").arg(QFileInfo(path).fileName(), err));
        return false;
    }
    if (doc->props.title.isEmpty()) doc->props.title = QFileInfo(path).completeBaseName();
    m_ed->setDocument(std::move(doc), path);
    Settings::get().addRecentFile(path);
    hideBackstage();
    if (fromPub && !rep.warnings.isEmpty())
        statusBar()->showMessage(QStringLiteral("Opened .pub file. %1").arg(rep.warnings.first()), 8000);
    return true;
}

bool MainWindow::save()
{
    const QString p = m_ed->filePath();
    if (p.isEmpty()) return saveAs();
    return saveTo(p);
}

bool MainWindow::saveTo(const QString &pathIn)
{
    QString path = pathIn;
    QString err;
    const QImage thumb = pageThumbnail(0, 256);
    m_ed->endTextEdit();
    if (path.endsWith(QLatin1String(".pub"), Qt::CaseInsensitive)) {
        if (!exportPublisher(*m_ed->doc(), path, &err)) {
            QMessageBox::warning(this, QStringLiteral("Save"), err);
            return false;
        }
        // Saved, but some objects have no .pub form yet: say which.
        if (!err.isEmpty())
            QMessageBox::information(this, QStringLiteral("Save as .pub File"),
                                     QStringLiteral("\"%1\" was saved, but %2.\n\nThey're kept when you save as a JeffPub publication (.jpub).")
                                         .arg(QFileInfo(path).fileName(), err));
    } else {
        if (!path.endsWith(QLatin1String(".jpub"), Qt::CaseInsensitive)) path += QStringLiteral(".jpub");
        m_ed->doc()->props.modified = QDateTime::currentDateTime();
        if (m_ed->doc()->props.author.isEmpty()) m_ed->doc()->props.author = Settings::get().userName();
        if (!savePublication(*m_ed->doc(), path, thumb, &err)) {
            QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("JeffPub 79 couldn't save \"%1\".\n%2%3").arg(QFileInfo(path).fileName(), err, saveFailureHint(err)));
            return false;
        }
    }
    m_ed->setFilePath(path);
    m_ed->markSaved();
    Settings::get().addRecentFile(path);
    updateTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()), 4000);
    return true;
}

bool MainWindow::saveAs(const QString &format)
{
    QString dir = m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                             : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName();
    QString filters = QStringLiteral("JeffPub Publication (*.jpub);;.pub Publication Files (*.pub);;PDF (*.pdf);;JeffPub Template (*.jpub)");
    QString selected = format == QLatin1String("pub") ? QStringLiteral(".pub Publication Files (*.pub)") : QStringLiteral("JeffPub Publication (*.jpub)");
    QString path = askSavePath(this, QStringLiteral("Save As"), dir, filters, &selected);
    if (path.isEmpty()) return false;
    if (selected.startsWith("PDF")) {
        if (!path.endsWith(".pdf", Qt::CaseInsensitive)) path += ".pdf";
        exportPdf(path);
        return true;
    }
    if (selected.startsWith(".pub") && !path.endsWith(".pub", Qt::CaseInsensitive)) path += ".pub";
    if (selected.startsWith("JeffPub Template")) {
        const QString tdir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Templates";
        QDir().mkpath(tdir);
        QString err;
        if (!savePublication(*m_ed->doc(), tdir + "/" + QFileInfo(path).completeBaseName() + ".jpub", pageThumbnail(0, 256), &err))
            QMessageBox::warning(this, QStringLiteral("Save as Template"), err);
        else statusBar()->showMessage(QStringLiteral("Saved to My Templates."), 4000);
        return true;
    }
    return saveTo(path);
}

bool MainWindow::maybeSave()
{
    m_ed->flushTyping();
    if (!m_ed->isModified()) return true;
    const auto r = QMessageBox::question(this, QStringLiteral("JeffPub 79"), QStringLiteral("Do you want to save changes to %1?").arg(m_ed->displayName()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (maybeSave()) e->accept(); else e->ignore();
}

void MainWindow::autoRecover()
{
    if (!m_ed->isModified()) return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/AutoRecover";
    QDir().mkpath(dir);
    QString err;
    savePublication(*m_ed->doc(), dir + "/" + m_ed->displayName() + ".autorecover.jpub", QImage(), &err);
}

QImage MainWindow::pageThumbnail(int page, int maxSide)
{
    const Document *d = m_ed->doc();
    if (page < 0 || page >= d->pages.size()) return {};
    PaintContext ctx;
    ctx.doc = d;
    ctx.cache = &m_ed->cache();
    ctx.opt.output = true;
    ctx.opt.mergeRecord = m_ed->mergeRecord();
    const QSizeF ps = d->pageSize();
    const double s = maxSide / std::max(ps.width(), ps.height());
    return Renderer::renderToImage(ctx, page, s);
}

void MainWindow::exportPdf(const QString &pathIn, bool merged, bool archival)
{
    QString path = pathIn;
    if (path.isEmpty()) {
        path = askSavePath(this, archival ? QStringLiteral("Export PDF/A") : QStringLiteral("Export PDF"),
                                            (m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                                                        : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName()) + ".pdf",
                                            QStringLiteral("PDF (*.pdf)"));
        if (path.isEmpty()) return;
    }
    PdfSettings s;
    s.merged = merged;
    s.archival = archival;
    exportPdfTo(path, s);
}

void MainWindow::exportPdfWithOptions()
{
    m_ed->endTextEdit();
    const Document *d = m_ed->doc();
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Create PDF"));
    auto *v = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout();
    auto *preset = new QComboBox(&dlg);
    preset->addItems({"Minimum size (online viewing, pictures at 96 dpi)", "Standard (online distribution, pictures at 150 dpi)",
                      "High quality printing (desktop printers, pictures at 300 dpi)", "Commercial press (full-resolution pictures, marks and bleeds)"});
    preset->setCurrentIndex(Settings::get().value(QStringLiteral("pdf/preset"), 2).toInt());
    form->addRow(QStringLiteral("Optimize for:"), preset);
    auto *range = new QComboBox(&dlg);
    range->addItems({"All pages", "Current page", "Pages from:"});
    auto *fromBox = new QSpinBox(&dlg), *toBox = new QSpinBox(&dlg);
    fromBox->setRange(1, d->pages.size());
    toBox->setRange(1, d->pages.size());
    toBox->setValue(d->pages.size());
    auto *rh = new QHBoxLayout();
    rh->addWidget(range, 1);
    rh->addWidget(fromBox);
    rh->addWidget(new QLabel(QStringLiteral("to"), &dlg));
    rh->addWidget(toBox);
    auto syncRange = [=] { fromBox->setEnabled(range->currentIndex() == 2); toBox->setEnabled(range->currentIndex() == 2); };
    QObject::connect(range, &QComboBox::currentIndexChanged, &dlg, syncRange);
    syncRange();
    form->addRow(QStringLiteral("Pages:"), rh);
    auto *props = new QCheckBox(QStringLiteral("Include document properties (title, author, subject, keywords)"), &dlg);
    props->setChecked(true);
    auto *pdfa = new QCheckBox(QStringLiteral("PDF/A for long-term archiving"), &dlg);
    form->addRow(props);
    form->addRow(pdfa);
    v->addLayout(form);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    bb->addButton(QStringLiteral("Save PDF…"), QDialogButtonBox::AcceptRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    Settings::get().setValue(QStringLiteral("pdf/preset"), preset->currentIndex());
    PdfSettings s;
    s.preset = PdfSettings::Preset(preset->currentIndex());
    s.properties = props->isChecked();
    s.archival = pdfa->isChecked();
    if (range->currentIndex() == 1) s.from = s.to = m_ed->currentPage();
    else if (range->currentIndex() == 2) { s.from = fromBox->value() - 1; s.to = std::max(s.from, toBox->value() - 1); }
    const QString path = askSavePath(this, QStringLiteral("Create PDF"),
                                     (m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                                                 : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName()) + ".pdf",
                                     QStringLiteral("PDF (*.pdf)"));
    if (!path.isEmpty()) exportPdfTo(path, s);
}

static QByteArray pdfXmp(const DocProps &pr, const QString &title)
{
    auto esc = [](const QString &t) { return t.toHtmlEscaped().toUtf8(); };
    QByteArray x = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                   "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                   "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\" "
                   "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n";
    x += "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" + esc(title) + "</rdf:li></rdf:Alt></dc:title>\n";
    if (!pr.author.isEmpty()) x += "<dc:creator><rdf:Seq><rdf:li>" + esc(pr.author) + "</rdf:li></rdf:Seq></dc:creator>\n";
    if (!pr.subject.isEmpty()) x += "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" + esc(pr.subject) + "</rdf:li></rdf:Alt></dc:description>\n";
    if (!pr.keywords.isEmpty()) x += "<pdf:Keywords>" + esc(pr.keywords) + "</pdf:Keywords>\n";
    x += "<xmp:CreatorTool>JeffPub 79</xmp:CreatorTool>\n</rdf:Description>\n</rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";
    return x;
}

bool MainWindow::exportPdfTo(const QString &path, const PdfSettings &s)
{
    m_ed->endTextEdit();
    Document *d = m_ed->doc();
    const bool press = s.preset == PdfSettings::CommercialPress;
    const double margin = press ? kMarksMargin : 0;
    QPdfWriter pdf(path);
    pdf.setCreator(QStringLiteral("JeffPub 79"));
    const QString title = d->props.title.isEmpty() ? m_ed->displayName() : d->props.title;
    pdf.setTitle(title);
    // PDF/A-1b: Qt embeds every font, writes the XMP identification and an sRGB
    // output intent, and leaves out transparency.
    if (s.archival) pdf.setPdfVersion(QPagedPaintDevice::PdfVersion_A1b);
    else if (s.properties) pdf.setDocumentXmpMetadata(pdfXmp(d->props, title));
    const QSizeF ps = d->pageSize();
    pdf.setPageSize(QPageSize(ps + QSizeF(2 * margin, 2 * margin), QPageSize::Point, QString(), QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setResolution(1200);
    QPainter p;
    if (!p.begin(&pdf)) {
        QMessageBox::warning(this, QStringLiteral("Create PDF"), QStringLiteral("JeffPub 79 couldn't write \"%1\".%2").arg(path, saveFailureHint(QStringLiteral("access denied"))));
        return false;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const double scale = 1200.0 / 72.0;
    PaintContext ctx;
    ctx.doc = d;
    ctx.cache = &m_ed->cache();
    ctx.opt.output = true;
    ctx.opt.flattenTransparency = s.archival;   // PDF/A-1 allows no transparency
    ctx.opt.maxImageDpi = s.preset == PdfSettings::Minimum ? 96 : s.preset == PdfSettings::Standard ? 150 : s.preset == PdfSettings::HighQuality ? 300 : 0;
    PrinterMarks marks;
    marks.crop = marks.bleed = marks.registration = marks.colorBars = marks.jobInfo = press;
    const QString stamp = QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat);
    const int from = std::clamp(s.from, 0, int(d->pages.size()) - 1);
    const int to = s.to < 0 ? d->pages.size() - 1 : std::clamp(s.to, from, int(d->pages.size()) - 1);
    QVector<int> records{m_ed->mergeRecord()};
    if (s.merged && !d->merge.isEmpty()) records = d->merge.includedRows();
    bool first = true;
    for (int rec : records) {
        ctx.opt.mergeRecord = rec;
        for (int i = from; i <= to; ++i) {
            if (!first) pdf.newPage();
            first = false;
            p.save();
            p.scale(scale, scale);
            p.translate(margin, margin);
            if (press) p.setClipRect(QRectF(-marks.bleedSize, -marks.bleedSize, ps.width() + 2 * marks.bleedSize, ps.height() + 2 * marks.bleedSize));
            Renderer::paintPage(&p, ctx, i);
            p.restore();
            if (press) {
                p.save();
                p.scale(scale, scale);
                drawPrinterMarks(&p, QRectF(QPointF(margin, margin), ps), marks,
                                 QStringLiteral("%1  ·  Page %2 of %3  ·  %4").arg(m_ed->displayName()).arg(i + 1).arg(d->pages.size()).arg(stamp));
                p.restore();
            }
        }
    }
    p.end();
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage(QStringLiteral("Exported %1%2").arg(QFileInfo(path).fileName(), s.archival ? QStringLiteral(" (PDF/A)") : QString()), 5000);
    return true;
}

void MainWindow::exportImages()
{
    const QString base = m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/" + m_ed->displayName()
                                                    : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName();
    QString selected;
    const QString path = askSavePath(this, QStringLiteral("Save as Picture"), base + ".png",
                                                      QStringLiteral("PNG (*.png);;JPEG (*.jpg);;GIF (*.gif);;TIFF (*.tif);;Bitmap (*.bmp)"), &selected);
    if (path.isEmpty()) return;
    bool ok = false;
    const int dpi = QInputDialog::getItem(this, QStringLiteral("Save as Picture"), QStringLiteral("Resolution (dots per inch):"),
                                          {"96", "150", "300", "600"}, 2, false, &ok).toInt();
    if (!ok) return;
    const QFileInfo fi(path);
    PaintContext ctx;
    ctx.doc = m_ed->doc();
    ctx.cache = &m_ed->cache();
    ctx.opt.output = true;
    ctx.opt.mergeRecord = m_ed->mergeRecord();
    const int n = m_ed->doc()->pages.size();
    for (int i = 0; i < n; ++i) {
        const bool transparent = fi.suffix().compare("png", Qt::CaseInsensitive) == 0 && false;
        QImage img = Renderer::renderToImage(ctx, i, dpi / 72.0, transparent);
        img.setDotsPerMeterX(int(dpi / 0.0254));
        img.setDotsPerMeterY(int(dpi / 0.0254));
        const QString out = n == 1 ? path : fi.absolutePath() + "/" + fi.completeBaseName() + QStringLiteral("-%1.").arg(i + 1) + fi.suffix();
        img.save(out);
    }
    statusBar()->showMessage(QStringLiteral("Saved %1 picture(s).").arg(n), 5000);
}

void MainWindow::exportHtml()
{
    const QString path = askSavePath(this, QStringLiteral("Save as Web Page"),
                                                      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName() + ".html",
                                                      QStringLiteral("Web Page (*.html)"));
    if (path.isEmpty()) return;
    // Single-file page: each publication page as an embedded image with its text in alt text.
    PaintContext ctx;
    ctx.doc = m_ed->doc();
    ctx.cache = &m_ed->cache();
    ctx.opt.output = true;
    QString html = QStringLiteral("<!doctype html><html><head><meta charset=\"utf-8\"><title>%1</title><style>body{background:#e8e8e8;margin:0;padding:24px;font-family:sans-serif}"
                                  "img{display:block;margin:0 auto 24px;max-width:100%;height:auto;box-shadow:0 2px 8px rgba(0,0,0,.25);background:#fff}</style></head><body>")
                       .arg(m_ed->displayName().toHtmlEscaped());
    for (int i = 0; i < m_ed->doc()->pages.size(); ++i) {
        const QImage img = Renderer::renderToImage(ctx, i, 2.0);
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        QStringList text;
        walkItems(m_ed->doc()->pages[i]->items, [&](const ItemPtr &it) {
            if (it->type() == ItemType::Text)
                if (QTextDocument *td = m_ed->doc()->storyDoc(static_cast<TextItem *>(it.get())->storyId)) text << td->toPlainText();
        });
        html += QStringLiteral("<img alt=\"%1\" src=\"data:image/png;base64,%2\">").arg(text.join(' ').left(2000).toHtmlEscaped(), QString::fromLatin1(png.toBase64()));
    }
    html += "</body></html>";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(html.toUtf8());
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()), 5000);
}

void MainWindow::printPublication()
{
    m_ed->endTextEdit();
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(m_ed->displayName());
    printer.setPageSize(QPageSize(m_ed->doc()->setup.sheet, QPageSize::Point));
    printer.setFullPage(true);
    printer.setFromTo(1, m_ed->doc()->pages.size());
    QPrintDialog dlg(&printer, this);
    dlg.setOption(QAbstractPrintDialog::PrintPageRange);
    dlg.setOption(QAbstractPrintDialog::PrintCurrentPage);
    if (dlg.exec() != QDialog::Accepted) return;
    printDocument(m_ed, &printer, QJsonObject());
}

// ---------------- pictures ----------------
static QString imageFilter()
{
    return QStringLiteral("All Pictures (*.png *.jpg *.jpeg *.gif *.bmp *.tif *.tiff *.webp *.svg *.wmf *.emf *.ico);;All Files (*)");
}

static bool readPicture(const QString &path, QByteArray *bytes, QString *fmt, QSize *px)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    *bytes = f.readAll();
    *fmt = QFileInfo(path).suffix().toLower();
    if (*fmt == "jpeg") *fmt = "jpg";
    if (*fmt == "tiff") *fmt = "tif";
    if (*fmt == "wmf" || *fmt == "emf" || Metafile::looksLikeMetafile(*bytes)) {
        Metafile m;
        if (!m.load(*bytes)) return false;
        const QSizeF s = m.naturalSize();
        *px = QSize(int(s.width() / 0.75), int(s.height() / 0.75));
        return true;
    }
    QImageReader r(path);
    *px = r.size();
    return px->isValid() || *fmt == "svg";
}

void MainWindow::insertPictureFromFile(const QString &replaceItemId, const QPointF &at)
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, replaceItemId.isEmpty() ? QStringLiteral("Insert Picture") : QStringLiteral("Change Picture"),
                                                            Settings::get().value("dirs/pictures", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString(),
                                                            imageFilter());
    if (paths.isEmpty()) return;
    Settings::get().setValue("dirs/pictures", QFileInfo(paths.first()).absolutePath());
    if (!replaceItemId.isEmpty()) {
        auto *pic = dynamic_cast<PictureItem *>(m_ed->doc()->item(replaceItemId));
        QByteArray bytes;
        QString fmt;
        QSize px;
        if (!pic || !readPicture(paths.first(), &bytes, &fmt, &px)) return;
        m_ed->change(QStringLiteral("Change Picture"), [&] {
            pic->imageId = m_ed->doc()->addImage(bytes, fmt, paths.first());
            pic->fitImage(m_ed->doc()->imageSize(pic->imageId), true);
        });
        return;
    }
    insertFiles(paths, at);
}

void MainWindow::insertFiles(const QStringList &paths, const QPointF &atIn)
{
    Document *d = m_ed->doc();
    const QSizeF ps = d->pageSize();
    QPointF at = atIn;
    m_ed->beginChange(QStringLiteral("Insert Picture"));
    QStringList made;
    for (const QString &path : paths) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "txt" || suffix == "html" || suffix == "htm" || suffix == "rtf" || suffix == "md" || suffix == "docx") {
            auto t = std::static_pointer_cast<TextItem>(m_ed->newTextBox(QRectF(at.x() < 0 ? ps.width() * 0.15 : at.x(), at.y() < 0 ? ps.height() * 0.2 : at.y(), ps.width() * 0.7, ps.height() * 0.5)));
            loadTextFileInto(d->storyDoc(t->storyId), path);
            m_ed->surfaceItems().push_back(t);
            made << t->id;
            continue;
        }
        QByteArray bytes;
        QString fmt;
        QSize px;
        if (!readPicture(path, &bytes, &fmt, &px)) continue;
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = d->addImage(bytes, fmt, path);
        // Natural size from the picture's resolution (96 dpi if unknown), limited to the page.
        QImageReader r(path);
        double dpi = 96;
        if (fmt != "svg" && fmt != "wmf" && fmt != "emf") {
            const QImage probe = r.read();
            if (!probe.isNull() && probe.dotsPerMeterX() > 0) dpi = probe.dotsPerMeterX() * 0.0254;
            if (dpi < 30 || dpi > 2400) dpi = 96;
        }
        QSizeF sz(px.width() * 72.0 / dpi, px.height() * 72.0 / dpi);
        if (sz.isEmpty()) sz = QSizeF(144, 144);
        const QSizeF maxSz = ps * 0.8;
        if (sz.width() > maxSz.width() || sz.height() > maxSz.height()) sz.scale(maxSz, Qt::KeepAspectRatio);
        QPointF tl = at.x() < 0 ? QPointF((ps.width() - sz.width()) / 2, (ps.height() - sz.height()) / 2) : at;
        pic->rect = QRectF(tl, sz);
        pic->imgRect = QRectF(QPointF(0, 0), sz);
        m_ed->surfaceItems().push_back(pic);
        made << pic->id;
        if (at.x() >= 0) at += QPointF(18, 18);
    }
    m_ed->endChange();
    m_ed->select(made);
}

void MainWindow::editTextArt(const QString &id) { textArtTextDialog(this, m_ed, id); }

// ---------------- context menu ----------------
void MainWindow::contextMenu(const QPoint &global)
{
    QMenu menu(this);
    const QString kind = m_ed->selectionKind();
    if (m_ed->isEditingText()) {
        // Spelling suggestions for the misspelled word under the caret.
        const QTextCursor cur = m_ed->cursor();
        const int pos = cur.position();
        for (const auto &m : m_ed->misspelledIn(m_ed->textTarget().storyId)) {
            if (pos < m.first || pos > m.second || cur.hasSelection()) continue;
            QTextCursor wc = cur;
            wc.setPosition(m.first);
            wc.setPosition(m.second, QTextCursor::KeepAnchor);
            const QString word = wc.selectedText();
            const QStringList sugg = spellingSuggestions(word).mid(0, 6);
            QFont bold = menu.font();
            bold.setBold(true);
            for (const QString &sgg : sugg) {
                QAction *a = menu.addAction(sgg, this, [this, m, sgg] {
                    m_ed->editText([&](QTextCursor &c) {
                        c.setPosition(m.first);
                        c.setPosition(m.second, QTextCursor::KeepAnchor);
                        c.insertText(sgg);
                    });
                });
                a->setFont(bold);
            }
            if (sugg.isEmpty()) menu.addAction(QStringLiteral("(No spelling suggestions)"))->setEnabled(false);
            menu.addAction(QStringLiteral("Ignore All"), this, [this, word] { spellingIgnore(word); m_ed->invalidateSpelling(); });
            menu.addAction(QStringLiteral("Add to Dictionary"), this, [this, word] { spellingAdd(word); m_ed->invalidateSpelling(); });
            menu.addSeparator();
            break;
        }
        menu.addAction(act("edit.cut"));
        menu.addAction(act("edit.copy"));
        menu.addAction(act("edit.paste"));
        menu.addAction(act("edit.pasteText"));
        menu.addSeparator();
        menu.addAction(act("fmt.fontDialog"));
        menu.addAction(act("para.dialog"));
        menu.addAction(act("para.bulletsDialog"));
        menu.addSeparator();
        menu.addAction(act("ins.link"));
        menu.addAction(act("rev.thesaurus"));
        menu.addAction(act("tb.textFit"));
        menu.addSeparator();
        menu.addAction(act("obj.format"));
    } else if (!kind.isEmpty()) {
        menu.addAction(act("edit.cut"));
        menu.addAction(act("edit.copy"));
        menu.addAction(act("edit.paste"));
        menu.addAction(act("edit.duplicate"));
        menu.addAction(act("edit.delete"));
        menu.addSeparator();
        if (kind == "multi") menu.addAction(act("arr.group"));
        if (kind == "group") menu.addAction(act("arr.ungroup"));
        QMenu *order = menu.addMenu(icon("layers"), QStringLiteral("Order"));
        order->addAction(act("arr.front"));
        order->addAction(act("arr.forward"));
        order->addAction(act("arr.backward"));
        order->addAction(act("arr.back"));
        QMenu *wrap = menu.addMenu(icon("wrap-text"), QStringLiteral("Wrap Text"));
        for (const char *id : {"wrap.none", "wrap.square", "wrap.tight", "wrap.through", "wrap.topBottom"}) wrap->addAction(act(id));
        if (kind == "picture") {
            menu.addSeparator();
            menu.addAction(act("pic.change"));
            menu.addAction(act("pic.crop"));
            menu.addAction(act("pic.saveAs"));
            menu.addAction(act("pic.caption"));
        }
        if (kind == "text") {
            menu.addSeparator();
            menu.addAction(act("tb.link"));
            menu.addAction(act("tb.break"));
            menu.addAction(act("tb.textFit"));
        }
        if (kind == "shape") menu.addAction(act("shape.addText"));
        if (kind == "textart") menu.addAction(act("wa.edit"));
        menu.addSeparator();
        menu.addAction(act("obj.lock"));
        menu.addAction(act("obj.saveBlock"));
        menu.addAction(act("obj.format"));
        menu.addAction(act("ins.link"));
    } else {
        menu.addAction(act("edit.paste"));
        menu.addSeparator();
        menu.addAction(act("page.insert"));
        menu.addAction(act("page.insertDup"));
        menu.addAction(act("page.delete"));
        menu.addSeparator();
        menu.addAction(act("pd.pageSetup"));
        menu.addAction(act("pd.guidesDialog"));
        QMenu *zoom = menu.addMenu(icon("zoom-in"), QStringLiteral("Zoom"));
        zoom->addAction(act("zoom.page"));
        zoom->addAction(act("zoom.width"));
        zoom->addAction(act("zoom.100"));
    }
    menu.exec(global);
}

void MainWindow::screenshotTo(const QString &path)
{
    refreshUi();
    grab().save(path);
}

} // namespace jp
