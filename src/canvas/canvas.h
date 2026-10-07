#pragma once
// The workspace: gray scratch area, the current page or two-page spread,
// rulers, guides, selection handles and all direct manipulation.

#include "app/editor.h"

#include <QAbstractScrollArea>
#include <QPainterPath>
#include <QElapsedTimer>
#include <QTimer>

namespace jp {

class Canvas;

class Ruler : public QWidget {
    Q_OBJECT
public:
    Ruler(Canvas *c, Qt::Orientation o);
    QSize sizeHint() const override;
    void setMouse(double scenePos) { m_mouse = scenePos; update(); }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
    struct TextRuler { bool on = false; double left = 0, right = 0; double firstIndent = 0, leftIndent = 0, rightIndent = 0; QList<QTextOption::Tab> tabs; QTransform map; };
    TextRuler textRuler() const;
    Canvas *m_c;
    Qt::Orientation m_o;
    double m_mouse = -1e9;
    int m_dragMarker = -1;     // 0 first-line, 1 left, 2 right, 10+ tab index
    bool m_dragGuide = false;
};

class Canvas : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit Canvas(Editor *ed, QWidget *parent = nullptr);

    Editor *editor() const { return m_ed; }
    double zoom() const { return m_zoom; }
    double ppp() const;                                 // device-independent pixels per point
    void setZoom(double z, const QPointF &anchorView = QPointF(-1, -1));
    enum class Fit { None, WholePage, PageWidth, Selection };
    void zoomToFit(Fit f);
    Fit fitMode() const { return m_fit; }

    struct Slot { int page = -1; QPointF origin; QSizeF size; };
    QVector<Slot> slots() const;
    QPointF currentOrigin() const;                      // scene origin of the editable surface
    QRectF sceneRect() const;
    QPointF toScene(const QPointF &view) const;
    QPointF toView(const QPointF &scene) const;
    QPointF toPage(const QPointF &view) const { return toScene(view) - currentOrigin(); }
    QPointF pageToView(const QPointF &page) const { return toView(page + currentOrigin()); }

    void ensureVisible(const QRectF &pageRect);
    void scrollToPage();
    QPointF lastMousePage() const { return m_lastPage; }
    void startGuideDrag(Qt::Orientation o, const QPoint &globalPos);
    void updateGuideDrag(const QPoint &globalPos);
    void endGuideDrag(const QPoint &globalPos);
    Ruler *hRuler() const { return m_hRuler; }
    Ruler *vRuler() const { return m_vRuler; }
    void setRulersVisible(bool on);
    QRectF caretViewRect() const;

Q_SIGNALS:
    void zoomChanged(double zoom);
    void mouseMovedPage(const QPointF &pagePos);
    void contextMenuWanted(const QPoint &globalPos);
    void insertPictureWanted(const QString &itemId);   // empty = new picture at the last drawn rect
    void editTextArtWanted(const QString &itemId);
    void editBarcodeWanted();   // a barcode was double-clicked (and selected)
    void tabsDialogWanted();   // the horizontal ruler was double-clicked while typing
    void openFileWanted(const QString &path);
    void insertFilesWanted(const QStringList &paths, const QPointF &pagePos);
    void pictureTabWanted();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool viewportEvent(QEvent *e) override;

public:
    enum class HitKind { None, Item, Handle, Rotate, LineEnd, LineBend, Adjust, Crop, Overflow, LinkPrev, LinkNext, ColBorder, RowBorder, Guide, Point, PointEdge, WrapPoint, WrapEdge };
    struct Hit {
        HitKind kind = HitKind::None;
        QString id;
        int index = -1;
        bool textInterior = false;
        int row = -1, col = -1;   // table cell
    };
    Hit hitTest(const QPointF &view) const;
    QString itemAt(const QPointF &page, bool enterGroups = false, int *row = nullptr, int *col = nullptr, bool *textInterior = nullptr) const;
    int textPosAt(const QString &itemId, const QPointF &page, int row = -1, int col = -1) const;

private:
    enum class Drag { None, Pending, Move, Resize, Rotate, LineEnd, LineBend, Free, Adjust, Marquee, Draw, TextSelect, TextMove, Guide, Crop, CropMove, ColResize, RowResize, Pan, Point, WrapPoint };

    void updateScrollBars();
    void paintPageSlot(QPainter &p, const Slot &s, bool current);
    void paintGuides(QPainter &p, const Slot &s);
    void paintCatalogArea(QPainter &p, const Slot &s);
    void paintOverlay(QPainter &p);
    void paintHandles(QPainter &p, Item *it);
    QVector<QPointF> handlePoints(const Item *it) const;     // 8 resize handles in view coordinates
    QPointF rotateHandle(const Item *it) const;
    PaintContext paintContext() const;
    bool caretInfo(QLineF *pageLine, QString *frameId, int atPos = -1) const;   // atPos: another place in the text
    QPointF snapPoint(const QPointF &page, QVector<QLineF> *lines, const QSet<QString> &exclude) const;
    QPointF snapMove(const QRectF &box, QVector<QLineF> *lines, const QSet<QString> &exclude) const;
    void collectSnapTargets(QVector<double> &xs, QVector<double> &ys, const QSet<QString> &exclude) const;
    void finishDraw(const QRectF &r, bool clicked);
    void finishFreeform(bool closed);
    void handleTextKey(QKeyEvent *e);
    void moveCaret(QTextCursor::MoveOperation op, bool select, int n = 1);
    void verticalCaret(int dir, bool select);
    void beginResize(const Hit &h, const QPointF &page);
    void updateCursorShape(const QPointF &view);
    void applyFormatPainter(const QString &id);
    QStringList moveSet() const;
    // A connection site near a point: the object under it (or whose frame
    // it's near) and, when close enough to snap, its nearest site.
    struct SiteHit {
        QString over;      // the object whose sites to show
        QString id;        // the object snapped to, or empty
        int site = -1;
        QPointF at;
    };
    SiteHit siteNear(const QPointF &page, const QSet<QString> &exclude) const;

    Editor *m_ed;
    double m_zoom = 1.0;
    Fit m_fit = Fit::WholePage;
    Ruler *m_hRuler, *m_vRuler;
    QWidget *m_corner;
    QTimer m_caretTimer;
    bool m_caretOn = true;

    // drag state
    Drag m_drag = Drag::None;
    QPointF m_pressView, m_pressPage, m_lastPage;
    Qt::KeyboardModifiers m_mods;
    Hit m_hit;
    QPainterPath m_origPath;
    QPolygonF m_origWrap;              // shape outline when a point drag started
    QPointF m_origPoint;                  // that point, frame-local
    QHash<QString, QJsonObject> m_orig;   // item states at drag start
    QRectF m_origBox;
    QRectF m_rubber;
    QVector<QLineF> m_snapLines;
    SiteHit m_siteStart, m_siteHover;   // connection sites while drawing or dragging a line's end
    QVector<QPointF> m_freePts;          // a curve, freeform or scribble being drawn
    double m_rotStart = 0;
    QString m_tip;
    QPointF m_tipPos;
    Qt::Orientation m_guideOrient = Qt::Horizontal;
    int m_guideIndex = -1;
    double m_guidePos = 0;
    bool m_guideOnMaster = false;
    int m_clicks = 0;
    int m_textPress = -1;               // where a text selection drag started
    int m_moveFrom = -1, m_moveTo = -1;  // the selected text being dragged
    int m_movePos = -1;                  // where it would drop
    QElapsedTimer m_clickTimer;
    QPointF m_lastClickView;
    bool m_copyDrag = false;
};

} // namespace jp
