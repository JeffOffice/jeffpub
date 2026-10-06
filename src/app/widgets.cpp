#include "app/widgets.h"
#include "app/ribbon.h"

#include "app/icons.h"
#include "app/settings.h"
#include "core/fonts.h"

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>
#include <QPainter>
#include <QStyleHints>
#include <QGuiApplication>
#include <QPushButton>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace jp {

static QList<QColor> &recentColors()
{
    static QList<QColor> r;
    return r;
}

QWidget *popupFor(QWidget *content, QWidget *anchor)
{
    auto *pop = new QFrame(anchor, Qt::Popup);
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setFrameShape(QFrame::StyledPanel);
    auto *l = new QVBoxLayout(pop);
    l->setContentsMargins(4, 4, 4, 4);
    l->addWidget(content);
    pop->adjustSize();
    QPoint p = anchor->mapToGlobal(QPoint(0, anchor->height()));
    const QRect screen = anchor->screen() ? anchor->screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
    if (p.x() + pop->width() > screen.right()) p.setX(screen.right() - pop->width());
    if (p.y() + pop->height() > screen.bottom()) p.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - pop->height());
    pop->move(p);
    pop->show();
    return pop;
}

// ---------------- ColorPopup ----------------
class Swatch : public QToolButton {
public:
    Swatch(const QColor &c, QWidget *parent) : QToolButton(parent), m_c(c)
    {
        setFixedSize(18, 18);
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(2, 2, -2, -2);
        p.fillRect(r, m_c);
        p.setPen(underMouse() ? QColor(230, 120, 30) : QColor(0, 0, 0, 50));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        if (underMouse()) p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    QColor m_c;
};

ColorPopup::ColorPopup(const ColorScheme &s, bool allowNone, const QString &noneLabel, QWidget *parent)
    : QFrame(parent, Qt::Popup), m_scheme(s)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);
    if (allowNone) {
        auto *none = new QPushButton(icon("ban"), noneLabel, this);
        none->setFlat(true);
        connect(none, &QPushButton::clicked, this, [this] { Q_EMIT picked(ColorRef::none()); close(); });
        v->addWidget(none);
    }
    auto label = [&](const QString &t) {
        auto *l = new QLabel(t, this);
        QFont f = l->font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 0.9);
        l->setFont(f);
        v->addWidget(l);
    };
    label(QStringLiteral("Scheme Colors"));
    auto *grid = new QGridLayout();
    grid->setSpacing(0);
    for (int slot = 0; slot < SlotCount; ++slot) {
        const ColorRef base = ColorRef::scheme(slot);
        grid->addWidget(swatch(base, base.resolve(s), base.displayName()), 0, slot);
        const int tints[] = {20, 40, 60, 80};
        for (int i = 0; i < 4; ++i) {
            const ColorRef t = ColorRef::scheme(slot, tints[i]);
            grid->addWidget(swatch(t, t.resolve(s), t.displayName()), i + 2, slot);
        }
        const ColorRef sh = ColorRef::scheme(slot, 0, 30);
        grid->addWidget(swatch(sh, sh.resolve(s), sh.displayName()), 6, slot);
    }
    grid->setRowMinimumHeight(1, 4);
    v->addLayout(grid);
    label(QStringLiteral("Standard Colors"));
    auto *std = new QHBoxLayout();
    std->setSpacing(0);
    const char *stdColors[] = {"#C00000", "#FF0000", "#FFC000", "#FFFF00", "#92D050", "#00B050", "#00B0F0", "#0070C0", "#002060", "#7030A0", "#000000", "#FFFFFF"};
    for (const char *c : stdColors) std->addWidget(swatch(ColorRef::rgb(QColor(c)), QColor(c), QString::fromLatin1(c)));
    std->addStretch(1);
    v->addLayout(std);
    if (!recentColors().isEmpty()) {
        label(QStringLiteral("Recent Colors"));
        auto *rec = new QHBoxLayout();
        rec->setSpacing(0);
        for (const QColor &c : recentColors()) rec->addWidget(swatch(ColorRef::rgb(c), c, c.name().toUpper()));
        rec->addStretch(1);
        v->addLayout(rec);
    }
    auto *more = new QPushButton(icon("palette"), QStringLiteral("More Colors…"), this);
    more->setFlat(true);
    connect(more, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(Qt::black, parentWidget(), QStringLiteral("Colors"), QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            addRecent(c);
            Q_EMIT picked(ColorRef::rgb(c));
        }
        close();
    });
    v->addWidget(more);
}

QWidget *ColorPopup::swatch(const ColorRef &c, const QColor &shown, const QString &tip)
{
    auto *b = new Swatch(shown, this);
    b->setToolTip(tip);
    connect(b, &QToolButton::clicked, this, [this, c] {
        if (c.kind() == ColorRef::Rgb) addRecent(c.rgbValue());
        Q_EMIT picked(c);
        close();
    });
    return b;
}

void ColorPopup::addRecent(const QColor &c)
{
    auto &r = recentColors();
    r.removeAll(c);
    r.prepend(c);
    while (r.size() > 10) r.removeLast();
}

void ColorPopup::showBelow(QWidget *anchor)
{
    adjustSize();
    move(anchor->mapToGlobal(QPoint(0, anchor->height())));
    show();
}

// ---------------- ColorButton ----------------
ColorButton::ColorButton(const QString &iconName, const QString &text, bool allowNone, const QString &noneLabel, QWidget *parent)
    : QToolButton(parent), m_icon(iconName), m_allowNone(allowNone), m_noneLabel(noneLabel)
{
    setToolTip(text);
    setPopupMode(QToolButton::MenuButtonPopup);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setIconSize(QSize(16, 16));
    setFixedHeight(24);
    auto *menu = new QMenu(this);
    setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        QTimer::singleShot(0, menu, &QMenu::close);
        openPopup();
    });
    connect(this, &QToolButton::clicked, this, [this] { Q_EMIT colorPicked(m_current); });
    m_current = ColorRef::scheme(Accent1);
    refreshIcon();
}

void ColorButton::refreshIcon()
{
    setIcon(colorBarIcon(m_icon, m_current.isNone() ? QColor(Qt::transparent) : m_current.resolve(m_scheme)));
}

void ColorButton::setWell(bool on)
{
    m_well = on;
    setAutoRaise(!on);
    setFocusPolicy(on ? Qt::StrongFocus : Qt::NoFocus);
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setMinimumHeight(on ? 32 : 0);
    setMaximumHeight(on ? 32 : 24);
    setSizePolicy(on ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Fixed);
    updateGeometry();
    update();
}

QSize ColorButton::sizeHint() const
{
    if (!m_well) return QToolButton::sizeHint();
    return QSize(180, 32);
}

void ColorButton::paintEvent(QPaintEvent *e)
{
    if (!m_well) return QToolButton::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool dk = uiDark();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool hot = underMouse() || isDown() || hasFocus();
    p.setPen(QPen(hot ? uiAccent() : (dk ? QColor(0x4A, 0x52, 0x5E) : QColor(0xC9, 0xCD, 0xD4)), 1));
    p.setBrush(dk ? QColor(0x2B, 0x30, 0x38) : QColor(Qt::white));
    p.drawRoundedRect(r, 6, 6);
    // Swatch.
    const QRectF sw(r.left() + 8, r.center().y() - 8, 26, 16);
    if (m_current.isNone()) {
        p.setPen(QPen(dk ? QColor(0x80, 0x88, 0x94) : QColor(0xA0, 0xA6, 0xB0), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(sw, 3, 3);
        p.setPen(QPen(QColor(0xD0, 0x30, 0x30), 1.4));
        p.drawLine(sw.bottomLeft() + QPointF(2, -2), sw.topRight() + QPointF(-2, 2));
    } else {
        p.setPen(QPen(dk ? QColor(255, 255, 255, 60) : QColor(0, 0, 0, 50), 1));
        p.setBrush(m_current.resolve(m_scheme));
        p.drawRoundedRect(sw, 3, 3);
    }
    // Name.
    QString name;
    if (m_current.isNone()) name = m_noneLabel.isEmpty() ? QStringLiteral("No Color") : m_noneLabel;
    else if (m_current.kind() == ColorRef::Scheme) name = slotName(m_current.slot());
    else name = m_current.resolve(m_scheme).name(QColor::HexRgb).toUpper();
    p.setPen(isEnabled() ? uiText() : QColor(0x9C, 0xA3, 0xAF));
    p.setFont(font());
    const QRectF tr(sw.right() + 8, r.top(), r.right() - 26 - sw.right() - 8, r.height());
    p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(name, Qt::ElideRight, int(tr.width())));
    // Chevron.
    const QPointF c(r.right() - 14, r.center().y());
    p.setPen(QPen(uiText(), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF({c + QPointF(-4, -2), c + QPointF(0, 2), c + QPointF(4, -2)}));
}

void ColorButton::openPopup()
{
    auto *pop = new ColorPopup(m_scheme, m_allowNone, m_noneLabel, this);
    if (!m_extra.isEmpty()) {
        for (QAction *a : m_extra) {
            auto *b = new QPushButton(a->icon(), a->text(), pop);
            b->setFlat(true);
            connect(b, &QPushButton::clicked, pop, [a, pop] { pop->close(); a->trigger(); });
            pop->layout()->addWidget(b);
        }
    }
    connect(pop, &ColorPopup::picked, this, [this](const ColorRef &c) {
        m_current = c;
        refreshIcon();
        Q_EMIT colorPicked(c);
    });
    pop->showBelow(this);
}

// ---------------- Gallery ----------------
namespace {
// Gallery items as rounded tiles: a soft border, a light lift on hover, and an
// accent ring around the current choice.
class TileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark || uiText().lightness() > 128;
        const QRectF r = QRectF(opt.rect).adjusted(2.5, 2.5, -2.5, -2.5);
        const bool sel = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
        QColor bg = dark ? QColor(0x2a, 0x2f, 0x37) : QColor(Qt::white);
        QColor border = dark ? QColor(0x3a, 0x40, 0x4a) : QColor(0xdf, 0xe2, 0xe8);
        if (hover) border = dark ? QColor(0x5a, 0x61, 0x6d) : QColor(0xb8, 0xbe, 0xc8);
        p->setPen(QPen(sel ? uiAccent() : border, sel ? 2 : 1));
        p->setBrush(bg);
        p->drawRoundedRect(r, 7, 7);
        const QIcon ic = idx.data(Qt::DecorationRole).value<QIcon>();
        const QRect ir = r.adjusted(4, 4, -4, -4).toRect();
        ic.paint(p, ir, Qt::AlignCenter, QIcon::Normal);
        p->restore();
    }
};
} // namespace

Gallery::Gallery(const QSize &itemSize, int visibleColumns, QWidget *parent) : QFrame(parent), m_itemSize(itemSize), m_cols(visibleColumns)
{
    setFrameShape(QFrame::NoFrame);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    m_list = new QListWidget(this);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(true);
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setIconSize(itemSize);
    m_list->setGridSize(itemSize + QSize(14, 14));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setMouseTracking(true);
    m_list->setStyleSheet(QStringLiteral("QListWidget{background:transparent; border:none;}"));
    m_list->setItemDelegate(new TileDelegate(m_list));
    const int rowH = itemSize.height() + 14;
    m_list->setFixedSize((itemSize.width() + 14) * visibleColumns + 6, rowH * std::max(1, 66 / rowH) + 2);
    h->addWidget(m_list);
    auto *side = new QVBoxLayout();
    side->setSpacing(3);
    side->setContentsMargins(0, 1, 0, 1);
    auto mk = [&](const QString &ic, const QString &tip, std::function<void()> fn) {
        auto *b = new QToolButton(this);
        b->setIcon(icon(ic));
        b->setIconSize(QSize(12, 12));
        b->setAutoRaise(true);
        b->setFixedSize(20, std::max(18, (m_list->height() - 6) / 3));
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QToolButton{border:none; border-radius:6px; background:transparent;}"
                                        "QToolButton:hover{background:rgba(127,127,127,0.18);}"));
        connect(b, &QToolButton::clicked, this, fn);
        side->addWidget(b);
    };
    mk("chevron-up", "Previous row", [this] { scroll(-1); });
    mk("chevron-down", "Next row", [this] { scroll(1); });
    mk("layout-grid", "Show all", [this] { openMore(); });
    h->addLayout(side);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { Q_EMIT activated(it->data(Qt::UserRole).toString()); });
}

void Gallery::setItems(const QVector<GalleryItem> &items)
{
    m_items = items;
    m_list->clear();
    for (const auto &g : items) {
        auto *it = new QListWidgetItem(g.icon, QString());
        it->setToolTip(g.tip);
        it->setData(Qt::UserRole, g.id);
        it->setSizeHint(m_itemSize + QSize(14, 14));
        m_list->addItem(it);
        if (g.id == m_current) it->setSelected(true);
    }
}

void Gallery::reload()
{
    if (m_provider) setItems(m_provider());
}

void Gallery::showEvent(QShowEvent *e)
{
    QFrame::showEvent(e);
    if (m_items.isEmpty()) reload();
}

void Gallery::setCurrent(const QString &id)
{
    m_current = id;
    for (int i = 0; i < m_list->count(); ++i) m_list->item(i)->setSelected(m_list->item(i)->data(Qt::UserRole).toString() == id);
}

void Gallery::scroll(int dir)
{
    m_list->verticalScrollBar()->setValue(m_list->verticalScrollBar()->value() + dir * (m_itemSize.height() + 14));
}

void Gallery::openMore()
{
    const QVector<GalleryItem> items = m_provider ? m_provider() : m_items;
    galleryPopup(items, m_itemSize, std::max(m_cols, std::min(8, int(std::ceil(std::sqrt(double(items.size())))) + 2)), m_footer,
                 [this](const QString &id) { Q_EMIT activated(id); }, this);
}

QFrame *galleryPopup(const QVector<GalleryItem> &items, const QSize &itemSize, int columns, const QList<QAction *> &footer,
                     const std::function<void(const QString &)> &onPick, QWidget *parent)
{
    auto *pop = new QFrame(parent, Qt::Popup);
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setFrameShape(QFrame::StyledPanel);
    auto *v = new QVBoxLayout(pop);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(2);
    QString group = QStringLiteral("\x01");
    QListWidget *list = nullptr;
    int totalRows = 0;
    auto newList = [&]() {
        list = new QListWidget(pop);
        list->setViewMode(QListView::IconMode);
        list->setMovement(QListView::Static);
        list->setResizeMode(QListView::Adjust);
        list->setIconSize(itemSize);
        list->setGridSize(itemSize + QSize(8, 8));
        list->setFrameShape(QFrame::NoFrame);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setFixedWidth((itemSize.width() + 8) * columns + 6);
        QObject::connect(list, &QListWidget::itemClicked, pop, [pop, onPick](QListWidgetItem *it) {
            const QString id = it->data(Qt::UserRole).toString();
            pop->close();
            onPick(id);
        });
        v->addWidget(list);
    };
    QHash<QListWidget *, int> counts;
    for (const auto &g : items) {
        if (g.group != group) {
            group = g.group;
            if (!group.isEmpty()) {
                auto *l = new QLabel(group, pop);
                QFont f = l->font();
                f.setBold(true);
                l->setFont(f);
                l->setStyleSheet(QStringLiteral("padding:3px 2px;"));
                v->addWidget(l);
            }
            newList();
        }
        if (!list) newList();
        auto *it = new QListWidgetItem(g.icon, QString());
        it->setToolTip(g.tip);
        it->setData(Qt::UserRole, g.id);
        list->addItem(it);
        ++counts[list];
    }
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        const int rows = (it.value() + columns - 1) / columns;
        it.key()->setFixedHeight(rows * (itemSize.height() + 8) + 4);
        totalRows += rows;
    }
    for (QAction *a : footer) {
        auto *b = new QPushButton(a->icon(), a->text(), pop);
        b->setFlat(true);
        b->setStyleSheet(QStringLiteral("text-align:left; padding:4px 6px;"));
        QObject::connect(b, &QPushButton::clicked, pop, [a, pop] { pop->close(); a->trigger(); });
        v->addWidget(b);
    }
    pop->adjustSize();
    const QRect screen = parent->screen() ? parent->screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
    if (pop->height() > screen.height() - 40) {
        // Too tall: wrap the content in a scroll area.
        pop->setFixedHeight(screen.height() - 60);
    }
    QPoint p = parent->mapToGlobal(QPoint(0, parent->height()));
    if (p.x() + pop->width() > screen.right()) p.setX(screen.right() - pop->width());
    if (p.y() + pop->height() > screen.bottom()) p.setY(std::max(screen.top(), screen.bottom() - pop->height()));
    pop->move(p);
    pop->show();
    Q_UNUSED(totalRows);
    return pop;
}

QSize GalleryButton::sizeHint() const { return m_large ? largeRibbonButtonHint(this) : QToolButton::sizeHint(); }
QSize GalleryButton::minimumSizeHint() const { return m_large ? largeRibbonButtonHint(this) : QToolButton::minimumSizeHint(); }
void GalleryButton::paintEvent(QPaintEvent *e)
{
    if (m_large) paintLargeRibbonButton(this);
    else QToolButton::paintEvent(e);
}
bool GalleryButton::event(QEvent *e)
{
    if (m_large && (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)) update();
    return QToolButton::event(e);
}

GalleryButton::GalleryButton(const QIcon &ic, const QString &text, const QSize &itemSize, int columns, bool large, QWidget *parent)
    : QToolButton(parent), m_itemSize(itemSize), m_cols(columns)
{
    setIcon(ic);
    setText(text);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setPopupMode(QToolButton::InstantPopup);
    m_large = large;
    if (large) {
        setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        setIconSize(QSize(30, 30));
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setIconSize(QSize(16, 16));
        setFixedHeight(24);
    }
    auto *menu = new QMenu(this);
    setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        QTimer::singleShot(0, menu, &QMenu::close);
        QTimer::singleShot(0, this, &GalleryButton::openGrid);
    });
}

void GalleryButton::openGrid()
{
    if (!m_provider) return;
    galleryPopup(m_provider(), m_itemSize, m_cols, m_footer, [this](const QString &id) { Q_EMIT activated(id); }, this);
}

// ---------------- FontCombo ----------------
// Each font's name is drawn in that font, scaled so every name has the same
// capital height (fonts vary wildly at a fixed point size) and centered on
// that cap height rather than on the font's own ascent and descent, which
// some fonts set far too large. Symbol fonts, and fonts that can't draw
// their own name, show the name in the interface font with a sample of
// their glyphs beside it.
namespace {
struct FontPreview {
    bool useUi = true;     // draw the name in the interface font
    bool symbol = false;   // also show a glyph sample
    qreal pixel = 0;       // pixel size giving the target cap height
};

FontPreview fontPreview(const QString &fam, qreal targetCap)
{
    static QHash<QString, FontPreview> cache;
    const QString key = fam + QLatin1Char('|') + QString::number(targetCap, 'f', 2);
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    FontPreview pv;
    if (!fam.isEmpty()) {
        const auto ws = QFontDatabase::writingSystems(fam);
        pv.symbol = ws.contains(QFontDatabase::Symbol);
        const bool latin = ws.contains(QFontDatabase::Latin);
        QFont f(fam);
        f.setPixelSize(100);
        const QFontMetricsF fm(f);
        bool drawsName = QFontInfo(f).family().compare(fam, Qt::CaseInsensitive) == 0 || QFontInfo(f).exactMatch();
        for (const QChar c : fam)
            if (!c.isSpace() && !fm.inFont(c)) { drawsName = false; break; }
        const qreal cap = fm.inFont(QLatin1Char('H')) ? fm.tightBoundingRect(QStringLiteral("H")).height() : 0;
        if (!pv.symbol && latin && drawsName && cap > 20 && cap < 200) {
            pv.useUi = false;
            pv.pixel = qBound(targetCap * 1.1, 100.0 * targetCap / cap, targetCap * 2.4);
        }
    }
    cache.insert(key, pv);
    return pv;
}
} // namespace

class FontDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static qreal targetCap(const QFont &ui) { return QFontMetricsF(ui).capHeight() * 1.35; }
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);
        const QString fam = idx.data(Qt::UserRole + 1).toString();
        const bool header = idx.data(Qt::UserRole + 2).toBool();
        p->save();
        p->setClipRect(o.rect);
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);
        if (header) {
            QFont f = o.font;
            f.setBold(true);
            f.setPointSizeF(f.pointSizeF() * 0.9);
            p->setFont(f);
            QColor c = o.palette.text().color();
            c.setAlphaF(0.6);
            p->setPen(c);
            p->drawText(o.rect.adjusted(10, 4, 0, 0), Qt::AlignVCenter, idx.data().toString().toUpper());
            p->restore();
            return;
        }
        const bool sel = o.state & (QStyle::State_Selected | QStyle::State_MouseOver);
        if (sel) {
            p->setPen(Qt::NoPen);
            p->setBrush(o.palette.highlight());
            p->drawRoundedRect(QRectF(o.rect).adjusted(3, 1, -3, -1), 6, 6);
        }
        const QColor ink = sel ? o.palette.highlightedText().color() : o.palette.text().color();
        const qreal cap = targetCap(o.font);
        const FontPreview pv = fontPreview(fam, cap);
        const QRectF r = QRectF(o.rect).adjusted(10, 0, -6, 0);
        const QString name = idx.data().toString();
        if (pv.useUi) {
            QFont ui = o.font;
            ui.setPointSizeF(o.font.pointSizeF() * 1.1);
            const QFontMetricsF um(ui);
            qreal nameRight = r.right();
            if (pv.symbol && !fam.isEmpty()) {
                // A sample of what the font draws, right-aligned and muted.
                QFont sf(fam);
                sf.setPixelSize(qRound(cap * 1.6));
                const QString sample = QStringLiteral("AaBbCc");
                const qreal sw = QFontMetricsF(sf).horizontalAdvance(sample);
                const qreal sampleW = qMin(sw, r.width() * 0.4);
                QColor mc = ink;
                mc.setAlphaF(sel ? 0.85 : 0.55);
                p->setFont(sf);
                p->setPen(mc);
                p->drawText(QRectF(r.right() - sampleW, r.top(), sampleW, r.height()), Qt::AlignVCenter | Qt::AlignRight, sample);
                nameRight = r.right() - sampleW - 8;
            }
            p->setFont(ui);
            p->setPen(ink);
            const qreal base = r.center().y() + um.capHeight() / 2;
            p->drawText(QPointF(r.left(), base), um.elidedText(name, Qt::ElideRight, nameRight - r.left()));
        } else {
            QFont f(fam);
            f.setPixelSize(qMax(1, qRound(pv.pixel)));
            f.setHintingPreference(QFont::PreferNoHinting);
            const QFontMetricsF fm(f);
            p->setFont(f);
            p->setPen(ink);
            const qreal base = r.center().y() + cap / 2;
            p->drawText(QPointF(r.left(), base), fm.elidedText(name, Qt::ElideRight, r.width()));
        }
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        const bool header = i.data(Qt::UserRole + 2).toBool();
        const int h = qRound(targetCap(o.font) * 2.9);
        return QSize(QStyledItemDelegate::sizeHint(o, i).width(), header ? h : h);
    }
};

FontCombo::FontCombo(QWidget *parent) : QComboBox(parent)
{
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    setMinimumWidth(150);
    setMaximumWidth(170);
    setMaxVisibleItems(18);
    setItemDelegate(new FontDelegate(this));
    populate();
    auto choose = [this](int i) {
        if (m_updating || i < 0) return;
        const QString fam = itemData(i, Qt::UserRole).toString();
        if (!fam.isEmpty()) Q_EMIT familyChosen(fam);
    };
    connect(this, QOverload<int>::of(&QComboBox::activated), this, choose);
    connect(lineEdit(), &QLineEdit::returnPressed, this, [this] {
        const QString t = currentText().trimmed();
        if (!t.isEmpty() && !m_updating) Q_EMIT familyChosen(t);
    });
}

void FontCombo::populate()
{
    m_updating = true;
    clear();
    auto add = [&](const QString &text, const QString &value, const QString &fam, bool header = false) {
        addItem(text);
        setItemData(count() - 1, value, Qt::UserRole);
        setItemData(count() - 1, fam, Qt::UserRole + 1);
        setItemData(count() - 1, header, Qt::UserRole + 2);
        if (header) setItemData(count() - 1, 0, Qt::UserRole - 1);   // disabled
    };
    add(QStringLiteral("Scheme Fonts"), QString(), QString(), true);
    add(m_heading.isEmpty() ? QStringLiteral("+Headings") : m_heading + QStringLiteral(" (Headings)"), QStringLiteral("+Heading"), m_heading);
    add(m_body.isEmpty() ? QStringLiteral("+Body") : m_body + QStringLiteral(" (Body)"), QStringLiteral("+Body"), m_body);
    add(QStringLiteral("All Fonts"), QString(), QString(), true);
    QStringList fams = QFontDatabase::families();
    fams.removeDuplicates();
    for (const QString &f : fams) {
        if (QFontDatabase::isPrivateFamily(f) || f.startsWith('.')) continue;
        // Old bitmap and vector fonts (Modern, Roman, Script, Small Fonts...)
        // can't be scaled for print, so they aren't listed.
        if (!QFontDatabase::isSmoothlyScalable(f)) continue;
        add(f, f, f);
    }
    m_updating = false;
}

void FontCombo::setSchemeFonts(const QString &heading, const QString &body)
{
    if (heading == m_heading && body == m_body) return;
    m_heading = heading;
    m_body = body;
    const QString keep = currentText();
    populate();
    setEditText(keep);
}

void FontCombo::setCurrentFamily(const QString &family)
{
    m_updating = true;
    if (!lineEdit()->hasFocus()) setEditText(family);
    m_updating = false;
}

// ---------------- SizeCombo ----------------
SizeCombo::SizeCombo(QWidget *parent) : QComboBox(parent)
{
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    setFixedWidth(70);
    for (int s : {6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, 96, 120, 144, 200, 288}) addItem(QString::number(s));
    auto emitSize = [this] {
        if (m_updating) return;
        bool ok = false;
        const double v = currentText().trimmed().toDouble(&ok);
        if (ok && v > 0 && v <= 1638) Q_EMIT sizeChosen(v);
    };
    connect(this, QOverload<int>::of(&QComboBox::activated), this, emitSize);
    connect(lineEdit(), &QLineEdit::returnPressed, this, emitSize);
}

void SizeCombo::setSize(double pt)
{
    m_updating = true;
    if (!lineEdit()->hasFocus()) setEditText(pt > 0 ? QString::number(std::round(pt * 1000) / 1000) : QString());   // up to three decimals
    m_updating = false;
}

// ---------------- DecimalSpin ----------------
DecimalSpin::DecimalSpin(QWidget *parent) : QDoubleSpinBox(parent) { setDecimals(6); }

QString DecimalSpin::textFromValue(double v) const
{
    QLocale l = locale();
    l.setNumberOptions(QLocale::OmitGroupSeparator);
    QString n = l.toString(v, 'f', 3);
    if (n.contains(l.decimalPoint())) {
        while (n.endsWith('0')) n.chop(1);
        if (n.endsWith(l.decimalPoint())) n.chop(l.decimalPoint().size());
    }
    return n;
}

// ---------------- MeasureSpin ----------------
// Values are points, kept to six places so measurements typed to three
// decimals in any unit come back exactly.
MeasureSpin::MeasureSpin(QWidget *parent) : QDoubleSpinBox(parent)
{
    setRange(0, 72 * 240);
    setDecimals(6);
    setSingleStep(9);
    setKeyboardTracking(false);
    setFixedWidth(86);
    setAccelerated(true);
}

QString MeasureSpin::textFromValue(double v) const { return Settings::get().format(v, 3); }

double MeasureSpin::valueFromText(const QString &text) const
{
    double pt = 0;
    return Settings::get().parse(text, &pt) ? pt : value();
}

QValidator::State MeasureSpin::validate(QString &input, int &) const
{
    double pt;
    if (Settings::get().parse(input, &pt)) return QValidator::Acceptable;
    return QValidator::Intermediate;
}

void MeasureSpin::setPoints(double pt)
{
    if (hasFocus()) return;
    const QSignalBlocker b(this);
    setValue(pt);
}

// ---------------- TableGrid ----------------
TableGrid::TableGrid(QWidget *parent) : QFrame(parent) { setMouseTracking(true); }

QSize TableGrid::sizeHint() const { return QSize(10 * 20 + 4, 8 * 20 + 26); }

void TableGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(palette().text().color());
    p.drawText(QRect(4, 0, width(), 20), Qt::AlignVCenter,
               m_rows ? QStringLiteral("%1 × %2 Table").arg(m_cols).arg(m_rows) : QStringLiteral("Insert Table"));
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 10; ++c) {
            const QRect cell(2 + c * 20, 22 + r * 20, 17, 17);
            const bool on = r < m_rows && c < m_cols;
            p.fillRect(cell, on ? QColor(255, 210, 160) : palette().base().color());
            p.setPen(on ? QColor(220, 120, 40) : QColor(150, 150, 150));
            p.drawRect(cell.adjusted(0, 0, -1, -1));
        }
}

void TableGrid::mouseMoveEvent(QMouseEvent *e)
{
    m_cols = std::clamp(int((e->position().x() - 2) / 20) + 1, 1, 10);
    m_rows = std::clamp(int((e->position().y() - 22) / 20) + 1, 1, 8);
    update();
}

void TableGrid::mousePressEvent(QMouseEvent *)
{
    if (m_rows && m_cols) Q_EMIT picked(m_rows, m_cols);
    if (QWidget *w = window(); w && (w->windowFlags() & Qt::Popup)) w->close();
}

} // namespace jp
