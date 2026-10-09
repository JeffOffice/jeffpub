#include "app/pagespane.h"
#include "app/theme.h"

#include "app/editor.h"
#include "app/icons.h"
#include "app/mainwindow.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDropEvent>
#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyleHints>
#include <QGuiApplication>

namespace jp {

namespace {
bool darkUi() { return uiDark(); }

// Page thumbnails: the current page gets a rounded accent outline and an
// accent number pill; others a soft hover.
class PageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF cell = QRectF(opt.rect).adjusted(4, 2, -4, -2);
        const bool sel = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
        const QColor accent = uiAccent();
        const QSize is = opt.decorationSize;
        const QRectF ir(cell.center().x() - is.width() / 2.0, cell.top() + 4, is.width(), is.height());
        if (hover && !sel) {
            p->setPen(Qt::NoPen);
            p->setBrush(darkUi() ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 10));
            p->drawRoundedRect(cell, 8, 8);
        }
        idx.data(Qt::DecorationRole).value<QIcon>().paint(p, ir.toRect(), Qt::AlignCenter, QIcon::Normal);
        if (sel) {
            p->setPen(QPen(accent, 2));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(ir.adjusted(-3, -3, 3, 3), 6, 6);
        }
        QFont f = opt.font;
        f.setPointSizeF(f.pointSizeF() * 0.9);
        if (sel) f.setWeight(QFont::DemiBold);
        p->setFont(f);
        const QString text = idx.data(Qt::DisplayRole).toString();
        const QFontMetricsF fm(f);
        const double tw = std::min(fm.horizontalAdvance(text) + 14, cell.width() - 8);
        const QRectF pill(cell.center().x() - tw / 2, ir.bottom() + 6, tw, fm.height() + 4);
        if (sel) {
            p->setPen(Qt::NoPen);
            p->setBrush(accent);
            p->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
            p->setPen(Qt::white);
        } else {
            QColor c = uiText();
            c.setAlphaF(0.65f);
            p->setPen(c);
        }
        p->drawText(pill, Qt::AlignCenter, fm.elidedText(text, Qt::ElideRight, pill.width() - 8));
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const override
    {
        return QSize(opt.decorationSize.width() + 20, opt.decorationSize.height() + QFontMetrics(opt.font).height() + 22);
    }
};
} // namespace

PagesPane::PagesPane(Editor *ed, MainWindow *win) : QListWidget(win), m_ed(ed), m_win(win)
{
    setViewMode(QListView::IconMode);
    setFlow(QListView::TopToBottom);
    setWrapping(false);
    setMovement(QListView::Snap);
    setDragDropMode(QAbstractItemView::InternalMove);
    setIconSize(QSize(110, 110));
    setSpacing(6);
    setMinimumWidth(130);
    setMaximumWidth(260);
    setUniformItemSizes(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setItemDelegate(new PageDelegate(this));
    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    auto restyle = [this] {
        setStyleSheet(QStringLiteral("QListWidget{background:%1; border:none; border-right:1px solid %2; outline:0; padding-top:6px;}").arg(uiPanel().name(), uiLine().name()));
    };
    restyle();
    connect(UiTheme::instance(), &UiTheme::changed, this, restyle);   // the look switched in Options
    m_timer.setSingleShot(true);
    m_timer.setInterval(15);
    connect(&m_timer, &QTimer::timeout, this, &PagesPane::renderNext);
    connect(this, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_updating || row < 0) return;
        m_ed->setCurrentPage(row);
    });
}

void PagesPane::refresh(bool thumbnailsOnly)
{
    const int n = m_ed->doc()->pages.size();
    m_updating = true;
    if (!thumbnailsOnly || count() != n) {
        clear();
        for (int i = 0; i < n; ++i) {
            auto *it = new QListWidgetItem(QString::number(i + 1));
            it->setTextAlignment(Qt::AlignHCenter);
            QPixmap blank(110, 110);
            blank.fill(Qt::transparent);
            it->setIcon(QIcon(blank));
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            addItem(it);
        }
    }
    for (int i = 0; i < n; ++i) {
        const Page &pg = *m_ed->doc()->pages[i];
        item(i)->setText(pg.title.isEmpty() ? QString::number(i + 1) : QStringLiteral("%1  %2").arg(i + 1).arg(pg.title));
    }
    if (m_ed->masterView().isEmpty()) setCurrentRow(m_ed->currentPage());
    else clearSelection();
    m_updating = false;
    // Current page first, then the rest.
    m_pending.clear();
    m_pending << m_ed->currentPage();
    for (int i = 0; i < n; ++i)
        if (i != m_ed->currentPage()) m_pending << i;
    m_timer.start();
}

void PagesPane::renderNext()
{
    while (!m_pending.isEmpty()) {
        const int i = m_pending.takeFirst();
        if (i < 0 || i >= count()) continue;
        const QImage img = m_win->pageThumbnail(i, 104);
        QPixmap pm(110, 110);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        const QPointF o((110 - img.width()) / 2.0, (110 - img.height()) / 2.0);
        p.fillRect(QRectF(o + QPointF(2, 2), QSizeF(img.size())), QColor(0, 0, 0, 50));
        p.drawImage(o, img);
        p.setPen(QColor(150, 150, 150));
        p.drawRect(QRectF(o, QSizeF(img.size())).adjusted(0, 0, -1, -1));
        p.end();
        item(i)->setIcon(QIcon(pm));
        break;
    }
    if (!m_pending.isEmpty()) m_timer.start();
}

void PagesPane::dropEvent(QDropEvent *e)
{
    const int from = currentRow();
    QListWidget::dropEvent(e);
    const int to = currentRow();
    if (from >= 0 && to >= 0 && from != to) m_ed->movePage(from, to);
    refresh();
}

void PagesPane::contextMenuEvent(QContextMenuEvent *e)
{
    QListWidgetItem *it = itemAt(e->pos());
    if (it) m_ed->setCurrentPage(row(it));
    QMenu menu(this);
    for (const char *id : {"page.insert", "page.insertDup", "page.insertDialog", "page.delete", "page.rename", "page.move"}) menu.addAction(m_win->act(id));
    menu.addSeparator();
    QMenu *mp = menu.addMenu(QStringLiteral("Master Pages"));
    for (const auto &m : m_ed->doc()->masters) {
        QAction *a = mp->addAction(QStringLiteral("(%1) %2").arg(m->abbr, m->name));
        const QString id = m->id;
        connect(a, &QAction::triggered, this, [this, id] { m_ed->applyMaster(m_ed->currentPage(), id); });
    }
    mp->addAction(m_win->act("mp.none"));
    menu.addAction(m_win->act("view.spread"));
    menu.exec(e->globalPos());
}

} // namespace jp
