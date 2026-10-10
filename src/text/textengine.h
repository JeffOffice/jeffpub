#pragma once
// Lays out one story across a chain of frames (linked text boxes), each with
// columns, insets, vertical alignment and wrap obstacles. One QTextLayout is
// kept per paragraph; each line is positioned in the frame it landed in. Line
// y-coordinates are offset by the heights of the frames before (frameY) so
// that a single paragraph can span frames while staying in one QTextLayout.

#include "core/color.h"
#include "core/items.h"

#include <QDateTime>
#include <QFont>
#include <QLineF>
#include <QPolygonF>
#include <QSizeF>
#include <QTextBlock>
#include <QTextLayout>
#include <memory>

class QPainter;
class QTextDocument;

namespace jp {

class Document;

// A stock font's average character width in ems (OS/2 xAvgCharWidth), the
// unit .pub tracking is measured in; -1 when it isn't one JeffPub knows.
double originalAverageCharEm(const QString &family, bool bold, bool italic);

struct FieldContext {
    const Document *doc = nullptr;
    int pageNumber = 1;           // 1-based
    int pageCount = 1;
    int mergeRecord = -1;         // -1 shows «Field» codes
    int continuedOnPage = 0, continuedFromPage = 0;
    int nextPage = 0, prevPage = 0;   // pages of the next and previous boxes of a linked chain (0: none)
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
    double hyphenZone = 18;         // points: hyphenate only if moving the word would leave more space
    double baselineGrid = 0;        // the page's baseline guides: spacing (0 = none)
    double baselineOrigin = 0;      // and one baseline's position, frame-local
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
    struct LineInfo { QRectF rect; QString text; QString family; double pointSize = 0; int docStart = 0; double baseline = 0; double below = 0; };   // below: line spacing under the text
    QVector<LineInfo> lineInfo(int frame) const;  // for diagnostics (jpubtool layout)

    // An object set in the text (tp::InlineObject): where it's drawn,
    // frame-local, and the item as JSON. It sits on the baseline, padded by
    // its wrap distances, as in Publisher.
    struct InlineObject { int frame = -1; QRectF rect; QString json; int docPos = 0; };
    const QVector<InlineObject> &inlineObjects() const { return m_inline; }

    struct Seg { int docPos, docLen, dispPos, dispLen; };
    struct Line { int frame = -1; int column = 0; QRectF rect; double below = 0; };   // below: the spacing under the text
    struct Block {
        int docStart = 0, docLen = 0;
        QString disp;
        QVector<Seg> map;
        std::unique_ptr<QTextLayout> tl;
        QVector<Line> lines;
        QString marker; QFont markerFont; QColor markerColor; double markerX = 0;
        QString dropText; QFont dropFont; QColor dropColor; int dropLines = 0; double dropWidth = 0;
        bool dropUp = false;   // a raised cap: on the first line, rising above it
        QVector<QTextLayout::FormatRange> effects;   // ranges with shadow/emboss/engrave/glow
        bool directGlyphs = false;                   // drawn run by run (some letters drawn taller or shorter)
        QVector<QPair<int, int>> fieldRanges;        // display ranges of fields
        QString leaders;                             // a leader character per tab stop (space = none)
        QVector<QPair<int, int>> noteRefs;           // a note reference's display position, and its index in notes()
        struct Box { int disp = 0; int docPos = 0; QString json; QSizeF size; QMarginsF pad; };
        QVector<Box> objects;                        // objects set in the text, by display position
        int dispFromDoc(int rel) const;
        int docFromDisp(int d) const;
    };

    // Footnotes and endnotes. A note is the field "footnote:<story>" or
    // "endnote:<story>" in the text, shown as its number (footnotes and
    // endnotes counted apart, in order within the story); its text is that
    // story, laid out at the bottom of the column the reference lands in
    // (footnotes, under a short rule) or after the story's last line, under
    // a "Notes" heading (endnotes). A footnote too tall for a whole column
    // is cut where the column ends: the rest goes on at the bottom of the
    // next column (or text box), above that column's own notes, under a rule
    // as wide as the column.
    struct Note {
        // The rest of a cut note in one column: its own layout, starting
        // where the column before left off.
        struct Piece { int frame = -1, column = 0; QRectF rect; double height = 0; std::shared_ptr<StoryLayout> layout; };
        QString storyId;
        bool endnote = false;
        int number = 0;
        int frame = -1, column = 0;    // where it's drawn (-1: not, as overflow)
        QRectF rect;                   // frame-local
        double width = -1, height = 0, numberWidth = 0, firstBaseline = 0;
        QFont numberFont;
        QColor numberColor;
        std::shared_ptr<StoryLayout> layout;   // the part with the number (all of a note that isn't cut)
        QVector<Piece> more;           // the rest of a cut note, a piece for each column it goes on in
        int from = 0;                  // while a cut note goes on: where its rest starts in the note's text
    };
    const QVector<Note> &notes() const { return m_notes; }
    struct Rule { int frame; QLineF line; };
    const QVector<Rule> &noteRules() const { return m_noteRules; }   // above each column's footnotes

private:
    const Block *blockAt(int pos, int *rel) const;
    double frameY(int frame) const;   // where a frame's lines start in the paragraph layouts (frameCount() for overflow)
    // One layout pass; returns how far each frame's text moved down for its
    // vertical alignment.
    QVector<double> buildOnce(const QTextDocument *doc, const QVector<FrameSpec> &frames, const LayoutEnv &env);
    std::vector<std::unique_ptr<Block>> m_blocks;
    QVector<FrameSpec> m_frames;
    QVector<double> m_frameY;
    QVector<double> m_used;
    bool m_overflow = false;
    LayoutEnv m_env;
    QVector<Note> m_notes;
    QVector<Rule> m_noteRules;
    struct Heading { int frame = -1; QPointF baseline; QFont font; QColor color; };
    Heading m_notesHeading;            // above the endnotes
    QVector<InlineObject> m_inline;
};

// Resolve a character format for display: scheme colors, scheme fonts, scaling.
// Draws one line of text with its left end of baseline at `baseline`. Unlike
// QPainter::drawText, the size does not depend on the paint device's DPI, so a
// PDF at 1200 dpi matches the screen. Fonts must already be in layout units.
void drawPlainText(QPainter *p, const QPointF &baseline, const QFont &font, const QString &text);
// A font's single line spacing in ems as Publisher spaces it: the known
// metrics of common proprietary fonts (measured from Publisher's PDFs) when
// there are any, else the font's own tables.
double naturalLineEm(const QFont &f, const QString &requestedFamily);

QTextCharFormat resolveCharFormat(const QTextCharFormat &f, const LayoutEnv &env);

// The date and time formats Publisher offers (US English), in its order and
// in Qt's letters: a .pub date field's format number is the place here plus 1.
const QStringList &dateTimeFormats();
QFont baseFontFor(const QTextBlock &b, const LayoutEnv &env);

} // namespace jp
