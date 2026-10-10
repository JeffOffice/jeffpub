#pragma once
// The File screen: a sidebar of grouped commands (New, Open, Save, Print,
// Export, Share, Properties, Close, Settings, About) beside the chosen page.

#include <QHash>
#include <QIcon>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QStackedWidget;

class QFrame;
class QLabel;

namespace jp {

class MainWindow;

class Backstage : public QWidget {
    Q_OBJECT
public:
    explicit Backstage(MainWindow *win);
    void showPage(const QString &name);
    QString currentPage() const;   // "info", "new", "change", "print"...

Q_SIGNALS:
    void closeRequested();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    QWidget *buildInfo();
    // The gallery of designs: for a new publication, or, with `change`, for
    // Change Template (the open publication's text and pictures move into it).
    QWidget *buildNew(bool change = false);
    QWidget *buildOpen();
    QWidget *buildPrint();
    QWidget *buildShare();
    QWidget *buildExport();
    QWidget *buildAbout();
    void rebuild(const QString &name);
    void restyle();   // the colors that follow the light or dark look
    MainWindow *m_win;
    QFrame *m_side = nullptr;
    QLabel *m_version = nullptr;
    QHash<QString, QAbstractButton *> m_navItems;
    QButtonGroup *m_navGroup = nullptr;
    QStackedWidget *m_stack;
    QHash<QString, int> m_index;
    // New page thumbnails made so far, by template id, all made with the
    // options in m_thumbOptions (other options start the collection afresh).
    QHash<QString, QIcon> m_thumbs;
    QString m_thumbOptions;
};

} // namespace jp
