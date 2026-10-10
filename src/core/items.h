#pragma once
// Frames that sit on a page. All geometry is in points (1/72 inch), in page
// coordinates. `rect` is the unrotated frame; rotation turns it about its center.

#include "core/style.h"

#include <QHash>
#include <QJsonObject>
#include <QMarginsF>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <memory>
#include <vector>

namespace jp {

enum class ItemType { Text, Picture, Shape, Line, Table, TextArt, Group };
QString itemTypeName(ItemType t);

enum class VAlign { Top, Middle, Bottom };

class Item;
using ItemPtr = std::shared_ptr<Item>;
using ItemList = std::vector<ItemPtr>;

QString newId(const char *prefix = "i");

class Item {
public:
    virtual ~Item() = default;
    virtual ItemType type() const = 0;
    virtual ItemPtr clone() const = 0;

    QString id = newId();
    QString name;
    QString altText;
    QString hyperlink;
    QRectF rect{72, 72, 144, 72};
    double rotation = 0;     // degrees clockwise
    bool flipH = false, flipV = false;
    bool locked = false;     // position and size locked
    Fill fill;
    Stroke stroke;
    Effects fx;
    Wrap wrap;

    // Maps frame-local coordinates (0,0)-(w,h) to page coordinates.
    QTransform transform() const;
    QPolygonF outline() const;           // rotated frame corners in page coords
    virtual QRectF bounds() const;       // axis-aligned page bounds
    virtual void moveBy(double dx, double dy);
    // Scale from one page-space box into another (used by group and multi-select resize).
    virtual void scaleInto(const QRectF &from, const QRectF &to);
    virtual void rotateAround(double deg, const QPointF &c);
    virtual bool hasText() const { return false; }

    virtual QJsonObject toJson() const;
    virtual void fromJson(const QJsonObject &o);
    static ItemPtr create(ItemType t);
    static ItemPtr fromJsonAny(const QJsonObject &o);

protected:
    void copyBase(const Item &o);
};

class TextItem : public Item {
public:
    enum Autofit { NoAutofit, BestFit, ShrinkOnOverflow, GrowBox };
    TextItem();
    ItemType type() const override { return ItemType::Text; }
    ItemPtr clone() const override;
    bool hasText() const override { return true; }

    QString storyId;
    QString nextId;              // next frame in the linked chain
    QMarginsF insets{2.88, 2.88, 2.88, 2.88};
    int columns = 1;
    double columnGap = 9;
    VAlign valign = VAlign::Top;
    Autofit autofit = NoAutofit;
    bool vertical = false;       // text direction rotated 90°
    bool continuedOn = false, continuedFrom = false;
    bool hyphenate = true;       // automatic hyphenation (on by default for text boxes, as in .pub files)
    double hyphenZone = 18;      // points: break a word only if moving it whole would leave more empty
    double fitScale = 1.0;       // computed by autofit, saved so files open identically
    // Best Fit from a .pub: the text keeps the size the file stores (the
    // other program fitted it with its own fonts, and opening its files
    // gives that size again), shrinking only if a stand-in font no longer
    // fits, until Best Fit is chosen again.
    bool fitAsStored = false;

    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

class PictureItem : public Item {
public:
    enum Recolor { NoRecolor, Grayscale, Sepia, Washout, BlackWhite, ColorTint };
    PictureItem();
    ItemType type() const override { return ItemType::Picture; }
    ItemPtr clone() const override;

    QString imageId;            // empty = picture placeholder
    QRectF imgRect;             // where the full picture sits, frame-local (crop = outside the frame)
    QString maskShape = QStringLiteral("rect");
    double brightness = 0;      // -100..100
    double contrast = 0;        // -100..100
    Recolor recolor = NoRecolor;
    ColorRef recolorColor;
    bool hasTransparentColor = false;
    QColor transparentColor;
    double transparency = 0;
    QString caption;            // caption style id when inserted from Caption gallery

    void fitImage(const QSize &px, bool fill);  // fill: crop to cover, else fit inside
    void keepProportions(const QSize &px);      // the picture's height follows its width, around the same center
    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
    void scaleInto(const QRectF &from, const QRectF &to) override;
};

class ShapeItem : public Item {
public:
    ShapeItem();
    ItemType type() const override { return ItemType::Shape; }
    ItemPtr clone() const override;
    bool hasText() const override { return !storyId.isEmpty(); }

    QString shape = QStringLiteral("rect");
    QVector<double> adj;        // adjustment handle values (0..1)
    QPainterPath customPath;    // edited points, frame-local; overrides shape when not empty
    QString storyId;            // text inside the shape
    QMarginsF insets{7.2, 3.6, 7.2, 3.6};
    VAlign valign = VAlign::Middle;

    // Artwork (shape "art": an icon, or an SVG picture turned into shapes)
    // scales like a picture: its line weight grows and shrinks with it, and
    // corner handles keep its proportions.
    bool isArt() const { return shape == QLatin1String("art"); }
    // Fits the edited points (and artwork's line weight) to the frame after
    // its size changed from `before`.
    void resized(const QSizeF &before);
    void scaleInto(const QRectF &from, const QRectF &to) override;
    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

class LineItem : public Item {
public:
    LineItem();
    ItemType type() const override { return ItemType::Line; }
    ItemPtr clone() const override;
    QPointF p1{72, 72}, p2{216, 72};

    // Connectors: the line can take an elbow or curved route between its
    // ends, and each end can be attached to a connection site of another
    // object (0 top, 1 left, 2 bottom, 3 right), which it then follows.
    enum Route { Straight, Elbow, Curved };
    Route route = Straight;
    double bend = 0.5;           // where the middle of the route sits between the ends
    struct Glue {
        QString id;              // the object the end is attached to, or empty
        int site = -1;
        bool operator==(const Glue &o) const { return id == o.id && site == o.site; }
    };
    Glue start, end;
    bool startVertical = false;  // the route leaves p1 (arrives at p2) vertically
    bool endVertical = false;
    // The route's points: [p1, p2] when straight, the corners of an elbow,
    // or one curve's [p1, control, control, p2].
    QVector<QPointF> routePoints() const;
    QPainterPath path() const;

    QRectF bounds() const override;
    void moveBy(double dx, double dy) override;
    void scaleInto(const QRectF &from, const QRectF &to) override;
    void rotateAround(double deg, const QPointF &c) override;
    void syncRect();
    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

struct CellBorder {
    Stroke top, bottom, left, right;
};

struct TableCell {
    QString storyId;
    Fill fill;
    int rowSpan = 1, colSpan = 1;
    bool covered = false;       // hidden under a merged cell
    int diagonal = 0;           // 0 none, 1 down (\), 2 up (/)
    VAlign valign = VAlign::Top;
    bool vertical = false;      // text direction rotated 90°
    bool hyphenate = true;      // automatic hyphenation
    double hyphenZone = 18;     // points: break a word only if moving it whole would leave more empty
    QMarginsF margins{2.88, 2.88, 2.88, 2.88};
    CellBorder border;
};

// A rectangle of table cells: rows r0..r1 and columns c0..c1, both inclusive.
struct CellRange {
    int r0 = -1, c0 = -1, r1 = -1, c1 = -1;
    bool valid() const { return r0 >= 0 && c0 >= 0 && r1 >= r0 && c1 >= c0; }
    bool contains(int r, int c) const { return valid() && r >= r0 && r <= r1 && c >= c0 && c <= c1; }
    bool operator==(const CellRange &o) const { return r0 == o.r0 && c0 == o.c0 && r1 == o.r1 && c1 == o.c1; }
};

class TableItem : public Item {
public:
    TableItem();
    ItemType type() const override { return ItemType::Table; }
    ItemPtr clone() const override;
    bool hasText() const override { return true; }

    int rows = 0, cols = 0;
    QVector<double> colW, rowH;
    QVector<TableCell> cells;   // row-major
    QString format;             // table format name
    bool growToFit = true;
    bool lockSize = false;
    bool header = true;         // format the first row as a header
    bool banded = true;         // alternate row shading

    TableCell &cell(int r, int c) { return cells[r * cols + c]; }
    const TableCell &cell(int r, int c) const { return cells[r * cols + c]; }
    QRectF cellRect(int r, int c) const;   // frame-local, spans included
    // The cell (the first of a merged cell) under a frame-local point; a
    // point outside the table is taken to the nearest cell.
    void cellAt(const QPointF &local, int *row, int *col) const;
    // The rectangle of cells between two cells, grown until it holds every
    // merged cell it touches whole.
    CellRange cellsBetween(int r0, int c0, int r1, int c1) const;
    void syncRect();                       // rect size = sum of columns and rows
    void scaleInto(const QRectF &from, const QRectF &to) override;
    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

class TextArtItem : public Item {
public:
    TextArtItem();
    ItemType type() const override { return ItemType::TextArt; }
    ItemPtr clone() const override;

    QString text;               // starts as "Your Text Here" in the program's language (set in the constructor)
    QString font = QStringLiteral("Archivo Black");
    bool bold = false, italic = false;
    double size = 36;           // nominal size (TextArt stretches to the frame)
    bool evenHeight = false;
    bool vertical = false;
    double spacing = 1.0;       // 0.8 very tight .. 1.5 very loose
    int align = 1;              // 0 left, 1 center, 2 right, 3 word justify, 4 letter justify, 5 stretch
    QString transform_ = QStringLiteral("plain");
    double transformAdj = 0.5;
    QString styleId;            // gallery style it came from

    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

class GroupItem : public Item {
public:
    GroupItem();
    ItemType type() const override { return ItemType::Group; }
    ItemPtr clone() const override;
    ItemList children;
    QJsonObject barcode;        // the settings of a barcode made by Insert > Barcode, for editing it

    QRectF bounds() const override;
    void moveBy(double dx, double dy) override;
    void scaleInto(const QRectF &from, const QRectF &to) override;
    void rotateAround(double deg, const QPointF &c) override;
    void syncRect() { rect = bounds(); }
    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

// Walk an item tree depth-first.
template <typename F>
void walkItems(const ItemList &list, F &&fn)
{
    for (const auto &it : list) {
        fn(it);
        if (it->type() == ItemType::Group) walkItems(static_cast<GroupItem *>(it.get())->children, fn);
    }
}

QRectF unionBounds(const ItemList &items);

// Where a connector attaches to an object: the middle of each side of its
// frame (0 top, 1 left, 2 bottom, 3 right), in page coordinates, turned and
// flipped with it. `vertical` says whether a line leaves that side upright.
constexpr int kConnectionSites = 4;
QPointF connectionSite(const Item &it, int site, bool *vertical = nullptr);
// After copying objects (old id -> new id in `ids`): connectors among the
// copies attach to the copies; ends attached to anything else come loose.
void remapGlue(const ItemList &items, const QHash<QString, QString> &ids);

} // namespace jp
