#pragma once
// KeyTips: press and release Alt (or hold Alt and type) and letters appear
// on the ribbon. Typing a tab's letters opens it and shows letters on its
// controls; typing a control's letters uses it (a button is pressed, a menu
// or gallery opens, a box takes the focus). Escape goes back a step; a
// click or another window puts the letters away. The letters come from the
// ribbon's layout file (resources/ribbon.json).

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>

class QLabel;
class QWidget;

namespace jp {

class Ribbon;

class KeyTips : public QObject {
public:
    KeyTips(QWidget *window, Ribbon *ribbon, std::function<void()> openFile);
    bool eventFilter(QObject *o, QEvent *e) override;

    enum Level { Off, Top, InTab, InGroup };
    Level level() const { return m_level; }
    // The letters shown now, and the control each belongs to (tests).
    QVector<QPair<QString, QWidget *>> shown() const;

    void showTop();   // the Quick Access Toolbar, File and the tabs
    void hide();

private:
    struct Tip {
        QString key;
        QPointer<QWidget> target;
        std::function<void()> act;
        QPointer<QLabel> badge;
    };
    void showTab();
    void showGroup(QWidget *popup);
    void clear();
    void add(const QString &key, QWidget *target, std::function<void()> act);
    void typed(const QString &c);
    void back();
    void use(QWidget *w);
    bool mine() const;   // the event is for this window

    QWidget *m_window;
    Ribbon *m_ribbon;
    std::function<void()> m_openFile;
    Level m_level = Off;
    QVector<Tip> m_tips;
    QString m_typed;
    bool m_altDown = false, m_altUsed = false;
};

} // namespace jp
