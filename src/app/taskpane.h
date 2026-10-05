#pragma once
// Right-hand task panes: Design Checker, Mail Merge, Find and Replace,
// Graphics Manager, Research and Styles.

#include <QWidget>

class QStackedWidget;
class QLabel;

namespace jp {

class MainWindow;

class TaskPane : public QWidget {
    Q_OBJECT
public:
    explicit TaskPane(MainWindow *win);
    void open(const QString &name);
    QString current() const { return m_current; }
    void refresh();

Q_SIGNALS:
    void closed();

private:
    QWidget *create(const QString &name);
    MainWindow *m_win;
    QStackedWidget *m_stack;
    QLabel *m_title;
    QString m_current;
    QHash<QString, QWidget *> m_panes;
};

} // namespace jp
