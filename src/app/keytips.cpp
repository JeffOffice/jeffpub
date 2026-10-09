#include "app/keytips.h"

#include "app/icons.h"
#include "app/ribbon.h"
#include "app/widgets.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace jp {

KeyTips::KeyTips(QWidget *window, Ribbon *ribbon, std::function<void()> openFile)
    : QObject(window), m_window(window), m_ribbon(ribbon), m_openFile(std::move(openFile))
{
    qApp->installEventFilter(this);
}

bool KeyTips::mine() const
{
    if (QWidget *pop = QApplication::activePopupWidget()) return pop->parentWidget() && pop->parentWidget()->window() == m_window;
    return QApplication::activeWindow() == m_window;
}

bool KeyTips::eventFilter(QObject *o, QEvent *e)
{
    switch (e->type()) {
    case QEvent::KeyPress: {
        auto *k = static_cast<QKeyEvent *>(e);
        if (!mine() || k->isAutoRepeat()) return false;
        if (k->key() == Qt::Key_Alt) {
            m_altDown = true;
            m_altUsed = false;
            return false;
        }
        if (m_altDown) m_altUsed = true;
        const QString text = k->text().toUpper();
        const bool letter = text.size() == 1 && text.at(0).isLetterOrNumber() && !(k->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
        // Alt held while typing: straight to the letters (Alt+H opens Home).
        if (m_level == Off && m_altDown && letter) {
            showTop();
            typed(text);
            return true;
        }
        if (m_level == Off) return false;
        if (k->key() == Qt::Key_Escape) {
            back();
            return true;
        }
        if (letter) {
            typed(text);
            return true;
        }
        if (k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control) return false;
        hide();   // any other key goes on as usual
        return false;
    }
    case QEvent::KeyRelease: {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() != Qt::Key_Alt || k->isAutoRepeat() || !mine()) return false;
        // Alt pressed and released by itself shows the letters, or puts them away.
        if (m_altDown && !m_altUsed) {
            if (m_level == Off) showTop();
            else hide();
        }
        m_altDown = false;
        return false;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
        if (m_altDown) m_altUsed = true;   // Alt with the mouse (a drag without snapping) isn't KeyTips
        if (m_level != Off) hide();
        return false;
    case QEvent::WindowDeactivate:
        if (o == m_window && !QApplication::activePopupWidget()) {
            m_altDown = false;
            if (m_level != Off) hide();
        }
        return false;
    case QEvent::Resize:
        if (o == m_window && m_level != Off) hide();
        return false;
    default:
        return false;
    }
}

QVector<QPair<QString, QWidget *>> KeyTips::shown() const
{
    QVector<QPair<QString, QWidget *>> out;
    for (const Tip &t : m_tips)
        if (t.badge && t.badge->isVisible()) out << qMakePair(t.key, t.target.data());
    return out;
}

void KeyTips::clear()
{
    for (Tip &t : m_tips) delete t.badge;
    m_tips.clear();
    m_typed.clear();
}

void KeyTips::hide()
{
    clear();
    m_level = Off;
}

// A badge under its control: centered under large buttons and the top
// row, at the left of small ones; kept inside the window it's in.
void KeyTips::add(const QString &key, QWidget *target, std::function<void()> act)
{
    if (key.isEmpty() || !target || !target->isVisible()) return;
    QWidget *host = target->window();
    auto *b = new QLabel(key, host);
    b->setObjectName(QStringLiteral("jpKeyTip"));
    b->setAttribute(Qt::WA_TransparentForMouseEvents);
    QColor bg, fg, edge;
    if (uiHighContrast()) {
        bg = QApplication::palette().color(QPalette::ToolTipBase);
        fg = edge = QApplication::palette().color(QPalette::ToolTipText);
    } else {
        bg = uiDark() ? QColor(0xE8, 0xEA, 0xEF) : QColor(0x2A, 0x30, 0x3B);
        fg = uiDark() ? QColor(0x1B, 0x1F, 0x27) : QColor(Qt::white);
        edge = bg;
    }
    b->setStyleSheet(QStringLiteral("QLabel#jpKeyTip{background:%1; color:%2; border:1px solid %3; border-radius:3px; padding:0px 3px; font-weight:bold;}")
                         .arg(bg.name(), fg.name(), edge.name()));
    QFont f = b->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    b->setFont(f);
    b->adjustSize();
    const QRect r(target->mapTo(host, QPoint(0, 0)), target->size());
    const bool centered = r.height() >= 40 || target->objectName().startsWith(QLatin1String("jpRibbon")) || target->window() != m_window;
    QPoint at = centered ? QPoint(r.center().x() - b->width() / 2, r.bottom() - b->height() / 2) : QPoint(r.left() + 4, r.bottom() - b->height() / 2);
    at.setX(std::clamp(at.x(), 0, std::max(0, host->width() - b->width())));
    at.setY(std::clamp(at.y(), 0, std::max(0, host->height() - b->height())));
    b->move(at);
    b->show();
    b->raise();
    m_tips << Tip{key, target, std::move(act), b};
}

void KeyTips::showTop()
{
    clear();
    m_level = Top;
    int i = 0;
    for (QToolButton *b : m_ribbon->quickAccessButtons()) {
        ++i;
        add(keytip(b).isEmpty() ? QString::number(i) : keytip(b), b, [this, b] {
            hide();
            b->click();
        });
    }
    if (QWidget *f = m_ribbon->fileButton())
        add(m_ribbon->fileKeytip().isEmpty() ? QStringLiteral("F") : m_ribbon->fileKeytip(), f, [this] {
            hide();
            if (m_openFile) m_openFile();
        });
    for (int t = 0; t < m_ribbon->tabCount(); ++t) {
        if (!m_ribbon->tabVisible(t)) continue;
        RibbonTab *page = m_ribbon->tabPage(t);
        add(m_ribbon->tabKeytip(t), m_ribbon->tabButton(t), [this, page] {
            if (m_ribbon->isMinimized()) m_ribbon->setMinimized(false);
            m_ribbon->showTab(page);
            showTab();
        });
    }
}

void KeyTips::showTab()
{
    clear();
    m_level = InTab;
    RibbonTab *page = m_ribbon->current();
    if (!page) return hide();
    // Each control with letters; a group that has folded into one button
    // shows the group's letters there, and opens with them.
    for (QWidget *w : page->findChildren<QWidget *>()) {
        if (qobject_cast<RibbonGroup *>(w) || !w->isVisible()) continue;
        if (const QString k = keytip(w); !k.isEmpty()) add(k, w, [this, w] { use(w); });
    }
    const auto &groups = page->groups();
    for (int i = 0; i < groups.size(); ++i)
        if (page->isCollapsed(i))
            add(keytip(groups[i]), page->collapsedButton(i), [this, page, i] {
                page->popUp(i);
                if (auto *pop = page->findChild<QFrame *>(QStringLiteral("jpRibbonPop"), Qt::FindDirectChildrenOnly)) showGroup(pop);
                else hide();
            });
}

void KeyTips::showGroup(QWidget *popup)
{
    clear();
    m_level = InGroup;
    for (QWidget *w : popup->findChildren<QWidget *>())
        if (const QString k = keytip(w); !k.isEmpty() && w->isVisible() && !qobject_cast<RibbonGroup *>(w)) add(k, w, [this, w] { use(w); });
}

void KeyTips::back()
{
    if (m_level == InGroup) {
        if (QWidget *pop = QApplication::activePopupWidget()) pop->close();
        showTab();
    } else if (m_level == InTab) {
        showTop();
    } else {
        hide();
    }
}

void KeyTips::typed(const QString &c)
{
    m_typed += c;
    bool any = false;
    for (Tip &t : m_tips) {
        const bool fits = t.key.startsWith(m_typed);
        any |= fits;
        if (t.badge) t.badge->setVisible(fits);
    }
    if (!any) {
        // Not a start of any letters here: start over.
        m_typed.clear();
        for (Tip &t : m_tips)
            if (t.badge) t.badge->setVisible(true);
        return;
    }
    for (const Tip &t : m_tips)
        if (t.key == m_typed) {
            const auto act = t.act;   // the act may clear the list
            act();
            return;
        }
}

// Uses a control as its letters ask: a button is pressed (one that only
// opens a menu or gallery opens it), a box or a gallery takes the focus.
void KeyTips::use(QWidget *w)
{
    hide();
    if (!w) return;
    if (auto *b = qobject_cast<QToolButton *>(w)) {
        if (b->menu() && (b->popupMode() == QToolButton::InstantPopup || qobject_cast<ColorButton *>(b))) b->showMenu();
        else b->click();
        return;
    }
    if (auto *b = qobject_cast<QAbstractButton *>(w)) {
        b->click();
        return;
    }
    QWidget *focus = w;
    if (auto *g = qobject_cast<Gallery *>(w))
        if (auto *list = g->findChild<QAbstractItemView *>()) focus = list;
    focus->setFocus(Qt::ShortcutFocusReason);
    if (auto *c = qobject_cast<QComboBox *>(focus); c && c->lineEdit()) c->lineEdit()->selectAll();
    if (auto *s = qobject_cast<QAbstractSpinBox *>(focus)) s->selectAll();
}

} // namespace jp
