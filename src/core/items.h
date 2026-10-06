#pragma once
// Frames that sit on a page. All geometry is in points (1/72 inch), in page
// coordinates. `rect` is the unrotated frame; rotation turns it about its center.

#include "core/style.h"

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

    QJsonObject toJson() const override;
    void fromJson(const QJsonObject &o) override;
};

class LineItem : public Item {
public:
    LineItem();
    ItemType type() const override { return ItemType::Line; }
    ItemPtr clone() const override;
    QPointF p1{72, 72}, p2{216, 72};

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
    QMarginsF margins{2.88, 2.88, 2.88, 2.88};
    CellBorder border;
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

    QString text = QStringLiteral("Your Text Here");
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

} // namespace jp
