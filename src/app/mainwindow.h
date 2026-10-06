#pragma once
// The JeffPub 79 window: ribbon, pages pane, workspace, task panes, status
// bar and the File backstage. Commands live in mainwindow_actions.cpp and the
// ribbon layout in mainwindow_ribbon.cpp.

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

class Canvas;
class Ribbon;
class RibbonTab;
class PagesPane;
class TaskPane;
class Backstage;
class FontCombo;
class SizeCombo;
class ColorButton;
class MeasureSpin;
class Gallery;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    Editor *editor() const { return m_ed; }
    Canvas *canvas() const { return m_canvas; }
    QAction *act(const QString &id) const;
    QStringList actionIds() const { QStringList ids = m_actions.keys(); ids.sort(); return ids; }

    // File operations (used by the backstage too).
    void newPublication(std::unique_ptr<Document> doc);
    bool openFile(const QString &path);
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
        bool pdfx = false;              // PDF/X-1a:2001 for a commercial printer
        int pdfxCondition = 0;          // its printing condition (pdfXConditions())
        bool merged = false;
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
    void exportHtml();
    bool exportHtmlTo(const QString &path);
    void printPublication();
    void showBackstage(const QString &page = QString());
    void hideBackstage();
    void showTaskPane(const QString &name);
    void hideTaskPane();
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
    void buildStatusBar();
    void updateTitle();
    void updateContextTabs();
    void contextMenu(const QPoint &global);
    void autoRecover();
    QAction *mk(const QString &id, const QString &text, const QString &iconName, const QKeySequence &key, const std::function<void()> &fn,
                bool checkable = false);

    Editor *m_ed;
    Canvas *m_canvas;
    Ribbon *m_ribbon;
    PagesPane *m_pages;
    TaskPane *m_task;
    Backstage *m_backstage;
    QSplitter *m_split;
    QHash<QString, QAction *> m_actions;

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
    QPointer<QWidget> m_measurement;
};

// Opens documents the system hands the running program: macOS sends a
// FileOpen event for a file double-clicked in the Finder or dropped on the
// Dock icon, not a command-line argument. Installed on the application.
void installFileOpenHandler(QObject *app);

} // namespace jp
