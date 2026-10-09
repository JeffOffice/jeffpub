#include "app/keyboardnav.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QSlider>
#include <QTextEdit>
#include <QToolButton>
#include <limits>

namespace jp {

namespace {

bool takesFocus(const QWidget *w, const QWidget *scope)
{
    return w->isVisibleTo(scope) && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus);
}

// Controls whose arrow keys move within them.
bool usesArrows(const QWidget *w)
{
    if (qobject_cast<const QLineEdit *>(w) || qobject_cast<const QTextEdit *>(w) || qobject_cast<const QAbstractSpinBox *>(w)
        || qobject_cast<const QAbstractItemView *>(w) || qobject_cast<const QSlider *>(w))
        return true;
    if (auto *c = qobject_cast<const QComboBox *>(w)) return c->isEditable();
    return false;
}

class ArrowNav : public QObject {
public:
    explicit ArrowNav(QObject *parent) : QObject(parent) { qApp->installEventFilter(this); }
    QList<QPointer<QWidget>> scopes;

    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() != QEvent::KeyPress) return false;
        auto *w = qobject_cast<QWidget *>(o);
        if (!w || w != QApplication::focusWidget()) return false;
        QWidget *scope = nullptr;
        for (const auto &s : scopes)
            if (s && s->isAncestorOf(w)) { scope = s; break; }
        if (!scope) return false;
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->modifiers() & ~(Qt::KeypadModifier)) return false;
        const int key = k->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || (key == Qt::Key_Space && !usesArrows(w))) {
            if (auto *b = qobject_cast<QToolButton *>(w)) {
                if (b->menu() && b->popupMode() == QToolButton::InstantPopup) b->showMenu();
                else b->click();
                return true;
            }
            if (auto *b = qobject_cast<QAbstractButton *>(w)) { b->click(); return true; }
            return false;
        }
        if (key != Qt::Key_Left && key != Qt::Key_Right && key != Qt::Key_Up && key != Qt::Key_Down) return false;
        if (usesArrows(w)) return false;
        QList<QWidget *> candidates;
        for (QWidget *c : scope->findChildren<QWidget *>())
            if (c != w && takesFocus(c, scope)) candidates << c;
        if (QWidget *next = nearestInDirection(w, key, candidates)) {
            next->setFocus(Qt::TabFocusReason);
            return true;
        }
        return true;   // at the edge: stay put rather than leave the controls
    }
};

ArrowNav *nav()
{
    static ArrowNav *n = new ArrowNav(qApp);
    return n;
}

} // namespace

QWidget *nearestInDirection(QWidget *from, int key, const QList<QWidget *> &candidates)
{
    QWidget *top = from->window();
    const QRect a(from->mapTo(top, QPoint(0, 0)), from->size());
    QWidget *best = nullptr;
    double bestScore = std::numeric_limits<double>::max();
    for (QWidget *c : candidates) {
        if (c->window() != top) continue;
        const QRect b(c->mapTo(top, QPoint(0, 0)), c->size());
        const QPointF d = QPointF(b.center()) - QPointF(a.center());
        double along = 0, across = 0;
        switch (key) {
        case Qt::Key_Right: if (b.left() < a.right() - 1 && d.x() <= 1) continue; along = d.x(); across = std::abs(d.y()); break;
        case Qt::Key_Left: if (b.right() > a.left() + 1 && d.x() >= -1) continue; along = -d.x(); across = std::abs(d.y()); break;
        case Qt::Key_Down: if (b.top() < a.bottom() - 1 && d.y() <= 1) continue; along = d.y(); across = std::abs(d.x()); break;
        case Qt::Key_Up: if (b.bottom() > a.top() + 1 && d.y() >= -1) continue; along = -d.y(); across = std::abs(d.x()); break;
        default: return nullptr;
        }
        if (along <= 0) continue;
        // Controls in line count first: a sideways step costs more than a straight one.
        const double score = along + 3 * across;
        if (score < bestScore) { bestScore = score; best = c; }
    }
    return best;
}

void installArrowNavigation(QWidget *scope)
{
    nav()->scopes.removeAll(QPointer<QWidget>());   // popups closed since
    nav()->scopes << scope;
}

} // namespace jp
