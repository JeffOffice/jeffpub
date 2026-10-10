#include "canvas/canvas.h"
#include "canvas/canvasaccessible.h"

#include <QAccessible>
#include "app/icons.h"

#include "app/settings.h"
#include "render/shapes.h"
#include "text/textprops.h"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMimeData>
#include <QPainter>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextDocument>
#include <QTextList>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>

namespace jp {

static const int kRuler = 22;
static const double kHandle = 4.5;     // handle radius in pixels

// How people move the rulers' zero points; the gestures are all here. Shift
// and the right button on a ruler move that ruler's zero to where you let
// go. The left button from the box where the rulers meet moves both, and a
// double-click on that box puts both back at the page's corner.
static bool movesRulerZero(Qt::MouseButton b, Qt::KeyboardModifiers m) { return b == Qt::RightButton && (m & Qt::ShiftModifier); }
static bool movesBothZeros(Qt::MouseButton b) { return b == Qt::LeftButton; }
static bool resetsZeros(Qt::MouseButton b) { return b == Qt::LeftButton; }   // double-clicking a ruler, or the box
static QString zeroLabel() { return QCoreApplication::translate("Canvas", "Move Ruler Zero Point"); }

// The small box where the two rulers meet.
class RulerCorner : public QWidget {
public:
    explicit RulerCorner(Canvas *c) : QWidget(c), m_c(c)
    {
        setAutoFillBackground(true);
        setCursor(Qt::SizeAllCursor);
        setAccessibleName(QCoreApplication::translate("Canvas", "Ruler zero point"));
        setAccessibleDescription(QCoreApplication::translate("Canvas", "Drag to move both rulers' zero point. Double-click to put it back at the page's corner."));
        setToolTip(accessibleDescription());
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        m_button = movesBothZeros(e->button()) ? e->button() : Qt::NoButton;
        m_press = e->position().toPoint();
        if (m_button != Qt::NoButton) m_c->editor()->beginChange(zeroLabel());
    }
    void mouseMoveEvent(QMouseEvent *e) override { if (m_button != Qt::NoButton) follow(e); }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_button == Qt::NoButton || e->button() != m_button) return;
        follow(e);
        m_button = Qt::NoButton;
        m_c->editor()->endChange();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (!resetsZeros(e->button())) return;
        m_button = Qt::NoButton;
        m_c->editor()->beginChange(zeroLabel());
        m_c->hRuler()->setZero(0);
        m_c->vRuler()->setZero(0);
        m_c->editor()->endChange();
    }

private:
    // Both zeros go to the pointer; a click that never leaves its spot moves nothing.
    void follow(QMouseEvent *e)
    {
        if (e->position().toPoint() == m_press) return;
        const QPointF page = m_c->toPage(m_c->viewport()->mapFromGlobal(e->globalPosition().toPoint()));
        m_c->hRuler()->setZero(page.x());
        m_c->vRuler()->setZero(page.y());
    }
    Canvas *m_c;
    Qt::MouseButton m_button = Qt::NoButton;   // the button dragging, if one is
    QPoint m_press;
};

static bool darkUi()
{
    return uiDark();
}

static QCursor rotateCursor()
{
    static QCursor c = [] {
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        for (int pass = 0; pass < 2; ++pass) {
            p.setPen(QPen(pass ? Qt::black : Qt::white, pass ? 1.6 : 4));
            p.drawArc(QRectF(5, 5, 14, 14), 30 * 16, 280 * 16);
        }
        p.setBrush(Qt::black);
        p.drawPolygon(QPolygonF({QPointF(17, 2), QPointF(21, 8), QPointF(14, 8)}));
        return QCursor(pm, 12, 12);
    }();
    return c;
}

static QCursor pitcherCursor()
{
    static QCursor c = [] {
        QPixmap pm(28, 28);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath jug;
        jug.moveTo(6, 8); jug.lineTo(18, 8); jug.lineTo(17, 22); jug.lineTo(7, 22); jug.closeSubpath();
        jug.moveTo(18, 11); jug.cubicTo(24, 11, 24, 18, 17, 18);
        p.setPen(QPen(Qt::white, 3));
        p.drawPath(jug);
        p.setPen(QPen(Qt::black, 1.4));
        p.setBrush(QColor(255, 255, 255, 230));
        p.drawPath(jug);
        p.setPen(QPen(QColor(30, 90, 200), 1.4));
        p.drawLine(QPointF(5, 8), QPointF(1, 4));
        return QCursor(pm, 1, 4);
    }();
    return c;
}

static QCursor painterCursor()
{
    static QCursor c = [] {
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, 1.2));
        p.setBrush(QColor(255, 210, 60));
        p.drawRect(QRectF(8, 2, 13, 7));
        p.setBrush(Qt::black);
        p.drawRect(QRectF(13, 9, 3, 10));
        p.drawLine(QPointF(2, 2), QPointF(2, 12));
        p.drawLine(QPointF(0, 7), QPointF(5, 7));
        return QCursor(pm, 2, 7);
    }();
    return c;
}

// ======================= Canvas =======================
Canvas::Canvas(Editor *ed, QWidget *parent) : QAbstractScrollArea(parent), m_ed(ed)
{
    setFrameShape(QFrame::NoFrame);
    // Scroll bars always show, so the page area's size doesn't depend on the
    // zoom: with bars that come and go, fitting the page near the size where
    // one is just needed brought the bar in, which shrank the area, refitted
    // the page smaller, took the bar away again, and so on endlessly (a
    // whole processor busy while JeffPub sat idle).
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    installCanvasAccessibility();
    setAccessibleName(QCoreApplication::translate("Canvas", "Page"));
    setAccessibleDescription(QCoreApplication::translate("Canvas", "Tab selects the next object, the arrow keys move it, Enter edits its text, and Delete removes it. F6 goes to the ribbon."));
    setAttribute(Qt::WA_InputMethodEnabled);
    setAcceptDrops(true);
    m_hRuler = new Ruler(this, Qt::Horizontal);
    m_vRuler = new Ruler(this, Qt::Vertical);
    m_corner = new RulerCorner(this);
    setViewportMargins(kRuler, kRuler, 0, 0);
    m_caretTimer.setInterval(QApplication::cursorFlashTime() / 2 > 0 ? QApplication::cursorFlashTime() / 2 : 530);
    connect(&m_caretTimer, &QTimer::timeout, this, [this] {
        m_caretOn = !m_caretOn;
        viewport()->update(caretViewRect().toAlignedRect().adjusted(-2, -2, 2, 2));
    });
    auto refresh = [this] {
        updateScrollBars();
        viewport()->update();
        m_hRuler->update();
        m_vRuler->update();
    };
    connect(ed, &Editor::changed, this, refresh);
    connect(ed, &Editor::selectionChanged, this, refresh);
    // Screen readers follow the selection, and the text while typing.
    connect(ed, &Editor::selectionChanged, this, &Canvas::announceFocus);
    connect(ed, &Editor::textCursorChanged, this, [this] { announceText(false); });
    connect(ed, &Editor::changed, this, [this] { announceText(true); });
    connect(ed, &Editor::viewChanged, this, refresh);
    connect(ed, &Editor::toolChanged, this, [this] { updateCursorShape(mapFromGlobal(QCursor::pos())); });
    connect(ed, &Editor::pageChanged, this, [this] {
        updateScrollBars();
        if (m_fit != Fit::None) zoomToFit(m_fit); else scrollToPage();
        viewport()->update();
        m_hRuler->update();
        m_vRuler->update();
    });
    connect(ed, &Editor::documentReplaced, this, [this] {
        m_fit = Fit::WholePage;
        QTimer::singleShot(0, this, [this] { zoomToFit(Fit::WholePage); });
    });
    connect(ed, &Editor::textCursorChanged, this, [this] {
        m_caretOn = true;
        if (m_ed->isEditingText()) {
            m_caretTimer.start();
            // Follow the caret into another linked text box (maybe on another page).
            QLineF line;
            QString frame;
            if (caretInfo(&line, &frame) && !frame.isEmpty() && frame != m_ed->textTarget().itemId) {
                const auto loc = m_ed->doc()->find(frame);
                m_ed->retargetText(frame);
                if (loc.page >= 0 && loc.page != m_ed->currentPage() && m_ed->masterView().isEmpty()) m_ed->setCurrentPage(loc.page, true);
            }
            const QRectF cr = caretViewRect();
            if (cr.isValid() && !viewport()->rect().contains(cr.toRect())) {
                const QPointF c = cr.center();
                if (c.y() < 0) verticalScrollBar()->setValue(verticalScrollBar()->value() + int(c.y()) - 40);
                if (c.y() > viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value() + int(c.y() - viewport()->height()) + 40);
            }
        } else {
            m_caretTimer.stop();
        }
        viewport()->update();
        m_hRuler->update();
    });
}

double Canvas::ppp() const { return m_zoom * logicalDpiX() / 72.0; }

QVector<Canvas::Slot> Canvas::slots() const
{
    const Document *d = m_ed->doc();
    const QSizeF ps = d->pageSize();
    if (!m_ed->masterView().isEmpty()) return {Slot{-1, QPointF(0, 0), m_ed->surfaceSize()}};
    const QVector<int> pages = m_ed->visiblePages();
    QVector<Slot> out;
    if (pages.size() == 2) {
        out << Slot{pages[0], QPointF(0, 0), ps} << Slot{pages[1], QPointF(ps.width(), 0), ps};
    } else if (!pages.isEmpty()) {
        // In spread view the first page sits on the right like a book cover.
        const bool right = m_ed->twoPageSpread() && pages[0] == 0 && d->pages.size() > 1;
        out << Slot{pages[0], QPointF(right ? ps.width() : 0, 0), ps};
    }
    return out;
}

QPointF Canvas::currentOrigin() const
{
    for (const Slot &s : slots())
        if (s.page == m_ed->currentPage() || s.page < 0) return s.origin;
    return QPointF(0, 0);
}

QRectF Canvas::sceneRect() const
{
    QRectF r;
    for (const Slot &s : slots()) r = r.isNull() ? QRectF(s.origin, s.size) : r.united(QRectF(s.origin, s.size));
    if (m_ed->twoPageSpread() && m_ed->masterView().isEmpty() && m_ed->doc()->pages.size() > 1) {
        const QSizeF ps = m_ed->doc()->pageSize();
        r = r.united(QRectF(0, 0, ps.width() * 2, ps.height()));
    }
    const double m = std::max(144.0, 0.6 * std::max(r.width(), r.height()));
    r.adjust(-m, -m * 0.6, m, m * 0.6);
    if (m_ed->view.scratch) {
        const QRectF sb = unionBounds(m_ed->doc()->scratch).translated(currentOrigin());
        if (!sb.isNull()) r = r.united(sb.adjusted(-72, -72, 72, 72));
    }
    return r;
}

QPointF Canvas::toView(const QPointF &scene) const
{
    const QRectF sr = sceneRect();
    const double s = ppp();
    const QSize vp = viewport()->size();
    const double cw = sr.width() * s, ch = sr.height() * s;
    const double ox = cw < vp.width() ? (vp.width() - cw) / 2 : -horizontalScrollBar()->value();
    const double oy = ch < vp.height() ? (vp.height() - ch) / 2 : -verticalScrollBar()->value();
    return QPointF(ox + (scene.x() - sr.left()) * s, oy + (scene.y() - sr.top()) * s);
}

QPointF Canvas::toScene(const QPointF &view) const
{
    const QRectF sr = sceneRect();
    const double s = ppp();
    const QSize vp = viewport()->size();
    const double cw = sr.width() * s, ch = sr.height() * s;
    const double ox = cw < vp.width() ? (vp.width() - cw) / 2 : -horizontalScrollBar()->value();
    const double oy = ch < vp.height() ? (vp.height() - ch) / 2 : -verticalScrollBar()->value();
    return QPointF((view.x() - ox) / s + sr.left(), (view.y() - oy) / s + sr.top());
}

void Canvas::updateScrollBars()
{
    const QRectF sr = sceneRect();
    const double s = ppp();
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, int(sr.width() * s) - vp.width()));
    verticalScrollBar()->setRange(0, std::max(0, int(sr.height() * s) - vp.height()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setPageStep(vp.height());
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setSingleStep(40);
}

void Canvas::setZoom(double z, const QPointF &anchorView)
{
    z = std::clamp(z, 0.1, 40.0);
    const QPointF anchor = anchorView.x() < 0 ? QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0) : anchorView;
    const QPointF sceneAt = toScene(anchor);
    m_zoom = z;
    updateScrollBars();
    const QPointF now = toView(sceneAt);
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(std::lround(now.x() - anchor.x())));
    verticalScrollBar()->setValue(verticalScrollBar()->value() + int(std::lround(now.y() - anchor.y())));
    viewport()->update();
    m_hRuler->update();
    m_vRuler->update();
    Q_EMIT zoomChanged(m_zoom);
}

void Canvas::zoomToFit(Fit f)
{
    m_fit = f;
    if (f == Fit::None) return;
    QRectF target;
    if (f == Fit::Selection && !m_ed->selection().isEmpty()) {
        target = m_ed->selectionBounds().translated(currentOrigin());
    } else {
        for (const Slot &s : slots()) target = target.isNull() ? QRectF(s.origin, s.size) : target.united(QRectF(s.origin, s.size));
    }
    if (target.isEmpty()) return;
    const QSize vp = viewport()->size();
    const double dpi = logicalDpiX() / 72.0;
    double z;
    if (f == Fit::PageWidth) z = (vp.width() - 40) / (target.width() * dpi);
    else z = std::min((vp.width() - 40) / (target.width() * dpi), (vp.height() - 40) / (target.height() * dpi));
    m_zoom = std::clamp(z, 0.1, 40.0);
    updateScrollBars();
    const QPointF c = toView(target.center());
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(c.x() - vp.width() / 2.0));
    verticalScrollBar()->setValue(horizontalScrollBar()->maximum() >= 0 && f == Fit::PageWidth
                                      ? verticalScrollBar()->value() + int(toView(target.topLeft()).y() - 20)
                                      : verticalScrollBar()->value() + int(c.y() - vp.height() / 2.0));
    if (f == Fit::Selection) m_fit = Fit::None;
    viewport()->update();
    m_hRuler->update();
    m_vRuler->update();
    Q_EMIT zoomChanged(m_zoom);
}

void Canvas::scrollToPage()
{
    const auto sl = slots();
    if (sl.isEmpty()) return;
    QRectF t;
    for (const Slot &s : sl) t = t.isNull() ? QRectF(s.origin, s.size) : t.united(QRectF(s.origin, s.size));
    const QPointF c = toView(t.center());
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(c.x() - viewport()->width() / 2.0));
    const QPointF top = toView(t.topLeft());
    verticalScrollBar()->setValue(verticalScrollBar()->value() + int(top.y() - 20));
}

void Canvas::ensureVisible(const QRectF &pageRect)
{
    const QRectF v(pageToView(pageRect.topLeft()), pageToView(pageRect.bottomRight()));
    const QRect vp = viewport()->rect().adjusted(20, 20, -20, -20);
    if (v.left() < vp.left()) horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(v.left() - vp.left()));
    else if (v.right() > vp.right()) horizontalScrollBar()->setValue(horizontalScrollBar()->value() + int(v.right() - vp.right()));
    if (v.top() < vp.top()) verticalScrollBar()->setValue(verticalScrollBar()->value() + int(v.top() - vp.top()));
    else if (v.bottom() > vp.bottom()) verticalScrollBar()->setValue(verticalScrollBar()->value() + int(v.bottom() - vp.bottom()));
}

void Canvas::setRulersVisible(bool on)
{
    m_hRuler->setVisible(on);
    m_vRuler->setVisible(on);
    m_corner->setVisible(on);
    setViewportMargins(on ? kRuler : 0, on ? kRuler : 0, 0, 0);
}

void Canvas::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    const QRect vg = viewport()->geometry();
    m_hRuler->setGeometry(vg.left(), vg.top() - kRuler, vg.width(), kRuler);
    m_vRuler->setGeometry(vg.left() - kRuler, vg.top(), kRuler, vg.height());
    m_corner->setGeometry(vg.left() - kRuler, vg.top() - kRuler, kRuler, kRuler);
    updateScrollBars();
    if (m_fit != Fit::None) zoomToFit(m_fit);
}

void Canvas::scrollContentsBy(int, int)
{
    viewport()->update();
    m_hRuler->update();
    m_vRuler->update();
}

PaintContext Canvas::paintContext() const
{
    PaintContext ctx;
    ctx.doc = m_ed->doc();
    ctx.cache = &m_ed->cache();
    ctx.opt = m_ed->renderOptions();
    ctx.pageNumber = m_ed->surfacePageNumber();
    ctx.pageCount = m_ed->doc()->pages.size();
    return ctx;
}

// ---------------- painting ----------------
void Canvas::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    const bool dark = darkUi();
    p.fillRect(viewport()->rect(), uiHighContrast() ? palette().color(QPalette::Window) : dark ? QColor(0x2B, 0x30, 0x38) : QColor(0xC9, 0xCE, 0xD6));
    const QString master = m_ed->masterView();
    for (const Slot &s : slots()) paintPageSlot(p, s, s.page == m_ed->currentPage() || s.page < 0);
    // Scratch area items, shared by every page.
    if (m_ed->view.scratch && master.isEmpty() && !m_ed->doc()->scratch.empty()) {
        p.save();
        p.translate(toView(currentOrigin()));
        p.scale(ppp(), ppp());
        PaintContext ctx = paintContext();
        Renderer::paintItems(&p, ctx, m_ed->doc()->scratch);
        p.restore();
    }
    paintOverlay(p);
}

void Canvas::paintPageSlot(QPainter &p, const Slot &s, bool current)
{
    const QRectF vr(toView(s.origin), toView(s.origin + QPointF(s.size.width(), s.size.height())));
    p.fillRect(vr.translated(3, 3), QColor(0, 0, 0, 60));
    p.fillRect(vr, Qt::white);
    p.save();
    p.translate(toView(s.origin));
    p.scale(ppp(), ppp());
    PaintContext ctx = paintContext();
    const Document *d = m_ed->doc();
    if (s.page < 0) {
        if (MasterPage *m = d->master(m_ed->masterView())) {
            Renderer::paintBackground(&p, ctx, m->background, QRectF(QPointF(0, 0), s.size));
            Renderer::paintItems(&p, ctx, m->items);
        }
    } else {
        if (!current) ctx.opt.editStory.clear();
        Renderer::paintPage(&p, ctx, s.page);
    }
    if (m_ed->view.guides) paintGuides(p, s);
    if (s.page >= 0) paintCatalogArea(p, s);
    p.restore();
    if (s.page >= 0 && !current) return;
}

// The catalog merge area: its outline and cells, never printed.
void Canvas::paintCatalogArea(QPainter &p, const Slot &s)
{
    const Document *d = m_ed->doc();
    const CatalogArea &cat = d->catalog;
    if (!cat.isActive() || s.page >= d->pages.size() || d->pages[s.page]->id != cat.pageId) return;
    p.save();
    QPen edge(QColor(230, 120, 20), 0, Qt::DashLine);
    p.setPen(edge);
    p.setBrush(Qt::NoBrush);
    p.drawRect(cat.rect);
    QPen inner(QColor(230, 120, 20, 150), 0, Qt::DotLine);
    p.setPen(inner);
    for (int c = 1; c < cat.cols; ++c) {
        const double x = cat.cell(c).left();
        p.drawLine(QPointF(x, cat.rect.top()), QPointF(x, cat.rect.bottom()));
    }
    for (int r = 1; r < cat.rows; ++r) {
        const double y = cat.cell(r * cat.cols).top();
        p.drawLine(QPointF(cat.rect.left(), y), QPointF(cat.rect.right(), y));
    }
    // A tag above the area, the same size at any zoom.
    QFont f = font();
    f.setPointSizeF(8.0 / std::max(0.05, ppp()));
    p.setFont(f);
    const QString tag = QCoreApplication::translate("Canvas", "Catalog area: %1 per page").arg(cat.perPage());
    const QFontMetricsF fm(f);
    const QRectF box(cat.rect.left(), cat.rect.top() - fm.height() * 1.3, fm.horizontalAdvance(tag) + fm.height(), fm.height() * 1.3);
    p.fillRect(box, QColor(230, 120, 20));
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, tag);
    p.restore();
}

void Canvas::paintGuides(QPainter &p, const Slot &s)
{
    const Document *d = m_ed->doc();
    const QSizeF ps = d->pageSize();
    const MasterPage *m = s.page < 0 ? d->master(m_ed->masterView()) : d->masterFor(*d->pages[s.page]);
    const int halves = (s.page < 0 && m && m->twoPage) ? 2 : 1;
    for (int half = 0; half < halves; ++half) {
        const double x0 = half * ps.width();
        const QRectF content = QRectF(x0, 0, ps.width(), ps.height()).marginsRemoved(d->setup.margins);
        QPen margin(QColor(232, 96, 196), 0);
        p.setPen(margin);
        p.setBrush(Qt::NoBrush);
        p.drawRect(content);
        if (m) {
            const GridGuides &g = m->grid;
            QPen grid(QColor(64, 128, 230), 0);
            p.setPen(grid);
            if (g.cols > 1) {
                const double cw = (content.width() - g.colGap * (g.cols - 1)) / g.cols;
                for (int c = 1; c < g.cols; ++c) {
                    const double xr = content.left() + c * cw + (c - 1) * g.colGap;
                    p.drawLine(QPointF(xr, content.top()), QPointF(xr, content.bottom()));
                    p.drawLine(QPointF(xr + g.colGap, content.top()), QPointF(xr + g.colGap, content.bottom()));
                }
            }
            if (g.rows > 1) {
                const double rh = (content.height() - g.rowGap * (g.rows - 1)) / g.rows;
                for (int r = 1; r < g.rows; ++r) {
                    const double yb = content.top() + r * rh + (r - 1) * g.rowGap;
                    p.drawLine(QPointF(content.left(), yb), QPointF(content.right(), yb));
                    p.drawLine(QPointF(content.left(), yb + g.rowGap), QPointF(content.right(), yb + g.rowGap));
                }
            }
            if (g.centerGuide) {
                p.drawLine(QPointF(content.center().x(), content.top()), QPointF(content.center().x(), content.bottom()));
                p.drawLine(QPointF(content.left(), content.center().y()), QPointF(content.right(), content.center().y()));
            }
            if (m_ed->view.baselines && g.baseline > 1) {
                QPen bl(QColor(190, 140, 90, 150), 0);
                bl.setStyle(Qt::DotLine);
                p.setPen(bl);
                for (double y = content.top() + g.baselineOffset; y <= content.bottom(); y += g.baseline)
                    p.drawLine(QPointF(content.left(), y), QPointF(content.right(), y));
            }
        }
    }
    // Ruler guides: master guides and page guides.
    QPen rg(QColor(30, 170, 90), 0);
    auto drawRulerGuides = [&](const RulerGuides &g) {
        for (double y : g.h) p.drawLine(QPointF(-ps.width() * 2, y), QPointF(ps.width() * 3, y));
        for (double x : g.v) p.drawLine(QPointF(x, -ps.height() * 2), QPointF(x, ps.height() * 3));
    };
    p.setPen(rg);
    if (m) drawRulerGuides(m->guides);
    if (s.page >= 0) drawRulerGuides(d->pages[s.page]->guides);
}

// The drawing tools' paths: a curve passes smoothly through its points; a
// freeform or scribble joins them with straight lines, thinned where a
// hand-drawn stroke sampled more points than it needs.
static void thinPoints(const QVector<QPointF> &in, int a, int b, double tol, QVector<bool> &keep)
{
    if (b <= a + 1) return;
    const QLineF chord(in[a], in[b]);
    double far = -1;
    int at = -1;
    for (int i = a + 1; i < b; ++i) {
        const QPointF d = in[i] - in[a], u = chord.length() > 1e-9 ? (in[b] - in[a]) / chord.length() : QPointF(0, 0);
        const double dist = chord.length() > 1e-9 ? std::abs(d.x() * u.y() - d.y() * u.x()) : std::hypot(d.x(), d.y());
        if (dist > far) { far = dist; at = i; }
    }
    if (far <= tol) return;
    keep[at] = true;
    thinPoints(in, a, at, tol, keep);
    thinPoints(in, at, b, tol, keep);
}

static QPainterPath freeformPath(QVector<QPointF> p, const QString &kind, bool closed)
{
    if (kind == QLatin1String("scribble") && p.size() > 2) {
        QVector<bool> keep(p.size(), false);
        keep.first() = keep.last() = true;
        thinPoints(p, 0, int(p.size()) - 1, 0.4, keep);
        QVector<QPointF> t;
        for (int i = 0; i < p.size(); ++i)
            if (keep[i]) t << p[i];
        p = t;
    }
    QPainterPath path(p.first());
    const int n = int(p.size());
    if (kind == QLatin1String("curve") && n > 2) {
        auto at = [&](int i) { return closed ? p[((i % n) + n) % n] : p[std::clamp(i, 0, n - 1)]; };
        for (int i = 0; i < (closed ? n : n - 1); ++i) {
            const QPointF p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
            path.cubicTo(p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2);
        }
    } else {
        for (int i = 1; i < n; ++i) path.lineTo(p[i]);
    }
    if (closed) path.closeSubpath();
    return path;
}

// The yellow handle that moves the middle of an elbow or curved line
// (shown when both ends leave level, or both upright).
static bool lineBendHandle(const LineItem &l, QPointF *at)
{
    if (l.route == LineItem::Straight || l.startVertical != l.endVertical) return false;
    const QVector<QPointF> pts = l.routePoints();
    if (pts.size() != 4) return false;
    *at = (pts[1] + pts[2]) / 2;
    return true;
}

QVector<QPointF> Canvas::handlePoints(const Item *it) const
{
    QVector<QPointF> out;
    if (it->type() == ItemType::Line) {
        const auto *l = static_cast<const LineItem *>(it);
        out << pageToView(l->p1) << pageToView(l->p2);
        return out;
    }
    const double w = it->rect.width(), h = it->rect.height();
    const QTransform t = it->type() == ItemType::Group ? QTransform::fromTranslate(it->bounds().left(), it->bounds().top()) : it->transform();
    const QSizeF sz = it->type() == ItemType::Group ? it->bounds().size() : QSizeF(w, h);
    const QPointF loc[8] = {{0, 0}, {sz.width() / 2, 0}, {sz.width(), 0}, {sz.width(), sz.height() / 2}, {sz.width(), sz.height()},
                            {sz.width() / 2, sz.height()}, {0, sz.height()}, {0, sz.height() / 2}};
    // With flips, handle i still means the same visual position because transform maps them.
    for (const QPointF &l : loc) out << pageToView(t.map(l));
    return out;
}

QPointF Canvas::rotateHandle(const Item *it) const
{
    if (it->type() == ItemType::Group) {
        const QRectF b = it->bounds();
        const QPointF top = pageToView(QPointF(b.center().x(), b.top()));
        return top - QPointF(0, 22);
    }
    const QPointF top = pageToView(it->transform().map(QPointF(it->rect.width() / 2, it->flipV ? it->rect.height() : 0)));
    const QPointF ctr = pageToView(it->rect.center());
    QPointF d = top - ctr;
    const double L = std::hypot(d.x(), d.y());
    if (L < 0.01) return top - QPointF(0, 22);
    return top + d / L * 22;
}

static void drawHandle(QPainter &p, const QPointF &c, bool round = true)
{
    p.setPen(QPen(QColor(90, 90, 90), 1));
    p.setBrush(Qt::white);
    if (round) p.drawEllipse(c, kHandle, kHandle);
    else p.drawRect(QRectF(c.x() - kHandle, c.y() - kHandle, kHandle * 2, kHandle * 2));
}


// ---------------- Edit Points ----------------
// A shape's outline as a flat list of path elements. Vertices are move-to,
// line-to and curve end points; a subpath's closing line back to its start is
// an alias of the start, not a point of its own.
namespace pts {
struct El { QPainterPath::ElementType t; QPointF p; };

static QVector<El> els(const QPainterPath &path)
{
    QVector<El> v;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        v << El{e.type, QPointF(e.x, e.y)};
    }
    return v;
}

static QPainterPath build(const QVector<El> &v)
{
    QPainterPath p;
    p.setFillRule(Qt::WindingFill);
    for (int i = 0; i < v.size(); ++i) {
        switch (v[i].t) {
        case QPainterPath::MoveToElement: p.moveTo(v[i].p); break;
        case QPainterPath::LineToElement: p.lineTo(v[i].p); break;
        case QPainterPath::CurveToElement:
            if (i + 2 < v.size()) { p.cubicTo(v[i].p, v[i + 1].p, v[i + 2].p); i += 2; }
            break;
        default: break;
        }
    }
    return p;
}

static int subpathStart(const QVector<El> &v, int i)
{
    while (i > 0 && v[i].t != QPainterPath::MoveToElement) --i;
    return i;
}

static int subpathEnd(const QVector<El> &v, int i)   // last element index of the subpath holding i
{
    int j = i + 1;
    while (j < v.size() && v[j].t != QPainterPath::MoveToElement) ++j;
    return j - 1;
}

static bool isVertex(const QVector<El> &v, int i)
{
    const auto t = v[i].t;
    if (t == QPainterPath::MoveToElement) return true;
    if (t == QPainterPath::LineToElement || (t == QPainterPath::CurveToDataElement && (i + 1 >= v.size() || v[i + 1].t != QPainterPath::CurveToDataElement))) {
        // The closing point back onto the subpath's start is not a separate vertex.
        const int s = subpathStart(v, i);
        return !(i == subpathEnd(v, i) && QLineF(v[i].p, v[s].p).length() < 0.01);
    }
    return false;
}

static QVector<int> vertices(const QPainterPath &path)
{
    const auto v = els(path);
    QVector<int> out;
    for (int i = 0; i < v.size(); ++i)
        if (isVertex(v, i)) out << i;
    return out;
}

// Moves vertex i by delta; curve handles next to it move along so curves keep their shape.
static QPainterPath moved(const QPainterPath &path, int i, const QPointF &delta)
{
    auto v = els(path);
    if (i < 0 || i >= v.size()) return path;
    auto shift = [&](int k) { if (k >= 0 && k < v.size()) v[k].p += delta; };
    auto withHandles = [&](int k) {
        shift(k);
        if (k + 1 < v.size() && v[k + 1].t == QPainterPath::CurveToElement) shift(k + 1);
        if (v[k].t == QPainterPath::CurveToDataElement) shift(k - 1);
    };
    withHandles(i);
    if (v[i].t == QPainterPath::MoveToElement) {
        const int e = subpathEnd(v, i);
        if (e > i && QLineF(v[e].p - delta, v[i].p - delta).length() < 0.01) withHandles(e);
    }
    return build(v);
}

static int vertexCountInSubpath(const QVector<El> &v, int i)
{
    int n = 0;
    for (int k = subpathStart(v, i); k <= subpathEnd(v, i); ++k)
        if (isVertex(v, k)) ++n;
    return n;
}

// Deletes vertex i (keeping at least three points in its outline).
static QPainterPath removed(const QPainterPath &path, int i)
{
    auto v = els(path);
    if (i < 0 || i >= v.size() || vertexCountInSubpath(v, i) <= 3) return path;
    if (v[i].t == QPainterPath::LineToElement) {
        v.remove(i);
    } else if (v[i].t == QPainterPath::CurveToDataElement) {
        v.remove(i - 2, 3);
    } else if (v[i].t == QPainterPath::MoveToElement) {
        // The next vertex becomes the start; the closing point follows it.
        const int e = subpathEnd(v, i);
        const bool closed = e > i && QLineF(v[e].p, v[i].p).length() < 0.01;
        int next = i + 1;
        if (v[next].t == QPainterPath::CurveToElement) { const QPointF np = v[next + 2].p; v.remove(next, 3); v[i].p = np; }
        else { v[i].p = v[next].p; v.remove(next); }
        if (closed) {
            const int e2 = subpathEnd(v, i);
            v[e2].p = v[i].p;
        }
    }
    return build(v);
}

// Inserts a vertex on the edge nearest to local point q (within tol); returns
// the new vertex's element index through *at.
static QPainterPath inserted(const QPainterPath &path, const QPointF &q, double tol, int *at)
{
    auto v = els(path);
    double best = tol;
    int seg = -1;
    double bestT = 0;
    for (int i = 1; i < v.size(); ++i) {
        if (v[i].t == QPainterPath::LineToElement) {
            const QPointF a = v[i - 1].p, b = v[i].p, d = b - a;
            const double len2 = QPointF::dotProduct(d, d);
            const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(q - a, d) / len2, 0.0, 1.0) : 0;
            const double dist = QLineF(a + d * t, q).length();
            if (dist < best && t > 0.02 && t < 0.98) { best = dist; seg = i; bestT = t; }
        } else if (v[i].t == QPainterPath::CurveToElement && i + 2 < v.size()) {
            const QPointF p0 = v[i - 1].p, p1 = v[i].p, p2 = v[i + 1].p, p3 = v[i + 2].p;
            for (int k = 1; k < 40; ++k) {
                const double t = k / 40.0, u = 1 - t;
                const QPointF pt = p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
                const double dist = QLineF(pt, q).length();
                if (dist < best) { best = dist; seg = i; bestT = t; }
            }
        }
    }
    if (seg < 0) { if (at) *at = -1; return path; }
    if (v[seg].t == QPainterPath::LineToElement) {
        const QPointF a = v[seg - 1].p, b = v[seg].p;
        v.insert(seg, El{QPainterPath::LineToElement, a + (b - a) * bestT});
        if (at) *at = seg;
    } else {
        // Split the cubic at t (de Casteljau) into two curves.
        const QPointF p0 = v[seg - 1].p, p1 = v[seg].p, p2 = v[seg + 1].p, p3 = v[seg + 2].p;
        const double t = bestT;
        const QPointF a = p0 + (p1 - p0) * t, b = p1 + (p2 - p1) * t, c = p2 + (p3 - p2) * t;
        const QPointF d = a + (b - a) * t, e = b + (c - b) * t, m = d + (e - d) * t;
        v[seg].p = a; v[seg + 1].p = d; v[seg + 2].p = m;
        v.insert(seg + 3, El{QPainterPath::CurveToElement, e});
        v.insert(seg + 4, El{QPainterPath::CurveToDataElement, c});
        v.insert(seg + 5, El{QPainterPath::CurveToDataElement, p3});
        if (at) *at = seg + 2;
    }
    return build(v);
}
} // namespace pts

void Canvas::paintHandles(QPainter &p, Item *it)
{
    if (it->type() == ItemType::Line) {
        const auto *l = static_cast<const LineItem *>(it);
        const auto pts = handlePoints(it);
        for (int i = 0; i < pts.size(); ++i) {
            // An end attached to an object shows as a green dot.
            if (!(i == 0 ? l->start : l->end).id.isEmpty()) {
                p.setPen(QPen(QColor(40, 110, 40), 1));
                p.setBrush(QColor(120, 200, 120));
                p.drawEllipse(pts[i], 4.5, 4.5);
            } else drawHandle(p, pts[i]);
        }
        if (QPointF b; lineBendHandle(*l, &b)) {
            const QPointF c = pageToView(b);
            p.setPen(QPen(QColor(120, 90, 0), 1));
            p.setBrush(QColor(255, 210, 40));
            p.drawPolygon(QPolygonF({c + QPointF(0, -5), c + QPointF(5, 0), c + QPointF(0, 5), c + QPointF(-5, 0)}));
        }
        return;
    }
    if (m_ed->wrapItem == it->id) {
        // Edit Wrap Points: the wrap outline, dashed, with a black square on
        // every point.
        const QTransform t = it->transform();
        QPolygonF poly;
        for (const QPointF &pt : it->wrap.points) poly << pageToView(t.map(pt));
        QPolygonF frame;
        for (const QPointF &pt : it->outline()) frame << pageToView(pt);
        p.setPen(QPen(QColor(70, 120, 200, 120), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(frame);
        p.setPen(QPen(QColor(200, 40, 120), 1.2, Qt::DashLine));
        p.drawPolygon(poly);
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(Qt::black);
        for (const QPointF &c : poly) p.drawRect(QRectF(c - QPointF(3.5, 3.5), QSizeF(7, 7)));
        return;
    }
    if (it->type() == ItemType::Shape && m_ed->pointsItem == it->id) {
        // Edit Points: the outline and a square on every vertex.
        auto *s = static_cast<ShapeItem *>(it);
        const QTransform t = it->transform();
        QTransform toView;
        toView.translate(pageToView(QPointF(0, 0)).x(), pageToView(QPointF(0, 0)).y());
        const double k = pageToView(QPointF(1, 0)).x() - pageToView(QPointF(0, 0)).x();
        toView.scale(k, k);
        p.setPen(QPen(QColor(70, 120, 200), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath((t * toView).map(s->customPath));
        for (int idx : pts::vertices(s->customPath)) {
            const auto e = s->customPath.elementAt(idx);
            const QPointF c = pageToView(t.map(QPointF(e.x, e.y)));
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(Qt::white);
            p.drawRect(QRectF(c - QPointF(3.5, 3.5), QSizeF(7, 7)));
        }
        return;
    }
    const bool crop = m_ed->cropItem == it->id;
    QPolygonF outline;
    if (it->type() == ItemType::Group) {
        const QRectF b = it->bounds();
        outline = QPolygonF(QRectF(pageToView(b.topLeft()), pageToView(b.bottomRight())));
    } else {
        for (const QPointF &pt : it->outline()) outline << pageToView(pt);
    }
    QPen sel(QColor(70, 120, 200), 1);
    if (it->type() == ItemType::Group) sel.setStyle(Qt::DashLine);
    p.setPen(sel);
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(outline);
    const auto pts = handlePoints(it);
    if (crop) {
        // Crop handles: black bars and corners.
        auto *pic = static_cast<PictureItem *>(it);
        const QTransform t = it->transform();
        QPolygonF ghost;
        for (const QPointF &c : {pic->imgRect.topLeft(), pic->imgRect.topRight(), pic->imgRect.bottomRight(), pic->imgRect.bottomLeft()})
            ghost << pageToView(t.map(c));
        p.setPen(QPen(QColor(60, 60, 60), 1, Qt::DashLine));
        p.drawPolygon(ghost);
        p.setPen(QPen(Qt::white, 5, Qt::SolidLine, Qt::SquareCap));
        for (int i = 0; i < 8; ++i) {
            const QPointF c = pts[i];
            const QPointF a = pts[(i + 7) % 8], b = pts[(i + 1) % 8];
            const QPointF u = (a - c) / std::max(1.0, std::hypot((a - c).x(), (a - c).y())) * 9;
            const QPointF v = (b - c) / std::max(1.0, std::hypot((b - c).x(), (b - c).y())) * 9;
            for (int pass = 0; pass < 2; ++pass) {
                p.setPen(pass ? QPen(Qt::black, 3, Qt::SolidLine, Qt::SquareCap) : QPen(Qt::white, 6, Qt::SolidLine, Qt::SquareCap));
                if (i % 2 == 0) { p.drawLine(c, c + u); p.drawLine(c, c + v); }
                else { p.drawLine(c - v, c + v); }
            }
        }
        return;
    }
    if (it->locked) {
        for (const QPointF &pt : pts) {
            p.setPen(QPen(QColor(150, 40, 40), 1.5));
            p.drawLine(pt + QPointF(-3, -3), pt + QPointF(3, 3));
            p.drawLine(pt + QPointF(-3, 3), pt + QPointF(3, -3));
        }
        return;
    }
    for (const QPointF &pt : pts) drawHandle(p, pt);
    const QPointF rh = rotateHandle(it);
    p.setPen(QPen(QColor(70, 120, 200), 1));
    p.drawLine(rh, (pts[1]));
    p.setBrush(QColor(120, 200, 120));
    p.setPen(QPen(QColor(40, 110, 40), 1));
    p.drawEllipse(rh, kHandle, kHandle);
    if (it->type() == ItemType::Shape) {
        auto *s = static_cast<ShapeItem *>(it);
        if (const ShapeDef *d = shapeDef(s->shape); d && s->customPath.isEmpty()) {
            const QVector<double> adj = shapeAdj(s->shape, s->adj);
            for (const AdjHandle &h : d->handles) {
                const QPointF c = pageToView(it->transform().map(adjHandlePos(h, it->rect.size(), adj)));
                QPolygonF dia({c + QPointF(0, -5), c + QPointF(5, 0), c + QPointF(0, 5), c + QPointF(-5, 0)});
                p.setPen(QPen(QColor(120, 90, 0), 1));
                p.setBrush(QColor(255, 210, 40));
                p.drawPolygon(dia);
            }
        }
    }
}

bool Canvas::caretInfo(QLineF *pageLine, QString *frameId, int atPos) const
{
    if (!m_ed->isEditingText()) return false;
    const auto &tt = m_ed->textTarget();
    Item *it = m_ed->doc()->item(tt.itemId);
    if (!it) return false;
    const int pos = atPos >= 0 ? atPos : m_ed->cursor().position();
    PaintContext ctx = paintContext();
    QRectF r;
    QTransform t;
    if (it->type() == ItemType::Text) {
        auto *tf = static_cast<TextItem *>(it);
        const auto fl = m_ed->cache().textFrame(*m_ed->doc(), *tf, m_ed->surfacePageNumber(), ctx.opt);
        if (!fl.layout) return false;
        int fi = -1;
        if (!fl.layout->caretRect(pos, &fi, &r) || fi < 0) return false;
        const auto chain = m_ed->doc()->chainOf(tf->id);
        if (fi >= chain.size()) return false;
        TextItem *frame = chain[fi];
        if (frameId) *frameId = frame->id;
        t = frame->transform();
        if (frame->vertical) {
            QTransform v;
            v.translate(frame->rect.width(), 0);
            v.rotate(90);
            t = v * t;
        }
    } else if (it->type() == ItemType::Shape) {
        QPointF o;
        const StoryLayout *lay = Renderer::shapeTextLayout(ctx, *static_cast<ShapeItem *>(it), &o);
        int fi;
        if (!lay || !lay->caretRect(pos, &fi, &r)) return false;
        r.translate(o);
        t = it->transform();
        if (frameId) *frameId = it->id;
    } else if (it->type() == ItemType::Table) {
        QPointF o;
        const auto *tb = static_cast<TableItem *>(it);
        const StoryLayout *lay = Renderer::cellLayout(ctx, *tb, tt.row, tt.col, &o);
        int fi;
        if (!lay || !lay->caretRect(pos, &fi, &r)) return false;
        t = it->transform();
        // Text turned 90 degrees runs down the cell from its top right.
        QTransform cell;
        cell.translate(o.x(), o.y());
        if (tb->cell(tt.row, tt.col).vertical) {
            cell.translate(tb->cellRect(tt.row, tt.col).width(), 0);
            cell.rotate(90);
        }
        t = cell * t;
        if (frameId) *frameId = it->id;
    } else {
        return false;
    }
    *pageLine = QLineF(t.map(QPointF(r.x(), r.top())), t.map(QPointF(r.x(), r.bottom())));
    return true;
}

QRectF Canvas::caretViewRect() const
{
    QLineF l;
    QString frame;
    if (!caretInfo(&l, &frame)) return QRectF();
    const auto loc = m_ed->doc()->find(frame);
    QPointF origin = currentOrigin();
    for (const Slot &s : slots())
        if (s.page == loc.page) origin = s.origin;
    const QPointF a = toView(l.p1() + origin), b = toView(l.p2() + origin);
    return QRectF(a, b).normalized().adjusted(-1, 0, 1, 0);
}

void Canvas::paintOverlay(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing);
    const Document *d = m_ed->doc();
    // Object boundaries.
    if (m_ed->view.boundaries) {
        QPen b(QColor(120, 120, 120, 170), 1, Qt::DotLine);
        p.setPen(b);
        p.setBrush(Qt::NoBrush);
        auto outline = [&](const ItemList &l) {
            walkItems(l, [&](const ItemPtr &it) {
                if (it->type() == ItemType::Group || it->type() == ItemType::Line) return;
                QPolygonF poly;
                for (const QPointF &pt : it->outline()) poly << pageToView(pt);
                p.drawPolygon(poly);
            });
        };
        outline(m_ed->surfaceItems());
        if (m_ed->view.scratch && m_ed->masterView().isEmpty()) outline(d->scratch);
    }
    // Selection.
    const auto sel = m_ed->selectedItems();
    for (Item *it : sel) paintHandles(p, it);
    // Selected table cells, shaded the way selected text is.
    if (const auto block = m_ed->cellBlock(); block.range.valid())
        if (const auto *tb = dynamic_cast<const TableItem *>(d->item(block.itemId))) {
            const CellRange &g = block.range;
            double x = 0, y = 0, w = 0, h = 0;
            for (int c = 0; c < g.c0; ++c) x += tb->colW[c];
            for (int r = 0; r < g.r0; ++r) y += tb->rowH[r];
            for (int c = g.c0; c <= g.c1; ++c) w += tb->colW[c];
            for (int r = g.r0; r <= g.r1; ++r) h += tb->rowH[r];
            QPolygonF shade;
            for (const QPointF &pt : tb->transform().map(QPolygonF(QRectF(x, y, w, h)))) shade << pageToView(pt);
            p.setPen(Qt::NoPen);
            p.setBrush(PaintOptions().selColor);
            p.drawPolygon(shade);
        }
    if (sel.size() > 1) {
        const QRectF b = m_ed->selectionBounds();
        p.setPen(QPen(QColor(70, 120, 200), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(pageToView(b.topLeft()), pageToView(b.bottomRight())));
    }
    // Text box link indicators and overflow.
    for (Item *it : sel) {
        if (it->type() != ItemType::Text) continue;
        auto *t = static_cast<TextItem *>(it);
        const auto chain = d->chainOf(t->id);
        const QTransform tr = t->transform();
        auto box = [&](const QPointF &c, const QString &label, const QColor &bg) {
            const QRectF r(c.x() - 10, c.y() - 8, 20, 16);
            p.setPen(QPen(QColor(70, 120, 200), 1));
            p.setBrush(bg);
            p.drawRoundedRect(r, 3, 3);
            p.setPen(bg == Qt::white ? QColor(40, 80, 160) : Qt::white);
            QFont f = font();
            f.setPointSizeF(7.5);
            f.setBold(true);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, label);
        };
        if (d->prevFrame(t->id)) box(pageToView(tr.map(QPointF(0, 0))) + QPointF(4, -12), QStringLiteral("◀"), Qt::white);
        if (!t->nextId.isEmpty()) box(pageToView(tr.map(QPointF(t->rect.width(), t->rect.height()))) + QPointF(-4, 12), QStringLiteral("▶"), Qt::white);
        else if (chain.size() > 0 && chain.last() == t) {
            const auto fl = m_ed->cache().textFrame(*d, *t, m_ed->surfacePageNumber(), m_ed->renderOptions());
            if (fl.layout && fl.layout->overflow())
                box(pageToView(tr.map(QPointF(t->rect.width(), t->rect.height()))) + QPointF(-4, 12), QStringLiteral("A…"), QColor(210, 60, 40));
        }
    }
    // Where dragged text would drop.
    if (m_drag == Drag::TextMove && m_movePos >= 0 && !(m_movePos >= m_moveFrom && m_movePos <= m_moveTo)) {
        QLineF l;
        if (caretInfo(&l, nullptr, m_movePos)) {
            QPen pen(QColor(30, 30, 30), std::max(1.5, ppp()), Qt::DotLine);
            p.setPen(pen);
            p.drawLine(pageToView(l.p1()), pageToView(l.p2()));
        }
    }
    // Caret.
    if (m_ed->isEditingText() && m_caretOn && hasFocus()) {
        const QRectF cr = caretViewRect();
        if (cr.isValid()) {
            p.setPen(QPen(darkUi() ? Qt::black : Qt::black, std::max(1.0, ppp() * 0.6)));
            p.drawLine(QPointF(cr.center().x(), cr.top()), QPointF(cr.center().x(), cr.bottom()));
        }
    }
    // The curve, freeform or scribble being drawn, up to the pointer.
    if (m_ed->tool() == Tool::Freeform && !m_freePts.isEmpty()) {
        QVector<QPointF> pts = m_freePts;
        if (m_ed->toolShape() != QLatin1String("scribble") && m_drag != Drag::Free) pts << m_lastPage;
        else if (m_ed->toolShape() == QLatin1String("curve")) pts << m_lastPage;
        QPainterPath vp;
        if (pts.size() >= 2) {
            const QPainterPath pp = freeformPath(pts, m_ed->toolShape(), false);
            QTransform tf;
            tf.translate(pageToView(QPointF(0, 0)).x(), pageToView(QPointF(0, 0)).y());
            tf.scale(ppp(), ppp());
            vp = tf.map(pp);
        }
        p.setPen(QPen(QColor(30, 30, 30), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(vp);
        p.setPen(QPen(QColor(70, 120, 200), 1));
        p.setBrush(Qt::white);
        if (m_ed->toolShape() != QLatin1String("scribble")) p.drawRect(QRectF(pageToView(m_freePts.first()) - QPointF(3, 3), QSizeF(6, 6)));
    }
    // Connection sites of the object a line is being drawn or dragged to.
    if (!m_siteHover.over.isEmpty() && (m_drag == Drag::None || m_drag == Drag::Draw || m_drag == Drag::LineEnd))
        if (const Item *o = d->item(m_siteHover.over)) {
            for (int i = 0; i < kConnectionSites; ++i) {
                const QPointF c = pageToView(connectionSite(*o, i));
                const bool on = m_siteHover.id == o->id && m_siteHover.site == i;
                p.setPen(QPen(on ? QColor(200, 40, 40) : QColor(70, 120, 200), 1));
                p.setBrush(on ? QColor(240, 90, 90) : QColor(255, 255, 255));
                p.drawRect(QRectF(c - QPointF(3.5, 3.5), QSizeF(7, 7)));
            }
        }
    // Rubber band / draw preview.
    if (!m_rubber.isNull()) {
        const QRectF r(pageToView(m_rubber.topLeft()), pageToView(m_rubber.bottomRight()));
        if (m_drag == Drag::Marquee) {
            p.setPen(QPen(QColor(70, 120, 200), 1));
            p.setBrush(QColor(70, 120, 200, 30));
            p.drawRect(r);
        } else if (m_drag == Drag::Draw) {
            p.setPen(QPen(QColor(30, 30, 30), 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            const Tool t = m_ed->tool();
            if (t == Tool::Line || t == Tool::Arrow || t == Tool::DoubleArrow) {
                LineItem l;
                l.p1 = m_pressPage;
                l.p2 = m_lastPage;
                l.route = m_ed->toolShape() == QLatin1String("elbow") ? LineItem::Elbow : m_ed->toolShape() == QLatin1String("curved") ? LineItem::Curved : LineItem::Straight;
                if (const Item *o = m_siteStart.id.isEmpty() ? nullptr : m_ed->doc()->item(m_siteStart.id)) connectionSite(*o, m_siteStart.site, &l.startVertical);
                if (const Item *o = m_siteHover.id.isEmpty() ? nullptr : m_ed->doc()->item(m_siteHover.id)) connectionSite(*o, m_siteHover.site, &l.endVertical);
                QPainterPath vp;
                const QVector<QPointF> rp = l.routePoints();
                vp.moveTo(pageToView(rp.first()));
                if (l.route == LineItem::Curved && rp.size() == 4) vp.cubicTo(pageToView(rp[1]), pageToView(rp[2]), pageToView(rp[3]));
                else for (int i = 1; i < rp.size(); ++i) vp.lineTo(pageToView(rp[i]));
                p.drawPath(vp);
            }
            else if (t == Tool::Shape) {
                QPainterPath sp = shapePath(m_ed->toolShape(), m_rubber.size());
                QTransform tf;
                tf.translate(r.left(), r.top());
                tf.scale(ppp(), ppp());
                p.drawPath(tf.map(sp));
            } else p.drawRect(r);
        }
    }
    // Snap lines.
    p.setPen(QPen(QColor(230, 40, 60), 1));
    for (const QLineF &l : m_snapLines) p.drawLine(pageToView(l.p1()), pageToView(l.p2()));
    // Guide being dragged.
    if (m_drag == Drag::Guide) {
        p.setPen(QPen(QColor(30, 170, 90), 1, Qt::DashLine));
        if (m_guideOrient == Qt::Horizontal) p.drawLine(QPointF(0, pageToView(QPointF(0, m_guidePos)).y()), QPointF(width(), pageToView(QPointF(0, m_guidePos)).y()));
        else p.drawLine(QPointF(pageToView(QPointF(m_guidePos, 0)).x(), 0), QPointF(pageToView(QPointF(m_guidePos, 0)).x(), height()));
    }
    if (!m_tip.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(8);
        p.setFont(f);
        const QRectF r = QFontMetricsF(f).boundingRect(m_tip).adjusted(-5, -3, 5, 3).translated(m_tipPos + QPointF(16, 26));
        p.setPen(QColor(90, 90, 90));
        p.setBrush(QColor(255, 255, 240));
        p.drawRoundedRect(r, 3, 3);
        p.setPen(Qt::black);
        p.drawText(r, Qt::AlignCenter, m_tip);
    }
    // Master page banner.
    if (!m_ed->masterView().isEmpty()) {
        const MasterPage *m = d->master(m_ed->masterView());
        const QString label = QCoreApplication::translate("Canvas", "Editing %1").arg(m ? m->name : QString());
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        const QRectF r(8, 8, QFontMetricsF(f).horizontalAdvance(label) + 20, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(184, 73, 47, 230));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, label);
    }
}

// ---------------- hit testing ----------------
static double distToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF d = b - a;
    const double L2 = d.x() * d.x() + d.y() * d.y();
    double t = L2 > 0 ? ((p - a).x() * d.x() + (p - a).y() * d.y()) / L2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + d * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

// A barcode (Insert > Barcode) is a group whose parts never come apart:
// clicks don't reach inside it, so it moves and edits as one.
static bool isBarcode(const Item *it)
{
    return it && it->type() == ItemType::Group && !static_cast<const GroupItem *>(it)->barcode.isEmpty();
}

QString Canvas::itemAt(const QPointF &page, bool enterGroups, int *row, int *col, bool *textInterior) const
{
    const double tol = 4.0 / ppp();
    const Document *d = m_ed->doc();
    std::function<bool(const Item *)> hits = [&](const Item *it) -> bool {
        if (it->type() == ItemType::Group) {
            for (auto c = static_cast<const GroupItem *>(it)->children.rbegin(); c != static_cast<const GroupItem *>(it)->children.rend(); ++c)
                if (hits(c->get())) return true;
            return false;
        }
        if (it->type() == ItemType::Line) {
            const auto *l = static_cast<const LineItem *>(it);
            const double w = std::max(tol, l->stroke.width / 2 + tol / 2);
            if (l->route == LineItem::Straight) return distToSegment(page, l->p1, l->p2) <= w;
            QPainterPathStroker st;
            st.setWidth(2 * w);
            return st.createStroke(l->path()).contains(page);
        }
        const QPointF local = it->transform().inverted().map(page);
        const QRectF r(QPointF(0, 0), it->rect.size());
        if (!r.adjusted(-tol, -tol, tol, tol).contains(local)) return false;
        if (it->type() == ItemType::Shape) {
            const auto *s = static_cast<const ShapeItem *>(it);
            const QPainterPath path = Renderer::silhouette(*d, *it);
            if (path.contains(local)) return true;
            QPainterPathStroker st;
            st.setWidth(tol * 2 + s->stroke.width);
            return st.createStroke(path).contains(local) || (!s->storyId.isEmpty() && r.contains(local) && false);
        }
        if (it->type() == ItemType::TextArt) {
            // TextArt is easy to grab anywhere in its frame.
            return r.adjusted(-tol, -tol, tol, tol).contains(local);
        }
        return true;
    };
    auto interior = [&](const Item *it) {
        const QPointF local = it->transform().inverted().map(page);
        const double e = std::max(3.0 / ppp(), std::min({4.5 / ppp(), it->rect.width() / 4, it->rect.height() / 4}));
        return QRectF(QPointF(0, 0), it->rect.size()).adjusted(e, e, -e, -e).contains(local);
    };
    auto scan = [&](const ItemList &list) -> QString {
        for (auto i = list.rbegin(); i != list.rend(); ++i) {
            const Item *it = i->get();
            if (!hits(it)) continue;
            const Item *target = it;
            if (it->type() == ItemType::Group && enterGroups && !isBarcode(it)) {
                std::function<const Item *(const Item *)> deepest = [&](const Item *g) -> const Item * {
                    for (auto c = static_cast<const GroupItem *>(g)->children.rbegin(); c != static_cast<const GroupItem *>(g)->children.rend(); ++c)
                        if (hits(c->get())) return (*c)->type() == ItemType::Group ? deepest(c->get()) : c->get();
                    return g;
                };
                target = deepest(it);
            }
            if (textInterior) *textInterior = (target->type() == ItemType::Text || target->type() == ItemType::Table) && interior(target);
            if (target->type() == ItemType::Table && (row || col)) {
                const auto *t = static_cast<const TableItem *>(target);
                int rr = 0, cc = 0;
                t->cellAt(t->transform().inverted().map(page), &rr, &cc);
                if (row) *row = rr;
                if (col) *col = cc;
            }
            return target->id;
        }
        return QString();
    };
    if (m_ed->view.scratch && m_ed->masterView().isEmpty()) {
        const QString s = scan(d->scratch);
        if (!s.isEmpty()) return s;
    }
    return scan(m_ed->surfaceItems());
}

Canvas::Hit Canvas::hitTest(const QPointF &view) const
{
    Hit h;
    const QPointF page = toPage(view);
    const auto sel = m_ed->selectedItems();
    auto near = [&](const QPointF &a) { return std::hypot(a.x() - view.x(), a.y() - view.y()) <= kHandle + 3; };
    if (sel.size() == 1 && sel.first()->id == m_ed->wrapItem && !sel.first()->wrap.points.isEmpty()) {
        Item *it = sel.first();
        const QTransform t = it->transform();
        const QPolygonF &w = it->wrap.points;
        for (int i = 0; i < w.size(); ++i)
            if (near(pageToView(t.map(w[i])))) return Hit{HitKind::WrapPoint, it->id, i};
        // Dragging an edge adds a point there.
        for (int i = 0; i < w.size(); ++i) {
            const QPointF a = pageToView(t.map(w[i])), b = pageToView(t.map(w[(i + 1) % w.size()]));
            const QPointF d = b - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            if (len2 < 1) continue;
            const double u = std::clamp(((view.x() - a.x()) * d.x() + (view.y() - a.y()) * d.y()) / len2, 0.0, 1.0);
            const QPointF c = a + d * u;
            if (std::hypot(c.x() - view.x(), c.y() - view.y()) <= 4) return Hit{HitKind::WrapEdge, it->id, i};
        }
    }
    if (sel.size() == 1 && sel.first()->id == m_ed->pointsItem && sel.first()->type() == ItemType::Shape) {
        auto *s = static_cast<ShapeItem *>(sel.first());
        const QTransform t = s->transform();
        for (int idx : pts::vertices(s->customPath)) {
            const auto e = s->customPath.elementAt(idx);
            if (near(pageToView(t.map(QPointF(e.x, e.y))))) return Hit{HitKind::Point, s->id, idx};
        }
        // Ctrl over an edge adds a point there.
        if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
            int at = -1;
            const double tol = 5.0 / ppp();
            pts::inserted(s->customPath, t.inverted().map(page), tol, &at);
            if (at >= 0) return Hit{HitKind::PointEdge, s->id};
        }
    }
    if (sel.size() == 1) {
        Item *it = sel.first();
        const Document *d = m_ed->doc();
        if (it->type() == ItemType::Text) {
            auto *t = static_cast<TextItem *>(it);
            const QTransform tr = t->transform();
            if (d->prevFrame(t->id) && near(pageToView(tr.map(QPointF(0, 0))) + QPointF(4, -12))) return Hit{HitKind::LinkPrev, it->id};
            const QPointF br = pageToView(tr.map(QPointF(t->rect.width(), t->rect.height()))) + QPointF(-4, 12);
            if (near(br)) return Hit{t->nextId.isEmpty() ? HitKind::Overflow : HitKind::LinkNext, it->id};
        }
        if (!it->locked) {
            const auto pts = handlePoints(it);
            if (it->type() == ItemType::Line) {
                for (int i = 0; i < pts.size(); ++i)
                    if (near(pts[i])) return Hit{HitKind::LineEnd, it->id, i};
                if (QPointF b; lineBendHandle(*static_cast<LineItem *>(it), &b) && near(pageToView(b))) return Hit{HitKind::LineBend, it->id};
            } else {
                if (m_ed->cropItem == it->id) {
                    for (int i = 0; i < pts.size(); ++i)
                        if (near(pts[i])) return Hit{HitKind::Crop, it->id, i};
                }
                if (near(rotateHandle(it)) && m_ed->cropItem != it->id) return Hit{HitKind::Rotate, it->id};
                if (it->type() == ItemType::Shape) {
                    auto *s = static_cast<ShapeItem *>(it);
                    if (const ShapeDef *sd = shapeDef(s->shape); sd && s->customPath.isEmpty()) {
                        const QVector<double> adj = shapeAdj(s->shape, s->adj);
                        for (int i = 0; i < sd->handles.size(); ++i)
                            if (near(pageToView(it->transform().map(adjHandlePos(sd->handles[i], it->rect.size(), adj)))))
                                return Hit{HitKind::Adjust, it->id, i};
                    }
                }
                if (m_ed->cropItem != it->id)
                    for (int i = 0; i < pts.size(); ++i)
                        if (near(pts[i])) return Hit{HitKind::Handle, it->id, i};
            }
            if (it->type() == ItemType::Table) {
                auto *t = static_cast<TableItem *>(it);
                const QPointF local = t->transform().inverted().map(page);
                const double tol = 3.0 / ppp();
                if (local.y() >= -tol && local.y() <= t->rect.height() + tol) {
                    double x = 0;
                    for (int c = 0; c < t->cols - 1; ++c) {
                        x += t->colW[c];
                        if (std::abs(local.x() - x) <= tol) return Hit{HitKind::ColBorder, it->id, c};
                    }
                }
                if (local.x() >= -tol && local.x() <= t->rect.width() + tol) {
                    double y = 0;
                    for (int r = 0; r < t->rows; ++r) {
                        y += t->rowH[r];
                        if (std::abs(local.y() - y) <= tol) return Hit{HitKind::RowBorder, it->id, r};
                    }
                }
            }
        }
    } else if (sel.size() > 1) {
        const QRectF b = m_ed->selectionBounds();
        const QPointF loc[8] = {b.topLeft(), {b.center().x(), b.top()}, b.topRight(), {b.right(), b.center().y()}, b.bottomRight(),
                                {b.center().x(), b.bottom()}, b.bottomLeft(), {b.left(), b.center().y()}};
        for (int i = 0; i < 8; ++i)
            if (near(pageToView(loc[i]))) return Hit{HitKind::Handle, QString(), i};
    }
    bool interior = false;
    int row = -1, col = -1;
    const bool enter = sel.size() == 1 && sel.first()->type() == ItemType::Group && !isBarcode(sel.first());
    QString id = itemAt(page, false, &row, &col, &interior);
    if (!id.isEmpty() && enter && id == sel.first()->id) {
        const QString child = itemAt(page, true, &row, &col, &interior);
        if (!child.isEmpty()) id = child;
    }
    // A selected group child stays targeted.
    if (!id.isEmpty() && sel.size() == 1) {
        const auto loc = m_ed->doc()->find(sel.first()->id);
        if (loc.parent && loc.parent->id == id) {
            const QString child = itemAt(page, true, &row, &col, &interior);
            if (!child.isEmpty()) id = child;
        }
    }
    if (!id.isEmpty()) {
        h.kind = HitKind::Item;
        h.id = id;
        h.textInterior = interior;
        h.row = row;
        h.col = col;
        return h;
    }
    if (m_ed->view.guides) {
        const double tol = 3.0 / ppp();
        auto check = [&](const RulerGuides &g, bool master) -> bool {
            for (int i = 0; i < g.h.size(); ++i)
                if (std::abs(page.y() - g.h[i]) <= tol) { h = Hit{HitKind::Guide, master ? QStringLiteral("m") : QString(), i}; h.row = 0; return true; }
            for (int i = 0; i < g.v.size(); ++i)
                if (std::abs(page.x() - g.v[i]) <= tol) { h = Hit{HitKind::Guide, master ? QStringLiteral("m") : QString(), i}; h.row = 1; return true; }
            return false;
        };
        if (check(m_ed->surface()->guides, !m_ed->masterView().isEmpty())) return h;
    }
    return h;
}

int Canvas::textPosAt(const QString &itemId, const QPointF &page, int row, int col) const
{
    Item *it = m_ed->doc()->item(itemId);
    if (!it) return -1;
    PaintContext ctx = paintContext();
    const QPointF local = it->transform().inverted().map(page);
    if (it->type() == ItemType::Text) {
        auto *t = static_cast<TextItem *>(it);
        const auto fl = m_ed->cache().textFrame(*m_ed->doc(), *t, m_ed->surfacePageNumber(), ctx.opt);
        if (!fl.layout) return -1;
        QPointF l = local;
        if (t->vertical) l = QPointF(local.y(), t->rect.width() - local.x());
        const int pos = fl.layout->hitTest(fl.frame, l);
        return pos < 0 ? fl.layout->lastPosition(fl.frame) : pos;
    }
    if (it->type() == ItemType::Shape) {
        QPointF o;
        const StoryLayout *lay = Renderer::shapeTextLayout(ctx, *static_cast<ShapeItem *>(it), &o);
        return lay ? lay->hitTest(0, local - o) : -1;
    }
    if (it->type() == ItemType::Table) {
        QPointF o;
        const auto *tb = static_cast<TableItem *>(it);
        const StoryLayout *lay = Renderer::cellLayout(ctx, *tb, row, col, &o);
        QPointF l = local - o;
        if (lay && tb->cell(row, col).vertical) l = QPointF(l.y(), tb->cellRect(row, col).width() - l.x());
        return lay ? lay->hitTest(0, l) : -1;
    }
    return -1;
}

// ---------------- snapping ----------------
void Canvas::collectSnapTargets(QVector<double> &xs, QVector<double> &ys, const QSet<QString> &exclude) const
{
    const Document *d = m_ed->doc();
    const QSizeF ps = m_ed->surfaceSize();
    if (m_ed->view.snapGuides) {
        xs << 0 << ps.width() << ps.width() / 2;
        ys << 0 << ps.height() << ps.height() / 2;
        const QRectF content = QRectF(QPointF(0, 0), d->pageSize()).marginsRemoved(d->setup.margins);
        xs << content.left() << content.right();
        ys << content.top() << content.bottom();
        const MasterPage *m = m_ed->masterView().isEmpty() ? d->masterFor(*d->pages[m_ed->currentPage()]) : d->master(m_ed->masterView());
        if (m) {
            const GridGuides &g = m->grid;
            if (g.cols > 1) {
                const double cw = (content.width() - g.colGap * (g.cols - 1)) / g.cols;
                for (int c = 1; c < g.cols; ++c) { const double x = content.left() + c * cw + (c - 1) * g.colGap; xs << x << x + g.colGap; }
            }
            if (g.rows > 1) {
                const double rh = (content.height() - g.rowGap * (g.rows - 1)) / g.rows;
                for (int r = 1; r < g.rows; ++r) { const double y = content.top() + r * rh + (r - 1) * g.rowGap; ys << y << y + g.rowGap; }
            }
            for (double v : m->guides.v) xs << v;
            for (double v : m->guides.h) ys << v;
        }
        for (double v : m_ed->surface()->guides.v) xs << v;
        for (double v : m_ed->surface()->guides.h) ys << v;
    }
    if (m_ed->view.snapObjects) {
        for (const auto &it : m_ed->surfaceItems()) {
            if (exclude.contains(it->id)) continue;
            const QRectF b = it->bounds();
            xs << b.left() << b.center().x() << b.right();
            ys << b.top() << b.center().y() << b.bottom();
        }
    }
}

QPointF Canvas::snapPoint(const QPointF &pt, QVector<QLineF> *lines, const QSet<QString> &exclude) const
{
    QVector<double> xs, ys;
    collectSnapTargets(xs, ys, exclude);
    const double thr = 6.0 / ppp();
    QPointF out = pt;
    double bx = thr, by = thr;
    for (double x : xs) if (std::abs(x - pt.x()) < bx) { bx = std::abs(x - pt.x()); out.setX(x); }
    for (double y : ys) if (std::abs(y - pt.y()) < by) { by = std::abs(y - pt.y()); out.setY(y); }
    if (lines) {
        const QSizeF ps = m_ed->surfaceSize();
        if (out.x() != pt.x()) *lines << QLineF(out.x(), -ps.height(), out.x(), ps.height() * 2);
        if (out.y() != pt.y()) *lines << QLineF(-ps.width(), out.y(), ps.width() * 2, out.y());
    }
    return out;
}

QPointF Canvas::snapMove(const QRectF &box, QVector<QLineF> *lines, const QSet<QString> &exclude) const
{
    QVector<double> xs, ys;
    collectSnapTargets(xs, ys, exclude);
    const double thr = 6.0 / ppp();
    double dx = 0, dy = 0, bx = thr, by = thr, sx = 0, sy = 0;
    bool hx = false, hy = false;
    for (double cand : {box.left(), box.center().x(), box.right()})
        for (double x : xs)
            if (std::abs(x - cand) < bx) { bx = std::abs(x - cand); dx = x - cand; sx = x; hx = true; }
    for (double cand : {box.top(), box.center().y(), box.bottom()})
        for (double y : ys)
            if (std::abs(y - cand) < by) { by = std::abs(y - cand); dy = y - cand; sy = y; hy = true; }
    if (lines) {
        const QSizeF ps = m_ed->surfaceSize();
        if (hx) *lines << QLineF(sx, -ps.height(), sx, ps.height() * 2);
        if (hy) *lines << QLineF(-ps.width(), sy, ps.width() * 2, sy);
    }
    return QPointF(dx, dy);
}

// ---------------- mouse ----------------
QStringList Canvas::moveSet() const
{
    QStringList ids;
    for (const auto &id : m_ed->selection()) {
        const auto loc = m_ed->doc()->find(id);
        if (loc.parent && m_ed->selection().contains(loc.parent->id)) continue;
        ids << id;
    }
    return ids;
}

void Canvas::applyFormatPainter(const QString &id)
{
    m_ed->pasteFormattingTo(id);
    if (!m_ed->painterLocked) m_ed->setTool(Tool::Select);
}

void Canvas::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    m_pressView = e->position();
    m_pressPage = toPage(e->position());
    m_lastPage = m_pressPage;
    m_mods = e->modifiers();
    m_snapLines.clear();
    m_tip.clear();
    if (e->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() == Qt::RightButton) {
        const Hit h = hitTest(e->position());
        // Right-clicking a word in the text being edited puts the caret there,
        // so the menu can offer spelling suggestions for it.
        if (m_ed->isEditingText() && h.kind == HitKind::Item && m_ed->textTarget().itemId == h.id && h.textInterior) {
            const int pos = textPosAt(h.id, m_pressPage, h.row, h.col);
            QTextCursor c = m_ed->cursor();
            if (pos >= 0 && !(c.hasSelection() && pos >= c.selectionStart() && pos <= c.selectionEnd())) {
                c.setPosition(pos);
                m_ed->setCursor(c);
            }
            return;
        }
        if (h.kind == HitKind::Item && !m_ed->selection().contains(h.id)) {
            if (!(m_ed->isEditingText() && m_ed->textTarget().itemId == h.id)) m_ed->select(h.id);
        } else if (h.kind == HitKind::None && !m_ed->isEditingText()) {
            m_ed->clearSelection();
        }
        return;
    }
    if (e->button() != Qt::LeftButton) return;

    // Multi-click detection for text.
    if (m_clickTimer.isValid() && m_clickTimer.elapsed() < QApplication::doubleClickInterval() &&
        std::hypot(e->position().x() - m_lastClickView.x(), e->position().y() - m_lastClickView.y()) < 5)
        ++m_clicks;
    else
        m_clicks = 1;
    m_clickTimer.restart();
    m_lastClickView = e->position();

    const Tool tool = m_ed->tool();
    if (tool != Tool::Freeform) m_freePts.clear();
    if (tool == Tool::Freeform) {
        // Curve and Freeform: each click adds a point (Freeform also draws
        // by hand while the button is down); clicking the first point again
        // closes the shape. Scribble: one stroke while the button is down.
        m_ed->endTextEdit();
        if (m_freePts.size() >= 3 && QLineF(pageToView(m_freePts.first()), e->position()).length() < 7) {
            finishFreeform(true);
            return;
        }
        if (m_ed->toolShape() == QLatin1String("scribble")) m_freePts.clear();
        m_freePts << m_pressPage;
        m_drag = Drag::Free;
        viewport()->update();
        return;
    }
    if (tool == Tool::Text || tool == Tool::Table || tool == Tool::Picture || tool == Tool::Shape || tool == Tool::Line ||
        tool == Tool::Arrow || tool == Tool::DoubleArrow || tool == Tool::TextArt) {
        m_ed->endTextEdit();
        m_drag = Drag::Draw;
        m_pressPage = snapPoint(m_pressPage, nullptr, {});
        m_siteStart = SiteHit();
        if (tool == Tool::Line || tool == Tool::Arrow || tool == Tool::DoubleArrow) {
            // A line drawn from an object starts attached to it.
            m_siteStart = siteNear(toPage(e->position()), {});
            if (!m_siteStart.id.isEmpty()) m_pressPage = m_siteStart.at;
        }
        m_rubber = QRectF(m_pressPage, QSizeF(0, 0));
        return;
    }
    const Hit h = hitTest(e->position());
    if (tool == Tool::Link) {
        const QString src = m_ed->linkSource;
        m_ed->setTool(Tool::Select);
        if (h.kind == HitKind::Item) {
            if (auto *t = dynamic_cast<TextItem *>(m_ed->doc()->item(h.id))) m_ed->linkFrames(src, t->id);
        } else if (Item *s = m_ed->doc()->item(src)) {
            // Clicking empty space creates a new box for the overflow.
            m_ed->beginChange(QCoreApplication::translate("Canvas", "Create Text Box Link"));
            auto t = std::static_pointer_cast<TextItem>(m_ed->newTextBox(QRectF(m_pressPage, s->rect.size())));
            t->insets = static_cast<TextItem *>(s)->insets;
            t->columns = static_cast<TextItem *>(s)->columns;
            m_ed->surfaceItems().push_back(t);
            m_ed->linkFrames(src, t->id);
            m_ed->endChange();
            m_ed->select(t->id);
        }
        return;
    }
    if (tool == Tool::FormatPainter) {
        if (h.kind == HitKind::Item) applyFormatPainter(h.id);
        else m_ed->setTool(Tool::Select);
        return;
    }
    m_hit = h;
    switch (h.kind) {
    case HitKind::WrapPoint:
    case HitKind::WrapEdge: {
        Item *it = m_ed->doc()->item(h.id);
        if (!it) return;
        const QPointF local = it->transform().inverted().map(m_pressPage);
        if (h.kind == HitKind::WrapPoint && (e->modifiers() & Qt::ControlModifier)) {
            // Ctrl+click a point deletes it (a wrap outline keeps at least three).
            if (it->wrap.points.size() > 3)
                m_ed->change(QCoreApplication::translate("Canvas", "Delete Wrap Point"), [&] { it->wrap.points.remove(h.index); });
            return;
        }
        m_ed->beginChange(h.kind == HitKind::WrapPoint ? QCoreApplication::translate("Canvas", "Move Wrap Point") : QCoreApplication::translate("Canvas", "Add Wrap Point"));
        m_hit = h;
        if (h.kind == HitKind::WrapEdge) {
            it->wrap.points.insert(h.index + 1, local);
            m_hit.index = h.index + 1;
        }
        m_origWrap = it->wrap.points;
        m_drag = Drag::WrapPoint;
        m_ed->notifyLive();
        return;
    }
    case HitKind::Point:
    case HitKind::PointEdge: {
        auto *sh = dynamic_cast<ShapeItem *>(m_ed->doc()->item(h.id));
        if (!sh) return;
        const QPointF local = sh->transform().inverted().map(m_pressPage);
        if (h.kind == HitKind::Point && (e->modifiers() & Qt::ControlModifier)) {
            // Ctrl+click a point deletes it.
            m_ed->change(QCoreApplication::translate("Canvas", "Delete Point"), [&] { sh->customPath = pts::removed(sh->customPath, h.index); });
            return;
        }
        m_ed->beginChange(h.kind == HitKind::Point ? QCoreApplication::translate("Canvas", "Move Point") : QCoreApplication::translate("Canvas", "Add Point"));
        m_hit = h;
        if (h.kind == HitKind::PointEdge) {
            int at = -1;
            sh->customPath = pts::inserted(sh->customPath, local, 5.0 / ppp(), &at);
            m_hit.index = at;
        }
        m_origPath = sh->customPath;
        const auto el = m_origPath.elementAt(std::clamp(m_hit.index, 0, m_origPath.elementCount() - 1));
        m_origPoint = QPointF(el.x, el.y);
        m_drag = Drag::Point;
        m_ed->notifyLive();
        return;
    }
    case HitKind::LinkPrev:
        if (TextItem *p = m_ed->doc()->prevFrame(h.id)) {
            const auto loc = m_ed->doc()->find(p->id);
            if (loc.page >= 0) m_ed->setCurrentPage(loc.page);
            m_ed->select(p->id);
        }
        return;
    case HitKind::LinkNext:
        if (auto *t = dynamic_cast<TextItem *>(m_ed->doc()->item(h.id))) {
            const auto loc = m_ed->doc()->find(t->nextId);
            if (loc.page >= 0) m_ed->setCurrentPage(loc.page);
            m_ed->select(t->nextId);
        }
        return;
    case HitKind::Overflow:
        m_ed->linkSource = h.id;
        m_ed->setTool(Tool::Link);
        m_ed->linkSource = h.id;
        Q_EMIT m_ed->status(QCoreApplication::translate("Canvas", "Click an empty text box to continue the story there, or click the page to create one."));
        return;
    case HitKind::Handle:
    case HitKind::Rotate:
    case HitKind::LineEnd:
    case HitKind::LineBend:
    case HitKind::Adjust:
    case HitKind::Crop:
    case HitKind::ColBorder:
    case HitKind::RowBorder:
        beginResize(h, m_pressPage);
        return;
    case HitKind::Guide: {
        m_drag = Drag::Guide;
        m_guideOrient = h.row == 0 ? Qt::Horizontal : Qt::Vertical;
        m_guideIndex = h.index;
        m_guideOnMaster = !h.id.isEmpty();
        m_ed->beginChange(QCoreApplication::translate("Canvas", "Move Guide"));
        RulerGuides &g = m_ed->surface()->guides;
        m_guidePos = m_guideOrient == Qt::Horizontal ? g.h[h.index] : g.v[h.index];
        return;
    }
    case HitKind::Item: {
        Item *it = m_ed->doc()->item(h.id);
        // Shift+click in a table selects the cells from the one the cursor is in (or the block began with) to the one clicked.
        if ((e->modifiers() & Qt::ShiftModifier) && it->type() == ItemType::Table && h.row >= 0 && !it->locked) {
            int fromRow = -1, fromCol = -1;
            const auto block = m_ed->cellBlock();
            if (m_ed->isEditingText() && m_ed->textTarget().itemId == h.id) {
                fromRow = m_ed->textTarget().row;
                fromCol = m_ed->textTarget().col;
            } else if (block.range.valid() && block.itemId == h.id) {
                fromRow = block.anchorRow;
                fromCol = block.anchorCol;
            }
            if (fromRow >= 0 && (!m_ed->isEditingText() || fromRow != h.row || fromCol != h.col)) {
                m_ed->selectCells(h.id, fromRow, fromCol, h.row, h.col);
                m_drag = Drag::CellBlock;
                return;
            }
        }
        // Clicking in the text of the box being edited moves the caret.
        if (m_ed->isEditingText() && m_ed->textTarget().itemId == h.id &&
            (h.textInterior || it->type() == ItemType::Shape) && (it->type() != ItemType::Table || (h.row == m_ed->textTarget().row && h.col == m_ed->textTarget().col))) {
            const int pos = textPosAt(h.id, m_pressPage, h.row, h.col);
            // Pressing in selected text starts dragging it (Options >
            // Advanced: drag and drop text).
            const QTextCursor sel = m_ed->cursor();
            if (pos >= 0 && m_clicks == 1 && sel.hasSelection() && pos >= sel.selectionStart() && pos < sel.selectionEnd() &&
                !(e->modifiers() & Qt::ShiftModifier) && Settings::get().value("edit/dragText", true).toBool()) {
                m_moveFrom = sel.selectionStart();
                m_moveTo = sel.selectionEnd();
                m_movePos = -1;
                m_textPress = pos;
                m_drag = Drag::TextMove;
                return;
            }
            m_textPress = pos;
            if (pos >= 0) {
                QTextCursor c = m_ed->cursor();
                if (m_clicks == 2) { c.setPosition(pos); c.select(QTextCursor::WordUnderCursor); }
                else if (m_clicks >= 3) { c.setPosition(pos); c.select(QTextCursor::BlockUnderCursor); if (c.anchor() > 0 && c.document()->characterAt(c.anchor()) == QChar::ParagraphSeparator) { int a = c.anchor() + 1, p2 = c.position(); c.setPosition(a); c.setPosition(p2, QTextCursor::KeepAnchor); } }
                else c.setPosition(pos, (e->modifiers() & Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
                m_ed->desiredX = -1;
                m_ed->setCursor(c);
                m_drag = Drag::TextSelect;
            }
            return;
        }
        if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) {
            if (e->modifiers() & Qt::ShiftModifier) { m_ed->toggleSelect(h.id); return; }
        }
        // Clicking inside a text box or table cell starts typing there.
        if (h.textInterior && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) && !it->locked &&
            (it->type() == ItemType::Text || it->type() == ItemType::Table) && !(m_ed->selection().size() == 1 && m_ed->selection().first() == h.id && !m_ed->isEditingText() && false)) {
            const int pos = textPosAt(h.id, m_pressPage, h.row, h.col);
            m_ed->beginTextEdit(h.id, pos, h.row, h.col);
            m_drag = Drag::TextSelect;
            return;
        }
        if (m_ed->isEditingText()) m_ed->endTextEdit();
        if (!m_ed->selection().contains(h.id)) {
            if (e->modifiers() & Qt::ControlModifier) m_ed->select(QStringList{h.id}, true);
            else m_ed->select(h.id);
        }
        if (m_ed->cropItem == h.id) {
            m_drag = Drag::CropMove;
            m_orig.clear();
            m_orig[h.id] = it->toJson();
            m_ed->beginChange(QCoreApplication::translate("Canvas", "Crop"));
            return;
        }
        m_copyDrag = (e->modifiers() & Qt::ControlModifier);
        m_drag = Drag::Pending;
        return;
    }
    case HitKind::None:
    default:
        if (m_ed->isEditingText()) m_ed->endTextEdit();
        if (!m_ed->cropItem.isEmpty()) m_ed->setCropItem(QString());
        if (!m_ed->pointsItem.isEmpty()) m_ed->setPointsItem(QString());
        if (!m_ed->wrapItem.isEmpty()) m_ed->setWrapItem(QString());
        if (!(e->modifiers() & Qt::ShiftModifier)) m_ed->clearSelection();
        m_drag = Drag::Marquee;
        m_rubber = QRectF(m_pressPage, QSizeF(0, 0));
        return;
    }
}

void Canvas::beginResize(const Hit &h, const QPointF &page)
{
    m_orig.clear();
    for (Item *it : m_ed->selectedItems()) m_orig[it->id] = it->toJson();
    m_origBox = m_ed->selectionBounds();
    switch (h.kind) {
    case HitKind::Handle: m_drag = Drag::Resize; m_ed->beginChange(QCoreApplication::translate("Canvas", "Resize")); break;
    case HitKind::Rotate: {
        m_drag = Drag::Rotate;
        const QPointF c = m_origBox.center();
        m_rotStart = qRadiansToDegrees(std::atan2(page.y() - c.y(), page.x() - c.x()));
        m_ed->beginChange(QCoreApplication::translate("Canvas", "Rotate"));
        break;
    }
    case HitKind::LineEnd: m_drag = Drag::LineEnd; m_ed->beginChange(QCoreApplication::translate("Canvas", "Move Line Point")); break;
    case HitKind::LineBend: m_drag = Drag::LineBend; m_ed->beginChange(QCoreApplication::translate("Canvas", "Adjust Line")); break;
    case HitKind::Adjust: m_drag = Drag::Adjust; m_ed->beginChange(QCoreApplication::translate("Canvas", "Adjust Shape")); break;
    case HitKind::Crop: m_drag = Drag::Crop; m_ed->beginChange(QCoreApplication::translate("Canvas", "Crop")); break;
    case HitKind::ColBorder: m_drag = Drag::ColResize; m_ed->beginChange(QCoreApplication::translate("Canvas", "Resize Column")); break;
    case HitKind::RowBorder: m_drag = Drag::RowResize; m_ed->beginChange(QCoreApplication::translate("Canvas", "Resize Row")); break;
    default: break;
    }
}

static void restoreItem(Item *it, const QJsonObject &o)
{
    const QString id = it->id;
    it->fromJson(o);
    it->id = id;
}

void Canvas::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF page = toPage(e->position());
    const QPointF prevPage = m_lastPage;
    m_lastPage = page;
    Q_EMIT mouseMovedPage(page);
    m_hRuler->setMouse(toScene(e->position()).x());
    m_vRuler->setMouse(toScene(e->position()).y());
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    Document *d = m_ed->doc();
    Settings &st = Settings::get();
    m_snapLines.clear();
    m_tip.clear();
    m_tipPos = e->position();

    switch (m_drag) {
    case Drag::None: {
        updateCursorShape(e->position());
        // With a line tool, the connection sites of the object under the pointer show.
        const Tool t = m_ed->tool();
        const QString was = m_siteHover.over;
        const int wasSite = m_siteHover.site;
        m_siteHover = (t == Tool::Line || t == Tool::Arrow || t == Tool::DoubleArrow) ? siteNear(page, {}) : SiteHit();
        if (m_siteHover.over != was || m_siteHover.site != wasSite || (t == Tool::Freeform && !m_freePts.isEmpty())) viewport()->update();
        return;
    }
    case Drag::Free: {
        // Drawing by hand: a point every couple of pixels the pointer moves.
        if (m_ed->toolShape() != QLatin1String("curve") && QLineF(pageToView(m_freePts.last()), e->position()).length() >= 2) m_freePts << page;
        viewport()->update();
        return;
    }
    case Drag::Pan: {
        const QPointF dv = e->position() - m_pressView;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - int(dv.x()));
        verticalScrollBar()->setValue(verticalScrollBar()->value() - int(dv.y()));
        m_pressView = e->position();
        return;
    }
    case Drag::Pending: {
        if (std::hypot(e->position().x() - m_pressView.x(), e->position().y() - m_pressView.y()) < 4) return;
        bool locked = false;
        for (Item *it : m_ed->selectedItems()) locked |= it->locked;
        if (locked) { m_drag = Drag::None; return; }
        m_ed->beginChange(m_copyDrag ? QCoreApplication::translate("Canvas", "Copy") : QCoreApplication::translate("Canvas", "Move"));
        if (m_copyDrag) {
            QStringList copies;
            ItemList originals;
            for (const auto &id : moveSet())
                if (ItemPtr it = d->itemPtr(id)) originals.push_back(it);
            for (const ItemPtr &c : d->cloneItems(originals)) {
                m_ed->surfaceItems().push_back(c);
                copies << c->id;
            }
            m_ed->select(copies);
        }
        // A connector dragged away from its objects comes loose from them.
        {
            QSet<QString> moving;
            ItemList moved;
            for (const auto &id : moveSet())
                if (ItemPtr it = d->itemPtr(id)) moved.push_back(it);
            walkItems(moved, [&](const ItemPtr &it) { moving.insert(it->id); });
            walkItems(moved, [&](const ItemPtr &it) {
                if (it->type() != ItemType::Line) return;
                auto *l = static_cast<LineItem *>(it.get());
                for (LineItem::Glue *g : {&l->start, &l->end})
                    if (!g->id.isEmpty() && !moving.contains(g->id)) *g = LineItem::Glue();
            });
        }
        m_orig.clear();
        for (const auto &id : moveSet()) m_orig[id] = d->item(id)->toJson();
        m_origBox = m_ed->selectionBounds();
        m_drag = Drag::Move;
        [[fallthrough]];
    }
    case Drag::Move: {
        QPointF delta = page - m_pressPage;
        if (shift) {
            if (std::abs(delta.x()) > std::abs(delta.y())) delta.setY(0); else delta.setX(0);
        }
        QSet<QString> ex;
        for (auto it = m_orig.begin(); it != m_orig.end(); ++it) ex.insert(it.key());
        if (!(e->modifiers() & Qt::AltModifier)) delta += snapMove(m_origBox.translated(delta), &m_snapLines, ex);
        for (auto it = m_orig.begin(); it != m_orig.end(); ++it) {
            Item *item = d->item(it.key());
            if (!item) continue;
            restoreItem(item, it.value());
            item->moveBy(delta.x(), delta.y());
            if (auto loc = d->find(it.key()); loc.parent) loc.parent->syncRect();
        }
        const QRectF nb = m_origBox.translated(delta);
        m_tip = QStringLiteral("%1, %2").arg(st.format(nb.left()), st.format(nb.top()));
        // One picture dragged over another: they swap when it's let go.
        m_swapTarget.clear();
        if (m_orig.size() == 1)
            if (auto *moved = dynamic_cast<PictureItem *>(d->item(m_orig.constBegin().key())); moved && !moved->imageId.isEmpty()) {
                const ItemList &l = m_ed->surfaceItems();
                for (auto it = l.rbegin(); it != l.rend(); ++it)
                    if ((*it)->id != moved->id && (*it)->bounds().contains(page)) {
                        if ((*it)->type() == ItemType::Picture) m_swapTarget = (*it)->id;
                        break;
                    }
            }
        if (!m_swapTarget.isEmpty()) m_tip = QCoreApplication::translate("Canvas", "Release to swap the pictures");
        m_ed->notifyLive();
        return;
    }
    case Drag::Resize: {
        const auto items = m_ed->selectedItems();
        if (items.size() == 1 && items.first()->type() != ItemType::Group && items.first()->type() != ItemType::Line) {
            Item *it = items.first();
            restoreItem(it, m_orig[it->id]);
            const int hx[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, hy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
            const int i = m_hit.index;
            const double w = it->rect.width(), h = it->rect.height();
            QPointF pg = page;
            if (it->rotation == 0 && !(e->modifiers() & Qt::AltModifier)) pg = snapPoint(page, &m_snapLines, {it->id});
            const QTransform T = it->transform();
            const QPointF pl = T.inverted().map(pg);
            double x0 = 0, x1 = w, y0 = 0, y1 = h;
            const bool fromCenter = ctrl;
            if (hx[i] == 1) x1 = pl.x(); else if (hx[i] == -1) x0 = pl.x();
            if (hy[i] == 1) y1 = pl.y(); else if (hy[i] == -1) y0 = pl.y();
            if (fromCenter) {
                if (hx[i]) { const double half = std::abs(pl.x() - w / 2); x0 = w / 2 - half; x1 = w / 2 + half; }
                if (hy[i]) { const double half = std::abs(pl.y() - h / 2); y0 = h / 2 - half; y1 = h / 2 + half; }
            }
            if (x1 - x0 < 2) { if (hx[i] == 1) x1 = x0 + 2; else x0 = x1 - 2; }
            if (y1 - y0 < 2) { if (hy[i] == 1) y1 = y0 + 2; else y0 = y1 - 2; }
            const bool art = it->type() == ItemType::Shape && static_cast<ShapeItem *>(it)->isArt();
            const bool locked = it->type() == ItemType::Table && static_cast<TableItem *>(it)->lockSize;   // Format Table > Size > Lock aspect ratio
            const bool keepAspect = (hx[i] && hy[i]) && (shift != (it->type() == ItemType::Picture || it->type() == ItemType::TextArt || art || locked));
            if (keepAspect && w > 0 && h > 0) {
                const double s = std::max((x1 - x0) / w, (y1 - y0) / h);
                const double nw = w * s, nh = h * s;
                if (fromCenter) { x0 = w / 2 - nw / 2; x1 = w / 2 + nw / 2; y0 = h / 2 - nh / 2; y1 = h / 2 + nh / 2; }
                else {
                    if (hx[i] == 1) x1 = x0 + nw; else x0 = x1 - nw;
                    if (hy[i] == 1) y1 = y0 + nh; else y0 = y1 - nh;
                }
            }
            const QRectF L(QPointF(x0, y0), QPointF(x1, y1));
            const QPointF c = T.map(L.center());
            const QRectF from = it->rect;
            const QRectF to(c - QPointF(L.width() / 2, L.height() / 2), L.size());
            // Resize in the frame's own axes.
            if (it->type() == ItemType::Picture) {
                auto *pic = static_cast<PictureItem *>(it);
                const double kx = to.width() / from.width(), ky = to.height() / from.height();
                pic->imgRect = QRectF(pic->imgRect.x() * kx, pic->imgRect.y() * ky, pic->imgRect.width() * kx, pic->imgRect.height() * ky);
            } else if (it->type() == ItemType::Table) {
                auto *t = static_cast<TableItem *>(it);
                const double kx = to.width() / from.width(), ky = to.height() / from.height();
                for (double &cw : t->colW) cw *= kx;
                for (double &rh : t->rowH) rh *= ky;
            }
            it->rect = to;
            if (it->type() == ItemType::Shape) static_cast<ShapeItem *>(it)->resized(from.size());
            m_tip = QStringLiteral("%1 × %2").arg(st.format(to.width()), st.format(to.height()));
        } else {
            // Scale the whole selection (or a group) inside its bounding box.
            const int hx[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, hy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
            const int i = m_hit.index;
            QRectF b = m_origBox;
            QPointF pg = (e->modifiers() & Qt::AltModifier) ? page : snapPoint(page, &m_snapLines, QSet<QString>(m_orig.keyBegin(), m_orig.keyEnd()));
            if (hx[i] == 1) b.setRight(std::max(b.left() + 2, pg.x())); else if (hx[i] == -1) b.setLeft(std::min(b.right() - 2, pg.x()));
            if (hy[i] == 1) b.setBottom(std::max(b.top() + 2, pg.y())); else if (hy[i] == -1) b.setTop(std::min(b.bottom() - 2, pg.y()));
            if (hx[i] && hy[i] && !shift) {
                const double s = std::max(b.width() / m_origBox.width(), b.height() / m_origBox.height());
                const QSizeF ns = m_origBox.size() * s;
                if (hx[i] == 1) b.setRight(b.left() + ns.width()); else b.setLeft(b.right() - ns.width());
                if (hy[i] == 1) b.setBottom(b.top() + ns.height()); else b.setTop(b.bottom() - ns.height());
            }
            for (auto it = m_orig.begin(); it != m_orig.end(); ++it) {
                Item *item = d->item(it.key());
                if (!item) continue;
                restoreItem(item, it.value());
                item->scaleInto(m_origBox, b);
            }
            m_tip = QStringLiteral("%1 × %2").arg(st.format(b.width()), st.format(b.height()));
        }
        m_ed->notifyLive();
        return;
    }
    case Drag::Rotate: {
        const QPointF c = m_origBox.center();
        const double ang = qRadiansToDegrees(std::atan2(page.y() - c.y(), page.x() - c.x()));
        double delta = ang - m_rotStart;
        const auto items = m_ed->selectedItems();
        const double base = items.size() == 1 ? m_orig.value(items.first()->id)["rot"].toDouble() : 0;
        if (shift) delta = std::round((base + delta) / 15.0) * 15.0 - base;
        for (auto it = m_orig.begin(); it != m_orig.end(); ++it) {
            Item *item = d->item(it.key());
            if (!item) continue;
            restoreItem(item, it.value());
            item->rotateAround(delta, c);
        }
        double shown = std::fmod(base + delta, 360.0);
        if (shown < 0) shown += 360;
        m_tip = QStringLiteral("%1°").arg(std::round(shown));
        m_ed->notifyLive();
        return;
    }
    case Drag::LineEnd: {
        auto *l = dynamic_cast<LineItem *>(m_ed->single());
        if (!l) return;
        QPointF pg = (e->modifiers() & Qt::AltModifier) ? page : snapPoint(page, &m_snapLines, {l->id});
        const QPointF other = m_hit.index == 0 ? l->p2 : l->p1;
        if (shift) {
            const QPointF dv = pg - other;
            const double a = std::round(std::atan2(dv.y(), dv.x()) / (M_PI / 4)) * (M_PI / 4);
            const double len = std::hypot(dv.x(), dv.y());
            pg = other + QPointF(std::cos(a), std::sin(a)) * len;
        }
        // Near another object, the end attaches to its nearest connection site.
        m_siteHover = siteNear(page, {l->id});
        LineItem::Glue &g = m_hit.index == 0 ? l->start : l->end;
        if (!m_siteHover.id.isEmpty()) {
            pg = m_siteHover.at;
            g = LineItem::Glue{m_siteHover.id, m_siteHover.site};
        } else {
            g = LineItem::Glue();
        }
        (m_hit.index == 0 ? l->p1 : l->p2) = pg;
        l->syncRect();
        const QPointF dv = l->p2 - l->p1;
        m_tip = QStringLiteral("%1, %2°").arg(st.format(std::hypot(dv.x(), dv.y()))).arg(std::round(qRadiansToDegrees(std::atan2(-dv.y(), dv.x()))));
        m_ed->notifyLive();
        return;
    }
    case Drag::WrapPoint: {
        Item *it = m_ed->single();
        if (!it || m_hit.index < 0 || m_hit.index >= m_origWrap.size()) return;
        it->wrap.points = m_origWrap;
        it->wrap.points[m_hit.index] = it->transform().inverted().map(page);
        m_ed->notifyLive();
        return;
    }
    case Drag::Point: {
        auto *sh = dynamic_cast<ShapeItem *>(m_ed->single());
        if (!sh || m_hit.index < 0) return;
        const QPointF local = sh->transform().inverted().map(page);
        sh->customPath = pts::moved(m_origPath, m_hit.index, local - m_origPoint);
        m_ed->notifyLive();
        return;
    }
    case Drag::LineBend: {
        auto *l = dynamic_cast<LineItem *>(m_ed->single());
        if (!l) return;
        const QPointF d0 = l->p2 - l->p1;
        if (!l->startVertical && std::abs(d0.x()) > 0.01) l->bend = (page.x() - l->p1.x()) / d0.x();
        else if (l->startVertical && std::abs(d0.y()) > 0.01) l->bend = (page.y() - l->p1.y()) / d0.y();
        l->bend = std::clamp(l->bend, -5.0, 6.0);
        l->syncRect();
        m_ed->notifyLive();
        return;
    }
    case Drag::Adjust: {
        auto *s = dynamic_cast<ShapeItem *>(m_ed->single());
        if (!s) return;
        const ShapeDef *sd = shapeDef(s->shape);
        if (!sd || m_hit.index >= sd->handles.size()) return;
        s->adj = shapeAdj(s->shape, s->adj);
        const QPointF local = s->transform().inverted().map(page);
        s->adj[sd->handles[m_hit.index].index] = adjFromPoint(sd->handles[m_hit.index], s->rect.size(), local);
        m_ed->notifyLive();
        return;
    }
    case Drag::Crop: {
        auto *pic = dynamic_cast<PictureItem *>(m_ed->single());
        if (!pic) return;
        restoreItem(pic, m_orig[pic->id]);
        const QTransform T0 = pic->transform();
        const int hx[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, hy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
        const int i = m_hit.index;
        const QPointF pl = T0.inverted().map(page);
        double x0 = 0, x1 = pic->rect.width(), y0 = 0, y1 = pic->rect.height();
        if (hx[i] == 1) x1 = std::max(x0 + 2, pl.x()); else if (hx[i] == -1) x0 = std::min(x1 - 2, pl.x());
        if (hy[i] == 1) y1 = std::max(y0 + 2, pl.y()); else if (hy[i] == -1) y0 = std::min(y1 - 2, pl.y());
        const QRectF L(QPointF(x0, y0), QPointF(x1, y1));
        const QPointF c = T0.map(L.center());
        const QRectF oldImg = pic->imgRect;
        pic->rect = QRectF(c - QPointF(L.width() / 2, L.height() / 2), L.size());
        // Keep the picture where it is on the page.
        const QTransform T1 = pic->transform();
        const QPointF a = T1.inverted().map(T0.map(oldImg.topLeft())), b = T1.inverted().map(T0.map(oldImg.bottomRight()));
        pic->imgRect = QRectF(a, b).normalized();
        m_ed->notifyLive();
        return;
    }
    case Drag::CropMove: {
        auto *pic = dynamic_cast<PictureItem *>(m_ed->single());
        if (!pic) return;
        restoreItem(pic, m_orig[pic->id]);
        const QTransform inv = pic->transform().inverted();
        const QPointF dl = inv.map(page) - inv.map(m_pressPage);
        pic->imgRect.translate(dl);
        m_ed->notifyLive();
        return;
    }
    case Drag::ColResize:
    case Drag::RowResize: {
        auto *t = dynamic_cast<TableItem *>(m_ed->single());
        if (!t) return;
        restoreItem(t, m_orig[t->id]);
        const QTransform inv = t->transform().inverted();
        const QPointF dl = inv.map(page) - inv.map(m_pressPage);
        const int i = m_hit.index;
        if (m_drag == Drag::ColResize) {
            const double dx = std::clamp(dl.x(), 6 - t->colW[i], t->colW[i + 1] - 6);
            t->colW[i] += dx;
            t->colW[i + 1] -= dx;
        } else {
            t->rowH[i] = std::max(6.0, t->rowH[i] + dl.y());
            const double oldH = t->rect.height();
            t->syncRect();
            Q_UNUSED(oldH);
        }
        m_ed->notifyLive();
        return;
    }
    case Drag::Marquee:
    case Drag::Draw: {
        QPointF a = m_pressPage, b = page;
        if (m_drag == Drag::Draw && !(e->modifiers() & Qt::AltModifier)) b = snapPoint(page, &m_snapLines, {});
        const Tool t = m_ed->tool();
        if (m_drag == Drag::Draw && shift) {
            if (t == Tool::Line || t == Tool::Arrow || t == Tool::DoubleArrow) {
                const QPointF dv = b - a;
                const double ang = std::round(std::atan2(dv.y(), dv.x()) / (M_PI / 4)) * (M_PI / 4);
                b = a + QPointF(std::cos(ang), std::sin(ang)) * std::hypot(dv.x(), dv.y());
            } else {
                const double s = std::max(std::abs(b.x() - a.x()), std::abs(b.y() - a.y()));
                b = QPointF(a.x() + (b.x() >= a.x() ? s : -s), a.y() + (b.y() >= a.y() ? s : -s));
            }
        }
        if (m_drag == Drag::Draw && ctrl) a = m_pressPage - (b - m_pressPage);
        if (m_drag == Drag::Draw && (t == Tool::Line || t == Tool::Arrow || t == Tool::DoubleArrow)) {
            m_siteHover = siteNear(page, {});
            if (!m_siteHover.id.isEmpty()) b = m_siteHover.at;
        }
        m_lastPage = b;
        m_rubber = QRectF(a, b).normalized();
        if (m_drag == Drag::Draw) m_tip = QStringLiteral("%1 × %2").arg(st.format(m_rubber.width()), st.format(m_rubber.height()));
        viewport()->update();
        return;
    }
    case Drag::CellBlock: {
        const auto block = m_ed->cellBlock();
        const auto *tb = dynamic_cast<TableItem *>(d->item(block.itemId));
        if (!tb) return;
        int row = 0, col = 0;
        tb->cellAt(tb->transform().inverted().map(page), &row, &col);
        if (!(tb->cellsBetween(block.anchorRow, block.anchorCol, row, col) == block.range)) m_ed->selectCells(block.itemId, block.anchorRow, block.anchorCol, row, col);
        return;
    }
    case Drag::TextSelect:
    case Drag::TextMove: {
        const auto &tt = m_ed->textTarget();
        if (tt.itemId.isEmpty()) return;
        // Dragging from one table cell into another selects cells, not text.
        if (m_drag == Drag::TextSelect && tt.row >= 0)
            if (const auto *tb = dynamic_cast<TableItem *>(d->item(tt.itemId))) {
                int row = 0, col = 0;
                tb->cellAt(tb->transform().inverted().map(page), &row, &col);
                if (row != tt.row || col != tt.col) {
                    const QString id = tt.itemId;
                    m_ed->selectCells(id, tt.row, tt.col, row, col);
                    m_drag = Drag::CellBlock;
                    viewport()->update();
                    return;
                }
            }
        int pos = textPosAt(tt.itemId, page, tt.row, tt.col);
        // Allow dragging into other linked boxes on this page.
        if (Item *it = d->item(tt.itemId); it && it->type() == ItemType::Text) {
            for (TextItem *f : d->chainOf(it->id)) {
                if (d->find(f->id).page != m_ed->currentPage()) continue;
                if (f->transform().inverted().map(page).y() >= 0 && QRectF(QPointF(0, 0), f->rect.size()).contains(f->transform().inverted().map(page))) {
                    pos = textPosAt(f->id, page);
                    break;
                }
            }
        }
        if (pos >= 0 && m_drag == Drag::TextMove) {
            m_movePos = pos;
            viewport()->update();
        } else if (pos >= 0) {
            QTextCursor c = m_ed->cursor();
            c.setPosition(pos, QTextCursor::KeepAnchor);
            // Options > Advanced: once a drag reaches past the first word,
            // the selection takes whole words.
            if (m_clicks == 1 && m_textPress >= 0 && Settings::get().value("edit/wholeWord", true).toBool()) {
                QTextCursor w(c.document());
                w.setPosition(m_textPress);
                w.select(QTextCursor::WordUnderCursor);
                const int ws = w.hasSelection() ? w.selectionStart() : m_textPress, we = w.hasSelection() ? w.selectionEnd() : m_textPress;
                if (pos < ws || pos > we) {
                    QTextCursor q(c.document());
                    q.setPosition(pos);
                    q.select(QTextCursor::WordUnderCursor);
                    const bool forward = pos > m_textPress;
                    int end = pos;
                    if (q.hasSelection() && q.selectionStart() < pos && pos < q.selectionEnd()) end = forward ? q.selectionEnd() : q.selectionStart();
                    c.setPosition(forward ? ws : we);
                    c.setPosition(end, QTextCursor::KeepAnchor);
                }
            }
            m_ed->setCursor(c);
        }
        Q_UNUSED(prevPage);
        return;
    }
    case Drag::Guide: {
        m_guidePos = m_guideOrient == Qt::Horizontal ? page.y() : page.x();
        if (!(e->modifiers() & Qt::AltModifier)) {
            const QPointF sp = snapPoint(page, nullptr, {});
            m_guidePos = m_guideOrient == Qt::Horizontal ? sp.y() : sp.x();
        }
        RulerGuides &g = m_ed->surface()->guides;
        if (m_guideIndex >= 0) (m_guideOrient == Qt::Horizontal ? g.h : g.v)[m_guideIndex] = m_guidePos;
        m_tip = st.format(m_guidePos);
        m_ed->notifyLive();
        return;
    }
    }
}

void Canvas::mouseReleaseEvent(QMouseEvent *e)
{
    const Drag drag = m_drag;
    m_drag = Drag::None;
    m_snapLines.clear();
    m_tip.clear();
    Document *d = m_ed->doc();
    switch (drag) {
    case Drag::Pan:
        updateCursorShape(e->position());
        break;
    case Drag::Move: {
        if (!m_swapTarget.isEmpty() && m_orig.size() == 1) {
            // Swapped: the dragged frame goes back where it was, and the two
            // pictures trade frames, each filling its new one.
            auto *moved = dynamic_cast<PictureItem *>(m_ed->doc()->item(m_orig.constBegin().key()));
            auto *target = dynamic_cast<PictureItem *>(m_ed->doc()->item(m_swapTarget));
            m_swapTarget.clear();
            if (moved && target) {
                restoreItem(moved, m_orig.constBegin().value());
                std::swap(moved->imageId, target->imageId);
                moved->fitImage(m_ed->doc()->imageSize(moved->imageId), true);
                target->fitImage(m_ed->doc()->imageSize(target->imageId), true);
                m_ed->endChange();
                m_ed->select(target->id);
                break;
            }
        }
        m_swapTarget.clear();
        const QStringList ids = m_orig.keys();
        m_ed->settleScratch(ids);
        m_ed->endChange();
        break;
    }
    case Drag::Resize:
        for (Item *it : m_ed->selectedItems()) {
            if (it->type() == ItemType::Text) m_ed->autoGrowText(static_cast<TextItem *>(it));
            if (it->type() == ItemType::Table) m_ed->fitTableRows(static_cast<TableItem *>(it));
        }
        m_ed->endChange();
        break;
    case Drag::Point:
        if (auto *sh = dynamic_cast<ShapeItem *>(m_ed->single()); sh && sh->rotation == 0 && !sh->flipH && !sh->flipV) {
            // Keep the frame wrapped around the outline.
            const QRectF b = sh->customPath.boundingRect();
            if (b.width() > 0.5 && b.height() > 0.5) {
                sh->customPath.translate(-b.topLeft());
                sh->rect = QRectF(sh->rect.topLeft() + b.topLeft(), b.size());
            }
        }
        m_ed->endChange();
        break;
    case Drag::LineEnd:
        m_siteHover = SiteHit();
        m_ed->endChange();
        break;
    case Drag::Free:
        if (m_ed->toolShape() == QLatin1String("scribble")) finishFreeform(false);
        break;
    case Drag::WrapPoint:
    case Drag::Rotate:
    case Drag::LineBend:
    case Drag::Adjust:
    case Drag::Crop:
    case Drag::CropMove:
    case Drag::ColResize:
    case Drag::RowResize:
        m_ed->endChange();
        break;
    case Drag::Guide: {
        // Dragging a guide back onto a ruler (off the page area) deletes it.
        const QPoint vp = e->position().toPoint();
        if (vp.x() < 0 || vp.y() < 0) {
            RulerGuides &g = m_ed->surface()->guides;
            if (m_guideIndex >= 0) {
                auto &v = m_guideOrient == Qt::Horizontal ? g.h : g.v;
                if (m_guideIndex < v.size()) v.removeAt(m_guideIndex);
            }
        }
        m_ed->endChange();
        break;
    }
    case Drag::Marquee: {
        const QRectF r = m_rubber;
        m_rubber = QRectF();
        if (r.width() > 1 || r.height() > 1) {
            QStringList ids;
            auto take = [&](const ItemList &l) {
                for (const auto &it : l)
                    if (r.contains(it->bounds())) ids << it->id;
            };
            take(m_ed->surfaceItems());
            if (m_ed->view.scratch && m_ed->masterView().isEmpty()) take(d->scratch);
            m_ed->select(ids, e->modifiers() & Qt::ShiftModifier);
        }
        viewport()->update();
        break;
    }
    case Drag::Draw: {
        const QRectF r = m_rubber;
        m_rubber = QRectF();
        const bool clicked = std::hypot(e->position().x() - m_pressView.x(), e->position().y() - m_pressView.y()) < 4;
        finishDraw(r, clicked);
        break;
    }
    case Drag::TextMove: {
        // Dropped outside the dragged text: move it there (Ctrl copies).
        // Dropped inside it, or not moved: a click that places the caret.
        const bool copy = e->modifiers() & Qt::ControlModifier;
        if (m_movePos < 0 || (m_movePos >= m_moveFrom && m_movePos <= m_moveTo)) {
            QTextCursor c = m_ed->cursor();
            c.setPosition(m_textPress >= 0 ? m_textPress : m_moveFrom);
            m_ed->setCursor(c);
        } else {
            const int from = m_moveFrom, to = m_moveTo, at = m_movePos;
            m_ed->editTextAs(copy ? QCoreApplication::translate("Canvas", "Copy Text") : QCoreApplication::translate("Canvas", "Move Text"), [&](QTextCursor &cur) {
                QTextDocument *doc = cur.document();
                QTextCursor src(doc);
                src.setPosition(from);
                src.setPosition(to, QTextCursor::KeepAnchor);
                const QTextDocumentFragment frag = src.selection();
                int dst = at;
                if (!copy) {
                    src.removeSelectedText();
                    if (dst > to) dst -= to - from;
                }
                QTextCursor ins(doc);
                ins.setPosition(dst);
                ins.insertFragment(frag);
                cur.setPosition(dst);
                cur.setPosition(dst + (to - from), QTextCursor::KeepAnchor);
            });
        }
        m_movePos = m_moveFrom = m_moveTo = -1;
        break;
    }
    case Drag::TextSelect:
        if (m_ed->tool() == Tool::FormatPainter && m_ed->painterHasText && m_ed->cursor().hasSelection()) {
            m_ed->mergeCharFormat(m_ed->painterText, QCoreApplication::translate("Canvas", "Format Painter"));
            if (!m_ed->painterLocked) m_ed->setTool(Tool::Select);
        }
        break;
    default:
        break;
    }
    m_orig.clear();
    viewport()->update();
}

void Canvas::finishFreeform(bool closed)
{
    QVector<QPointF> pts;
    // A double click lands the last point twice.
    for (const QPointF &pt : std::as_const(m_freePts))
        if (pts.isEmpty() || QLineF(pageToView(pts.last()), pageToView(pt)).length() >= 2) pts << pt;
    const QString kind = m_ed->toolShape();
    m_freePts.clear();
    m_drag = Drag::None;
    m_ed->setTool(Tool::Select);
    viewport()->update();
    if (pts.size() < 2) return;
    const QPainterPath path = freeformPath(pts, kind, closed);
    QRectF b = path.boundingRect();
    if (b.width() < 1) b.adjust(-0.5, 0, 0.5, 0);
    if (b.height() < 1) b.adjust(0, -0.5, 0, 0.5);
    auto s = std::make_shared<ShapeItem>();
    s->rect = b;
    s->customPath = path.translated(-b.topLeft());
    if (!closed) s->fill = Fill::none();
    m_ed->addItem(s);
}

Canvas::SiteHit Canvas::siteNear(const QPointF &page, const QSet<QString> &exclude) const
{
    SiteHit out;
    const double pad = 12.0 / ppp(), snap = 10.0 / ppp();
    const ItemList &items = m_ed->surfaceItems();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const Item *o = it->get();
        if (o->type() == ItemType::Line || exclude.contains(o->id)) continue;
        const bool group = o->type() == ItemType::Group;
        const QRectF frame = group ? o->bounds() : QRectF(QPointF(0, 0), o->rect.size());
        const QPointF local = group ? page : o->transform().inverted().map(page);
        if (!frame.adjusted(-pad, -pad, pad, pad).contains(local)) continue;
        out.over = o->id;
        double best = snap;
        for (int i = 0; i < kConnectionSites; ++i) {
            const QPointF at = connectionSite(*o, i);
            const double dd = QLineF(at, page).length();
            if (dd <= best) {
                best = dd;
                out.id = o->id;
                out.site = i;
                out.at = at;
            }
        }
        break;
    }
    return out;
}

void Canvas::finishDraw(const QRectF &rIn, bool clicked)
{
    const Tool tool = m_ed->tool();
    QRectF r = rIn;
    const QPointF p0 = m_pressPage;
    QPointF p1 = m_lastPage;
    if (clicked) {
        QSizeF def(144, 72);
        if (tool == Tool::Shape) def = QSizeF(72, 72);
        if (tool == Tool::Table) def = QSizeF(216, 72);
        if (tool == Tool::Picture) def = QSizeF(144, 108);
        r = QRectF(p0, def);
        p1 = p0 + QPointF(108, 0);
    }
    Document *d = m_ed->doc();
    switch (tool) {
    case Tool::Text: {
        auto t = m_ed->newTextBox(r);
        m_ed->addItem(t);
        m_ed->setTool(Tool::Select);
        m_ed->beginTextEdit(t->id, 0);
        return;
    }
    case Tool::Table: {
        auto t = m_ed->newTable(r, 3, 3);
        m_ed->addItem(t);
        break;
    }
    case Tool::Picture: {
        auto pic = std::make_shared<PictureItem>();
        pic->rect = r;
        pic->imgRect = QRectF(QPointF(0, 0), r.size());
        m_ed->addItem(pic);
        m_ed->setTool(Tool::Select);
        Q_EMIT insertPictureWanted(pic->id);
        return;
    }
    case Tool::Shape: {
        auto s = std::make_shared<ShapeItem>();
        s->shape = m_ed->toolShape();
        s->rect = r;
        if (const ShapeDef *sd = shapeDef(s->shape); sd && sd->open) s->fill = Fill::none();
        m_ed->addItem(s);
        break;
    }
    case Tool::Line:
    case Tool::Arrow:
    case Tool::DoubleArrow: {
        auto l = std::make_shared<LineItem>();
        l->p1 = p0;
        l->p2 = p1;
        if (tool != Tool::Line) l->stroke.endArrow = Arrow::Triangle;
        if (tool == Tool::DoubleArrow) l->stroke.startArrow = Arrow::Triangle;
        const QString route = m_ed->toolShape();
        l->route = route == QLatin1String("elbow") ? LineItem::Elbow : route == QLatin1String("curved") ? LineItem::Curved : LineItem::Straight;
        // Ends drawn on an object's connection site attach to it.
        if (!clicked) {
            if (!m_siteStart.id.isEmpty()) l->start = LineItem::Glue{m_siteStart.id, m_siteStart.site};
            if (!m_siteHover.id.isEmpty() && !(m_siteHover.id == m_siteStart.id && m_siteHover.site == m_siteStart.site))
                l->end = LineItem::Glue{m_siteHover.id, m_siteHover.site};
        }
        for (const LineItem::Glue *g : {&l->start, &l->end})
            if (const Item *o = g->id.isEmpty() ? nullptr : d->item(g->id)) connectionSite(*o, g->site, g == &l->start ? &l->startVertical : &l->endVertical);
        l->syncRect();
        m_siteStart = m_siteHover = SiteHit();
        m_ed->addItem(l);
        break;
    }
    case Tool::TextArt: {
        auto w = std::make_shared<TextArtItem>();
        w->rect = r;
        m_ed->addItem(w);
        Q_EMIT editTextArtWanted(w->id);
        break;
    }
    default:
        break;
    }
    Q_UNUSED(d);
    m_ed->setTool(Tool::Select);
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e)
{
    // A double click ends a curve or freeform where it is.
    if (m_ed->tool() == Tool::Freeform) {
        if (!m_freePts.isEmpty() && m_ed->toolShape() != QLatin1String("scribble")) finishFreeform(false);
        else mousePressEvent(e);
        return;
    }
    const Hit h = hitTest(e->position());
    if (h.kind != HitKind::Item) {
        QAbstractScrollArea::mouseDoubleClickEvent(e);
        return;
    }
    Item *it = m_ed->doc()->item(h.id);
    if (!it) return;
    if (m_ed->isEditingText() && m_ed->textTarget().itemId == h.id) {
        mousePressEvent(e);   // counted as the second click: select word
        return;
    }
    switch (it->type()) {
    case ItemType::Text:
    case ItemType::Shape:
    case ItemType::Table: {
        m_ed->beginTextEdit(h.id, -1, h.row, h.col);
        const int pos = textPosAt(h.id, toPage(e->position()), m_ed->textTarget().row, m_ed->textTarget().col);
        if (pos >= 0) {
            QTextCursor c = m_ed->cursor();
            c.setPosition(pos);
            c.select(QTextCursor::WordUnderCursor);
            m_ed->setCursor(c);
        }
        break;
    }
    case ItemType::Picture:
        if (static_cast<PictureItem *>(it)->imageId.isEmpty()) Q_EMIT insertPictureWanted(it->id);
        else Q_EMIT pictureTabWanted();
        break;
    case ItemType::TextArt: Q_EMIT editTextArtWanted(it->id); break;
    case ItemType::Group: {
        if (isBarcode(it)) {
            m_ed->select(it->id);
            Q_EMIT editBarcodeWanted();
            break;
        }
        const QString child = itemAt(toPage(e->position()), true);
        if (!child.isEmpty()) m_ed->select(child);
        break;
    }
    default: break;
    }
}

void Canvas::updateCursorShape(const QPointF &view)
{
    const Tool t = m_ed->tool();
    if (t == Tool::Link) { viewport()->setCursor(pitcherCursor()); return; }
    if (t == Tool::FormatPainter) { viewport()->setCursor(painterCursor()); return; }
    if (t != Tool::Select) { viewport()->setCursor(Qt::CrossCursor); return; }
    const Hit h = hitTest(view);
    switch (h.kind) {
    case HitKind::Handle:
    case HitKind::Crop: {
        // Choose a resize cursor matching the handle's direction on screen.
        static const Qt::CursorShape shapes[4] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
        double ang = h.index * 45.0;
        if (Item *it = m_ed->single()) ang += it->rotation;
        const int k = int(std::round(std::fmod(ang + 3600, 180.0) / 45.0)) % 4;
        viewport()->setCursor(shapes[k]);
        return;
    }
    case HitKind::Rotate: viewport()->setCursor(rotateCursor()); return;
    case HitKind::LineEnd:
    case HitKind::LineBend:
    case HitKind::Adjust: viewport()->setCursor(Qt::PointingHandCursor); return;
    case HitKind::Point: viewport()->setCursor(Qt::SizeAllCursor); return;
    case HitKind::WrapPoint: viewport()->setCursor(Qt::SizeAllCursor); return;
    case HitKind::WrapEdge: viewport()->setCursor(Qt::CrossCursor); return;
    case HitKind::PointEdge: viewport()->setCursor(Qt::CrossCursor); return;
    case HitKind::Overflow:
    case HitKind::LinkNext:
    case HitKind::LinkPrev: viewport()->setCursor(Qt::PointingHandCursor); return;
    case HitKind::ColBorder: viewport()->setCursor(Qt::SplitHCursor); return;
    case HitKind::RowBorder: viewport()->setCursor(Qt::SplitVCursor); return;
    case HitKind::Guide: viewport()->setCursor(h.row == 0 ? Qt::SplitVCursor : Qt::SplitHCursor); return;
    case HitKind::Item: {
        Item *it = m_ed->doc()->item(h.id);
        if (m_ed->cropItem == h.id) { viewport()->setCursor(Qt::OpenHandCursor); return; }
        if (it && (h.textInterior || (m_ed->isEditingText() && m_ed->textTarget().itemId == h.id && it->type() == ItemType::Shape)) && !it->locked)
            viewport()->setCursor(Qt::IBeamCursor);
        else viewport()->setCursor(it && it->locked ? Qt::ArrowCursor : Qt::SizeAllCursor);
        return;
    }
    default: viewport()->setCursor(Qt::ArrowCursor); return;
    }
}

void Canvas::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const double f = std::pow(1.0015, e->angleDelta().y());
        m_fit = Fit::None;
        setZoom(m_zoom * f, e->position());
        return;
    }
    if (e->modifiers() & Qt::ShiftModifier) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - e->angleDelta().y());
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

// ---------------- keyboard ----------------
void Canvas::moveCaret(QTextCursor::MoveOperation op, bool select, int n)
{
    QTextCursor c = m_ed->cursor();
    c.movePosition(op, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor, n);
    m_ed->desiredX = -1;
    m_ed->setCursor(c);
}

void Canvas::verticalCaret(int dir, bool select)
{
    const auto &tt = m_ed->textTarget();
    Item *it = m_ed->doc()->item(tt.itemId);
    if (!it) return;
    PaintContext ctx = paintContext();
    const StoryLayout *lay = nullptr;
    if (it->type() == ItemType::Text) lay = m_ed->cache().textFrame(*m_ed->doc(), *static_cast<TextItem *>(it), m_ed->surfacePageNumber(), ctx.opt).layout;
    else if (it->type() == ItemType::Shape) lay = Renderer::shapeTextLayout(ctx, *static_cast<ShapeItem *>(it), nullptr);
    else if (it->type() == ItemType::Table) lay = Renderer::cellLayout(ctx, *static_cast<TableItem *>(it), tt.row, tt.col, nullptr);
    if (!lay) return;
    QTextCursor c = m_ed->cursor();
    if (m_ed->desiredX < 0) {
        int f;
        QRectF r;
        if (lay->caretRect(c.position(), &f, &r)) m_ed->desiredX = r.x();
    }
    const int np = lay->moveVertical(c.position(), dir, m_ed->desiredX);
    if (np == c.position() && it->type() == ItemType::Table) {
        // Up/Down past the cell edge moves to the cell above/below.
        auto *t = static_cast<TableItem *>(it);
        const int nr = tt.row + dir;
        if (nr >= 0 && nr < t->rows) m_ed->beginTextEdit(t->id, dir > 0 ? 0 : -1, nr, tt.col);
        return;
    }
    const double keep = m_ed->desiredX;
    c.setPosition(np, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    m_ed->setCursor(c);
    m_ed->desiredX = keep;
}

void Canvas::handleTextKey(QKeyEvent *e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const auto &tt = m_ed->textTarget();
    Item *it = m_ed->doc()->item(tt.itemId);
    auto lineOp = [&](bool end) {
        PaintContext ctx = paintContext();
        const StoryLayout *lay = nullptr;
        if (it && it->type() == ItemType::Text) lay = m_ed->cache().textFrame(*m_ed->doc(), *static_cast<TextItem *>(it), m_ed->surfacePageNumber(), ctx.opt).layout;
        else if (it && it->type() == ItemType::Shape) lay = Renderer::shapeTextLayout(ctx, *static_cast<ShapeItem *>(it), nullptr);
        else if (it && it->type() == ItemType::Table) lay = Renderer::cellLayout(ctx, *static_cast<TableItem *>(it), tt.row, tt.col, nullptr);
        QTextCursor c = m_ed->cursor();
        const int p = lay ? (end ? lay->lineEnd(c.position()) : lay->lineStart(c.position())) : c.position();
        c.setPosition(p, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        m_ed->desiredX = -1;
        m_ed->setCursor(c);
    };
    switch (e->key()) {
    case Qt::Key_Left: moveCaret(ctrl ? QTextCursor::WordLeft : QTextCursor::PreviousCharacter, shift); return;
    case Qt::Key_Right: moveCaret(ctrl ? QTextCursor::NextWord : QTextCursor::NextCharacter, shift); return;
    case Qt::Key_Up: if (ctrl) moveCaret(QTextCursor::PreviousBlock, shift); else verticalCaret(-1, shift); return;
    case Qt::Key_Down: if (ctrl) moveCaret(QTextCursor::NextBlock, shift); else verticalCaret(1, shift); return;
    case Qt::Key_Home: if (ctrl) moveCaret(QTextCursor::Start, shift); else lineOp(false); return;
    case Qt::Key_End: if (ctrl) moveCaret(QTextCursor::End, shift); else lineOp(true); return;
    case Qt::Key_PageUp: for (int i = 0; i < 10; ++i) verticalCaret(-1, shift); return;
    case Qt::Key_PageDown: for (int i = 0; i < 10; ++i) verticalCaret(1, shift); return;
    case Qt::Key_Escape: m_ed->endTextEdit(); return;
    case Qt::Key_Backspace:
        m_ed->editText([&](QTextCursor &c) {
            if (c.hasSelection()) c.removeSelectedText();
            else if (ctrl) { c.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor); c.removeSelectedText(); }
            else {
                // Backspace at the start of a list item removes the bullet first.
                if (c.atBlockStart() && c.block().textList()) {
                    QTextBlock b = c.block();
                    b.textList()->remove(b);
                    QTextBlockFormat bf = b.blockFormat();
                    bf.setTextIndent(0);
                    c.setBlockFormat(bf);
                } else c.deletePreviousChar();
            }
        });
        return;
    case Qt::Key_Delete:
        m_ed->editText([&](QTextCursor &c) {
            if (c.hasSelection()) c.removeSelectedText();
            else if (ctrl) { c.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor); c.removeSelectedText(); }
            else c.deleteChar();
        });
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        m_ed->autoCorrectWord();   // a word ends here too
        m_ed->autoFormatWord();
        m_ed->editText([&](QTextCursor &c) {
            if (shift) { c.insertText(QString(QChar::LineSeparator)); return; }
            // An empty list item ends the list.
            if (c.block().textList() && c.block().text().isEmpty()) {
                QTextBlock b = c.block();
                b.textList()->remove(b);
                QTextBlockFormat bf = b.blockFormat();
                bf.setTextIndent(0);
                bf.setLeftMargin(std::max(0.0, bf.leftMargin() - 18));
                c.setBlockFormat(bf);
                return;
            }
            const QString style = c.blockFormat().stringProperty(tp::StyleName);
            const TextStyle *ts = m_ed->doc()->style(style);
            const bool atEnd = c.atBlockEnd();
            QTextBlockFormat nbf = c.blockFormat();
            nbf.clearProperty(tp::DropCapLines);
            nbf.clearProperty(tp::StartInNextBox);
            QTextCharFormat ncf = c.charFormat();
            if (ts && atEnd && !ts->next.isEmpty() && ts->next != style) {
                if (const TextStyle *nx = m_ed->doc()->style(ts->next)) {
                    tp::setProperties(nbf, nx->blk);
                    nbf.setProperty(tp::StyleName, nx->name);
                    ncf = nx->chr;
                }
            }
            c.insertBlock(nbf, ncf);
            c.setCharFormat(ncf);
        });
        return;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        if (it && it->type() == ItemType::Table) {
            auto *t = static_cast<TableItem *>(it);
            int r = tt.row, c = tt.col;
            const bool back = e->key() == Qt::Key_Backtab || shift;
            do {
                if (back) { if (--c < 0) { c = t->cols - 1; --r; } }
                else if (++c >= t->cols) { c = 0; ++r; }
            } while (r >= 0 && r < t->rows && t->cell(r, c).covered);
            if (r < 0) return;
            if (r >= t->rows) {
                // Tab in the last cell adds a row.
                m_ed->change(QCoreApplication::translate("Canvas", "Insert Row"), [&] {
                    t->rows += 1;
                    t->rowH << t->rowH.last();
                    for (int k = 0; k < t->cols; ++k) {
                        TableCell nc;
                        nc.storyId = m_ed->doc()->createStory();
                        const TableCell &above = t->cell(t->rows - 2, k);
                        nc.border = above.border;
                        nc.fill = above.fill;
                        nc.margins = above.margins;
                        nc.vertical = above.vertical;
                        nc.hyphenate = above.hyphenate;
                        nc.hyphenZone = above.hyphenZone;
                        t->cells.push_back(nc);
                    }
                    t->syncRect();
                });
                r = t->rows - 1;
                c = 0;
            }
            m_ed->beginTextEdit(t->id, 0, r, c);
            QTextCursor cur = m_ed->cursor();
            cur.select(QTextCursor::Document);
            m_ed->setCursor(cur);
            return;
        }
        m_ed->insertText(QStringLiteral("\t"));
        return;
    case Qt::Key_A:
        if (ctrl) {
            QTextCursor c = m_ed->cursor();
            c.select(QTextCursor::Document);
            m_ed->setCursor(c);
            return;
        }
        break;
    default: break;
    }
    const QString text = e->text();
    if (!text.isEmpty() && !ctrl && (text[0].isPrint() || text[0] == QChar(0x00A0))) {
        m_ed->typeText(text);
        return;
    }
    e->ignore();
}

void Canvas::keyPressEvent(QKeyEvent *e)
{
    if (m_ed->isEditingText()) {
        handleTextKey(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool alt = e->modifiers() & Qt::AltModifier;
    const double step = alt ? 1.0 / ppp() : Settings::get().nudge();
    // Esc or Enter ends a curve or freeform, keeping what's drawn.
    if (m_ed->tool() == Tool::Freeform && !m_freePts.isEmpty() && (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        finishFreeform(false);
        return;
    }
    switch (e->key()) {
    case Qt::Key_Escape:
        if (m_ed->tool() != Tool::Select) { m_ed->setTool(Tool::Select); return; }
        if (!m_ed->cropItem.isEmpty()) { m_ed->setCropItem(QString()); return; }
        if (!m_ed->pointsItem.isEmpty()) { m_ed->setPointsItem(QString()); return; }
        if (!m_ed->wrapItem.isEmpty()) { m_ed->setWrapItem(QString()); return; }
        if (m_ed->hasCellBlock()) { m_ed->clearCellBlock(); return; }   // the table stays selected
        if (m_ed->selection().size() == 1) {
            const auto loc = m_ed->doc()->find(m_ed->selection().first());
            if (loc.parent) { m_ed->select(loc.parent->id); return; }
        }
        m_ed->clearSelection();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        m_ed->deleteSelection();
        return;
    case Qt::Key_Left: m_ed->moveSelectionBy(-step, 0, QCoreApplication::translate("Canvas", "Nudge")); return;
    case Qt::Key_Right: m_ed->moveSelectionBy(step, 0, QCoreApplication::translate("Canvas", "Nudge")); return;
    case Qt::Key_Up: m_ed->moveSelectionBy(0, -step, QCoreApplication::translate("Canvas", "Nudge")); return;
    case Qt::Key_Down: m_ed->moveSelectionBy(0, step, QCoreApplication::translate("Canvas", "Nudge")); return;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: {
        const ItemList &l = m_ed->surfaceItems();
        if (l.empty()) return;
        int idx = -1;
        if (!m_ed->selection().isEmpty())
            for (int i = 0; i < int(l.size()); ++i)
                if (l[i]->id == m_ed->selection().first()) idx = i;
        idx = (e->key() == Qt::Key_Backtab || shift) ? (idx <= 0 ? int(l.size()) - 1 : idx - 1) : (idx + 1) % int(l.size());
        m_ed->select(l[idx]->id);
        return;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_F2:
        if (const auto block = m_ed->cellBlock(); block.range.valid()) {
            m_ed->beginTextEdit(block.itemId, -1, block.anchorRow, block.anchorCol);
            return;
        }
        if (Item *it = m_ed->single(); it && (it->type() == ItemType::Text || it->type() == ItemType::Shape || it->type() == ItemType::Table)) {
            m_ed->beginTextEdit(it->id);
            return;
        }
        if (Item *it = m_ed->single(); it && it->type() == ItemType::TextArt) Q_EMIT editTextArtWanted(it->id);
        return;
    case Qt::Key_PageDown:
        if (ctrl) { m_ed->setCurrentPage(m_ed->currentPage() + 1); return; }
        break;
    case Qt::Key_PageUp:
        if (ctrl) { m_ed->setCurrentPage(m_ed->currentPage() - 1); return; }
        break;
    default: break;
    }
    // Typing with a text box selected starts editing it.
    const QString text = e->text();
    if (!ctrl && !text.isEmpty() && text[0].isPrint()) {
        if (Item *it = m_ed->single(); it && (it->type() == ItemType::Text || it->type() == ItemType::Shape)) {
            m_ed->beginTextEdit(it->id);
            m_ed->typeText(text);
            return;
        }
    }
    QAbstractScrollArea::keyPressEvent(e);
}

void Canvas::inputMethodEvent(QInputMethodEvent *e)
{
    if (m_ed->isEditingText() && !e->commitString().isEmpty()) m_ed->insertText(e->commitString());
    e->accept();
}

QVariant Canvas::inputMethodQuery(Qt::InputMethodQuery q) const
{
    switch (q) {
    case Qt::ImEnabled: return m_ed->isEditingText();
    case Qt::ImCursorRectangle: return caretViewRect().translated(viewport()->pos()).toRect();
    default: return QAbstractScrollArea::inputMethodQuery(q);
    }
}

void Canvas::focusInEvent(QFocusEvent *e)
{
    QAbstractScrollArea::focusInEvent(e);
    if (m_ed->isEditingText()) m_caretTimer.start();
    announceFocus();   // the selected object, not just "Page"
}

QRect Canvas::screenRect(const QRectF &pageRect) const
{
    const QRectF v(pageToView(pageRect.topLeft()), pageToView(pageRect.bottomRight()));
    return QRect(viewport()->mapToGlobal(v.normalized().topLeft().toPoint()), v.normalized().size().toSize());
}

QRect Canvas::caretScreenRect(int pos) const
{
    QLineF line;
    QString frame;
    if (!caretInfo(&line, &frame, pos)) return QRect();
    const QRectF v = QRectF(pageToView(line.p1()), pageToView(line.p2())).normalized().adjusted(-1, 0, 1, 0);
    return QRect(viewport()->mapToGlobal(v.topLeft().toPoint()), v.size().toSize());
}

void Canvas::announceFocus()
{
    if (!QAccessible::isActive() || !hasFocus()) return;
    auto *page = static_cast<CanvasAccessible *>(QAccessible::queryAccessibleInterface(this));
    if (!page) return;
    if (QAccessibleInterface *obj = page->focusChild()) {
        QAccessibleEvent sel(obj, QAccessible::Selection);
        QAccessible::updateAccessibility(&sel);
        QAccessibleEvent focus(obj, QAccessible::Focus);
        QAccessible::updateAccessibility(&focus);
    } else {
        QAccessibleEvent focus(this, QAccessible::Focus);
        QAccessible::updateAccessibility(&focus);
    }
}

void Canvas::announceText(bool changed)
{
    if (!QAccessible::isActive() || !hasFocus() || !m_ed->isEditingText()) return;
    auto *page = static_cast<CanvasAccessible *>(QAccessible::queryAccessibleInterface(this));
    PageObjectAccessible *obj = page ? page->objectInterface(m_ed->textTarget().itemId) : nullptr;
    if (!obj) return;
    if (changed) {
        QAccessibleTextUpdateEvent ev(obj, 0, QString(), QString());
        QAccessible::updateAccessibility(&ev);
    }
    QAccessibleTextCursorEvent ev(obj, m_ed->cursor().position());
    QAccessible::updateAccessibility(&ev);
}

void Canvas::focusOutEvent(QFocusEvent *e)
{
    QAbstractScrollArea::focusOutEvent(e);
    m_caretTimer.stop();
    viewport()->update();
}

void Canvas::contextMenuEvent(QContextMenuEvent *e) { Q_EMIT contextMenuWanted(e->globalPos()); }

bool Canvas::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::Leave) {
        m_hRuler->setMouse(-1e9);
        m_vRuler->setMouse(-1e9);
    }
    return QAbstractScrollArea::viewportEvent(e);
}

void Canvas::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(QString::fromLatin1(kExtraContentMime)) || e->mimeData()->hasUrls() || e->mimeData()->hasImage() || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void Canvas::dragMoveEvent(QDragMoveEvent *e) { e->acceptProposedAction(); }

void Canvas::dropEvent(QDropEvent *e)
{
    const QPointF page = toPage(viewport()->mapFrom(this, e->position().toPoint()));
    if (e->mimeData()->hasFormat(QString::fromLatin1(kExtraContentMime))) {
        // Extra Content dropped on the page.
        const QStringList ids = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(kExtraContentMime))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        m_ed->beginChange(QCoreApplication::translate("Canvas", "Place Extra Content"));
        QPointF at = toPage(e->position());   // the drop came to the viewport
        for (const QString &id : ids) {
            m_ed->placeExtra(id, at);
            at += QPointF(18, 18);
        }
        m_ed->endChange();
        e->acceptProposedAction();
        return;
    }
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    if (files.size() == 1 && (files.first().endsWith(".jpub", Qt::CaseInsensitive) || files.first().endsWith(".pub", Qt::CaseInsensitive))) {
        Q_EMIT openFileWanted(files.first());
        e->acceptProposedAction();
        return;
    }
    if (!files.isEmpty()) {
        Q_EMIT insertFilesWanted(files, page);
        e->acceptProposedAction();
        return;
    }
    if (e->mimeData()->hasText()) {
        auto t = m_ed->newTextBox(QRectF(page, QSizeF(216, 108)), e->mimeData()->text());
        m_ed->addItem(t);
        e->acceptProposedAction();
    }
}

void Canvas::startGuideDrag(Qt::Orientation o, const QPoint &globalPos)
{
    m_ed->beginChange(QCoreApplication::translate("Canvas", "Add Ruler Guide"));
    RulerGuides &g = m_ed->surface()->guides;
    const QPointF page = toPage(viewport()->mapFromGlobal(globalPos));
    m_guideOrient = o;
    m_guidePos = o == Qt::Horizontal ? page.y() : page.x();
    auto &v = o == Qt::Horizontal ? g.h : g.v;
    v << m_guidePos;
    m_guideIndex = v.size() - 1;
    m_drag = Drag::Guide;
}

void Canvas::updateGuideDrag(const QPoint &globalPos)
{
    if (m_drag != Drag::Guide) return;
    QMouseEvent me(QEvent::MouseMove, viewport()->mapFromGlobal(globalPos), globalPos, Qt::NoButton, Qt::LeftButton, QGuiApplication::keyboardModifiers());
    mouseMoveEvent(&me);
}

void Canvas::endGuideDrag(const QPoint &globalPos)
{
    if (m_drag != Drag::Guide) return;
    QMouseEvent me(QEvent::MouseButtonRelease, viewport()->mapFromGlobal(globalPos), globalPos, Qt::LeftButton, Qt::NoButton, QGuiApplication::keyboardModifiers());
    mouseReleaseEvent(&me);
}

// ======================= Ruler =======================
Ruler::Ruler(Canvas *c, Qt::Orientation o) : QWidget(c), m_c(c), m_o(o)
{
    setMouseTracking(true);
}

QSize Ruler::sizeHint() const { return m_o == Qt::Horizontal ? QSize(100, kRuler) : QSize(kRuler, 100); }

double Ruler::zero() const
{
    const QPointF z = m_c->editor()->doc()->rulerZero;
    return m_o == Qt::Horizontal ? z.x() : z.y();
}

// One undo step, unless a gesture holds the change open, as a drag does.
void Ruler::setZero(double pagePt)
{
    if (pagePt == zero()) return;
    Editor *ed = m_c->editor();
    ed->beginChange(zeroLabel());
    QPointF &z = ed->doc()->rulerZero;
    (m_o == Qt::Horizontal ? z.rx() : z.ry()) = pagePt;
    ed->notifyLive();   // the rulers and the status bar follow, while dragging too
    ed->endChange();
}

double Ruler::valueAt(double pagePt) const { return Settings::get().toUnit(pagePt - zero()); }

// Where the pointer is in the page area, whichever ruler's coordinates it comes in.
QPointF Ruler::viewAt(const QMouseEvent *e) const { return m_c->viewport()->mapFromGlobal(e->globalPosition().toPoint()); }

// Where along this ruler the pointer is on the page, in points from the page's corner.
double Ruler::pageAlong(const QMouseEvent *e) const
{
    const QPointF p = m_c->toPage(viewAt(e));
    return m_o == Qt::Horizontal ? p.x() : p.y();
}

// A zero-point gesture is not for the page's pop-up menu.
void Ruler::contextMenuEvent(QContextMenuEvent *e)
{
    e->setAccepted(m_dragZero || movesRulerZero(Qt::RightButton, e->modifiers()));
}

Ruler::TextRuler Ruler::textRuler() const
{
    TextRuler tr;
    Editor *ed = m_c->editor();
    if (m_o != Qt::Horizontal || !ed->isEditingText()) return tr;
    Item *it = ed->doc()->item(ed->textTarget().itemId);
    if (!it || it->rotation != 0 || it->flipH) return tr;
    double left = 0, right = it->rect.width();
    if (it->type() == ItemType::Text) {
        auto *t = static_cast<TextItem *>(it);
        if (t->vertical) return tr;
        left = t->insets.left();
        right = t->rect.width() - t->insets.right();
        if (t->columns > 1) right = left + (right - left - t->columnGap * (t->columns - 1)) / t->columns;
    } else if (it->type() == ItemType::Table) {
        auto *t = static_cast<TableItem *>(it);
        const QRectF cr = t->cellRect(ed->textTarget().row, ed->textTarget().col);
        const auto &cell = t->cell(ed->textTarget().row, ed->textTarget().col);
        if (cell.vertical) return tr;
        left = cr.left() + cell.margins.left();
        right = cr.right() - cell.margins.right();
    } else if (it->type() == ItemType::Shape) {
        auto *s = static_cast<ShapeItem *>(it);
        const QRectF r = shapeTextRect(s->shape, s->rect.size(), s->adj);
        left = r.left() + s->insets.left();
        right = r.right() - s->insets.right();
    }
    tr.on = true;
    tr.left = it->rect.left() + left;
    tr.right = it->rect.left() + right;
    const QTextBlockFormat bf = ed->cursor().blockFormat();
    tr.leftIndent = bf.leftMargin();
    tr.firstIndent = bf.leftMargin() + bf.textIndent();
    tr.rightIndent = bf.rightMargin();
    tr.tabs = bf.tabPositions();
    return tr;
}

void Ruler::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const bool dark = darkUi();
    // In high contrast, the system's window and text colors.
    const bool contrast = uiHighContrast();
    const QColor bg = contrast ? palette().color(QPalette::Window) : dark ? QColor(0x24, 0x28, 0x2F) : QColor(0xF3, 0xF4, 0xF6);
    const QColor fg = contrast ? palette().color(QPalette::WindowText) : dark ? QColor(0xC8, 0xCC, 0xD3) : QColor(0x4A, 0x50, 0x5A);
    p.fillRect(rect(), bg);
    const bool horiz = m_o == Qt::Horizontal;
    Settings &st = Settings::get();
    const QPointF origin = m_c->currentOrigin();
    // Page extent highlighted in white.
    const QSizeF ps = m_c->editor()->surfaceSize();
    const QPointF a = m_c->toView(origin), b = m_c->toView(origin + QPointF(ps.width(), ps.height()));
    const QColor paper = contrast ? bg : dark ? QColor(0x3A, 0x40, 0x4A) : Qt::white;
    if (horiz) p.fillRect(QRectF(a.x(), 3, b.x() - a.x(), height() - 6), paper);
    else p.fillRect(QRectF(3, a.y(), width() - 6, b.y() - a.y()), paper);
    // Selection extent.
    const QRectF sb = m_c->editor()->selectionBounds();
    if (!sb.isNull()) {
        const QPointF s1 = m_c->pageToView(sb.topLeft()), s2 = m_c->pageToView(sb.bottomRight());
        const QColor sc = dark ? QColor(80, 110, 160) : QColor(205, 222, 245);
        if (horiz) p.fillRect(QRectF(s1.x(), 3, s2.x() - s1.x(), height() - 6), sc);
        else p.fillRect(QRectF(3, s1.y(), width() - 6, s2.y() - s1.y()), sc);
    }
    const TextRuler tr = textRuler();
    if (tr.on) {
        const double x1 = m_c->pageToView(QPointF(tr.left, 0)).x(), x2 = m_c->pageToView(QPointF(tr.right, 0)).x();
        p.fillRect(QRectF(x1, 3, x2 - x1, height() - 6), contrast ? bg : dark ? QColor(0x55, 0x5D, 0x6A) : QColor(255, 255, 255));
        p.setPen(contrast ? fg : QColor(150, 150, 150));
        p.drawRect(QRectF(x1, 3, x2 - x1, height() - 6));
    }
    // Ticks.
    const double ppp = m_c->ppp();
    const double unitPt = st.fromUnit(1.0);
    double major = unitPt;
    while (major * ppp < 40) major *= (st.unit() == Unit::Point ? 2 : (st.unit() == Unit::Inch ? 2 : 2));
    while (major * ppp > 160 && major > unitPt / 16) major /= 2;
    int subdiv = st.unit() == Unit::Inch ? 8 : 10;
    while (subdiv > 1 && major / subdiv * ppp < 5) subdiv /= 2;
    const double startScene = horiz ? m_c->toScene(QPointF(0, 0)).x() : m_c->toScene(QPointF(0, 0)).y();
    const double endScene = horiz ? m_c->toScene(QPointF(width(), 0)).x() : m_c->toScene(QPointF(0, height())).y();
    const double pageO = horiz ? origin.x() : origin.y();
    const double o = pageO + zero();   // where the numbers start
    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    p.setPen(fg);
    const double step = major / subdiv;
    const long first = long(std::floor((startScene - o) / step)), last = long(std::ceil((endScene - o) / step));
    for (long i = first; i <= last; ++i) {
        const double scene = o + i * step;
        const double v = horiz ? m_c->toView(QPointF(scene, 0)).x() : m_c->toView(QPointF(0, scene)).y();
        const bool isMajor = i % subdiv == 0;
        const bool isHalf = subdiv % 2 == 0 && i % (subdiv / 2) == 0;
        const int len = isMajor ? 9 : isHalf ? 6 : 3;
        const int extent = horiz ? height() : width();
        if (horiz) p.drawLine(QPointF(v, extent - len), QPointF(v, extent));
        else p.drawLine(QPointF(extent - len, v), QPointF(extent, v));
        if (isMajor) {
            const double val = valueAt(scene - pageO);
            const QString label = QString::number(std::abs(std::round(val * 100) / 100), 'g', 4);
            if (horiz) p.drawText(QPointF(v + 2, 9), label);
            else {
                p.save();
                p.translate(9, v + 2);
                p.rotate(-90);
                p.drawText(QPointF(-QFontMetricsF(f).horizontalAdvance(label) - 2, 0), label);
                p.restore();
            }
        }
    }
    // Text ruler markers.
    if (tr.on) {
        auto vx = [&](double pagex) { return m_c->pageToView(QPointF(pagex, 0)).x(); };
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(60, 60, 60), 1));
        p.setBrush(QColor(230, 230, 230));
        const double fx = vx(tr.left + tr.firstIndent), lx = vx(tr.left + tr.leftIndent), rx = vx(tr.right - tr.rightIndent);
        p.drawPolygon(QPolygonF({QPointF(fx - 4, 3), QPointF(fx + 4, 3), QPointF(fx, 8)}));
        p.drawPolygon(QPolygonF({QPointF(lx - 4, height() - 7), QPointF(lx + 4, height() - 7), QPointF(lx, height() - 12)}));
        p.drawRect(QRectF(lx - 4, height() - 7, 8, 4));
        p.drawPolygon(QPolygonF({QPointF(rx - 4, height() - 7), QPointF(rx + 4, height() - 7), QPointF(rx, height() - 12)}));
        p.setPen(QPen(Qt::black, 1.5));
        for (const auto &t : tr.tabs) {
            const double x = vx(tr.left + t.position);
            const double y = height() - 6;
            if (t.type == QTextOption::LeftTab) { p.drawLine(QPointF(x, y - 5), QPointF(x, y)); p.drawLine(QPointF(x, y), QPointF(x + 4, y)); }
            else if (t.type == QTextOption::RightTab) { p.drawLine(QPointF(x, y - 5), QPointF(x, y)); p.drawLine(QPointF(x - 4, y), QPointF(x, y)); }
            else if (t.type == QTextOption::CenterTab) { p.drawLine(QPointF(x, y - 5), QPointF(x, y)); p.drawLine(QPointF(x - 3, y), QPointF(x + 3, y)); }
            else { p.drawLine(QPointF(x, y - 5), QPointF(x, y)); p.drawLine(QPointF(x - 3, y), QPointF(x + 3, y)); p.drawPoint(QPointF(x + 2.5, y - 3)); }
        }
    }
    // Mouse position.
    if (m_mouse > -1e8) {
        p.setPen(QPen(QColor(200, 60, 60), 1));
        const double v = horiz ? m_c->toView(QPointF(m_mouse, 0)).x() : m_c->toView(QPointF(0, m_mouse)).y();
        if (horiz) p.drawLine(QPointF(v, 0), QPointF(v, height()));
        else p.drawLine(QPointF(0, v), QPointF(width(), v));
    }
    p.setPen(uiHighContrast() ? palette().color(QPalette::WindowText) : dark ? QColor(0x39, 0x41, 0x4D) : QColor(0xD5, 0xDA, 0xE1));
    if (horiz) p.drawLine(0, height() - 1, width(), height() - 1);
    else p.drawLine(width() - 1, 0, width() - 1, height());
}

void Ruler::mousePressEvent(QMouseEvent *e)
{
    if (movesRulerZero(e->button(), e->modifiers())) {
        m_c->editor()->beginChange(zeroLabel());
        m_dragZero = true;
        m_zeroButton = e->button();
        return;
    }
    const TextRuler tr = textRuler();
    if (tr.on && e->button() == Qt::LeftButton) {
        auto vx = [&](double pagex) { return m_c->pageToView(QPointF(pagex, 0)).x(); };
        const double x = e->position().x();
        const double fx = vx(tr.left + tr.firstIndent), lx = vx(tr.left + tr.leftIndent), rx = vx(tr.right - tr.rightIndent);
        const double y = e->position().y();
        m_dragMarker = -1;
        if (std::abs(x - fx) < 5 && y < height() / 2) m_dragMarker = 0;
        else if (std::abs(x - lx) < 5) m_dragMarker = 1;
        else if (std::abs(x - rx) < 5) m_dragMarker = 2;
        else {
            for (int i = 0; i < tr.tabs.size(); ++i)
                if (std::abs(x - vx(tr.left + tr.tabs[i].position)) < 4) { m_dragMarker = 10 + i; break; }
        }
        if (m_dragMarker < 0 && x > vx(tr.left) && x < vx(tr.right)) {
            // Click inside the text area adds a left tab stop.
            const double pos = m_c->toPage(viewAt(e)).x() - tr.left;
            QTextBlockFormat bf;
            QList<QTextOption::Tab> tabs = tr.tabs;
            tabs << QTextOption::Tab(pos, QTextOption::LeftTab);
            std::sort(tabs.begin(), tabs.end(), [](const QTextOption::Tab &a, const QTextOption::Tab &b) { return a.position < b.position; });
            bf.setTabPositions(tabs);
            m_c->editor()->mergeBlockFormat(bf, QCoreApplication::translate("Canvas", "Set Tab"));
            return;
        }
        if (m_dragMarker >= 0) {
            m_c->editor()->beginChange(QCoreApplication::translate("Canvas", "Indent"));
            return;
        }
    }
    if (e->button() == Qt::LeftButton) {
        m_dragGuide = true;
        m_c->startGuideDrag(m_o == Qt::Horizontal ? Qt::Horizontal : Qt::Vertical, e->globalPosition().toPoint());
    }
}

void Ruler::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragZero) {
        setZero(pageAlong(e));
        return;
    }
    if (m_dragGuide) {
        m_c->updateGuideDrag(e->globalPosition().toPoint());
        return;
    }
    if (m_dragMarker >= 0) {
        const TextRuler tr = textRuler();
        Editor *ed = m_c->editor();
        const double pagex = m_c->toPage(viewAt(e)).x();
        QTextBlockFormat bf = ed->cursor().blockFormat();
        if (m_dragMarker == 0) bf.setTextIndent(pagex - tr.left - bf.leftMargin());
        else if (m_dragMarker == 1) { const double first = bf.leftMargin() + bf.textIndent(); bf.setLeftMargin(std::max(0.0, pagex - tr.left)); bf.setTextIndent(first - bf.leftMargin()); }
        else if (m_dragMarker == 2) bf.setRightMargin(std::max(0.0, tr.right - pagex));
        else {
            QList<QTextOption::Tab> tabs = bf.tabPositions();
            const int i = m_dragMarker - 10;
            if (i < tabs.size()) {
                if (e->position().y() > height() + 20) { tabs.removeAt(i); m_dragMarker = -1; }
                else tabs[i].position = std::max(0.0, pagex - tr.left);
            }
            bf.setTabPositions(tabs);
        }
        QTextCursor c = ed->cursor();
        c.setBlockFormat(bf);
        ed->notifyLive();
        update();
        return;
    }
    const QPointF scene = m_c->toScene(viewAt(e));
    m_mouse = m_o == Qt::Horizontal ? scene.x() : scene.y();
    const TextRuler tr = textRuler();
    setCursor(tr.on ? Qt::ArrowCursor : (m_o == Qt::Horizontal ? Qt::SplitVCursor : Qt::SplitHCursor));
    update();
}

void Ruler::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragZero && e->button() == m_zeroButton) {
        m_dragZero = false;
        setZero(pageAlong(e));
        m_c->editor()->endChange();
        return;
    }
    if (m_dragGuide) {
        m_dragGuide = false;
        m_c->endGuideDrag(e->globalPosition().toPoint());
        return;
    }
    if (m_dragMarker >= 0 || m_c->editor()->undoStack()) {
        if (m_dragMarker >= 0) m_c->editor()->endChange();
        m_dragMarker = -1;
    }
}

// Double-clicking the horizontal ruler while typing opens the Tabs settings;
// otherwise a double-click puts this ruler's zero back at the page's corner.
void Ruler::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (m_o == Qt::Horizontal && m_c->editor()->isEditingText()) Q_EMIT m_c->tabsDialogWanted();
    else if (resetsZeros(e->button())) setZero(0);
}

} // namespace jp
