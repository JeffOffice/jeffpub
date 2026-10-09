#include "app/mainwindow.h"
#include "app/recovery.h"
#include "app/keytips.h"
#include <QComboBox>
#include <QLineEdit>
#include <QLocale>
#include <QDateTime>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>

#include "io/importers.h"
#include "io/pdfspots.h"
#include "io/pdfx.h"
#include "app/appfuncs.h"
#include "app/backstage.h"
#include "app/dialogs.h"
#include "app/icons.h"
#include "app/pagespane.h"
#include "app/ribbon.h"
#include "app/settings.h"
#include "app/taskpane.h"
#include "app/telemetry.h"
#include "app/widgets.h"
#include "render/svgexport.h"
#include "io/epub.h"
#include "canvas/canvas.h"
#include "io/jpubfile.h"
#include "io/pubimport.h"
#include "render/metafile.h"
#include "render/pdfpage.h"
#include "text/storyio.h"
#include "text/textprops.h"

#include <QAction>
#include <QApplication>
#include <QPushButton>
#include <QSet>
#include <QTextBlock>
#include <QFontDatabase>
#include <QFileOpenEvent>
#include <QUrl>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QSaveFile>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QVBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QShortcut>
#include <QDialog>
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
#include <QColorSpace>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPdfOutputIntent>
#include <QProgressDialog>

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
    // The color drop-downs offer this publication's spot colors.
    setSpotColorSource([ed = QPointer<Editor>(m_ed)] {
        QVector<QPair<QString, QColor>> out;
        if (!ed || !ed->doc() || !ed->doc()->print.usesSpots()) return out;
        const PrintInfo &pi = ed->doc()->print;
        for (int i = 0; i < pi.spotColors.size(); ++i) out << qMakePair(pi.spotName(i), pi.spotColors[i]);
        return out;
    });
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
    m_keyTips = new KeyTips(this, m_ribbon, [this] { showBackstage(); });
    connect(m_ribbon, &Ribbon::fileClicked, this, [this] { showBackstage(); });
    connect(m_ribbon, &Ribbon::leaveRequested, this, [this] { m_canvas->setFocus(Qt::OtherFocusReason); });
    // F6 and Shift+F6 move the keyboard between the window's parts: the
    // ribbon, the page thumbnails, and the page.
    auto cycle = [this](int dir) {
        QWidget *f = QApplication::focusWidget();
        int at = m_ribbon->isAncestorOf(f) ? 0 : (f == m_pages || m_pages->isAncestorOf(f)) ? 1 : 2;
        int next = (at + dir + 3) % 3;
        if (next == 1 && !m_pages->isVisible()) next = (next + dir + 3) % 3;
        if (next == 0) m_ribbon->focusCurrentTab();
        else if (next == 1) m_pages->setFocus(Qt::TabFocusReason);
        else m_canvas->setFocus(Qt::TabFocusReason);
    };
    auto *f6 = new QShortcut(QKeySequence(Qt::Key_F6), this);
    connect(f6, &QShortcut::activated, this, [cycle] { cycle(1); });
    auto *sf6 = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6), this);
    connect(sf6, &QShortcut::activated, this, [cycle] { cycle(-1); });
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
    connect(m_canvas, &Canvas::editBarcodeWanted, this, [this] { barcodeDialog(this, m_ed); });
    connect(m_canvas, &Canvas::tabsDialogWanted, this, [this] { paragraphDialog(this, m_ed, 2); });
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

    static quint64 s_windows = 0;
    m_serial = ++s_windows;
    m_recoverTimer.setInterval(std::max(1, Settings::get().autoRecoverMinutes()) * 60 * 1000);
    connect(&m_recoverTimer, &QTimer::timeout, this, &MainWindow::autoRecover);
    m_recoverTimer.start();
    // A copy is only for work that isn't saved: it goes once the document
    // is saved, replaced, or closed.
    connect(m_ed, &Editor::documentReplaced, this, [this] {
        dropRecoveryCopy();
        m_recoveredFrom.clear();
        m_recovered = false;
    });
    connect(m_ed, &Editor::modifiedChanged, this, [this](bool modified) {
        if (!modified) dropRecoveryCopy();
    });

    // The size, place and state (maximized...) the last window had when it
    // closed; a window opened while another shows sits a little lower right.
    resize(1400, 900);
    const QByteArray geometry = Settings::get().value(QStringLiteral("ui/windowGeometry")).toByteArray();
    if (!geometry.isEmpty() && restoreGeometry(geometry))
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w != this && w->inherits("jp::MainWindow") && w->isVisible()) {
                move(pos() + QPoint(32, 32));
                break;
            }
    updateTitle();
    m_pages->refresh();
    refreshUi();
    updateContextTabs();
    m_canvas->setFocus();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    // Children (the editor, panes, the ribbon) are destroyed after this body,
    // when only the QWidget part of this window is left: a signal they send
    // while going (the undo stack emits cleanChanged as it is destroyed)
    // would call this window's slots on a half-destroyed object. Cut those
    // connections, and the application-wide event filter, while whole.
    qApp->removeEventFilter(this);
    for (QObject *child : findChildren<QObject *>()) child->disconnect(this);
}

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
    a->setObjectName(id);   // the command's id (KeyTips, automation, tests)
    if (!iconName.isEmpty()) a->setIcon(icon(iconName));
    if (!key.isEmpty()) {
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        a->setToolTip(tr("%1 (%2)").arg(QString(text).remove('&'), key.toString(QKeySequence::NativeText)));
    }
    a->setCheckable(checkable);
    // Each command counts by its id for the usage statistics (when they're on).
    connect(a, &QAction::triggered, this, [fn, id] {
        telemetry::count(id);
        fn();
    });
    addAction(a);
    m_actions.insert(id, a);
    return a;
}

void MainWindow::updateTitle()
{
    // Recovered work says so until it's saved; the publication's own title
    // (saved in it, and in its PDFs) stays as its author set it.
    const QString name = m_recovered && m_ed->filePath().isEmpty() ? tr("%1 (Recovered)").arg(m_ed->displayName()) : m_ed->displayName();
    setWindowTitle(tr("%1%2 - JeffPub").arg(name, m_ed->isModified() ? QStringLiteral("*") : QString()));
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
        QMessageBox::warning(this, tr("Open"), tr("JeffPub can't open \"%1\".\n%2").arg(QFileInfo(path).fileName(), f.errorString()));
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
        QMessageBox::warning(this, tr("Open"), tr("JeffPub can't open \"%1\".\n%2").arg(QFileInfo(path).fileName(), err));
        return false;
    }
    if (doc->props.title.isEmpty()) doc->props.title = QFileInfo(path).completeBaseName();
    m_ed->setDocument(std::move(doc), path);
    Settings::get().addRecentFile(path);
    telemetry::count(fromPub ? QStringLiteral("file.open.pub") : QStringLiteral("file.open.jpub"));
    hideBackstage();
    if (fromPub && !rep.warnings.isEmpty())
        statusBar()->showMessage(tr("Opened .pub file. %1").arg(rep.warnings.first()), 8000);
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
    // Options > Save > "Always create backup copy": the file as it was
    // before this save is kept as "Backup of <name>".
    if (Settings::get().value("save/backup", false).toBool() && QFile::exists(path)) {
        const QFileInfo fi(path);
        const QString backup = fi.absoluteDir().filePath(QStringLiteral("Backup of ") + fi.fileName());
        QFile::remove(backup);
        QFile::copy(path, backup);
    }
    if (path.endsWith(QLatin1String(".pub"), Qt::CaseInsensitive)) {
        if (!exportPublisher(*m_ed->doc(), path, &err, pageThumbnail(0, 160))) {
            QMessageBox::warning(this, tr("Save"), err);
            return false;
        }
        // Saved, but some objects have no .pub form yet: say which.
        if (!err.isEmpty())
            QMessageBox::information(this, tr("Save as .pub File"),
                                     tr("\"%1\" was saved, but %2.\n\nThey're kept when you save as a JeffPub publication (.jpub).")
                                         .arg(QFileInfo(path).fileName(), err));
    } else {
        if (!path.endsWith(QLatin1String(".jpub"), Qt::CaseInsensitive)) path += QStringLiteral(".jpub");
        m_ed->doc()->props.modified = QDateTime::currentDateTime();
        if (m_ed->doc()->props.author.isEmpty()) m_ed->doc()->props.author = Settings::get().userName();
        if (!savePublication(*m_ed->doc(), path, thumb, &err)) {
            QMessageBox::warning(this, tr("Save"), tr("JeffPub couldn't save \"%1\".\n%2%3").arg(QFileInfo(path).fileName(), err, saveFailureHint(err)));
            return false;
        }
    }
    m_ed->setFilePath(path);
    m_ed->markSaved();
    Settings::get().addRecentFile(path);
    telemetry::count(path.endsWith(QLatin1String(".pub"), Qt::CaseInsensitive) ? QStringLiteral("file.save.pub") : QStringLiteral("file.save.jpub"));
    updateTitle();
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 4000);
    return true;
}

bool MainWindow::saveAs(const QString &format)
{
    const QString from = m_ed->filePath().isEmpty() ? m_recoveredFrom : m_ed->filePath();
    QString dir = from.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                 : QFileInfo(from).absolutePath() + "/" + QFileInfo(from).completeBaseName();
    // The dialog hands back the filter text it showed, so the choice is
    // recognized by comparing with these same (translated) texts.
    const QString jpubFilter = tr("JeffPub Publication (*.jpub)"), pubFilter = tr(".pub Publication Files (*.pub)");
    const QString pdfFilter = tr("PDF (*.pdf)"), templateFilter = tr("JeffPub Template (*.jpub)");
    QString filters = jpubFilter + QStringLiteral(";;") + pubFilter + QStringLiteral(";;") + pdfFilter + QStringLiteral(";;") + templateFilter;
    QString selected = format == QLatin1String("pub") ? pubFilter : jpubFilter;
    QString path = askSavePath(this, tr("Save As"), dir, filters, &selected);
    if (path.isEmpty()) return false;
    if (selected == pdfFilter) {
        if (!path.endsWith(".pdf", Qt::CaseInsensitive)) path += ".pdf";
        exportPdf(path);
        return true;
    }
    if (selected == pubFilter && !path.endsWith(".pub", Qt::CaseInsensitive)) path += ".pub";
    if (selected == templateFilter) {
        const QString tdir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Templates";
        QDir().mkpath(tdir);
        QString err;
        if (!savePublication(*m_ed->doc(), tdir + "/" + QFileInfo(path).completeBaseName() + ".jpub", pageThumbnail(0, 256), &err))
            QMessageBox::warning(this, tr("Save as Template"), err);
        else statusBar()->showMessage(tr("Saved to My Templates."), 4000);
        return true;
    }
    return saveTo(path);
}

bool MainWindow::maybeSave()
{
    m_ed->flushTyping();
    if (!m_ed->isModified()) return true;
    const auto r = QMessageBox::question(this, QStringLiteral("JeffPub"), tr("Do you want to save changes to %1?").arg(m_ed->displayName()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    Settings::get().setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
    dropRecoveryCopy();
    e->accept();
}

void MainWindow::autoRecover()
{
    m_ed->flushTyping();
    if (!m_ed->isModified()) return;
    // Named for the document as it is now (Save As moves it).
    const QString copy = recovery::copyPath(m_ed->filePath(), m_ed->displayName(), m_serial);
    if (copy != m_recoveryCopy) dropRecoveryCopy();
    QString err;
    if (recovery::write(*m_ed->doc(), copy, m_ed->filePath().isEmpty() ? m_recoveredFrom : m_ed->filePath(), m_ed->displayName(), &err))
        m_recoveryCopy = copy;
}

void MainWindow::dropRecoveryCopy()
{
    recovery::remove(m_recoveryCopy);
    m_recoveryCopy.clear();
}

void MainWindow::offerRecovery(bool askIfNone)
{
    const QVector<recovery::Recovered> found = recovery::orphans();
    if (found.isEmpty()) {
        if (askIfNone) QMessageBox::information(this, tr("Recover Unsaved Work"), tr("There's no unsaved work to recover."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Recover Unsaved Work"));
    auto *v = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("JeffPub didn't close normally, and saved copies of your unsaved work. "
                                "Choose which to open; you can save each one where you like."),
                             &dlg);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto *list = new QListWidget(&dlg);
    list->setAccessibleName(tr("Recovered publications"));
    const QLocale loc;
    for (const auto &r : found) {
        const QString where = r.source.isEmpty() ? tr("never saved") : QDir::toNativeSeparators(r.source);
        auto *it = new QListWidgetItem(tr("%1\n%2, saved %3").arg(r.title, where, loc.toString(r.saved, QLocale::ShortFormat)), list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Checked);
        it->setIcon(icon(QStringLiteral("life-buoy")));
    }
    list->setMinimumSize(480, 180);
    v->addWidget(list);
    auto *buttons = new QDialogButtonBox(&dlg);
    QPushButton *open = buttons->addButton(tr("Open"), QDialogButtonBox::AcceptRole);
    QPushButton *del = buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(tr("Not Now"), QDialogButtonBox::RejectRole);
    v->addWidget(buttons);
    open->setDefault(true);
    int choice = 0;
    connect(open, &QPushButton::clicked, &dlg, [&] { choice = 1; dlg.accept(); });
    connect(del, &QPushButton::clicked, &dlg, [&] {
        if (QMessageBox::question(&dlg, tr("Delete"), tr("Delete the checked copies? Their unsaved work can't be brought back.")) != QMessageBox::Yes)
            return;
        choice = 2;
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    for (int i = 0; i < found.size(); ++i) {
        if (list->item(i)->checkState() != Qt::Checked) continue;
        const recovery::Recovered &r = found[i];
        if (choice == 2) {
            recovery::discard(r);
            continue;
        }
        QString err;
        if (!openRecovered(r, &err))
            QMessageBox::warning(this, tr("Recover Unsaved Work"), tr("JeffPub can't open the copy of \"%1\".\n%2").arg(r.title, err));
    }
}

MainWindow *MainWindow::openRecovered(const recovery::Recovered &r, QString *error)
{
    auto doc = loadPublication(r.file, error);
    if (!doc) return nullptr;
    MainWindow *target = this;
    if (!m_ed->filePath().isEmpty() || m_ed->isModified()) {
        target = new MainWindow();
        target->setAttribute(Qt::WA_DeleteOnClose);
        target->show();
    }
    target->m_ed->setDocument(std::move(doc));
    target->m_ed->setUntitledName(r.title);
    target->m_recoveredFrom = r.source;
    target->m_recovered = true;
    target->m_ed->markUnsaved();
    target->hideBackstage();
    target->updateTitle();
    // This run keeps its own copy before the old one goes.
    target->autoRecover();
    if (!target->m_recoveryCopy.isEmpty()) recovery::discard(r);
    return target;
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
        path = askSavePath(this, archival ? tr("Export PDF/A") : tr("Export PDF"),
                                            (m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                                                        : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName()) + ".pdf",
                                            tr("PDF (*.pdf)"));
        if (path.isEmpty()) return;
    }
    PdfSettings s;
    s.merged = merged;
    s.archival = archival;
    exportPdfTo(path, s);
}

// What a printer would object to, checked before a PDF/X is made (the
// Design Checker's final publishing checks): pictures under 150 ppi at their
// printed size, and fonts this computer doesn't have (another stands in).
// Transparency needs no warning: PDF/X flattens it.
QStringList pressProblems(const Document &d)
{
    QStringList out;
    QSet<QString> fonts;
    for (int p = 0; p < d.pages.size(); ++p)
        walkItems(d.pages[p]->items, [&](const ItemPtr &it) {
            if (it->type() == ItemType::Picture) {
                auto *pic = static_cast<PictureItem *>(it.get());
                const QSize px = pic->imageId.isEmpty() ? QSize() : d.imageSize(pic->imageId);
                const QString fmt = d.images.value(pic->imageId).format;
                if (px.isValid() && fmt != QLatin1String("svg") && fmt != QLatin1String("wmf") && fmt != QLatin1String("emf") && pic->imgRect.width() > 0 &&
                    pic->imgRect.height() > 0) {
                    const double ppi = std::min(px.width() / (pic->imgRect.width() / 72.0), px.height() / (pic->imgRect.height() / 72.0));
                    if (ppi < 150) out << QCoreApplication::translate("MainWindow", "A picture on page %1 has low resolution (%2 ppi; printers ask for 300).").arg(p + 1).arg(int(ppi));
                }
            }
            if (it->type() == ItemType::Text)
                if (QTextDocument *sd = d.storyDoc(static_cast<TextItem *>(it.get())->storyId))
                    for (QTextBlock b = sd->begin(); b.isValid(); b = b.next())
                        for (auto f = b.begin(); !f.atEnd(); ++f) {
                            const QStringList fams = f.fragment().charFormat().fontFamilies().toStringList();
                            if (!fams.isEmpty() && !QFontDatabase::hasFamily(fams.first()) && !fonts.contains(fams.first())) {
                                fonts.insert(fams.first());
                                out << QCoreApplication::translate("MainWindow", "The font \"%1\" isn't on this computer; another font stands in.").arg(fams.first());
                            }
                        }
        });
    return out;
}

void MainWindow::exportPdfWithOptions()
{
    m_ed->endTextEdit();
    const Document *d = m_ed->doc();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Create PDF"));
    auto *v = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout();
    auto *preset = new QComboBox(&dlg);
    preset->addItems({tr("Minimum size (online viewing, pictures at 96 dpi)"), tr("Standard (online distribution, pictures at 150 dpi)"),
                      tr("High quality printing (desktop printers, pictures at 300 dpi)"), tr("Commercial press (full-resolution pictures, marks and bleeds)")});
    preset->setCurrentIndex(Settings::get().value(QStringLiteral("pdf/preset"), 2).toInt());
    form->addRow(tr("Optimize for:"), preset);
    auto *range = new QComboBox(&dlg);
    range->addItems({tr("All pages"), tr("Current page"), tr("Pages from:")});
    auto *fromBox = new QSpinBox(&dlg), *toBox = new QSpinBox(&dlg);
    fromBox->setRange(1, d->pages.size());
    toBox->setRange(1, d->pages.size());
    toBox->setValue(d->pages.size());
    auto *rh = new QHBoxLayout();
    rh->addWidget(range, 1);
    rh->addWidget(fromBox);
    rh->addWidget(new QLabel(tr("to"), &dlg));
    rh->addWidget(toBox);
    auto syncRange = [=] { fromBox->setEnabled(range->currentIndex() == 2); toBox->setEnabled(range->currentIndex() == 2); };
    QObject::connect(range, &QComboBox::currentIndexChanged, &dlg, syncRange);
    syncRange();
    form->addRow(tr("Pages:"), rh);
    auto *props = new QCheckBox(tr("Include document properties (title, author, subject, keywords)"), &dlg);
    props->setChecked(true);
    auto *pdfa = new QCheckBox(tr("PDF/A for long-term archiving"), &dlg);
    auto *pdfx = new QCheckBox(tr("PDF/X for a commercial printer"), &dlg);
    auto *condition = new QComboBox(&dlg);
    // PDF/X-1a flattens transparency; PDF/X-4 keeps it and carries the
    // condition's color profile (ECI's, or the printer's own file).
    QString ownProfile = Settings::get().value(QStringLiteral("pdf/pdfxProfile")).toString();
    auto conditionText = [&ownProfile](const PdfXCondition &c) {
        QString t = (c.x4 ? tr("PDF/X-4: %1") : tr("PDF/X-1a: %1")).arg(c.name);
        if (c.ownProfile && QFileInfo::exists(ownProfile)) t = tr("PDF/X-4: Your printer's color profile (%1)").arg(QFileInfo(ownProfile).fileName());
        return t;
    };
    for (const PdfXCondition &c : pdfXConditions()) condition->addItem(conditionText(c));
    condition->setCurrentIndex(std::clamp(Settings::get().value(QStringLiteral("pdf/pdfxCondition"), 0).toInt(), 0, condition->count() - 1));
    auto lastCondition = std::make_shared<int>(condition->currentIndex());
    QObject::connect(condition, &QComboBox::activated, &dlg, [&, condition, lastCondition](int i) {
        const PdfXCondition c = pdfXConditions().value(i);
        if (!c.ownProfile) {
            *lastCondition = i;
            return;
        }
        // The printer's own profile: chosen now, and checked to be one for
        // printing in CMYK.
        const QString file = QFileDialog::getOpenFileName(&dlg, tr("Your Printer's Color Profile"),
                                                          QFileInfo(ownProfile).absolutePath(), tr("Color profiles (*.icc *.icm)") + QStringLiteral(";;") + tr("All Files (*)"));
        QFile f(file);
        const QColorSpace cs = !file.isEmpty() && f.open(QIODevice::ReadOnly) ? QColorSpace::fromIccProfile(f.read(64 << 20)) : QColorSpace();
        if (!cs.isValid() || cs.colorModel() != QColorSpace::ColorModel::Cmyk) {
            if (!file.isEmpty())
                QMessageBox::warning(&dlg, tr("Create PDF"), tr("\"%1\" isn't a color profile for printing in CMYK.").arg(QFileInfo(file).fileName()));
            condition->setCurrentIndex(*lastCondition);
            return;
        }
        ownProfile = file;
        Settings::get().setValue(QStringLiteral("pdf/pdfxProfile"), file);
        condition->setItemText(i, conditionText(c));
        *lastCondition = i;
    });
    pdfx->setChecked(Settings::get().value(QStringLiteral("pdf/pdfx"), false).toBool());
    // One or the other: PDF/A keeps screen (RGB) color, PDF/X prints in ink.
    auto syncStandards = [=] {
        condition->setEnabled(pdfx->isChecked());
        pdfa->setEnabled(!pdfx->isChecked());
        if (pdfx->isChecked()) pdfa->setChecked(false);
    };
    QObject::connect(pdfx, &QCheckBox::toggled, &dlg, syncStandards);
    syncStandards();
    auto *openAfter = new QCheckBox(tr("Open the PDF after saving it"), &dlg);
    openAfter->setChecked(openPdfAfterSaving());
    auto *booklet = new QCheckBox(tr("Booklet sheets (two pages to a side, in folding order)"), &dlg);
    booklet->setChecked(true);
    booklet->setVisible(d->setup.layout == PageSetup::Booklet);
    form->addRow(booklet);
    form->addRow(props);
    form->addRow(pdfa);
    form->addRow(pdfx);
    form->addRow(tr("Printing condition:"), condition);
    form->addRow(openAfter);
    v->addLayout(form);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    bb->addButton(tr("Save PDF…"), QDialogButtonBox::AcceptRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    Settings::get().setValue(QStringLiteral("pdf/preset"), preset->currentIndex());
    setOpenPdfAfterSaving(openAfter->isChecked());
    PdfSettings s;
    s.preset = PdfSettings::Preset(preset->currentIndex());
    s.properties = props->isChecked();
    s.archival = pdfa->isChecked();
    s.pdfx = pdfx->isChecked();
    s.pdfxCondition = condition->currentIndex();
    s.pdfxProfile = ownProfile;
    s.booklet = booklet->isChecked();
    Settings::get().setValue(QStringLiteral("pdf/pdfx"), s.pdfx);
    Settings::get().setValue(QStringLiteral("pdf/pdfxCondition"), s.pdfxCondition);
    if (range->currentIndex() == 1) s.from = s.to = m_ed->currentPage();
    else if (range->currentIndex() == 2) { s.from = fromBox->value() - 1; s.to = std::max(s.from, toBox->value() - 1); }
    // Before a file for a printer: what the printer would object to.
    if (s.pdfx) {
        const QStringList problems = pressProblems(*d);
        if (!problems.isEmpty()) {
            QMessageBox box(QMessageBox::Warning, tr("Create PDF"),
                            tr("Before you send this to a printer:"), QMessageBox::NoButton, this);
            box.setInformativeText(QStringLiteral("• ") + problems.mid(0, 8).join(QStringLiteral("\n• ")) +
                                   (problems.size() > 8 ? QStringLiteral("\n• ") + tr("…and %1 more").arg(problems.size() - 8) : QString()));
            QPushButton *anyway = box.addButton(tr("Create PDF Anyway"), QMessageBox::AcceptRole);
            QPushButton *check = box.addButton(tr("Open Design Checker"), QMessageBox::ActionRole);
            box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == check) showTaskPane(QStringLiteral("designchecker"));
            if (box.clickedButton() != anyway) return;
        }
    }
    const QString path = askSavePath(this, tr("Create PDF"),
                                     (m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                                                 : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName()) + ".pdf",
                                     tr("PDF (*.pdf)"));
    if (!path.isEmpty()) exportPdfTo(path, s);
}

// A PDF/X-4 printing condition's color profile: the printer's own, or
// ECI's, downloaded once (after asking) and kept with JeffPub's other
// downloads. Empty, with the reason, when there's none to be had; with no
// window to ask from (the command line) nothing is downloaded.
static QByteArray pdfX4Profile(QWidget *parent, const PdfXCondition &c, const QString &ownProfile, QString *error)
{
    if (c.ownProfile) {
        QFile f(ownProfile);
        if (ownProfile.isEmpty() || !f.open(QIODevice::ReadOnly)) {
            *error = QCoreApplication::translate("MainWindow", "choose your printer's color profile (an .icc file) first");
            return {};
        }
        return f.read(64 << 20);
    }
    const QString saved = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/profiles/") + c.profileFile;
    QFile f(saved);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray icc = f.read(64 << 20);
        if (QCryptographicHash::hash(icc, QCryptographicHash::Sha256).toHex() == c.profileSha256) return icc;
    }
    if (!parent || QGuiApplication::platformName() == QLatin1String("offscreen")) {
        *error = QCoreApplication::translate("MainWindow", "its color profile, %1, hasn't been downloaded yet").arg(c.info);
        return {};
    }
    const QUrl url(c.profileUrl);
    if (QMessageBox::question(parent, QCoreApplication::translate("MainWindow", "Create PDF"),
                              QCoreApplication::translate("MainWindow", "A PDF/X-4 file carries the color profile of its printing condition, \"%1\". The European Color Initiative, "
                                                                        "which makes it, lets anyone use it and put it in PDFs but not pass it on, so it doesn't come with JeffPub.\n\n"
                                                                        "Download it from %2 now? (%3, once.)")
                                  .arg(c.info, url.host(), c.profileSize),
                              QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes) {
        *error = QCoreApplication::translate("MainWindow", "its color profile wasn't downloaded");
        return {};
    }
    QNetworkAccessManager net;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JeffPub/%1").arg(QStringLiteral(JP_VERSION)));
    QNetworkReply *r = net.get(req);
    QProgressDialog progress(QCoreApplication::translate("MainWindow", "Downloading %1…").arg(c.info), QCoreApplication::translate("MainWindow", "Cancel"), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    QObject::connect(r, &QNetworkReply::downloadProgress, &progress, [&progress](qint64 got, qint64 total) {
        if (total <= 0) return;
        progress.setMaximum(int(total / 1024));
        progress.setValue(int(got / 1024));
    });
    QObject::connect(&progress, &QProgressDialog::canceled, r, &QNetworkReply::abort);
    QEventLoop loop;
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!r->isFinished()) loop.exec();
    progress.close();
    const QByteArray zip = r->error() == QNetworkReply::NoError ? r->read(32 << 20) : QByteArray();
    if (zip.isEmpty()) {
        *error = QCoreApplication::translate("MainWindow", "the download didn't finish (%1)").arg(r->errorString());
        return {};
    }
    const QByteArray icc = pdfXProfileFromZip(c, zip, error);
    if (icc.isEmpty()) return {};
    QDir().mkpath(QFileInfo(saved).path());
    QSaveFile out(saved);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(icc);
        out.commit();
    }
    return icc;
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
    x += "<xmp:CreatorTool>JeffPub</xmp:CreatorTool>\n</rdf:Description>\n</rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";
    return x;
}

bool MainWindow::exportPdfTo(const QString &path, const PdfSettings &sIn)
{
    m_ed->endTextEdit();
    Document *d = m_ed->doc();
    PdfSettings s = sIn;
    // A catalog makes a PDF of its merged pages: a record in each cell.
    std::unique_ptr<Document> catalogPages;
    if (s.merged && d->catalog.isActive() && !d->merge.isEmpty()) {
        catalogPages = mergeToNewPublication(*d);
        d = catalogPages.get();
        s.merged = false;
        s.from = 0;
        s.to = -1;
    }
    const bool press = s.preset == PdfSettings::CommercialPress;
    const double margin = press ? kMarksMargin : 0;
    // PDF/X-4 needs its printing condition's color profile before anything.
    const PdfXCondition &xcond = pdfXConditions().value(s.pdfxCondition, pdfXConditions().first());
    const bool x4 = s.pdfx && xcond.x4;
    QColorSpace x4Profile;
    if (x4) {
        QString why;
        const QByteArray icc = pdfX4Profile(isVisible() ? this : nullptr, xcond, s.pdfxProfile, &why);
        if (!icc.isEmpty()) {
            x4Profile = QColorSpace::fromIccProfile(icc);
            if (!x4Profile.isValid() || x4Profile.colorModel() != QColorSpace::ColorModel::Cmyk) why = tr("the color profile isn't one for printing in CMYK");
        }
        if (!why.isEmpty()) {
            if (isVisible()) QMessageBox::warning(this, tr("Create PDF"), tr("JeffPub couldn't make the PDF/X-4: %1.").arg(why));
            else qWarning("Create PDF: no PDF/X-4: %s", qPrintable(why));
            return false;
        }
    }
    QPdfWriter pdf(path);
    pdf.setCreator(QStringLiteral("JeffPub"));
    const QString title = d->props.title.isEmpty() ? m_ed->displayName() : d->props.title;
    pdf.setTitle(title);
    if (s.pdfx) s.archival = false;
    if (x4) {
        // PDF/X-4: Qt writes the output intent with the profile in it, and
        // the identification and document ID in the XMP metadata (its own,
        // so not the properties' version below).
        pdf.setPdfVersion(QPagedPaintDevice::PdfVersion_X4);
        QPdfOutputIntent intent;
        intent.setOutputConditionIdentifier(xcond.identifier);
        intent.setOutputCondition(xcond.ownProfile ? x4Profile.description() : xcond.condition);
        if (!xcond.ownProfile) intent.setRegistryName(QUrl(QStringLiteral("http://www.color.org")));
        intent.setOutputProfile(x4Profile);
        pdf.setOutputIntent(intent);
    }
    // PDF/A-1b: Qt embeds every font, writes the XMP identification and an sRGB
    // output intent, and leaves out transparency.
    else if (s.archival) pdf.setPdfVersion(QPagedPaintDevice::PdfVersion_A1b);
    else if (s.properties) pdf.setDocumentXmpMetadata(pdfXmp(d->props, title));
    // A publication set up for process-color printing (Commercial Print
    // Information) makes a CMYK PDF: colors given as ink amounts keep them.
    std::optional<InkOutput> inks;   // process colors give their inks
    if (s.pdfx || (!s.archival && (d->print.model == PrintInfo::ProcessCMYK || d->print.usesSpots()))) {
        pdf.setColorModel(QPdfWriter::ColorModel::CMYK);
        inks.emplace();
    }
    const QSizeF ps = d->pageSize();
    // A whole booklet (not for a press, which imposes its own) goes out as
    // its printed sheets: side by side, or one above the other for wide pages.
    const bool impose = s.booklet && d->setup.layout == PageSetup::Booklet && s.from <= 0 && s.to < 0 && !press && !s.pdfx && d->pages.size() > 1;
    const bool topFold = ps.width() > ps.height();
    pdf.setPageSize(QPageSize(impose ? (topFold ? QSizeF(ps.width(), 2 * ps.height()) : QSizeF(2 * ps.width(), ps.height())) : ps + QSizeF(2 * margin, 2 * margin),
                              QPageSize::Point, QString(), QPageSize::ExactMatch));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setResolution(1200);
    QPainter p;
    if (!p.begin(&pdf)) {
        QMessageBox::warning(this, tr("Create PDF"), tr("JeffPub couldn't write \"%1\".%2").arg(path, saveFailureHint(QStringLiteral("access denied"))));
        return false;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const double scale = 1200.0 / 72.0;
    PaintContext ctx;
    ctx.doc = d;
    ctx.cache = &m_ed->cache();
    ctx.opt.output = true;
    ctx.opt.flattenTransparency = s.archival || (s.pdfx && !x4);   // PDF/A-1 and PDF/X-1a allow no transparency
    ctx.opt.maxImageDpi = s.preset == PdfSettings::Minimum ? 96 : s.preset == PdfSettings::Standard ? 150 : s.preset == PdfSettings::HighQuality ? 300 : 0;
    PrinterMarks marks;
    marks.crop = marks.bleed = marks.registration = marks.colorBars = marks.jobInfo = press;
    const QString stamp = QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat);
    const int from = std::clamp(s.from, 0, int(d->pages.size()) - 1);
    const int to = s.to < 0 ? d->pages.size() - 1 : std::clamp(s.to, from, int(d->pages.size()) - 1);
    QVector<int> records{catalogPages ? -1 : m_ed->mergeRecord()};
    if (s.merged && !d->merge.isEmpty()) records = d->merge.includedRows();
    bool first = true;
    for (int rec : records) {
        ctx.opt.mergeRecord = rec;
        if (impose) {
            for (const QVector<int> &side : bookletOrder(int(d->pages.size()))) {
                if (!first) pdf.newPage();
                first = false;
                for (int k = 0; k < 2; ++k) {
                    if (side[k] < 0) continue;
                    p.save();
                    p.scale(scale, scale);
                    p.translate(topFold ? 0 : k * ps.width(), topFold ? k * ps.height() : 0);
                    p.setClipRect(QRectF(QPointF(0, 0), ps));
                    Renderer::paintPage(&p, ctx, side[k]);
                    p.restore();
                }
            }
            continue;
        }
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
    // Spot colors become Separation colors named for their inks.
    if (!s.archival && d->print.usesSpots() && !d->print.spotColors.isEmpty()) {
        QStringList names;
        for (int k2 = 0; k2 < d->print.spotColors.size(); ++k2) names << d->print.spotName(k2);
        QString spotErr;
        if (!addPdfSpotColors(path, d->print.spotColors, names, &spotErr))
            QMessageBox::warning(this, tr("Create PDF"), tr("The PDF was made, but its spot colors are process colors: %1").arg(spotErr));
    }
    // PDF/X: pictures in CMYK, the page's trim and bleed, and (PDF/X-1a)
    // the printing condition.
    if (s.pdfx) {
        PdfXOptions xo;
        xo.condition = s.pdfxCondition;
        xo.overprint = d->print.overprint;
        const QSizeF media = ps + QSizeF(2 * margin, 2 * margin);
        // PDF boxes count from the bottom left; the page sits margin in.
        xo.trim = QRectF(margin, media.height() - margin - ps.height(), ps.width(), ps.height());
        const double bleed = press ? std::min(marks.bleedSize, margin) : 0;
        xo.bleed = xo.trim.adjusted(-bleed, -bleed, bleed, bleed);
        QString xErr;
        if (!(x4 ? makePdfX4(path, xo, &xErr) : makePdfX1a(path, xo, &xErr))) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Create PDF"), tr("The PDF was made, but it isn't PDF/X: %1.").arg(xErr));
            return false;
        }
    }
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage(tr("Exported %1%2").arg(QFileInfo(path).fileName(), s.pdfx ? (x4 ? QStringLiteral(" (PDF/X-4)") : QStringLiteral(" (PDF/X-1a)")) : s.archival ? QStringLiteral(" (PDF/A)") : QString()), 5000);
    // Not for exports from the command line, which show no window.
    if (openPdfAfterSaving() && isVisible() && openFileHook) openFileHook(path);
    return true;
}

bool MainWindow::openPdfAfterSaving() { return Settings::get().value(QStringLiteral("pdf/openAfter"), true).toBool(); }
void MainWindow::setOpenPdfAfterSaving(bool on) { Settings::get().setValue(QStringLiteral("pdf/openAfter"), on); }
std::function<bool(const QString &)> MainWindow::openFileHook = [](const QString &path) {
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
};

void MainWindow::exportImages()
{
    const QString base = m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/" + m_ed->displayName()
                                                    : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName();
    QString selected;
    const QString path = askSavePath(this, tr("Save as Picture"), base + ".png",
                                                      tr("PNG (*.png)") + QStringLiteral(";;") + tr("JPEG (*.jpg)") + QStringLiteral(";;") + tr("GIF (*.gif)") + QStringLiteral(";;") +
                                                      tr("TIFF (*.tif)") + QStringLiteral(";;") + tr("Bitmap (*.bmp)") + QStringLiteral(";;") + tr("SVG vector drawing (*.svg)"), &selected);
    if (path.isEmpty()) return;
    // A vector drawing has no resolution to ask for.
    if (QFileInfo(path).suffix().compare(QLatin1String("svg"), Qt::CaseInsensitive) == 0) {
        QString error;
        if (exportSvgTo(path, &error)) statusBar()->showMessage(tr("Saved %1 drawing(s).").arg(m_ed->doc()->pages.size()), 5000);
        else QMessageBox::warning(this, tr("Save as Picture"), tr("The drawing couldn't be saved: %1").arg(error));
        return;
    }
    bool ok = false;
    const int dpi = QInputDialog::getItem(this, tr("Save as Picture"), tr("Resolution (dots per inch):"),
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
    statusBar()->showMessage(tr("Saved %1 picture(s).").arg(n), 5000);
}

bool MainWindow::exportSvgTo(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    PaintContext ctx;
    ctx.doc = m_ed->doc();
    ctx.cache = &m_ed->cache();
    ctx.opt.output = true;
    ctx.opt.mergeRecord = m_ed->mergeRecord();
    const int n = int(m_ed->doc()->pages.size());
    for (int i = 0; i < n; ++i) {
        const QString out = n == 1 ? path : fi.absolutePath() + "/" + fi.completeBaseName() + QStringLiteral("-%1.").arg(i + 1) + fi.suffix();
        const QString title = n == 1 ? m_ed->displayName() : QStringLiteral("%1, page %2").arg(m_ed->displayName()).arg(i + 1);
        if (!writePageSvg(ctx, i, out, title, error)) return false;
    }
    return true;
}

void MainWindow::exportEpub()
{
    Document *d = m_ed->doc();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save as E-book"));
    auto *v = new QVBoxLayout(&dlg);
    auto *about = new QLabel(tr("An EPUB e-book whose text flows to fit each reader's screen. Chapters start at each "
                                "Heading 1 paragraph, and the Heading 1-3 paragraphs make its table of contents."), &dlg);
    about->setWordWrap(true);
    v->addWidget(about);
    auto *form = new QFormLayout();
    auto *title = new QLineEdit(d->props.title.isEmpty() ? QFileInfo(m_ed->displayName()).completeBaseName() : d->props.title, &dlg);
    auto *author = new QLineEdit(d->props.author, &dlg);
    form->addRow(tr("Title:"), title);
    form->addRow(tr("Author:"), author);
    v->addLayout(form);
    auto *cover = new QCheckBox(tr("Use the first page as the cover"), &dlg);
    cover->setChecked(true);
    v->addWidget(cover);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    dlg.resize(460, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) return;
    const QString base = m_ed->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName()
                                                    : QFileInfo(m_ed->filePath()).absolutePath() + "/" + QFileInfo(m_ed->filePath()).completeBaseName();
    const QString path = askSavePath(this, tr("Save as E-book"), QFileInfo(base).absolutePath() + "/" + QFileInfo(base).completeBaseName() + ".epub",
                                     tr("EPUB e-book (*.epub)"));
    if (path.isEmpty()) return;
    QString error;
    if (exportEpubTo(path, title->text().trimmed(), author->text().trimmed(), cover->isChecked(), &error))
        statusBar()->showMessage(tr("Saved the e-book."), 5000);
    else
        QMessageBox::warning(this, tr("Save as E-book"), tr("The e-book couldn't be saved: %1").arg(error));
}

bool MainWindow::exportEpubTo(const QString &path, const QString &title, const QString &author, bool cover, QString *error)
{
    EpubOptions opt;
    opt.title = title;
    opt.author = author;
    if (cover && !m_ed->doc()->pages.isEmpty()) {
        // About what e-book stores ask for: 1,600 pixels tall.
        PaintContext ctx;
        ctx.doc = m_ed->doc();
        ctx.cache = &m_ed->cache();
        ctx.opt.output = true;
        opt.cover = Renderer::renderToImage(ctx, 0, 1600 / std::max(1.0, m_ed->doc()->pageSize().height()));
    }
    return jp::exportEpub(*m_ed->doc(), path, opt, error);
}

void MainWindow::exportHtml()
{
    const QString path = askSavePath(this, tr("Save as Web Page"),
                                                      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + m_ed->displayName() + ".html",
                                                      tr("Web Page (*.html)"));
    if (path.isEmpty()) return;
    exportHtmlTo(path);
}

bool MainWindow::exportHtmlTo(const QString &path)
{
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
        // The page's words (and pictures' alt text) for the description, and
        // its links as clickable areas: objects with a link, and linked text.
        QStringList text;
        QString areas;
        const double px = 2.0;   // image pixels per point
        auto area = [&](const QRectF &r, const QString &href) {
            if (href.isEmpty() || r.isEmpty()) return;
            areas += QStringLiteral("<area shape=\"rect\" coords=\"%1,%2,%3,%4\" href=\"%5\" alt=\"%5\">")
                         .arg(int(r.left() * px)).arg(int(r.top() * px)).arg(int(std::ceil(r.right() * px))).arg(int(std::ceil(r.bottom() * px)))
                         .arg(href.toHtmlEscaped());
        };
        walkItems(m_ed->doc()->pages[i]->items, [&](const ItemPtr &it) {
            area(it->bounds(), it->hyperlink);
            if (it->type() == ItemType::Picture && !it->altText.isEmpty()) text << it->altText;
            if (it->type() != ItemType::Text) return;
            auto *t = static_cast<TextItem *>(it.get());
            QTextDocument *td = m_ed->doc()->storyDoc(t->storyId);
            if (!td) return;
            if (!m_ed->doc()->prevFrame(t->id)) text << td->toPlainText();
            const auto fl = m_ed->cache().textFrame(*m_ed->doc(), *t, i + 1, ctx.opt);
            if (!fl.layout) return;
            for (QTextBlock b = td->begin(); b.isValid(); b = b.next())
                for (auto f = b.begin(); !f.atEnd(); ++f) {
                    const QTextCharFormat cf = f.fragment().charFormat();
                    if (!cf.isAnchor() || cf.anchorHref().isEmpty()) continue;
                    for (const QRectF &r : fl.layout->rangeRects(fl.frame, f.fragment().position(), f.fragment().position() + f.fragment().length()))
                        area(t->transform().mapRect(r), cf.anchorHref());
                }
        });
        const QString map = areas.isEmpty() ? QString() : QStringLiteral("p%1").arg(i + 1);
        html += QStringLiteral("<img alt=\"%1\" src=\"data:image/png;base64,%2\"%3>").arg(text.join(' ').left(2000).toHtmlEscaped(), QString::fromLatin1(png.toBase64()),
                                                                                         map.isEmpty() ? QString() : QStringLiteral(" usemap=\"#%1\"").arg(map));
        if (!map.isEmpty()) html += QStringLiteral("<map name=\"%1\">%2</map>").arg(map, areas);
    }
    html += "</body></html>";
    QSaveFile f(path);
    const QByteArray out = html.toUtf8();
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size() || !f.commit()) return false;
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 5000);
    return true;
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
    return QCoreApplication::translate("MainWindow", "All Pictures (*.png *.jpg *.jpeg *.gif *.bmp *.tif *.tiff *.webp *.svg *.wmf *.emf *.ico *.pdf)") + QStringLiteral(";;") +
           QCoreApplication::translate("MainWindow", "All Files (*)");
}

// Which page of a PDF to place: the only one, or the one chosen from
// pictures of them all. -1 when cancelled.
static int choosePdfPage(QWidget *parent, const PdfDocument &pdf, const QString &name)
{
    const int pages = pdf.pageCount();
    if (pages <= 1) return pages == 1 ? 0 : -1;
    QDialog dlg(parent);
    dlg.setWindowTitle(QCoreApplication::translate("MainWindow", "Insert PDF Page"));
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(QCoreApplication::translate("MainWindow", "%1 has %2 pages. Choose the page to insert:").arg(name).arg(pages)));
    auto *list = new QListWidget;
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(120, 120));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setMinimumSize(560, 380);
    // Pictures for the first 200 pages; the rest by number.
    for (int i = 0; i < pages; ++i) {
        QIcon icon;
        if (i < 200) {
            const QSizeF pt = pdf.pageSize(i);
            QSize px = pt.scaled(120, 120, Qt::KeepAspectRatio).toSize().expandedTo(QSize(1, 1));
            QImage img(px, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::white);
            QPainter p(&img);
            p.drawImage(0, 0, pdf.render(i, px));
            p.setPen(QColor(160, 160, 160));
            p.drawRect(img.rect().adjusted(0, 0, -1, -1));
            p.end();
            icon = QIcon(QPixmap::fromImage(img));
        }
        auto *item = new QListWidgetItem(icon, QCoreApplication::translate("MainWindow", "Page %1").arg(i + 1));
        item->setData(Qt::UserRole, i);
        list->addItem(item);
    }
    list->setCurrentRow(0);
    lay->addWidget(list);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted || !list->currentItem()) return -1;
    return list->currentItem()->data(Qt::UserRole).toInt();
}

static bool readPicture(QWidget *parent, const QString &path, QByteArray *bytes, QString *fmt, QSize *px)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    *bytes = f.readAll();
    *fmt = QFileInfo(path).suffix().toLower();
    if (*fmt == "jpeg") *fmt = "jpg";
    if (*fmt == "tiff") *fmt = "tif";
    if (*fmt == "pdf" || PdfDocument::looksLikePdf(*bytes)) {
        // A PDF page is kept as a PDF of that page alone; its size in
        // points, given at 96 pixels an inch like other pictures.
        const PdfDocument pdf(*bytes);
        const int page = pdf.isValid() ? choosePdfPage(parent, pdf, QFileInfo(path).fileName()) : -1;
        if (page < 0) return false;
        *bytes = pdf.extractPage(page);
        *fmt = QStringLiteral("pdf");
        *px = (pdf.pageSize(page) * 96.0 / 72.0).toSize();
        return !bytes->isEmpty() && px->isValid();
    }
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
    const QStringList paths = QFileDialog::getOpenFileNames(this, replaceItemId.isEmpty() ? tr("Insert Picture") : tr("Change Picture"),
                                                            Settings::get().value("dirs/pictures", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString(),
                                                            imageFilter());
    if (paths.isEmpty()) return;
    Settings::get().setValue("dirs/pictures", QFileInfo(paths.first()).absolutePath());
    if (!replaceItemId.isEmpty()) {
        auto *pic = dynamic_cast<PictureItem *>(m_ed->doc()->item(replaceItemId));
        QByteArray bytes;
        QString fmt;
        QSize px;
        if (!pic || !readPicture(this, paths.first(), &bytes, &fmt, &px)) return;
        m_ed->change(tr("Change Picture"), [&] {
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
    m_ed->beginChange(tr("Insert Picture"));
    QStringList made;
    // Several pictures at once go to the picture tray on the scratch area.
    int pictures = 0;
    for (const QString &path : paths) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        pictures += !(suffix == "txt" || suffix == "html" || suffix == "htm" || suffix == "rtf" || suffix == "md" || suffix == "docx");
    }
    const bool toTray = pictures > 1 && atIn.x() < 0 && m_ed->view.scratch && m_ed->masterView().isEmpty();
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
        if (!readPicture(this, path, &bytes, &fmt, &px)) continue;
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = d->addImage(bytes, fmt, path);
        // Natural size from the picture's resolution (96 dpi if unknown), limited to the page.
        QImageReader r(path);
        double dpi = 96;
        if (fmt != "svg" && fmt != "wmf" && fmt != "emf" && fmt != "pdf") {
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
        if (toTray) d->scratch.push_back(pic);
        else m_ed->surfaceItems().push_back(pic);
        made << pic->id;
        if (at.x() >= 0) at += QPointF(18, 18);
    }
    m_ed->endChange();
    if (toTray) {
        m_ed->arrangeThumbnails();
        Q_EMIT m_ed->status(tr("%1 pictures are on the scratch area beside the page. Drag them onto the page, or onto a picture to swap.").arg(pictures));
    }
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
            QTextCursor lc = cur;
            lc.setPosition(m.first + 1);
            const QStringList sugg = spellingSuggestions(word, lc.charFormat().stringProperty(tp::Language)).mid(0, 6);
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
            if (sugg.isEmpty()) menu.addAction(tr("(No spelling suggestions)"))->setEnabled(false);
            menu.addAction(tr("Ignore All"), this, [this, word] { spellingIgnore(word); m_ed->invalidateSpelling(); });
            menu.addAction(tr("Add to Dictionary"), this, [this, word] { spellingAdd(word); m_ed->invalidateSpelling(); });
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
        QMenu *fit = menu.addMenu(act("tb.textFit")->icon(), tr("Text Fit"));
        for (const char *id : {"fit.best", "fit.shrink", "fit.grow", "fit.none"}) fit->addAction(act(id));
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
        if (m_ed->canRegroup()) menu.addAction(act("arr.regroup"));
        QMenu *order = menu.addMenu(icon("layers"), tr("Order"));
        order->addAction(act("arr.front"));
        order->addAction(act("arr.forward"));
        order->addAction(act("arr.backward"));
        order->addAction(act("arr.back"));
        QMenu *wrap = menu.addMenu(icon("wrap-text"), tr("Wrap Text"));
        for (const char *id : {"wrap.none", "wrap.square", "wrap.tight", "wrap.through", "wrap.topBottom", "wrap.inline"}) wrap->addAction(act(id));
        if (kind == "picture") {
            menu.addSeparator();
            menu.addAction(act("pic.change"));
            menu.addAction(act("pic.crop"));
            menu.addAction(act("pic.saveAs"));
            menu.addAction(act("pic.caption"));
            if (act("pic.toShapes")->isEnabled()) menu.addAction(act("pic.toShapes"));
        }
        if (kind == "text") {
            menu.addSeparator();
            menu.addAction(act("tb.link"));
            menu.addAction(act("tb.break"));
            QMenu *fit = menu.addMenu(act("tb.textFit")->icon(), tr("Text Fit"));
            for (const char *id : {"fit.best", "fit.shrink", "fit.grow", "fit.none"}) fit->addAction(act(id));
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
        QMenu *zoom = menu.addMenu(icon("zoom-in"), tr("Zoom"));
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

namespace {
class FileOpener : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() != QEvent::FileOpen) return QObject::eventFilter(o, e);
        const QString path = static_cast<QFileOpenEvent *>(e)->file();
        if (path.isEmpty()) return false;
        // Into a window that is still empty, else a new one.
        MainWindow *target = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (auto *mw = qobject_cast<MainWindow *>(top))
                if (mw->isVisible() && mw->editor()->filePath().isEmpty() && !mw->editor()->isModified()) target = mw;
        if (!target) {
            target = new MainWindow();
            target->setAttribute(Qt::WA_DeleteOnClose);
            target->show();
        }
        target->openFile(path);
        target->raise();
        target->activateWindow();
        return true;
    }
};
} // namespace

void installFileOpenHandler(QObject *app) { app->installEventFilter(new FileOpener(app)); }

} // namespace jp
