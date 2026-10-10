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

// The KeyTip of a ribbon control, group, launcher or tab: the one or two
// letters or digits that reach it from the keyboard. It is a dynamic property
// of the widget so the KeyTip display can read it straight from the tree.
void setKeytip(QObject *w, const QString &k);
QString keytip(const QObject *w);

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
    QToolButton *launcher() const { return m_launcher; }
    QString title() const { return m_title; }
    // Every control the group lays out, in order (rows and columns
    // flattened; the launcher sits outside the layout and isn't one).
    QList<QWidget *> controls() const;

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QHBoxLayout *m_cols;
    QVBoxLayout *m_col = nullptr;
    int m_rows = 0;
    QString m_title;
    QToolButton *m_launcher = nullptr;
    QMetaObject::Connection m_launcherClicked;
};

// A ribbon page. When the window is too narrow for every group, groups
// collapse from the right into one button each that opens the group in a
// dropdown, so every command stays reachable.
class RibbonTab : public QWidget {
    Q_OBJECT
public:
    explicit RibbonTab(QWidget *parent = nullptr);
    RibbonGroup *addGroup(const QString &title);
    QList<RibbonGroup *> groups() const { return m_groups; }
    void finish();
    QSize minimumSizeHint() const override;
    int collapsedCount() const;
    // The single button each group shows when there's no room.
    QToolButton *collapsedButton(int i) const { return m_buttons.value(i); }
    bool isCollapsed(int i) const { return m_collapsed.value(i); }
    void popUp(int i);   // a collapsed group in a dropdown

protected:
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void relayout();
    QHBoxLayout *m_layout;
    QVector<RibbonGroup *> m_groups;
    QVector<QToolButton *> m_buttons;   // collapsed form of each group
    QVector<bool> m_collapsed;
    int m_popped = -1;                  // group currently shown in a dropdown
    bool m_inRelayout = false;
};

class RibbonHeader;
class HeaderButton;

class Ribbon : public QWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget *parent = nullptr);
    // `name` identifies the tab in code (tab(), setContextVisible()); `title`
    // is what the header shows, the name translated (the name when empty).
    RibbonTab *addTab(const QString &name, const QString &contextGroup = QString(), const QColor &color = QColor(),
                      const QString &title = QString());
    // With `first`, the group's tabs come before all the others, right after
    // File, while shown, and the first of them is the current tab when they
    // appear (the Master Page tab does this).
    void setContextVisible(const QString &group, bool visible, bool first = false);
    void setTabVisible(const QString &name, bool visible);   // any tab, by name
    void showTab(RibbonTab *t);
    RibbonTab *current() const;
    RibbonTab *tab(const QString &name) const;
    QToolButton *addQuickAccess(QAction *a);
    void setMinimized(bool m);
    bool isMinimized() const { return m_minimized; }
    // The tabs in order, for KeyTips and tests.
    int tabCount() const { return int(m_tabs.size()); }
    RibbonTab *tabAt(int i) const { return m_tabs.value(i).page; }
    QString tabName(int i) const { return m_tabs.value(i).name; }
    QString tabTitle(int i) const { return m_tabs.value(i).title; }
    QString tabKeytip(int i) const { return m_tabs.value(i).keytip; }
    bool tabVisible(int i) const { return m_tabs.value(i).visible; }
    RibbonTab *tabPage(int i) const { return m_tabs.value(i).page; }
    QWidget *tabButton(int i) const;   // the tab's control in the top row
    QWidget *fileButton() const;
    void setTabKeytip(RibbonTab *t, const QString &k);
    // The File button isn't a tab; it only records its KeyTip.
    QString fileKeytip() const { return m_fileKeytip; }
    void setFileKeytip(const QString &k) { m_fileKeytip = k; }
    QList<QToolButton *> quickAccessButtons() const;
    // Everything the ribbon holds, one control per line: tabs, groups, each
    // control's kind, command and menu, in order (tests compare it). With
    // `keytips`, each line that has a KeyTip ends with it.
    QString describe(bool keytips = false) const;

    void focusCurrentTab();   // the keyboard enters the ribbon (F6)
    void restyle();           // the colors, again for the current look

Q_SIGNALS:
    void fileClicked();
    void helpClicked();       // the "?" beside the collapse chevron
    void tabChanged();
    void leaveRequested();    // Escape: back to the page

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    friend class RibbonHeader;
    friend class HeaderButton;
    struct Tab {
        QString name, group;
        QColor color;
        RibbonTab *page = nullptr;
        QScrollArea *scroll = nullptr;
        bool visible = true;
        QString title, keytip;
        int home = 0;         // its place as built
        bool first = false;   // placed before the rest
    };
    void sortTabs();
    QVector<Tab> m_tabs;
    int m_current = 0;
    RibbonHeader *m_header;
    QStackedWidget *m_stack;
    QList<QAction *> m_qat;
    QString m_fileKeytip;
    bool m_minimized = false;
};

QToolButton *ribbonButton(QAction *a, bool large, QWidget *parent);
// Drawing shared by every large ribbon button (icon over a label wrapped to two lines).
QSize largeRibbonButtonHint(const QToolButton *b);
void paintLargeRibbonButton(QToolButton *b);

} // namespace jp
