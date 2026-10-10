#pragma once
// The JeffPub window: ribbon, pages pane, workspace, task panes, status
// bar and the File backstage. Commands live in mainwindow_actions.cpp, the
// ribbon's layout in resources/ribbon.json (see ribbonbuilder.h) and the
// controls it names in mainwindow_ribbon.cpp.

#include "app/editor.h"

#include <QHash>
#include <QMainWindow>
#include <functional>
#include <QPointer>
#include <QTimer>

class QLabel;
class QSlider;
class QStackedWidget;
class QSplitter;
class QToolButton;
class QComboBox;

namespace jp {
namespace recovery {
struct Recovered;
}

class Canvas;
class Ribbon;
class KeyTips;
class RibbonTab;
class PagesPane;
class TaskPane;
class Backstage;
class HelpView;
class HelpWindow;
class FontCombo;
class SizeCombo;
class ColorButton;
class MeasureSpin;
class Gallery;
struct RibbonParts;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    Editor *editor() const { return m_ed; }
    Canvas *canvas() const { return m_canvas; }
    Ribbon *ribbon() const { return m_ribbon; }
    QAction *act(const QString &id) const;
    // What went wrong building the ribbon from ribbon.json (empty when nothing did).
    QString ribbonError() const { return m_ribbonError; }
    QStringList actionIds() const { QStringList ids = m_actions.keys(); ids.sort(); return ids; }

    // File operations (used by the backstage too).
    void newPublication(std::unique_ptr<Document> doc);
    bool openFile(const QString &path);
    // Copies of unsaved work left by a run of JeffPub that didn't close
    // normally: offered for opening or deleting (askIfNone: say so when
    // there are none, from File > Open).
    void offerRecovery(bool askIfNone = false);
    // Opens one recovered copy, in this window while it holds nothing, else
    // in a new one, which is returned (null if the copy can't be read).
    MainWindow *openRecovered(const recovery::Recovered &r, QString *error = nullptr);
    // The AutoRecover copy this window keeps of its document, if any.
    QString recoveryCopy() const { return m_recoveryCopy; }
    KeyTips *keyTips() const { return m_keyTips; }
    void autoRecover();
    // After File > Options: settings this window keeps its own copy of (the
    // AutoRecover interval).
    void settingsChanged();
    int autoRecoverInterval() const { return m_recoverTimer.interval(); }   // ms
    bool save();
    bool saveAs(const QString &format = QString());
    bool saveTo(const QString &path);
    bool maybeSave();
    // archival: PDF/A-1b (ISO 19005-1) for long-term storage.
    void exportPdf(const QString &path = QString(), bool merged = false, bool archival = false);
    // Create PDF with choices: quality preset, page range, document properties.
    struct PdfSettings {
        enum Preset { Minimum, Standard, HighQuality, CommercialPress };
        Preset preset = HighQuality;
        int from = 0, to = -1;          // page indexes; to < 0 means the last page
        bool properties = true;         // title, author, subject and keywords
        bool archival = false;          // PDF/A-1b
        bool pdfx = false;              // PDF/X (1a:2001 or 4) for a commercial printer
        int pdfxCondition = 0;          // its version and printing condition (pdfXConditions())
        QString pdfxProfile;            // the printer's own profile (.icc), for a condition that takes one
        bool merged = false;
        // A booklet's PDF holds its printed sheets: two pages to a side, in
        // folding order (as Publisher makes it). Off: one page per PDF page.
        bool booklet = true;
    };
    void exportPdfWithOptions();
    bool exportPdfTo(const QString &path, const PdfSettings &s);
    // Whether a PDF opens in the system's viewer once it is saved (on by
    // default, as the other program's "open file after publishing"), and how
    // it is opened (replaceable for tests; does nothing without a screen).
    static bool openPdfAfterSaving();
    static void setOpenPdfAfterSaving(bool on);
    static std::function<bool(const QString &path)> openFileHook;
    void exportImages();
    // Each page as an SVG drawing: `path` for one page, name-1.svg, name-2.svg... for several.
    bool exportSvgTo(const QString &path, QString *error = nullptr);
    void exportHtml();
    // An EPUB e-book; `cover` makes the first page its cover.
    void exportEpub();
    bool exportEpubTo(const QString &path, const QString &title, const QString &author, bool cover, QString *error = nullptr);
    // Page for page: each page as designed (a fixed-layout EPUB).
    bool exportFixedEpubTo(const QString &path, const QString &title, const QString &author, QString *error = nullptr);
    bool exportHtmlTo(const QString &path);
    void printPublication();
    void showBackstage(const QString &page = QString());
    void hideBackstage();
    void showTaskPane(const QString &name);
    void hideTaskPane();
    // Help on a topic ("index", the contents, when empty): in the Help pane,
    // or in a window of its own over the File page, which covers the pane.
    void showHelp(const QString &topic = QString());
    // The topic for what is being done now: the ribbon tab, task pane or
    // File page the keyboard is in, else the selected object (empty: none).
    QString helpContext() const;
    HelpView *helpView() const;
    // Help over a dialog, which blocks the window and so its Help pane.
    HelpWindow *showHelpOver(QWidget *owner, const QString &topic);
    QString currentTaskPane() const;
    QImage pageThumbnail(int page, int maxSide);
    void insertPictureFromFile(const QString &replaceItemId = QString(), const QPointF &at = QPointF(-1, -1));
    void insertFiles(const QStringList &paths, const QPointF &at);
    void editTextArt(const QString &id);
    void refreshUi();
    void screenshotTo(const QString &path);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void createActions();
    void buildRibbon();
    // The controls and menus ribbon.json names (mainwindow_ribbon.cpp).
    void ribbonControlParts(RibbonParts &parts);
    void ribbonGalleryParts(RibbonParts &parts);
    void ribbonMenuParts(RibbonParts &parts);
    void buildStatusBar();
    void updateTitle();
    void updateContextTabs();
    void contextMenu(const QPoint &global);
    void dropRecoveryCopy();
    QAction *mk(const QString &id, const QString &text, const QString &iconName, const QKeySequence &key, const std::function<void()> &fn,
                bool checkable = false);

    Editor *m_ed;
    Canvas *m_canvas;
    Ribbon *m_ribbon;
    KeyTips *m_keyTips = nullptr;
    PagesPane *m_pages;
    TaskPane *m_task;
    Backstage *m_backstage;
    QSplitter *m_split;
    QHash<QString, QAction *> m_actions;
    QString m_ribbonError;

    // Ribbon controls that mirror the selection.
    QVector<FontCombo *> m_fontCombos;
    QVector<SizeCombo *> m_sizeCombos;
    QVector<ColorButton *> m_colorButtons;
    QVector<MeasureSpin *> m_widthSpins, m_heightSpins;
    Gallery *m_styleGallery = nullptr;
    Gallery *m_schemeGallery = nullptr;

    // Status bar.
    QLabel *m_pageLabel, *m_posLabel, *m_sizeLabel, *m_zoomLabel;
    QSlider *m_zoomSlider;
    QTimer m_refreshTimer, m_recoverTimer;
    quint64 m_serial;               // this window, among this run's (names an unsaved document's copy)
    QString m_recoveryCopy;         // this window's AutoRecover copy, if written
    QString m_recoveredFrom;        // a recovered document's original file, for Save As
    bool m_recovered = false;       // the document is recovered work, not yet saved
    QPointer<QWidget> m_measurement;
};

// Opens documents the system hands the running program: macOS sends a
// FileOpen event for a file double-clicked in the Finder or dropped on the
// Dock icon, not a command-line argument. Installed on the application.
void installFileOpenHandler(QObject *app);

} // namespace jp
