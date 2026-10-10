#pragma once
// The publication: page setup, master pages, pages, the shared scratch area,
// text stories, embedded pictures, styles, schemes, business information and
// the mail-merge recipient list.

#include "core/items.h"

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QSizeF>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <algorithm>
#include <memory>

class QTextDocument;

namespace jp {

constexpr double PT_PER_IN = 72.0;
inline double in2pt(double in) { return in * PT_PER_IN; }

// How a picture file joins a publication: copied in (Embed), only tied to
// its file (Link), or both (EmbedAndLink).
enum class PictureInsert { Embed, Link, EmbedAndLink };
// Where a picture stands with its file.
enum class LinkStatus { Embedded, Linked, Missing, Modified };

struct ImageData {
    // The picture's file bytes. Of a link whose file can't be found and that
    // keeps no copy, the preview stands in (and `format` is the preview's).
    QByteArray bytes;
    QString format;             // "png", "jpg", "svg", ...
    QSize pixelSize;
    QString sourcePath;         // where it was inserted from (Graphics Manager); a link's file
    bool linked = false;        // tied to the file at sourcePath
    // Of a link:
    bool keepsCopy = true;      // the full picture is stored in the publication too (Insert and Link)
    int linkPage = 0;           // which page of a PDF file
    qint64 fileSize = 0;        // the file as it was when the link was made or updated,
    qint64 fileTime = 0;        // msecs since the epoch
    QByteArray fileHash;        // SHA-1 of the file's bytes, in hex: a file with other contents is a Modified
                                // one. Empty in files from before it was kept: size and date decide then.
    QByteArray preview;         // a small picture of it (at most 512 pixels across), for when the file is gone
    QString previewFormat;      // "jpg" or "png"
    QImage image() const;       // decoded, cached
    void makePreview();         // fills preview from the picture
    mutable QImage cache;
    // Not saved: when the publication opened, the file was read and its
    // contents were not the linked ones. (A status check looks only at the
    // size and date, and doesn't read the file.)
    bool changed = false;
};

struct Story {
    QString id;
    std::unique_ptr<QTextDocument> doc;
    quint64 serial = nextSerial();   // unique per Story object, so caches never confuse a rebuilt story
    static quint64 nextSerial();
};

struct RulerGuides {
    QVector<double> h, v;       // page coordinates
    // Where a new guide goes: at `at`, or, when a guide is already there,
    // `step` further on (back to the start past `limit`), so guides added
    // one after another don't sit on top of each other.
    static double freeSpot(const QVector<double> &list, double at, double step, double limit);
    // Adds `count` guides from `start`, `spacing` apart, within 0..limit and
    // skipping places that already have one; returns how many were added.
    static int addSeries(QVector<double> *list, double start, double spacing, int count, double limit);
};

struct GridGuides {
    int cols = 1, rows = 1;
    double colGap = 14.4, rowGap = 14.4;
    bool centerGuide = false;
    double baseline = 12, baselineOffset = 0;
    double hBaseline = 12, hBaselineOffset = 0;
    QJsonObject toJson() const;
    static GridGuides fromJson(const QJsonObject &o);
};

struct PageBase {
    QString id = newId("p");
    ItemList items;
    Fill background;
    RulerGuides guides;
};

struct MasterPage : PageBase {
    QString name = QStringLiteral("Master Page");
    QString abbr = QStringLiteral("A");
    bool twoPage = false;
    GridGuides grid;
};

struct Page : PageBase {
    QString masterId = QStringLiteral("A"); // empty = no master
    QString title;
};

struct PageSetup {
    enum Layout { OnePerSheet, Booklet, MultiplePerSheet, Envelope, FoldedCard, Labels };
    enum Fold { SideFoldQuarter, TopFoldQuarter, SideFoldHalf, TopFoldHalf };
    QSizeF size{612, 792};
    QMarginsF margins{36, 36, 36, 36};
    QString sizeName = QStringLiteral("Letter");
    Layout layout = OnePerSheet;
    Fold fold = SideFoldHalf;
    QSizeF sheet{612, 792};
    int gridRows = 1, gridCols = 1;
    double gapH = 0, gapV = 0, sideMargin = 0, topMargin = 0;
    // Format Page Numbers: the first page's number, and the style of the page
    // numbers that don't choose one: "" 1 2 3, "alpha" a b c, "ALPHA" A B C,
    // "roman" i ii iii, "ROMAN" I II III.
    int firstPageNumber = 1;
    QString pageNumberFormat;
    QJsonObject toJson() const;
    static PageSetup fromJson(const QJsonObject &o);
};

struct TextStyle {
    QString name;
    bool charOnly = false;
    QString basedOn;
    QString next;               // style for the following paragraph
    QTextCharFormat chr;
    QTextBlockFormat blk;
    QJsonObject toJson() const;
    static TextStyle fromJson(const QJsonObject &o);
};

struct BusinessInfo {
    QString setName = QStringLiteral("Primary Business");
    QString name, tagline, person, title, address, phone, fax, email, web;
    QString logoImageId;
    QString field(const QString &key) const;
    void setField(const QString &key, const QString &v);
    QJsonObject toJson() const;
    static BusinessInfo fromJson(const QJsonObject &o);
    static QStringList keys();
    static QString label(const QString &key);
};

struct MergeSource {
    QString path;
    QStringList fields;
    QVector<QStringList> rows;
    QVector<bool> include;      // per row
    QString pictureField;       // field holding picture file paths
    bool isEmpty() const { return fields.isEmpty(); }
    QString value(int row, const QString &field) const;
    QVector<int> includedRows() const;
    QJsonObject toJson() const;
    static MergeSource fromJson(const QJsonObject &o);
};

// Catalog merge: an area on one page that repeats for each record of the
// data source, in rows and columns of equal cells (filled across, then
// down). The objects in the first cell are the template the others copy.
struct CatalogArea {
    QString pageId;            // the page holding the area; empty = none
    QRectF rect;
    int rows = 2, cols = 1;
    bool isActive() const { return !pageId.isEmpty() && rect.width() > 1 && rect.height() > 1; }
    int perPage() const { return std::max(1, rows) * std::max(1, cols); }
    QRectF cell(int k) const;                 // cell k in reading order
    bool inTemplate(const QRectF &bounds) const;   // centered in the first cell
    QJsonObject toJson() const;
    static CatalogArea fromJson(const QJsonObject &o);
};

struct DocProps {
    QString title, subject, author, manager, company, category, keywords, comments;
    QDateTime created = QDateTime::currentDateTime(), modified = QDateTime::currentDateTime();
    QJsonObject toJson() const;
    static DocProps fromJson(const QJsonObject &o);
};

// Black overprinting in files for a printer (io/overprint applies it).
struct OverprintSettings {
    bool text = true;          // black text below textBelow points
    double textBelow = 24;
    bool lines = true;         // black lines and outlines
    bool fills = false;        // black fills
    int threshold = 95;        // a black of at least this percent counts
    bool any() const { return text || lines || fills; }
    bool operator==(const OverprintSettings &b) const {
        return text == b.text && textBelow == b.textBelow && lines == b.lines && fills == b.fills && threshold == b.threshold;
    }
};

struct PrintInfo {
    enum ColorModel { RGB, SingleSpot, SpotColors, ProcessCMYK, ProcessPlusSpot };
    ColorModel model = RGB;
    QVector<QColor> spotColors;
    QStringList spotNames;           // a name for each spot color (an ink's name, "PANTONE 286 C")
    bool usesSpots() const { return model == SingleSpot || model == SpotColors || model == ProcessPlusSpot; }
    QString spotName(int i) const { return i < spotNames.size() && !spotNames[i].isEmpty() ? spotNames[i] : QStringLiteral("Spot color %1").arg(i + 1); }
    bool embedFonts = true;   // kept from files; PDFs always embed their fonts (PDF/X needs them)
    OverprintSettings overprint;
};

class Document {
public:
    Document();
    ~Document();
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    PageSetup setup;
    QVector<std::shared_ptr<MasterPage>> masters;
    QVector<std::shared_ptr<Page>> pages;
    ItemList scratch;
    // Extra Content: the stories and pictures Change Template found no place
    // for in the new design, until they are placed on a page or discarded.
    ItemList extra;
    QMap<QString, std::shared_ptr<Story>> stories;
    QMap<QString, ImageData> images;
    ColorScheme colors;
    FontScheme fonts;
    QVector<TextStyle> styles;
    QVector<BusinessInfo> biz;
    int bizCurrent = 0;
    MergeSource merge;
    CatalogArea catalog;
    DocProps props;
    PrintInfo print;
    QString templateId;
    QJsonObject templateOptions;
    bool facingPages = false;   // default spread display
    // Where the rulers count from, in points from the page's corner. Saved
    // with the publication, and left out of the file at the corner.
    QPointF rulerZero;
    // The fonts a .pub file named, when the publication came from one:
    // saved back to .pub under these names, not swapped for the standard
    // fonts JeffPub's look-alikes match (interchangeFontName).
    QStringList pubFonts;
    // Where the publication is saved, when it is: a linked picture's path
    // relative to it is kept, and wins when the publication and its pictures
    // were moved together.
    QString folder;

    // pages
    QSizeF pageSize() const { return setup.size; }
    MasterPage *master(const QString &id) const;
    MasterPage *masterFor(const Page &p) const;
    std::shared_ptr<Page> addPage(int at = -1, const QString &masterId = QStringLiteral("A"));
    int pageIndexOf(const QString &pageId) const;
    // After setup.size changed from `from`, as Publisher does it: nothing is
    // scaled, every object moves by half the change on each axis (it keeps its
    // distance from the page center), ruler guides keep their share of the
    // page, and an object left wholly off its page goes to the scratch area,
    // where it stays. On a two-page master the objects keep their places
    // across their page, and move half the change down.
    void pageSizeChanged(const QSizeF &from);

    // stories
    Story *story(const QString &id) const;
    QTextDocument *storyDoc(const QString &id) const;
    QString createStory(const QString &text = QString());
    QString copyStory(const QString &id);   // deep copy, returns new id
    void removeStory(const QString &id);
    // Stories in use: those of text boxes, shapes and table cells, and those
    // their text refers to (a note's, an object set in text's own), on down.
    QSet<QString> storiesInUse() const;
    void applyDefaultFont(QTextDocument *d) const;

    // pictures
    // `decoded`, when the caller has it, is the picture as image() would
    // read it, saving the decoding.
    QString addImage(const QByteArray &bytes, const QString &format, const QString &sourcePath = QString(), const QImage &decoded = QImage());
    QImage image(const QString &id) const;
    QSize imageSize(const QString &id) const;
    // A picture tied to its file at `path`; `bytes` are the file's (see
    // readPictureFile), kept in the publication when `keepCopy`, else only
    // a small preview is.
    // `fileHash` is the file's, as readPictureFile gives it (made from `bytes`
    // when left out, unless those are a PDF page, which is not the file).
    QString addLinkedImage(const QByteArray &bytes, const QString &format, const QString &path, bool keepCopy, int pdfPage = 0, const QImage &decoded = QImage(),
                           const QByteArray &fileHash = QByteArray());
    LinkStatus linkStatus(const QString &imageId) const;
    // After a publication opens: reads each link's file again (a changed
    // file replaces the stored copy of an Insert and Link picture; a Link
    // picture draws from the file and is Modified until updated), and a
    // file that is gone leaves the stored copy or the preview.
    void refreshLinks();

    // items
    struct Loc { Item *item = nullptr; ItemList *list = nullptr; int index = -1; int page = -1; QString masterId; bool scratch = false; GroupItem *parent = nullptr; };
    Loc find(const QString &itemId) const;
    Item *item(const QString &itemId) const { return find(itemId).item; }
    ItemPtr itemPtr(const QString &itemId) const;
    QVector<TextItem *> chainOf(const QString &frameId) const;   // whole linked chain in order
    TextItem *prevFrame(const QString &frameId) const;
    // Deep copy incl. stories. A copied connector comes loose from its
    // objects unless `ids` collects old -> new ids for remapGlue.
    ItemPtr cloneItem(const Item &it, QHash<QString, QString> *ids = nullptr);
    // Copies of several objects; connectors among them stay attached.
    ItemList cloneItems(const ItemList &items);
    void forEachItem(const std::function<void(Item *, int page, const QString &master)> &fn) const;
    // Moves connector ends to the connection sites they're attached to (after
    // those objects moved); ends whose object is gone come loose. Returns
    // whether any line changed.
    bool routeConnectors();

    // styles and info
    const TextStyle *style(const QString &name) const;
    // A style with what it doesn't set taken from the style it's based on
    // (and that one's, and so on). A null name when there is no such style.
    TextStyle resolvedStyle(const QString &name) const;
    BusinessInfo &business() { return biz[std::clamp(bizCurrent, 0, int(biz.size()) - 1)]; }
    const BusinessInfo &business() const { return biz[std::clamp(bizCurrent, 0, int(biz.size()) - 1)]; }

    // serialization (pictures are stored separately)
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);
    void resetDefaults();

    static std::unique_ptr<Document> blank(const QSizeF &size, const QString &sizeName = QStringLiteral("Letter"), int pageCount = 1);
};

// The most a picture file can be, in bytes; a larger file isn't read.
constexpr qint64 kMaxPictureFile = 256 * 1024 * 1024;
// A picture file's bytes: of a regular file only (never a pipe or a device),
// from 1 byte to kMaxPictureFile, and never more than a byte past that.
// Empty when the file isn't one of those or can't be read.
QByteArray readPictureBytes(const QString &path);

// Reads a picture file as Insert Picture does: its bytes and format (a PDF as
// the one page `pdfPage` alone), and its size in pixels. `bytes` may already
// hold the file's bytes. False when it can't be read as a picture (readPictureBytes
// says which files are not read at all).
// `fileHash`, when given, gets the SHA-1 of the file's bytes in hex.
bool readPictureFile(const QString &path, int pdfPage, QByteArray *bytes, QString *format, QSize *pixels, QByteArray *fileHash = nullptr);

QVector<TextStyle> defaultStyles();

} // namespace jp
