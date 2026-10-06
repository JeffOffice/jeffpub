#pragma once
// The File screen: a sidebar of grouped commands (New, Open, Save, Print,
// Export, Share, Properties, Close, Settings, About) beside the chosen page.

#include <QHash>
#include <QIcon>
#include <QWidget>

class QAbstractButton;
class QStackedWidget;

namespace jp {

class MainWindow;

class Backstage : public QWidget {
    Q_OBJECT
public:
    explicit Backstage(MainWindow *win);
    void showPage(const QString &name);

Q_SIGNALS:
    void closeRequested();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    QWidget *buildInfo();
    QWidget *buildNew();
    QWidget *buildOpen();
    QWidget *buildPrint();
    QWidget *buildShare();
    QWidget *buildExport();
    QWidget *buildAbout();
    void rebuild(const QString &name);
    MainWindow *m_win;
    QHash<QString, QAbstractButton *> m_navItems;
    QStackedWidget *m_stack;
    QHash<QString, int> m_index;
    // New page thumbnails made so far, by template id, all made with the
    // options in m_thumbOptions (other options start the collection afresh).
    QHash<QString, QIcon> m_thumbs;
    QString m_thumbOptions;
};

} // namespace jp
