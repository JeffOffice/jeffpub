#include "app/focusring.h"

#include "app/icons.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QWidget>

namespace jp {

namespace {

// The ring: a frame laid over the focused control in its window, which
// lets every click through.
class Ring : public QWidget {
public:
    explicit Ring(QWidget *window) : QWidget(window)
    {
        setObjectName(QStringLiteral("jpFocusRing"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(focusRingColor(), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
};

class Manager : public QObject {
public:
    explicit Manager(QApplication *app) : QObject(app)
    {
        app->installEventFilter(this);
        connect(app, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) { follow(now); });
    }

    bool eventFilter(QObject *o, QEvent *e) override
    {
        switch (e->type()) {
        // Any key counts; a shortcut's (F6 into the ribbon) never arrives as
        // a key press, only as this check before it.
        case QEvent::ShortcutOverride:
        case QEvent::KeyPress:
            if (!m_keyboard) {
                m_keyboard = true;
                follow(QApplication::focusWidget());
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            if (m_keyboard) {
                m_keyboard = false;
                hideRing();
            }
            break;
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
            if (m_target && (o == m_target || m_chain.contains(o))) place();
            break;
        case QEvent::Hide:
            if (o == m_target) hideRing();
            break;
        default:
            break;
        }
        return false;
    }

private:
    // Controls with a focus of their own to show (the page, where the
    // selection and the text cursor show it; menus) get no ring.
    static bool wants(QWidget *w)
    {
        if (!w || w->focusPolicy() == Qt::NoFocus || !w->isVisible()) return false;
        if (w->inherits("jp::Canvas") || qobject_cast<QMenu *>(w)) return false;
        return !w->window()->inherits("QMenu");
    }

    void follow(QWidget *w)
    {
        m_chain.clear();
        m_target = w;
        if (!m_keyboard || !wants(w)) {
            hideRing();
            return;
        }
        // Moves of the control or of anything holding it (a ribbon page
        // that scrolls) move the ring; the application's filter sees them.
        for (QWidget *a = w->parentWidget(); a && !a->isWindow(); a = a->parentWidget()) m_chain << a;
        place();
    }

    void place()
    {
        if (!m_target) return hideRing();
        QWidget *win = m_target->window();
        if (!m_ring || m_ring->parentWidget() != win) {
            delete m_ring;
            m_ring = new Ring(win);
        }
        const QPoint at = m_target->mapTo(win, QPoint(0, 0));
        m_ring->setGeometry(QRect(at, m_target->size()).adjusted(-3, -3, 3, 3));
        m_ring->raise();
        m_ring->show();
        m_ring->update();
    }

    void hideRing()
    {
        if (m_ring) m_ring->hide();
    }

    bool m_keyboard = false;
    QPointer<QWidget> m_target;
    QPointer<Ring> m_ring;
    QList<QPointer<QObject>> m_chain;
};

} // namespace

void installFocusRing(QApplication *app)
{
    static Manager *m = nullptr;
    if (!m) m = new Manager(app);
}

} // namespace jp
