#pragma once
// The ribbon: Quick Access Toolbar, File button, tabs (including colored
// contextual tabs), and groups of large/small buttons with dialog launchers.

#include <QFrame>
#include <QMap>
#include <QToolButton>
#include <functional>

class QAction;
class QMenu;
class QHBoxLayout;
class QVBoxLayout;
class QStackedWidget;
class QScrollArea;
class QLabel;

namespace jp {

class RibbonGroup : public QFrame {
    Q_OBJECT
public:
    explicit RibbonGroup(const QString &title, QWidget *parent = nullptr);
    QToolButton *addLarge(QAction *a, QMenu *menu = nullptr, bool split = false);
    void beginColumn();
    QToolButton *addSmall(QAction *a, QMenu *menu = nullptr, bool split = false, bool iconOnly = false);
    void addRow(const QList<QWidget *> &widgets);   // a row of widgets in the current column
    void addWidget(QWidget *w);                       // its own column
    void addSeparator();
    void setLauncher(const std::function<void()> &fn, const QString &tip = QString());
    QString title() const { return m_title; }

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QHBoxLayout *m_cols;
    QVBoxLayout *m_col = nullptr;
    int m_rows = 0;
    QString m_title;
    QToolButton *m_launcher = nullptr;
};

// A ribbon page. When the window is too narrow for every group, groups
// collapse from the right into one button each that opens the group in a
// dropdown, so every command stays reachable.
class RibbonTab : public QWidget {
    Q_OBJECT
public:
    explicit RibbonTab(QWidget *parent = nullptr);
    RibbonGroup *addGroup(const QString &title);
    void finish();
    QSize minimumSizeHint() const override;
    int collapsedCount() const;

protected:
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void relayout();
    void popUp(int i);
    QHBoxLayout *m_layout;
    QVector<RibbonGroup *> m_groups;
    QVector<QToolButton *> m_buttons;   // collapsed form of each group
    QVector<bool> m_collapsed;
    int m_popped = -1;                  // group currently shown in a dropdown
    bool m_inRelayout = false;
};

class RibbonHeader;

class Ribbon : public QWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget *parent = nullptr);
    RibbonTab *addTab(const QString &name, const QString &contextGroup = QString(), const QColor &color = QColor());
    void setContextVisible(const QString &group, bool visible);
    void showTab(RibbonTab *t);
    RibbonTab *current() const;
    RibbonTab *tab(const QString &name) const;
    void addQuickAccess(QAction *a);
    void setMinimized(bool m);
    bool isMinimized() const { return m_minimized; }

Q_SIGNALS:
    void fileClicked();
    void tabChanged();

private:
    friend class RibbonHeader;
    struct Tab {
        QString name, group;
        QColor color;
        RibbonTab *page = nullptr;
        QScrollArea *scroll = nullptr;
        bool visible = true;
    };
    QVector<Tab> m_tabs;
    int m_current = 0;
    RibbonHeader *m_header;
    QStackedWidget *m_stack;
    QList<QAction *> m_qat;
    bool m_minimized = false;
};

QToolButton *ribbonButton(QAction *a, bool large, QWidget *parent);
// Drawing shared by every large ribbon button (icon over a label wrapped to two lines).
QSize largeRibbonButtonHint(const QToolButton *b);
void paintLargeRibbonButton(QToolButton *b);

} // namespace jp
