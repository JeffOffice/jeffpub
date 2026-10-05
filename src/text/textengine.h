#pragma once
// Lays out one story across a chain of frames (linked text boxes), each with
// columns, insets, vertical alignment and wrap obstacles. One QTextLayout is
// kept per paragraph; each line is positioned in the frame it landed in. Line
// y-coordinates are offset by frameIndex * kStride so that a single paragraph
// can span frames while staying in one QTextLayout.

#include "core/color.h"
#include "core/items.h"

#include <QDateTime>
#include <QFont>
#include <QPolygonF>
#include <QSizeF>
#include <QTextBlock>
#include <QTextLayout>
#include <memory>

class QPainter;
class QTextDocument;

namespace jp {

class Document;

struct FieldContext {
    const Document *doc = nullptr;
    int pageNumber = 1;           // 1-based
    int pageCount = 1;
    int mergeRecord = -1;         // -1 shows «Field» codes
    int continuedOnPage = 0, continuedFromPage = 0;
    QDateTime now = QDateTime::currentDateTime();
    QString resolve(const QString &code) const;
    QString key() const;
};

struct FrameSpec {
    QSizeF size;
    QMarginsF insets;
    int columns = 1;
    double gap = 9;
    VAlign valign = VAlign::Top;
    bool hyphenate = true;          // insert soft hyphens at allowed break points
    QVector<QPolygonF> obstacles;   // frame-local, already expanded by wrap distances
    FieldContext ctx;
};

struct LayoutEnv {
    ColorScheme colors;
    FontScheme fonts;
    double fontScale = 1.0;
    bool showFieldShading = false;
    QString key() const;
};

struct PaintOptions {
    int selFrom = -1, selTo = -1;   // document positions
    QColor selColor = QColor(51, 153, 255, 90);
    bool showSpecial = false;       // ¶, ·, →
    bool shadeFields = false;
    bool showSpelling = false;
    QVector<QPair<int, int>> misspelled;  // document ranges
};

class StoryLayout {
public:
    static constexpr double kStride = 1.0e6;

    StoryLayout();
    ~StoryLayout();

    void build(const QTextDocument *doc, const QVector<FrameSpec> &frames, const LayoutEnv &env);

    int frameCount() const { return int(m_frames.size()); }
    bool overflow() const { return m_overflow; }
    double usedHeight(int frame) const;      // content height used, including insets
    int firstPosition(int frame) const;      // first document position laid out in frame (-1 if none)
    int lastPosition(int frame) const;

    void paint(QPainter *p, int frame, const PaintOptions &o) const;

    int hitTest(int frame, const QPointF &local) const;
    bool caretRect(int pos, int *frame, QRectF *rect) const;
    int frameOf(int pos) const;
    int moveVertical(int pos, int dir, double x) const;  // dir -1 up, +1 down
    int lineStart(int pos) const;
    int lineEnd(int pos) const;
    QVector<QRectF> rangeRects(int frame, int from, int to) const;
    QVector<QRectF> lineRects(int frame) const;   // every line box in the frame, frame-local
    struct LineInfo { QRectF rect; QString text; QString family; double pointSize = 0; int docStart = 0; };
    QVector<LineInfo> lineInfo(int frame) const;  // for diagnostics (jpubtool layout)

    struct Seg { int docPos, docLen, dispPos, dispLen; };
    struct Line { int frame = -1; int column = 0; QRectF rect; };
    struct Block {
        int docStart = 0, docLen = 0;
        QString disp;
        QVector<Seg> map;
        std::unique_ptr<QTextLayout> tl;
        QVector<Line> lines;
        QString marker; QFont markerFont; QColor markerColor; double markerX = 0;
        QString dropText; QFont dropFont; QColor dropColor; int dropLines = 0; double dropWidth = 0;
        QVector<QTextLayout::FormatRange> effects;   // ranges with shadow/emboss/engrave/glow
        QVector<QPair<int, int>> fieldRanges;        // display ranges of fields
        int dispFromDoc(int rel) const;
        int docFromDisp(int d) const;
    };

private:
    const Block *blockAt(int pos, int *rel) const;
    std::vector<std::unique_ptr<Block>> m_blocks;
    QVector<FrameSpec> m_frames;
    QVector<double> m_used;
    bool m_overflow = false;
    LayoutEnv m_env;
};

// Resolve a character format for display: scheme colors, scheme fonts, scaling.
// Draws one line of text with its left end of baseline at `baseline`. Unlike
// QPainter::drawText, the size does not depend on the paint device's DPI, so a
// PDF at 1200 dpi matches the screen. Fonts must already be in layout units.
void drawPlainText(QPainter *p, const QPointF &baseline, const QFont &font, const QString &text);

QTextCharFormat resolveCharFormat(const QTextCharFormat &f, const LayoutEnv &env);
QFont baseFontFor(const QTextBlock &b, const LayoutEnv &env);

} // namespace jp
