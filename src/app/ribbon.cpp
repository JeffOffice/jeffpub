#include "app/ribbon.h"
#include "app/keyboardnav.h"

#include "app/icons.h"

#include <QAction>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QWidgetAction>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTimer>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <climits>

namespace jp {

static bool dark() { return uiDark(); }
static QColor ribbonBg() { return dark() ? QColor(0x26, 0x2B, 0x33) : QColor(0xFB, 0xFB, 0xFC); }
static QColor headerBg() { return dark() ? QColor(0x1E, 0x22, 0x29) : QColor(0xF0, 0xF1, 0xF4); }
static QColor lineColor() { return dark() ? QColor(0x3A, 0x41, 0x4C) : QColor(0xD9, 0xDC, 0xE2); }
static QColor mutedText() { return dark() ? QColor(0xA9, 0xB0, 0xBC) : QColor(0x5E, 0x66, 0x73); }

static QStringList largeLabelLines(const QToolButton *b)
{
    QString t = b->text();
    t.remove(QLatin1Char('&'));
    t.remove(QStringLiteral("...")).remove(QChar(0x2026));
    const QFontMetrics fm(b->font());
    if (fm.horizontalAdvance(t) <= 70 || !t.contains(' ')) return {t};
    // Break at the space that best balances the two lines.
    int best = -1, bestDiff = INT_MAX;
    for (int i = 0; i < t.size(); ++i)
        if (t[i] == ' ') {
            const int d = std::abs(fm.horizontalAdvance(t.left(i)) - fm.horizontalAdvance(t.mid(i + 1)));
            if (d < bestDiff) { bestDiff = d; best = i; }
        }
    return {t.left(best), t.mid(best + 1)};
}

QSize largeRibbonButtonHint(const QToolButton *b)
{
    const QFontMetrics fm(b->font());
    int w = b->iconSize().width() + 18;
    const QStringList lines = largeLabelLines(b);
    // A two-line label with a menu carries its chevron at the end of the
    // second line; there is no room for it underneath.
    const bool menu = b->menu() || b->popupMode() == QToolButton::InstantPopup;
    for (int i = 0; i < lines.size(); ++i)
        w = std::max(w, fm.horizontalAdvance(lines[i]) + 14 + (menu && i == 1 ? 12 : 0));
    return QSize(std::min(w, 116), 64);
}

// Icon on top, label on up to two lines, a small chevron for menus, and soft
// rounded hover, press and on states. Qt's own tool buttons can only elide.
void paintLargeRibbonButton(QToolButton *b)
{
    QPainter p(b);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(b->rect()).adjusted(1, 1, -1, -1);
    const bool down = b->isDown() || (b->menu() && b->menu()->isVisible()), on = b->isChecked(), hover = b->underMouse() && b->isEnabled();
    const QColor accent = uiAccent();
    if (on) {
        QColor bg = accent;
        bg.setAlphaF(dark() ? 0.30f : 0.14f);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r, 7, 7);
    } else if (down || hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(dark() ? QColor(255, 255, 255, down ? 34 : 20) : QColor(0, 0, 0, down ? 22 : 11));
        p.drawRoundedRect(r, 7, 7);
    }
    const QSize is = b->iconSize();
    const QRect ir(int(r.center().x() - is.width() / 2.0), int(r.top() + 4), is.width(), is.height());
    b->icon().paint(&p, ir, Qt::AlignCenter, b->isEnabled() ? QIcon::Normal : QIcon::Disabled);
    const QColor tc = b->isEnabled() ? uiText() : QColor(0x9C, 0xA3, 0xAF);
    p.setPen(tc);
    p.setFont(b->font());
    const QFontMetrics fm(b->font());
    const bool menu = b->menu() || b->popupMode() == QToolButton::InstantPopup;
    const QStringList lines = largeLabelLines(b);
    auto chevron = [&](QPointF c) {
        p.setPen(QPen(tc, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF({c + QPointF(-3.5, -1.5), c + QPointF(0, 2), c + QPointF(3.5, -1.5)}));
    };
    int y = ir.bottom() + 4;
    for (int i = 0; i < lines.size(); ++i) {
        const bool inlineChevron = menu && i == 1;
        const int avail = int(r.width()) - 4 - (inlineChevron ? 12 : 0);
        const QString text = fm.elidedText(lines[i], Qt::ElideRight, avail);
        if (inlineChevron) {
            // Center the text and its chevron together.
            const qreal tw = fm.horizontalAdvance(text), total = tw + 12;
            const qreal x = r.center().x() - total / 2;
            p.setPen(tc);
            p.drawText(QRectF(x, y, tw + 1, fm.height()), Qt::AlignLeft | Qt::AlignTop, text);
            chevron(QPointF(x + tw + 7.5, y + fm.ascent() - fm.xHeight() / 2.0));
        } else {
            p.setPen(tc);
            p.drawText(QRect(int(r.left()), y, int(r.width()), fm.height()), Qt::AlignHCenter | Qt::AlignTop, text);
        }
        y += fm.height() - 1;
    }
    if (menu && lines.size() < 2) chevron(QPointF(r.center().x(), std::min<double>(y + 5, r.bottom() - 4)));
}

namespace {
class LargeButton : public QToolButton {
public:
    using QToolButton::QToolButton;
    QSize sizeHint() const override { return largeRibbonButtonHint(this); }
    QSize minimumSizeHint() const override { return largeRibbonButtonHint(this); }

protected:
    void paintEvent(QPaintEvent *) override { paintLargeRibbonButton(this); }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave) update();
        return QToolButton::event(e);
    }
};
} // namespace

QToolButton *ribbonButton(QAction *a, bool large, QWidget *parent)
{
    auto *b = large ? new LargeButton(parent) : new QToolButton(parent);
    if (large) b->setAttribute(Qt::WA_Hover);
    b->setDefaultAction(a);
    b->setAutoRaise(true);
    // The keyboard reaches it (Tab, the arrow keys, KeyTips); a click
    // leaves the focus where it was, in the text being edited.
    b->setFocusPolicy(Qt::TabFocus);
    if (large) {
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(30, 30));
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        b->setProperty("ribbonLarge", true);
    } else {
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(16, 16));
        b->setFixedHeight(24);
    }
    return b;
}

void setKeytip(QObject *w, const QString &k) { w->setProperty("keytip", k); }
QString keytip(const QObject *w) { return w->property("keytip").toString(); }

// ---------------- group ----------------
RibbonGroup::RibbonGroup(const QString &title, QWidget *parent) : QFrame(parent), m_title(title)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 19, 8, 4);   // title sits on top of the card
    outer->setSpacing(0);
    m_cols = new QHBoxLayout();
    m_cols->setSpacing(2);
    m_cols->setContentsMargins(0, 0, 0, 0);
    outer->addLayout(m_cols);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
}

QToolButton *RibbonGroup::addLarge(QAction *a, QMenu *menu, bool split)
{
    m_col = nullptr;
    QToolButton *b = ribbonButton(a, true, this);
    if (menu) {
        b->setMenu(menu);
        b->setPopupMode(split ? QToolButton::MenuButtonPopup : QToolButton::InstantPopup);
    }
    m_cols->addWidget(b);
    return b;
}

void RibbonGroup::beginColumn()
{
    m_col = new QVBoxLayout();
    m_col->setSpacing(1);
    m_col->setContentsMargins(0, 0, 0, 0);
    m_col->addStretch(1);
    m_cols->addLayout(m_col);
    m_rows = 0;
}

QToolButton *RibbonGroup::addSmall(QAction *a, QMenu *menu, bool split, bool iconOnly)
{
    if (!m_col || m_rows >= 3) beginColumn();
    QToolButton *b = ribbonButton(a, false, this);
    if (iconOnly) b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    if (menu) {
        b->setMenu(menu);
        b->setPopupMode(split ? QToolButton::MenuButtonPopup : QToolButton::InstantPopup);
    }
    m_col->insertWidget(m_col->count() - 1, b, 0, Qt::AlignLeft);
    ++m_rows;
    return b;
}

void RibbonGroup::addRow(const QList<QWidget *> &widgets)
{
    if (!m_col || m_rows >= 3) beginColumn();
    auto *row = new QHBoxLayout();
    row->setSpacing(1);
    row->setContentsMargins(0, 0, 0, 0);
    for (QWidget *w : widgets) {
        w->setParent(this);
        row->addWidget(w);
    }
    row->addStretch(1);
    m_col->insertLayout(m_col->count() - 1, row);
    ++m_rows;
}

void RibbonGroup::addWidget(QWidget *w)
{
    m_col = nullptr;
    w->setParent(this);
    m_cols->addWidget(w);
}

void RibbonGroup::addSeparator()
{
    m_col = nullptr;
    auto *f = new QFrame(this);
    f->setFixedWidth(1);
    f->setStyleSheet(QStringLiteral("background:%1;").arg(lineColor().name()));
    m_cols->addSpacing(2);
    m_cols->addWidget(f);
    m_cols->addSpacing(2);
}

void RibbonGroup::setLauncher(const std::function<void()> &fn, const QString &tip)
{
    if (!m_launcher) {
        m_launcher = new QToolButton(this);
        m_launcher->setObjectName(QStringLiteral("launcher"));
        m_launcher->setAutoRaise(true);
        m_launcher->setFocusPolicy(Qt::TabFocus);
        m_launcher->setIcon(icon("ellipsis"));
        m_launcher->setIconSize(QSize(13, 13));
        m_launcher->setFixedSize(18, 15);
    }
    m_launcher->setToolTip(tip.isEmpty() ? QStringLiteral("%1 Settings").arg(m_title) : tip);
    m_launcher->setAccessibleName(m_launcher->toolTip() + QStringLiteral("…"));
    m_launcher->setAccessibleDescription(QStringLiteral("Opens the %1 dialog").arg(m_launcher->toolTip()));
    QObject::disconnect(m_launcher, nullptr, nullptr, nullptr);
    connect(m_launcher, &QToolButton::clicked, this, [fn] { fn(); });
}

static void collectControls(const QLayout *l, QList<QWidget *> &out)
{
    for (int i = 0; i < l->count(); ++i) {
        QLayoutItem *it = l->itemAt(i);
        if (QWidget *w = it->widget()) out << w;
        else if (QLayout *sub = it->layout()) collectControls(sub, out);
    }
}

QList<QWidget *> RibbonGroup::controls() const
{
    QList<QWidget *> out;
    collectControls(layout(), out);
    return out;
}

void RibbonGroup::paintEvent(QPaintEvent *e)
{
    QFrame::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // A soft rounded card per group, titled at the top left in small capitals.
    const QRectF card = QRectF(rect()).adjusted(3, 3, -3, -2);
    p.setPen(Qt::NoPen);
    p.setBrush(dark() ? QColor(255, 255, 255, 10) : QColor(0x1E, 0x29, 0x3B, 9));
    p.drawRoundedRect(card, 9, 9);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.74);
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    p.setFont(f);
    p.setPen(mutedText());
    const QRectF label(card.left() + 9, card.top() + 3, card.width() - (m_launcher ? 30 : 18), 13);
    p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(m_title.toUpper(), Qt::ElideRight, int(label.width())));
    if (m_launcher) m_launcher->move(int(card.right()) - 20, int(card.top()) + 2);
}

// ---------------- tab ----------------
RibbonTab::RibbonTab(QWidget *parent) : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(6, 3, 6, 3);
    m_layout->setSpacing(4);
}

RibbonGroup *RibbonTab::addGroup(const QString &title)
{
    auto *g = new RibbonGroup(title, this);
    m_layout->addWidget(g);
    // Its collapsed form: one large button with the group's name.
    auto *a = new QAction(title, this);
    auto *b = ribbonButton(a, true, this);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setMenu(new QMenu(b));   // shows the chevron; the dropdown itself is our own popup
    connect(b->menu(), &QMenu::aboutToShow, this, [this, idx = int(m_groups.size()), b] {
        QTimer::singleShot(0, b->menu(), &QMenu::close);
        QTimer::singleShot(0, this, [this, idx] { popUp(idx); });
    });
    b->hide();
    m_layout->addWidget(b);
    m_groups << g;
    m_buttons << b;
    m_collapsed << false;
    return g;
}

void RibbonTab::finish()
{
    m_layout->addStretch(1);
    // The collapsed button shows the group's first icon.
    for (int i = 0; i < m_groups.size(); ++i) {
        QIcon ic;
        for (QToolButton *tb : m_groups[i]->findChildren<QToolButton *>())
            if (!tb->icon().isNull() && !tb->text().isEmpty() && tb->toolButtonStyle() != Qt::ToolButtonIconOnly) { ic = tb->icon(); break; }
        if (QAction *a = m_buttons[i]->defaultAction()) a->setIcon(ic.isNull() ? icon("layout-grid") : ic);
    }
}

QSize RibbonTab::minimumSizeHint() const { return QSize(120, QWidget::minimumSizeHint().height()); }

int RibbonTab::collapsedCount() const { return int(std::count(m_collapsed.begin(), m_collapsed.end(), true)); }

void RibbonTab::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    relayout();
}

void RibbonTab::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    relayout();
}

void RibbonTab::relayout()
{
    if (m_inRelayout || m_groups.isEmpty()) return;
    m_inRelayout = true;
    const QMargins mg = m_layout->contentsMargins();
    const int avail = width() - mg.left() - mg.right() - 4;
    QVector<int> full(m_groups.size()), small(m_groups.size());
    int total = 0;
    for (int i = 0; i < m_groups.size(); ++i) {
        full[i] = m_groups[i]->sizeHint().width();
        small[i] = m_buttons[i]->sizeHint().width() + 8;
        total += full[i];
    }
    QVector<bool> col(m_groups.size(), false);
    for (int i = int(m_groups.size()) - 1; i >= 0 && total > avail; --i) {
        total += small[i] - full[i];
        col[i] = true;
    }
    for (int i = 0; i < m_groups.size(); ++i) {
        m_collapsed[i] = col[i];
        if (i != m_popped) m_groups[i]->setVisible(!col[i]);
        m_buttons[i]->setVisible(col[i]);
    }
    m_inRelayout = false;
}

void RibbonTab::popUp(int i)
{
    if (i < 0 || i >= m_groups.size() || m_popped >= 0) return;
    RibbonGroup *g = m_groups[i];
    QToolButton *b = m_buttons[i];
    auto *pop = new QFrame(this, Qt::Popup);
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setObjectName(QStringLiteral("jpRibbonPop"));
    pop->setStyleSheet(QStringLiteral("#jpRibbonPop{background:%1; border:1px solid %2; border-radius:8px;}").arg(ribbonBg().name(), lineColor().name()));
    auto *pl = new QVBoxLayout(pop);
    pl->setContentsMargins(6, 6, 6, 4);
    m_popped = i;
    g->setParent(pop);
    pl->addWidget(g);
    g->show();
    // A command chosen in the dropdown closes it.
    for (QToolButton *tb : g->findChildren<QToolButton *>())
        if (!tb->menu()) connect(tb, &QToolButton::clicked, pop, &QWidget::close);
    connect(pop, &QObject::destroyed, this, [this, g, b] {
        // Put the group back beside its button, hidden while collapsed.
        g->setParent(this);
        m_layout->insertWidget(m_layout->indexOf(b), g);
        m_popped = -1;
        relayout();
    });
    pop->adjustSize();
    QPoint at = b->mapToGlobal(QPoint(0, b->height()));
    pop->move(at);
    pop->show();
}

// ---------------- header ----------------
// One control in the ribbon's top row: the File button, a tab, or the
// chevron that collapses the ribbon. Drawn as pills, but real controls:
// the keyboard reaches them and screen readers see them. Only the current
// tab takes the focus with Tab; the arrow keys move along the tabs and
// switch to each (as tab lists do).
class HeaderButton : public QAbstractButton {
public:
    enum Kind { File, Tab, Collapse };
    HeaderButton(Kind k, Ribbon *r, int tab, QWidget *parent) : QAbstractButton(parent), m_kind(k), m_r(r), m_tab(tab)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(k == Tab ? Qt::NoFocus : Qt::TabFocus);
        setObjectName(k == File ? QStringLiteral("jpRibbonFile") : k == Tab ? QStringLiteral("jpRibbonTab") : QStringLiteral("jpRibbonCollapse"));
        if (k == Tab) setProperty("jpOwnArrows", true);   // they move along the tabs
        if (k == File) {
            setText(QStringLiteral("File"));
            setAccessibleDescription(QStringLiteral("Opens the File page: new, open, save, print, share, export, and options."));
        }
        connect(this, &QAbstractButton::clicked, this, [this] {
            if (m_kind == File) Q_EMIT m_r->fileClicked();
            else if (m_kind == Collapse) m_r->setMinimized(!m_r->m_minimized);
            else {
                if (m_r->m_minimized) m_r->setMinimized(false);
                m_r->showTab(m_r->m_tabs[m_tab].page);
            }
        });
    }
    Kind kind() const { return m_kind; }
    int tab() const { return m_tab; }
    bool isCurrent() const { return m_kind == Tab && m_tab == m_r->m_current && !m_r->m_minimized; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool hot = underMouse();
        if (m_kind == File) {
            // One pill holding the "JP" badge, the label and a chevron, so it
            // reads as a single control that opens the File screen.
            QColor bg = uiAccent();
            bg.setAlphaF(dark() ? (hot ? 0.34f : 0.22f) : (hot ? 0.18f : 0.10f));
            QColor edge = uiAccent();
            edge.setAlphaF(dark() ? 0.55f : 0.35f);
            p.setPen(QPen(edge, 1));
            p.setBrush(bg);
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
            const QRectF badge(r.left() + 4, r.center().y() - 9, 18, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(uiAccent());
            p.drawEllipse(badge);
            QFont bf = font();
            bf.setBold(true);
            bf.setPixelSize(9);
            p.setFont(bf);
            p.setPen(uiHighContrast() ? palette().color(QPalette::HighlightedText) : QColor(Qt::white));
            p.drawText(badge, Qt::AlignCenter, QStringLiteral("JP"));
            QFont lf = font();
            lf.setWeight(QFont::DemiBold);
            p.setFont(lf);
            const QColor fg = uiHighContrast() ? uiText() : dark() ? uiAccent().lighter(140) : uiAccent();
            p.setPen(fg);
            p.drawText(QRectF(badge.right() + 7, r.top(), r.right() - badge.right() - 25, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text());
            const QPointF c(r.right() - 12, r.center().y() + 0.5);
            p.setPen(QPen(fg, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF({c + QPointF(-3, -1.5), c + QPointF(0, 1.5), c + QPointF(3, -1.5)}));
            return;
        }
        if (m_kind == Collapse) {
            if (hot) {
                p.setPen(Qt::NoPen);
                p.setBrush(dark() ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 12));
                p.drawRoundedRect(r, 6, 6);
            }
            p.setPen(QPen(mutedText(), 1.5));
            const QRect cr(width() / 2 - 8, height() / 2 - 8, 16, 16);
            if (m_r->m_minimized) { p.drawLine(cr.left() + 3, cr.top() + 6, cr.center().x(), cr.top() + 11); p.drawLine(cr.center().x(), cr.top() + 11, cr.right() - 3, cr.top() + 6); }
            else { p.drawLine(cr.left() + 3, cr.top() + 11, cr.center().x(), cr.top() + 6); p.drawLine(cr.center().x(), cr.top() + 6, cr.right() - 3, cr.top() + 11); }
            return;
        }
        // Tabs are pills: the current one filled, others lit on hover.
        // Contextual tabs (Text Box, Picture...) carry a colored dot.
        const auto &t = m_r->m_tabs[m_tab];
        const bool cur = isCurrent();
        const QColor base = t.group.isEmpty() || uiHighContrast() ? uiAccent() : t.color;
        if (cur) {
            QColor bg = base;
            bg.setAlphaF(uiHighContrast() ? 1.0f : dark() ? 0.30f : 0.13f);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        } else if (hot) {
            p.setPen(uiHighContrast() ? QPen(uiText(), 1) : Qt::NoPen);
            p.setBrush(uiHighContrast() ? Qt::NoBrush : dark() ? QBrush(QColor(255, 255, 255, 18)) : QBrush(QColor(0, 0, 0, 12)));
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
        QRectF tr = r;
        if (!t.group.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(uiHighContrast() ? (cur ? palette().color(QPalette::HighlightedText) : uiText()) : t.color);
            p.drawEllipse(QPointF(r.left() + 13, r.center().y()), 3.5, 3.5);
            tr.setLeft(r.left() + 12);
        }
        QFont tf = font();
        if (cur) tf.setWeight(QFont::DemiBold);
        p.setFont(tf);
        if (uiHighContrast()) p.setPen(cur ? palette().color(QPalette::HighlightedText) : uiText());
        else p.setPen(cur ? (dark() ? base.lighter(140) : base.darker(t.group.isEmpty() ? 100 : 130)) : uiText());
        p.drawText(tr, Qt::AlignCenter, t.title);
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (m_kind == Tab) m_r->setMinimized(!m_r->m_minimized);
        else QAbstractButton::mouseDoubleClickEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (m_kind != Tab || (e->modifiers() & ~Qt::KeypadModifier)) return QAbstractButton::keyPressEvent(e);
        switch (e->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right: {
            const int dir = e->key() == Qt::Key_Right ? 1 : -1;
            for (int i = m_tab + dir; i >= 0 && i < m_r->m_tabs.size(); i += dir)
                if (m_r->m_tabs[i].visible) {
                    m_r->showTab(m_r->m_tabs[i].page);
                    if (QWidget *b = m_r->tabButton(i)) b->setFocus(Qt::TabFocusReason);
                    return;
                }
            // Past the ends: the File button, or the collapse chevron.
            if (QWidget *w = m_r->findChild<QWidget *>(dir < 0 ? QStringLiteral("jpRibbonFile") : QStringLiteral("jpRibbonCollapse"))) w->setFocus(Qt::TabFocusReason);
            return;
        }
        case Qt::Key_Down: {
            // Into the tab's controls, the nearest below.
            QList<QWidget *> controls;
            if (RibbonTab *page = m_r->current())
                for (QWidget *w : page->findChildren<QWidget *>())
                    if (w->isVisible() && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus)) controls << w;
            if (QWidget *w = nearestInDirection(this, Qt::Key_Down, controls)) w->setFocus(Qt::TabFocusReason);
            return;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            click();
            return;
        default:
            QAbstractButton::keyPressEvent(e);
        }
    }

private:
    Kind m_kind;
    Ribbon *m_r;
    int m_tab;
};

// The top row: the Quick Access Toolbar, the File button, the tabs and the
// collapse chevron as controls, with the brand mark drawn behind them.
class RibbonHeader : public QWidget {
public:
    explicit RibbonHeader(Ribbon *r) : QWidget(r), m_r(r)
    {
        setObjectName(QStringLiteral("jpRibbonTabs"));
        setAccessibleName(QStringLiteral("Ribbon tabs"));
        setFixedHeight(50);   // room for a small gap under the tab pills
        m_file = new HeaderButton(HeaderButton::File, r, -1, this);
        m_collapse = new HeaderButton(HeaderButton::Collapse, r, -1, this);
    }
    QVector<QToolButton *> qatButtons;
    QVector<HeaderButton *> tabButtons;
    HeaderButton *m_file, *m_collapse;

    int qatWidth() const
    {
        int w = 6;
        for (auto *b : qatButtons) w += b->width() + 2;
        return w + 8;
    }
    void layoutQat()
    {
        int x = 6;
        for (auto *b : qatButtons) {
            b->setGeometry(x, 18, 26, 26);
            x += 28;
        }
        layoutButtons();
    }
    void addTabButton(int i)
    {
        auto *b = new HeaderButton(HeaderButton::Tab, m_r, i, this);
        const auto &t = m_r->m_tabs[i];
        b->setText(t.title);
        b->setAccessibleName(t.title);
        tabButtons << b;
        b->show();
        layoutButtons();
    }
    // Places the File button, the visible tabs and the chevron; the current
    // tab is the one Tab reaches, and every tab button says which it is.
    void layoutButtons()
    {
        QFontMetrics fm(font());
        int x = qatWidth();
        m_file->setGeometry(x + 2, 19, fm.horizontalAdvance(m_file->text()) + 58, 26);
        x += 6 + m_file->width() + 4;
        for (HeaderButton *b : tabButtons) {
            const auto &t = m_r->m_tabs[b->tab()];
            b->setVisible(t.visible);
            if (!t.visible) continue;
            const int w = fm.horizontalAdvance(t.title) + 26 + (t.group.isEmpty() ? 0 : 12);
            b->setGeometry(x, 19, w, 26);
            x += w + 4;
            b->setText(t.title);
            b->setAccessibleName(t.title);
            b->setAccessibleDescription(t.group.isEmpty() ? QStringLiteral("Ribbon tab") : QStringLiteral("Ribbon tab, %1").arg(t.group));
            b->setFocusPolicy(b->tab() == m_r->m_current ? Qt::TabFocus : Qt::NoFocus);
            b->update();
        }
        m_collapse->setGeometry(width() - 30, 18, 28, 26);
        m_collapse->setAccessibleName(m_r->m_minimized ? QStringLiteral("Expand the Ribbon") : QStringLiteral("Collapse the Ribbon"));
        m_collapse->update();
        m_file->update();
    }

protected:
    void resizeEvent(QResizeEvent *) override { layoutButtons(); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), headerBg());
        // Brand mark: four CMYK dots.
        const QColor dots[4] = {QColor(0, 163, 224), QColor(229, 0, 126), QColor(255, 212, 0), dark() ? QColor(220, 220, 220) : QColor(28, 35, 48)};
        for (int i = 0; i < 4; ++i) {
            p.setPen(Qt::NoPen);
            p.setBrush(dots[i]);
            p.drawEllipse(QPointF(width() - 120 + i * 9, 9), 3, 3);
        }
        QFont brand = font();
        brand.setBold(true);
        brand.setPointSizeF(brand.pointSizeF() * 0.85);
        p.setFont(brand);
        p.setPen(mutedText());
        p.drawText(QRect(width() - 80, 1, 74, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("JeffPub"));
        p.setPen(lineColor());
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    void wheelEvent(QWheelEvent *e) override
    {
        // The mouse wheel scrolls through tabs.
        int i = m_r->m_current;
        const int n = m_r->m_tabs.size();
        for (int k = 0; k < n; ++k) {
            i = (i + (e->angleDelta().y() < 0 ? 1 : n - 1)) % n;
            if (m_r->m_tabs[i].visible) break;
        }
        m_r->showTab(m_r->m_tabs[i].page);
    }

private:
    Ribbon *m_r;
};

// ---------------- ribbon ----------------
Ribbon::Ribbon(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_header = new RibbonHeader(this);
    m_stack = new QStackedWidget(this);
    m_stack->setFixedHeight(100);
    v->addWidget(m_header);
    v->addWidget(m_stack);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, ribbonBg());
    setPalette(pal);
    m_stack->setAutoFillBackground(true);
    m_stack->setPalette(pal);
    setAccessibleName(QStringLiteral("Ribbon"));
    installArrowNavigation(this);
}

void Ribbon::keyPressEvent(QKeyEvent *e)
{
    // Escape from a control (that didn't use it) goes back to the page.
    if (e->key() == Qt::Key_Escape) {
        Q_EMIT leaveRequested();
        return;
    }
    QWidget::keyPressEvent(e);
}

void Ribbon::focusCurrentTab()
{
    if (m_minimized) setMinimized(false);
    if (QWidget *b = tabButton(m_current)) b->setFocus(Qt::TabFocusReason);
}

RibbonTab *Ribbon::addTab(const QString &name, const QString &contextGroup, const QColor &color, const QString &title)
{
    auto *page = new RibbonTab();
    auto *scroll = new QScrollArea();
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->horizontalScrollBar()->setStyleSheet(QStringLiteral("QScrollBar:horizontal{height:6px;}"));
    QPalette pal = scroll->palette();
    pal.setColor(QPalette::Window, ribbonBg());
    scroll->setPalette(pal);
    page->setAutoFillBackground(true);
    page->setPalette(pal);
    m_stack->addWidget(scroll);
    m_tabs.push_back(Tab{name, contextGroup, color, page, scroll, contextGroup.isEmpty(), title.isEmpty() ? name : title, QString()});
    m_header->addTabButton(int(m_tabs.size()) - 1);
    return page;
}

void Ribbon::setTabKeytip(RibbonTab *t, const QString &k)
{
    for (Tab &tab : m_tabs)
        if (tab.page == t) tab.keytip = k;
}

void Ribbon::setContextVisible(const QString &group, bool visible)
{
    bool changed = false, currentHidden = false;
    for (int i = 0; i < m_tabs.size(); ++i) {
        auto &t = m_tabs[i];
        if (t.group != group || t.visible == visible) continue;
        t.visible = visible;
        changed = true;
        if (!visible && i == m_current) currentHidden = true;
    }
    if (currentHidden) {
        for (int i = 0; i < m_tabs.size(); ++i)
            if (m_tabs[i].name == QLatin1String("Home")) { showTab(m_tabs[i].page); break; }
    }
    if (changed) m_header->layoutButtons();
}

void Ribbon::showTab(RibbonTab *t)
{
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].page == t) {
            m_current = i;
            m_stack->setCurrentWidget(m_tabs[i].scroll);
        }
    m_header->layoutButtons();
    Q_EMIT tabChanged();
}

RibbonTab *Ribbon::current() const { return m_tabs.value(m_current).page; }

RibbonTab *Ribbon::tab(const QString &name) const
{
    for (const auto &t : m_tabs)
        if (t.name == name) return t.page;
    return nullptr;
}

QToolButton *Ribbon::addQuickAccess(QAction *a)
{
    auto *b = new QToolButton(m_header);
    b->setDefaultAction(a);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setFocusPolicy(Qt::TabFocus);   // the keyboard reaches it; a click leaves the focus in the text
    m_header->qatButtons << b;
    m_header->layoutQat();
    m_header->update();
    return b;
}

QWidget *Ribbon::tabButton(int i) const
{
    for (HeaderButton *b : m_header->tabButtons)
        if (b->tab() == i) return b;
    return nullptr;
}

QWidget *Ribbon::fileButton() const { return m_header->m_file; }

QList<QToolButton *> Ribbon::quickAccessButtons() const { return QList<QToolButton *>(m_header->qatButtons.begin(), m_header->qatButtons.end()); }

void Ribbon::setMinimized(bool m)
{
    m_minimized = m;
    m_stack->setVisible(!m);
    m_header->layoutButtons();
}

// ---------- description (tests) ----------
namespace {
QString describeAction(const QAction *a)
{
    if (!a) return QStringLiteral("(none)");
    if (a->isSeparator()) return QStringLiteral("-");
    if (auto *wa = qobject_cast<const QWidgetAction *>(a)) return QStringLiteral("widget:") + QString::fromLatin1(wa->defaultWidget() ? wa->defaultWidget()->metaObject()->className() : "?");
    QString s = a->objectName().isEmpty() ? QStringLiteral("\"%1\"").arg(a->text()) : a->objectName();
    if (a->menu()) s += QStringLiteral(" >");
    return s;
}

QString describeMenu(const QMenu *m, int depth = 0)
{
    if (!m) return QString();
    QStringList parts;
    for (const QAction *a : m->actions()) {
        QString s = describeAction(a);
        if (a->menu() && depth < 3) s += QStringLiteral(" {") + describeMenu(a->menu(), depth + 1) + QLatin1Char('}');
        parts << s;
    }
    return parts.join(QStringLiteral(", "));
}

QString keytipSuffix(const QObject *o, bool on, const char *label = "keytip")
{
    const QString k = on && o ? keytip(o) : QString();
    return k.isEmpty() ? QString() : QStringLiteral(" %1=%2").arg(QLatin1String(label), k);
}

QString describeWidget(const QWidget *w, bool keytips)
{
    QString s = QString::fromLatin1(w->metaObject()->className());
    if (auto *b = qobject_cast<const QToolButton *>(w)) {
        s += QLatin1Char(' ') + (b->defaultAction() ? describeAction(b->defaultAction()) : QStringLiteral("\"%1\"").arg(b->text()));
        static const char *styles[] = {"icon", "text", "beside", "under", "follow"};
        s += QStringLiteral(" style=%1 icon=%2").arg(QLatin1String(styles[b->toolButtonStyle()])).arg(b->iconSize().width());
        if (b->menu()) {
            static const char *modes[] = {"delayed", "split", "instant"};
            s += QStringLiteral(" popup=%1 menu=[%2]").arg(QLatin1String(modes[b->popupMode()]), describeMenu(b->menu()));
        }
    } else if (auto *l = qobject_cast<const QLabel *>(w)) {
        s += QStringLiteral(" \"%1\"").arg(l->text());
    }
    if (!w->toolTip().isEmpty()) s += QStringLiteral(" tip=\"%1\"").arg(w->toolTip());
    return s + keytipSuffix(w, keytips);
}

void describeLayout(const QLayout *l, int depth, QStringList &out, bool keytips)
{
    for (int i = 0; i < l->count(); ++i) {
        QLayoutItem *it = l->itemAt(i);
        const QString pad(depth * 2, QLatin1Char(' '));
        if (QWidget *w = it->widget()) out << pad + describeWidget(w, keytips);
        else if (QLayout *sub = it->layout()) {
            out << pad + QString::fromLatin1(qobject_cast<QHBoxLayout *>(sub) ? "row" : "column");
            describeLayout(sub, depth + 1, out, keytips);
        }
    }
}
} // namespace

QString Ribbon::describe(bool keytips) const
{
    QStringList out;
    if (keytips && !m_fileKeytip.isEmpty()) out << QStringLiteral("file keytip=") + m_fileKeytip;
    QStringList qat;
    for (const QToolButton *b : m_header->qatButtons) qat << describeAction(b->defaultAction()) + keytipSuffix(b, keytips);
    out << QStringLiteral("quick access: ") + qat.join(QStringLiteral(", "));
    for (const Tab &t : m_tabs) {
        QString line = QStringLiteral("tab %1%2").arg(t.name, t.group.isEmpty() ? QString() : QStringLiteral(" (%1, %2)").arg(t.group, t.color.name()));
        if (keytips && !t.keytip.isEmpty()) line += QStringLiteral(" keytip=") + t.keytip;
        out << line;
        for (const RibbonGroup *g : t.page->findChildren<RibbonGroup *>(Qt::FindDirectChildrenOnly)) {
            QString head = QStringLiteral("  group %1").arg(g->title());
            const QToolButton *l = g->findChild<QToolButton *>(QStringLiteral("launcher"), Qt::FindDirectChildrenOnly);
            if (l) head += QStringLiteral(" launcher=\"%1\"").arg(l->toolTip());
            head += keytipSuffix(g, keytips) + keytipSuffix(l, keytips, "launcher-keytip");
            out << head;
            describeLayout(g->layout(), 2, out, keytips);
        }
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace jp
