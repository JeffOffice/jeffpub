#pragma once
// The File screen: a sidebar of grouped commands (New, Open, Save, Print,
// Export, Share, Properties, Close, Settings, About) beside the chosen page.

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
};

} // namespace jp
