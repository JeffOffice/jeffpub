#include "core/document.h"

#include <cmath>

#include "core/svg.h"
#include "render/metafile.h"
#include "render/pdfpage.h"
#include "text/storyio.h"
#include "text/textprops.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <atomic>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QJsonArray>
#include <QSvgRenderer>
#include <QPainter>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>

namespace jp {

namespace {
bool hasGuideAt(const QVector<double> &list, double at)
{
    for (double g : list)
        if (std::abs(g - at) < 0.5) return true;
    return false;
}
} // namespace

double RulerGuides::freeSpot(const QVector<double> &list, double at, double step, double limit)
{
    if (step <= 0 || limit <= 0) return at;
    double x = at;
    for (int i = 0; i < int(limit / step) + 2 && hasGuideAt(list, x); ++i) {
        x += step;
        if (x > limit) x = std::fmod(x, step);
    }
    return x;
}

int RulerGuides::addSeries(QVector<double> *list, double start, double spacing, int count, double limit)
{
    int added = 0;
    for (int i = 0; i < count; ++i) {
        const double x = start + i * spacing;
        if (x < -0.01 || x > limit + 0.01) break;
        if (hasGuideAt(*list, x)) continue;
        *list << x;
        ++added;
    }
    return added;
}

// ---------- small JSON helpers ----------
static QJsonArray sizeJ(const QSizeF &s) { return {s.width(), s.height()}; }
static QSizeF sizeF(const QJsonValue &v, QSizeF d)
{
    const auto a = v.toArray();
    return a.size() == 2 ? QSizeF(a[0].toDouble(), a[1].toDouble()) : d;
}
static QJsonArray margJ(const QMarginsF &m) { return {m.left(), m.top(), m.right(), m.bottom()}; }
static QMarginsF margF(const QJsonValue &v, QMarginsF d)
{
    const auto a = v.toArray();
    return a.size() == 4 ? QMarginsF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : d;
}
static QJsonArray dblJ(const QVector<double> &v)
{
    QJsonArray a;
    for (double d : v) a.append(d);
    return a;
}
static QVector<double> dblF(const QJsonValue &v)
{
    QVector<double> out;
    for (const auto &x : v.toArray()) out << x.toDouble();
    return out;
}
static QJsonArray itemsJ(const ItemList &l)
{
    QJsonArray a;
    for (const auto &it : l) a.append(it->toJson());
    return a;
}
static ItemList itemsF(const QJsonValue &v)
{
    ItemList l;
    for (const auto &x : v.toArray())
        if (auto it = Item::fromJsonAny(x.toObject())) l.push_back(it);
    return l;
}

quint64 Story::nextSerial()
{
    static std::atomic<quint64> n{1};
    return n++;
}

// ---------- ImageData ----------
QImage ImageData::image() const
{
    if (!cache.isNull()) return cache;
    if (format == QLatin1String("wmf") || format == QLatin1String("emf") || Metafile::looksLikeMetafile(bytes)) {
        Metafile m;
        if (m.load(bytes)) cache = m.toImage(1600);
        return cache;
    }
    if (format == QLatin1String("pdf")) {
        // A PDF page: PDFium's drawing of it, about 2,400 pixels on its long side.
        const auto pdf = PdfDocument::shared(bytes);
        const QSizeF pt = pdf->pageSize(0);
        if (!pt.isEmpty()) {
            const double scale = std::min(300.0 / 72.0, 2400.0 / std::max(pt.width(), pt.height()));
            cache = pdf->render(0, QSize(std::max(1, qRound(pt.width() * scale)), std::max(1, qRound(pt.height() * scale))));
        }
        return cache;
    }
    if (format == QLatin1String("svg")) {
        QSvgRenderer r(bytes);
        QSize s = r.defaultSize().isValid() ? r.defaultSize() : QSize(512, 512);
        s.scale(std::max(s.width(), 1200), std::max(s.height(), 1200), Qt::KeepAspectRatio);
        QImage img(s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        svg::paint(r, bytes, &p, QRectF(QPointF(), QSizeF(s)));
        cache = img;
        return cache;
    }
    QBuffer buf;
    buf.setData(bytes);
    QImageReader rd(&buf);
    rd.setAutoTransform(true);
    cache = rd.read();
    return cache;
}

void ImageData::makePreview()
{
    preview.clear();
    QImage img = image();
    if (img.isNull()) return;
    if (std::max(img.width(), img.height()) > 512) img = img.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Photos as JPEG, anything that can be see-through as PNG.
    for (const char *fmt : {img.hasAlphaChannel() ? "png" : "jpg", "png"}) {
        QBuffer buf(&preview);
        buf.open(QIODevice::WriteOnly);
        if (img.save(&buf, fmt, 85)) {
            previewFormat = QLatin1String(fmt);
            return;
        }
        preview.clear();
    }
}

QByteArray readPictureBytes(const QString &path)
{
    // A pipe would block the open for good, and a device or a file the system
    // calls empty can go on without end: only a regular file is opened.
    const QFileInfo fi(path);
    if (!fi.isFile() || fi.size() <= 0 || fi.size() > kMaxPictureFile) return QByteArray();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    // The file as opened, and one byte more than it, to see that it didn't grow.
    const qint64 size = f.size();
    if (size <= 0 || size > kMaxPictureFile) return QByteArray();
    const QByteArray bytes = f.read(size + 1);
    return bytes.size() == size ? bytes : QByteArray();
}

static QByteArray hashOf(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex(); }

bool readPictureFile(const QString &path, int pdfPage, QByteArray *bytes, QString *format, QSize *pixels, QByteArray *fileHash)
{
    if (bytes->isEmpty()) {
        *bytes = readPictureBytes(path);
        if (bytes->isEmpty()) return false;
    }
    if (fileHash) *fileHash = hashOf(*bytes);
    *format = QFileInfo(path).suffix().toLower();
    if (*format == "jpeg") *format = "jpg";
    if (*format == "tiff") *format = "tif";
    if (*format == "pdf" || PdfDocument::looksLikePdf(*bytes)) {
        // A PDF page is kept as a PDF of that page alone; its size in
        // points, given at 96 pixels an inch like other pictures.
        const PdfDocument pdf(*bytes);
        if (!pdf.isValid() || pdfPage < 0 || pdfPage >= pdf.pageCount()) return false;
        *bytes = pdf.extractPage(pdfPage);
        *format = QStringLiteral("pdf");
        *pixels = (pdf.pageSize(pdfPage) * 96.0 / 72.0).toSize();
        return !bytes->isEmpty() && pixels->isValid();
    }
    if (*format == "wmf" || *format == "emf" || Metafile::looksLikeMetafile(*bytes)) {
        Metafile m;
        if (!m.load(*bytes)) return false;
        const QSizeF s = m.naturalSize();
        *pixels = QSize(int(s.width() / 0.75), int(s.height() / 0.75));
        return true;
    }
    // From the bytes read, not from the path opened again.
    QBuffer buffer(bytes);
    QImageReader r(&buffer, format->toLatin1());
    *pixels = r.size();
    return pixels->isValid() || *format == "svg";
}

// ---------- small structs ----------
QJsonObject GridGuides::toJson() const
{
    return {{"cols", cols}, {"rows", rows}, {"colGap", colGap}, {"rowGap", rowGap}, {"center", centerGuide},
            {"baseline", baseline}, {"baselineOffset", baselineOffset}, {"hBaseline", hBaseline}, {"hBaselineOffset", hBaselineOffset}};
}
GridGuides GridGuides::fromJson(const QJsonObject &o)
{
    GridGuides g;
    g.cols = std::max(1, o["cols"].toInt(1));
    g.rows = std::max(1, o["rows"].toInt(1));
    g.colGap = o["colGap"].toDouble(14.4);
    g.rowGap = o["rowGap"].toDouble(14.4);
    g.centerGuide = o["center"].toBool();
    g.baseline = o["baseline"].toDouble(12);
    g.baselineOffset = o["baselineOffset"].toDouble();
    g.hBaseline = o["hBaseline"].toDouble(12);
    g.hBaselineOffset = o["hBaselineOffset"].toDouble();
    return g;
}

QJsonObject PageSetup::toJson() const
{
    QJsonObject o{{"size", sizeJ(size)}, {"margins", margJ(margins)}, {"sizeName", sizeName}, {"layout", int(layout)}, {"fold", int(fold)},
                  {"sheet", sizeJ(sheet)}, {"gridRows", gridRows}, {"gridCols", gridCols}, {"gapH", gapH}, {"gapV", gapV},
                  {"sideMargin", sideMargin}, {"topMargin", topMargin}};
    if (firstPageNumber != 1) o["firstPageNumber"] = firstPageNumber;
    if (!pageNumberFormat.isEmpty()) o["pageNumberFormat"] = pageNumberFormat;
    return o;
}
PageSetup PageSetup::fromJson(const QJsonObject &o)
{
    PageSetup s;
    s.size = sizeF(o["size"], s.size);
    // From 1 point to 20,000 (the other program allows 241 inches, 17,352).
    auto side = [](double v, double def) { return std::isfinite(v) ? std::clamp(v, 1.0, 20000.0) : def; };
    s.size = QSizeF(side(s.size.width(), 612), side(s.size.height(), 792));
    s.margins = margF(o["margins"], s.margins);
    s.sizeName = o["sizeName"].toString(s.sizeName);
    s.layout = Layout(o["layout"].toInt());
    s.fold = Fold(o["fold"].toInt(SideFoldHalf));
    s.sheet = sizeF(o["sheet"], s.size);
    s.gridRows = o["gridRows"].toInt(1);
    s.gridCols = o["gridCols"].toInt(1);
    s.gapH = o["gapH"].toDouble();
    s.gapV = o["gapV"].toDouble();
    s.firstPageNumber = std::clamp(o["firstPageNumber"].toInt(1), 1, 99999);
    s.pageNumberFormat = o["pageNumberFormat"].toString();
    s.sideMargin = o["sideMargin"].toDouble();
    s.topMargin = o["topMargin"].toDouble();
    return s;
}

QJsonObject TextStyle::toJson() const
{
    return {{"name", name}, {"charOnly", charOnly}, {"basedOn", basedOn}, {"next", next}, {"chr", formatToJson(chr)}, {"blk", formatToJson(blk)}};
}
TextStyle TextStyle::fromJson(const QJsonObject &o)
{
    TextStyle s;
    s.name = o["name"].toString();
    s.charOnly = o["charOnly"].toBool();
    s.basedOn = o["basedOn"].toString();
    s.next = o["next"].toString();
    formatFromJson(s.chr, o["chr"].toObject());
    formatFromJson(s.blk, o["blk"].toObject());
    return s;
}

QStringList BusinessInfo::keys()
{
    return {"name", "tagline", "person", "title", "address", "phone", "fax", "email", "web"};
}
QString BusinessInfo::label(const QString &k)
{
    struct Label {
        const char *key, *text;
    };
    static const Label labels[] = {{"name", QT_TRANSLATE_NOOP("Core", "Organization Name")},
                                   {"tagline", QT_TRANSLATE_NOOP("Core", "Tagline or Motto")},
                                   {"person", QT_TRANSLATE_NOOP("Core", "Individual Name")},
                                   {"title", QT_TRANSLATE_NOOP("Core", "Job Position or Title")},
                                   {"address", QT_TRANSLATE_NOOP("Core", "Address")},
                                   {"phone", QT_TRANSLATE_NOOP("Core", "Phone")},
                                   {"fax", QT_TRANSLATE_NOOP("Core", "Fax")},
                                   {"email", QT_TRANSLATE_NOOP("Core", "Email")},
                                   {"web", QT_TRANSLATE_NOOP("Core", "Website")}};
    for (const Label &l : labels)
        if (k == QLatin1String(l.key)) return QCoreApplication::translate("Core", l.text);
    return k;
}
QString BusinessInfo::field(const QString &k) const
{
    if (k == "name") return name;
    if (k == "tagline") return tagline;
    if (k == "person") return person;
    if (k == "title") return title;
    if (k == "address") return address;
    if (k == "phone") return phone;
    if (k == "fax") return fax;
    if (k == "email") return email;
    if (k == "web") return web;
    return {};
}
void BusinessInfo::setField(const QString &k, const QString &v)
{
    if (k == "name") name = v;
    else if (k == "tagline") tagline = v;
    else if (k == "person") person = v;
    else if (k == "title") title = v;
    else if (k == "address") address = v;
    else if (k == "phone") phone = v;
    else if (k == "fax") fax = v;
    else if (k == "email") email = v;
    else if (k == "web") web = v;
}
QJsonObject BusinessInfo::toJson() const
{
    QJsonObject o{{"setName", setName}};
    for (const auto &k : keys()) o[k] = field(k);
    if (!logoImageId.isEmpty()) o["logo"] = logoImageId;
    return o;
}
BusinessInfo BusinessInfo::fromJson(const QJsonObject &o)
{
    BusinessInfo b;
    b.setName = o["setName"].toString(b.setName);
    for (const auto &k : keys()) b.setField(k, o[k].toString());
    b.logoImageId = o["logo"].toString();
    return b;
}

QString MergeSource::value(int row, const QString &field) const
{
    const int c = fields.indexOf(field);
    if (row < 0 || row >= rows.size() || c < 0) return {};
    return rows[row].value(c);
}
QVector<int> MergeSource::includedRows() const
{
    QVector<int> out;
    for (int i = 0; i < rows.size(); ++i)
        if (i >= include.size() || include[i]) out << i;
    return out;
}
QJsonObject MergeSource::toJson() const
{
    QJsonArray rs, inc;
    for (const auto &r : rows) rs.append(QJsonArray::fromStringList(r));
    for (bool b : include) inc.append(b);
    return {{"path", path}, {"fields", QJsonArray::fromStringList(fields)}, {"rows", rs}, {"include", inc}, {"pictureField", pictureField}};
}
MergeSource MergeSource::fromJson(const QJsonObject &o)
{
    MergeSource m;
    m.path = o["path"].toString();
    for (const auto &f : o["fields"].toArray()) m.fields << f.toString();
    for (const auto &r : o["rows"].toArray()) {
        QStringList l;
        for (const auto &x : r.toArray()) l << x.toString();
        m.rows << l;
    }
    for (const auto &b : o["include"].toArray()) m.include << b.toBool();
    m.pictureField = o["pictureField"].toString();
    return m;
}

QJsonObject DocProps::toJson() const
{
    return {{"title", title}, {"subject", subject}, {"author", author}, {"manager", manager}, {"company", company},
            {"category", category}, {"keywords", keywords}, {"comments", comments},
            {"created", created.toString(Qt::ISODate)}, {"modified", modified.toString(Qt::ISODate)}};
}
DocProps DocProps::fromJson(const QJsonObject &o)
{
    DocProps p;
    p.title = o["title"].toString();
    p.subject = o["subject"].toString();
    p.author = o["author"].toString();
    p.manager = o["manager"].toString();
    p.company = o["company"].toString();
    p.category = o["category"].toString();
    p.keywords = o["keywords"].toString();
    p.comments = o["comments"].toString();
    p.created = QDateTime::fromString(o["created"].toString(), Qt::ISODate);
    p.modified = QDateTime::fromString(o["modified"].toString(), Qt::ISODate);
    return p;
}

// ---------- default styles ----------
static TextStyle mkStyle(const QString &name, double pt, bool bold, bool heading, double before = 0, double after = 6,
                         const QString &next = QString(), bool italic = false, int slot = -1)
{
    TextStyle s;
    s.name = name;
    s.basedOn = name == "Normal" ? QString() : QStringLiteral("Normal");
    s.next = next.isEmpty() ? name : next;
    s.chr.setFontPointSize(pt);
    if (bold) s.chr.setFontWeight(QFont::Bold);
    if (italic) s.chr.setFontItalic(true);
    s.chr.setProperty(tp::ThemeFont, heading ? QStringLiteral("major") : QStringLiteral("minor"));
    if (slot >= 0) s.chr.setProperty(tp::ColorRefP, ColorRef::scheme(slot).toString());
    s.blk.setTopMargin(before);
    s.blk.setBottomMargin(after);
    s.blk.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
    s.blk.setProperty(tp::StyleName, name);
    return s;
}

QVector<TextStyle> defaultStyles()
{
    QVector<TextStyle> v;
    v << mkStyle("Normal", 11, false, false, 0, 6);
    v << mkStyle("Body Text", 11, false, false, 0, 8);
    v << mkStyle("Title", 36, true, true, 0, 6, "Normal", false, Accent1);
    v << mkStyle("Subtitle", 16, false, true, 0, 10, "Normal", false, Accent4);
    v << mkStyle("Heading 1", 22, true, true, 12, 6, "Normal", false, Accent1);
    v << mkStyle("Heading 2", 16, true, true, 10, 4, "Normal", false, Accent1);
    v << mkStyle("Heading 3", 13, true, true, 8, 3, "Normal");
    v << mkStyle("Heading 4", 12, true, true, 6, 2, "Normal", true);
    v << mkStyle("Quote", 14, false, true, 6, 6, "Normal", true, Accent2);
    v << mkStyle("Caption", 8.5, false, false, 2, 2, "Normal", true);
    v << mkStyle("List Bullet", 11, false, false, 0, 3);
    v << mkStyle("Address", 9, false, false, 0, 0);
    v << mkStyle("Organization Name", 14, true, true, 0, 2, "Normal", false, Accent1);
    v << mkStyle("Tagline", 10, false, false, 0, 2, "Normal", true);
    v << mkStyle("Phone Fax Email", 8, false, false, 0, 0);
    TextStyle emph;
    emph.name = "Emphasis";
    emph.charOnly = true;
    emph.chr.setFontItalic(true);
    v << emph;
    TextStyle strong;
    strong.name = "Strong";
    strong.charOnly = true;
    strong.chr.setFontWeight(QFont::Bold);
    v << strong;
    return v;
}

// ---------- Document ----------
Document::Document() { resetDefaults(); }
Document::~Document() = default;

void Document::resetDefaults()
{
    colors = builtinColorSchemes().front();
    fonts = builtinFontSchemes().front();
    styles = defaultStyles();
    biz = {BusinessInfo()};
    bizCurrent = 0;
    if (masters.isEmpty()) {
        auto m = std::make_shared<MasterPage>();
        m->id = "A";
        m->name = "Master Page A";
        masters << m;
    }
}

std::unique_ptr<Document> Document::blank(const QSizeF &size, const QString &sizeName, int pageCount)
{
    auto d = std::make_unique<Document>();
    d->setup.size = size;
    d->setup.sheet = size;
    d->setup.sizeName = sizeName;
    const double m = std::min(size.width(), size.height()) < 216 ? 9 : 36;
    d->setup.margins = QMarginsF(m, m, m, m);
    for (int i = 0; i < std::max(1, pageCount); ++i) d->addPage();
    return d;
}

MasterPage *Document::master(const QString &id) const
{
    for (const auto &m : masters)
        if (m->id == id) return m.get();
    return nullptr;
}

MasterPage *Document::masterFor(const Page &p) const { return p.masterId.isEmpty() ? nullptr : master(p.masterId); }

std::shared_ptr<Page> Document::addPage(int at, const QString &masterId)
{
    auto p = std::make_shared<Page>();
    p->masterId = masterId;
    if (at < 0 || at > pages.size()) pages << p;
    else pages.insert(at, p);
    return p;
}

int Document::pageIndexOf(const QString &pageId) const
{
    for (int i = 0; i < pages.size(); ++i)
        if (pages[i]->id == pageId) return i;
    return -1;
}

void Document::pageSizeChanged(const QSizeF &from)
{
    const QSizeF to = setup.size;
    if (from == to || from.width() <= 0 || from.height() <= 0) return;
    const double dw = to.width() - from.width(), dh = to.height() - from.height();
    const double kx = to.width() / from.width(), ky = to.height() / from.height();
    // Moves the objects of one page (or of a master's left or right page),
    // and sends those that no longer touch it to the scratch area.
    auto shift = [&](ItemList &items, bool twoPage) {
        ItemList kept;
        for (const ItemPtr &it : items) {
            const bool right = twoPage && it->bounds().center().x() >= from.width();
            it->moveBy(twoPage ? (right ? dw : 0) : dw / 2, dh / 2);
            const QRectF b = it->bounds().translated(right ? -to.width() : 0, 0);
            if (b.right() > 0 && b.left() < to.width() && b.bottom() > 0 && b.top() < to.height()) {
                kept.push_back(it);
            } else {
                if (right) it->moveBy(-to.width(), 0);
                scratch.push_back(it);
            }
        }
        items = kept;
    };
    auto guides = [&](PageBase &pb, bool twoPage) {
        for (double &x : pb.guides.v) x = twoPage && x >= from.width() ? to.width() + (x - from.width()) * kx : x * kx;
        for (double &y : pb.guides.h) y *= ky;
    };
    for (auto &pg : pages) {
        shift(pg->items, false);
        guides(*pg, false);
    }
    for (auto &mp : masters) {
        shift(mp->items, mp->twoPage);
        guides(*mp, mp->twoPage);
    }
    if (pageIndexOf(catalog.pageId) >= 0) catalog.rect.translate(dw / 2, dh / 2);
}

Story *Document::story(const QString &id) const
{
    auto it = stories.find(id);
    return it == stories.end() ? nullptr : it->get();
}

QTextDocument *Document::storyDoc(const QString &id) const
{
    Story *s = story(id);
    return s ? s->doc.get() : nullptr;
}

void Document::applyDefaultFont(QTextDocument *d) const
{
    QFont f(fonts.body);
    f.setPointSizeF(11);
    d->setDefaultFont(f);
    d->setDocumentMargin(0);
    d->setUndoRedoEnabled(false);
}

QString Document::createStory(const QString &text)
{
    auto s = std::make_shared<Story>();
    s->id = newId("s");
    s->doc = std::make_unique<QTextDocument>();
    applyDefaultFont(s->doc.get());
    if (!text.isEmpty()) setStoryText(s->doc.get(), text);
    // New text uses the Normal style.
    if (const TextStyle *n = style("Normal")) {
        QTextCursor c(s->doc.get());
        c.select(QTextCursor::Document);
        c.mergeBlockFormat(n->blk);
        c.mergeCharFormat(n->chr);
        c.mergeBlockCharFormat(n->chr);
    }
    stories.insert(s->id, s);
    return s->id;
}

QString Document::copyStory(const QString &id)
{
    Story *src = story(id);
    auto s = std::make_shared<Story>();
    s->id = newId("s");
    s->doc = std::make_unique<QTextDocument>();
    applyDefaultFont(s->doc.get());
    if (src) storyFromJson(s->doc.get(), storyToJson(src->doc.get()));
    stories.insert(s->id, s);
    return s->id;
}

void Document::removeStory(const QString &id) { stories.remove(id); }

QSet<QString> Document::storiesInUse() const
{
    QSet<QString> used;
    QStringList queue;
    auto use = [&](const QString &id) {
        if (id.isEmpty() || used.contains(id)) return;
        used.insert(id);
        queue << id;
    };
    auto usedBy = [&](Item *it) {
        if (it->type() == ItemType::Text) use(static_cast<TextItem *>(it)->storyId);
        if (it->type() == ItemType::Shape) use(static_cast<ShapeItem *>(it)->storyId);
        if (it->type() == ItemType::Table)
            for (const auto &c : static_cast<TableItem *>(it)->cells) use(c.storyId);
    };
    forEachItem([&](Item *it, int, const QString &) { usedBy(it); });
    walkItems(extra, [&](const ItemPtr &it) { usedBy(it.get()); });
    static const QRegularExpression storyKey(QStringLiteral("\"story\":\"([^\"]+)\""));
    while (!queue.isEmpty()) {
        const QTextDocument *sd = storyDoc(queue.takeFirst());
        if (!sd) continue;
        for (QTextBlock b = sd->begin(); b.isValid(); b = b.next())
            for (auto f = b.begin(); !f.atEnd(); ++f) {
                const QTextCharFormat cf = f.fragment().charFormat();
                const QString field = cf.stringProperty(tp::Field);
                if (field.startsWith(QLatin1String("footnote:")) || field.startsWith(QLatin1String("endnote:"))) use(field.section(QLatin1Char(':'), 1));
                const QString object = cf.stringProperty(tp::InlineObject);
                for (auto m = storyKey.globalMatch(object); m.hasNext();) use(m.next().captured(1));
            }
    }
    return used;
}

QString Document::addImage(const QByteArray &bytes, const QString &format, const QString &sourcePath, const QImage &decoded)
{
    for (auto it = images.cbegin(); it != images.cend(); ++it)
        if (!it->linked && it->bytes == bytes) return it.key();
    ImageData d;
    d.bytes = bytes;
    d.format = format.toLower();
    d.sourcePath = sourcePath;
    d.cache = decoded;
    const QImage img = d.image();
    d.pixelSize = img.size();
    const QString id = newId("img");
    images.insert(id, d);
    return id;
}

QString Document::addLinkedImage(const QByteArray &bytes, const QString &format, const QString &path, bool keepCopy, int pdfPage, const QImage &decoded,
                                 const QByteArray &fileHash)
{
    const QFileInfo fi(path);
    const QString file = QDir::cleanPath(fi.absoluteFilePath());
    const QByteArray hash = !fileHash.isEmpty() ? fileHash : format.compare(QLatin1String("pdf"), Qt::CaseInsensitive) ? hashOf(bytes) : QByteArray();
    for (auto it = images.cbegin(); it != images.cend(); ++it)
        if (it->linked && it->sourcePath == file && it->keepsCopy == keepCopy && it->linkPage == pdfPage && it->bytes == bytes && it->fileHash == hash &&
            it->fileSize == fi.size() && it->fileTime == fi.lastModified().toMSecsSinceEpoch())
            return it.key();
    ImageData d;
    d.bytes = bytes;
    d.format = format.toLower();
    d.sourcePath = file;
    d.linked = true;
    d.keepsCopy = keepCopy;
    d.linkPage = pdfPage;
    d.fileSize = fi.size();
    d.fileTime = fi.lastModified().toMSecsSinceEpoch();
    d.fileHash = hash;
    d.cache = decoded;
    d.pixelSize = d.image().size();
    d.makePreview();
    const QString id = newId("img");
    images.insert(id, d);
    return id;
}

namespace {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

bool isSeparator(QChar c) { return c == QLatin1Char('/') || c == QLatin1Char('\\'); }

// Written as absolute by some system: /x, \x, \\server\x, C:\x, C:/x.
bool looksAbsolute(const QString &p)
{
    return (!p.isEmpty() && isSeparator(p[0])) || (p.size() >= 2 && p[0].isLetter() && p[1] == QLatin1Char(':'));
}

// \\?\ and \\.\ (or with slashes): the system's own names for devices and long paths.
bool isDevicePath(const QString &p) { return p.size() >= 4 && isSeparator(p[0]) && isSeparator(p[1]) && (p[2] == QLatin1Char('?') || p[2] == QLatin1Char('.')) && isSeparator(p[3]); }

// \\server\share or //server/share.
bool isNetworkPath(const QString &p) { return p.size() >= 2 && isSeparator(p[0]) && isSeparator(p[1]); }

// `path` (cleaned) below one of `roots` (cleaned); the root it is below, or nothing.
QString rootOf(const QStringList &roots, const QString &path)
{
    for (const QString &root : roots) {
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        if (path.size() > prefix.size() && path.startsWith(prefix, kPathCase)) return root;
    }
    return QString();
}

bool pathInside(const QStringList &roots, const QString &path, int hops)
{
    if (path.isEmpty() || isDevicePath(path) || !QDir::isAbsolutePath(path)) return false;
    const QString clean = QDir::cleanPath(path);
    const QString root = rootOf(roots, clean);
    if (root.isEmpty()) return false;
    if (isNetworkPath(path) && !isNetworkPath(roots.first())) return false;
    // Each name on the way, from the folder down. A symbolic link is read, not
    // followed: where it points must be inside too, and then so must what
    // comes after it. (Windows shortcuts count as links to Qt.)
    const QStringList parts = clean.mid(root.endsWith(QLatin1Char('/')) ? root.size() : root.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString at = root;
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)) return false;
        at = QDir::cleanPath(at + QLatin1Char('/') + parts[i]);
        const QFileInfo fi(at);
        if (!fi.isSymLink()) continue;
        if (hops >= 8) return false;
        QString target = fi.symLinkTarget();
        if (target.isEmpty()) return false;
        if (QDir::isRelativePath(target)) target = fi.absolutePath() + QLatin1Char('/') + target;
        const QString rest = parts.mid(i + 1).join(QLatin1Char('/'));
        return pathInside(roots, rest.isEmpty() ? target : QDir::cleanPath(target) + QLatin1Char('/') + rest, hops + 1);
    }
    return true;
}
} // namespace

bool Document::inFolder(const QString &path) const
{
    if (folder.isEmpty() || isDevicePath(folder) || !QDir::isAbsolutePath(folder)) return false;
    // The folder as it is spelled, and as the system names it (a link in
    // the way, or a different case): a link pointing there is inside too.
    QStringList roots{QDir::cleanPath(folder)};
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    if (!canonical.isEmpty() && canonical != roots.first()) roots << canonical;
    return pathInside(roots, path, 0);
}

LinkStatus Document::linkStatus(const QString &imageId) const
{
    const auto it = images.constFind(imageId);
    if (it == images.cend() || !it->linked) return LinkStatus::Embedded;
    if (!it->followed) return LinkStatus::NotUpdated;
    const QFileInfo fi(it->sourcePath);
    if (!fi.isFile()) return LinkStatus::Missing;
    if (it->changed) return LinkStatus::Modified;
    if (fi.size() != it->fileSize || fi.lastModified().toMSecsSinceEpoch() != it->fileTime) return LinkStatus::Modified;
    return LinkStatus::Linked;
}

void Document::refreshLinks()
{
    for (auto it = images.begin(); it != images.end(); ++it) {
        ImageData &d = it.value();
        if (!d.linked) continue;
        // A file the publication may not look at stays untouched, not even
        // asked after; what the publication stores stands in for it.
        d.followed = d.followed || mayFollow(d.sourcePath);
        const LinkStatus status = linkStatus(it.key());
        // A stored copy that matches the file needs no reading of it.
        const bool stored = d.keepsCopy && !d.bytes.isEmpty();
        QByteArray bytes, hash;
        QString format;
        QSize px;
        const bool read = d.followed && status != LinkStatus::Missing && !(stored && status == LinkStatus::Linked) &&
                          readPictureFile(d.sourcePath, d.linkPage, &bytes, &format, &px, &hash);
        if (read) {
            d.bytes = bytes;
            d.format = format;
            d.cache = QImage();
            // The file it was linked to when its contents are the same (one
            // saved again unchanged has a new date, and two others of one
            // size can share a date); with no hash kept, when size and date are.
            const bool same = d.fileHash.isEmpty() ? status == LinkStatus::Linked : hash == d.fileHash;
            const QFileInfo fi(d.sourcePath);
            if (same) {
                d.fileSize = fi.size();
                d.fileTime = fi.lastModified().toMSecsSinceEpoch();
                d.fileHash = hash;
            } else {
                // Not the file the sizes were taken from: a picture of another
                // shape must not be stretched into the old one's place.
                if (d.keepsCopy) {
                    d.fileSize = fi.size();
                    d.fileTime = fi.lastModified().toMSecsSinceEpoch();
                    d.fileHash = hash;
                } else {
                    d.changed = true;
                }
                const QSize now = d.image().size();
                if (d.pixelSize.isValid() && now != d.pixelSize)
                    forEachItem([&](Item *item, int, const QString &) {
                        auto *pic = dynamic_cast<PictureItem *>(item);
                        if (pic && pic->imageId == it.key()) pic->keepProportions(now);
                    });
                d.pixelSize = now;
            }
        } else if (!stored && !d.preview.isEmpty()) {
            // No file and no stored copy: the preview stands in.
            d.bytes = d.preview;
            d.format = d.previewFormat;
            d.cache = QImage();
        }
    }
}

QImage Document::image(const QString &id) const
{
    auto it = images.find(id);
    return it == images.end() ? QImage() : it->image();
}

QSize Document::imageSize(const QString &id) const
{
    auto it = images.find(id);
    if (it == images.end()) return {};
    if (!it->pixelSize.isValid()) return it->image().size();
    return it->pixelSize;
}

Document::Loc Document::find(const QString &itemId) const
{
    Loc out;
    std::function<bool(ItemList &, GroupItem *)> scan = [&](ItemList &l, GroupItem *parent) {
        for (int i = 0; i < int(l.size()); ++i) {
            if (l[i]->id == itemId) {
                out.item = l[i].get(); out.list = &l; out.index = i; out.parent = parent;
                return true;
            }
            if (l[i]->type() == ItemType::Group && scan(static_cast<GroupItem *>(l[i].get())->children, static_cast<GroupItem *>(l[i].get())))
                return true;
        }
        return false;
    };
    for (int p = 0; p < pages.size(); ++p)
        if (scan(pages[p]->items, nullptr)) { out.page = p; return out; }
    for (const auto &m : masters)
        if (scan(m->items, nullptr)) { out.masterId = m->id; return out; }
    if (scan(const_cast<ItemList &>(scratch), nullptr)) { out.scratch = true; return out; }
    return Loc();
}

ItemPtr Document::itemPtr(const QString &itemId) const
{
    Loc l = find(itemId);
    return l.list ? (*l.list)[l.index] : nullptr;
}

TextItem *Document::prevFrame(const QString &frameId) const
{
    TextItem *found = nullptr;
    forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Text && static_cast<TextItem *>(it)->nextId == frameId) found = static_cast<TextItem *>(it);
    });
    return found;
}

QVector<TextItem *> Document::chainOf(const QString &frameId) const
{
    QVector<TextItem *> out;
    Item *it = item(frameId);
    if (!it || it->type() != ItemType::Text) return out;
    auto *head = static_cast<TextItem *>(it);
    QSet<QString> seen{head->id};
    while (TextItem *p = prevFrame(head->id)) {
        if (seen.contains(p->id)) break;
        seen.insert(p->id);
        head = p;
    }
    seen.clear();
    for (TextItem *f = head; f && !seen.contains(f->id);) {
        seen.insert(f->id);
        out << f;
        Item *n = f->nextId.isEmpty() ? nullptr : item(f->nextId);
        f = (n && n->type() == ItemType::Text) ? static_cast<TextItem *>(n) : nullptr;
    }
    return out;
}

ItemPtr Document::cloneItem(const Item &src, QHash<QString, QString> *ids)
{
    ItemPtr c = src.clone();
    if (ids) ids->insert(src.id, c->id);
    std::function<void(Item *, const Item *)> fix = [&](Item *it, const Item *from) {
        switch (it->type()) {
        case ItemType::Text: {
            auto *t = static_cast<TextItem *>(it);
            t->storyId = copyStory(t->storyId);
            t->nextId.clear();
            break;
        }
        case ItemType::Shape: {
            auto *s = static_cast<ShapeItem *>(it);
            if (!s->storyId.isEmpty()) s->storyId = copyStory(s->storyId);
            break;
        }
        case ItemType::Table:
            for (auto &cell : static_cast<TableItem *>(it)->cells)
                if (!cell.storyId.isEmpty()) cell.storyId = copyStory(cell.storyId);
            break;
        case ItemType::Group: {
            auto &kids = static_cast<GroupItem *>(it)->children;
            const auto &was = static_cast<const GroupItem *>(from)->children;
            for (int i = 0; i < int(kids.size()); ++i) {
                kids[i]->id = newId();
                if (ids && i < int(was.size())) ids->insert(was[i]->id, kids[i]->id);
                fix(kids[i].get(), i < int(was.size()) ? was[i].get() : kids[i].get());
            }
            break;
        }
        case ItemType::Line:
            if (!ids) {
                auto *l = static_cast<LineItem *>(it);
                l->start = l->end = LineItem::Glue();
            }
            break;
        default: break;
        }
    };
    fix(c.get(), &src);
    return c;
}

ItemList Document::cloneItems(const ItemList &items)
{
    QHash<QString, QString> ids;
    ItemList out;
    for (const auto &it : items) out.push_back(cloneItem(*it, &ids));
    remapGlue(out, ids);
    return out;
}

void Document::forEachItem(const std::function<void(Item *, int, const QString &)> &fn) const
{
    for (int p = 0; p < pages.size(); ++p) walkItems(pages[p]->items, [&](const ItemPtr &it) { fn(it.get(), p, QString()); });
    for (const auto &m : masters) walkItems(m->items, [&](const ItemPtr &it) { fn(it.get(), -1, m->id); });
    walkItems(scratch, [&](const ItemPtr &it) { fn(it.get(), -1, QString()); });
}

bool Document::routeConnectors()
{
    QVector<LineItem *> glued;
    forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Line) {
            auto *l = static_cast<LineItem *>(it);
            if (!l->start.id.isEmpty() || !l->end.id.isEmpty()) glued << l;
        }
    });
    if (glued.isEmpty()) return false;
    QHash<QString, Item *> byId;
    forEachItem([&](Item *it, int, const QString &) { byId.insert(it->id, it); });
    bool any = false;
    for (LineItem *l : glued) {
        bool moved = false;
        auto follow = [&](LineItem::Glue &g, QPointF &pt, bool &vertical) {
            if (g.id.isEmpty()) return;
            const Item *o = byId.value(g.id);
            if (!o || o == l || o->type() == ItemType::Line || g.site < 0 || g.site >= kConnectionSites) {
                g = LineItem::Glue();
                moved = true;
                return;
            }
            bool v = false;
            const QPointF at = connectionSite(*o, g.site, &v);
            if (at != pt || v != vertical) {
                pt = at;
                vertical = v;
                moved = true;
            }
        };
        follow(l->start, l->p1, l->startVertical);
        follow(l->end, l->p2, l->endVertical);
        if (moved) {
            l->syncRect();
            any = true;
        }
    }
    return any;
}

const TextStyle *Document::style(const QString &name) const
{
    for (const auto &s : styles)
        if (s.name == name) return &s;
    return nullptr;
}

TextStyle Document::resolvedStyle(const QString &name) const
{
    const TextStyle *s = style(name);
    if (!s) return TextStyle();
    // The chain from the style to its furthest base, stopping at a loop.
    QVector<const TextStyle *> chain{s};
    while (chain.size() < 16 && !chain.last()->basedOn.isEmpty()) {
        const TextStyle *b = style(chain.last()->basedOn);
        if (!b || chain.contains(b)) break;
        chain << b;
    }
    TextStyle out = *s;
    out.chr = QTextCharFormat();
    out.blk = QTextBlockFormat();
    for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
        tp::setProperties(out.chr, (*it)->chr);
        tp::setProperties(out.blk, (*it)->blk);
    }
    return out;
}

static QJsonObject pageBaseJ(const PageBase &p)
{
    QJsonObject o{{"id", p.id}, {"items", itemsJ(p.items)}};
    if (p.background.type != Fill::NoFill) o["background"] = p.background.toJson();
    if (!p.guides.h.isEmpty()) o["guidesH"] = dblJ(p.guides.h);
    if (!p.guides.v.isEmpty()) o["guidesV"] = dblJ(p.guides.v);
    return o;
}
static void pageBaseF(PageBase &p, const QJsonObject &o)
{
    p.id = o["id"].toString(p.id);
    p.items = itemsF(o["items"]);
    p.background = Fill::fromJson(o["background"].toObject());
    p.guides.h = dblF(o["guidesH"]);
    p.guides.v = dblF(o["guidesV"]);
}

QRectF CatalogArea::cell(int k) const
{
    const int c = std::max(1, cols), r = std::max(1, rows);
    const QSizeF sz(rect.width() / c, rect.height() / r);
    return QRectF(rect.left() + (k % c) * sz.width(), rect.top() + (k / c) * sz.height(), sz.width(), sz.height());
}

bool CatalogArea::inTemplate(const QRectF &bounds) const
{
    return isActive() && cell(0).contains(bounds.center());
}

QJsonObject CatalogArea::toJson() const
{
    return {{"page", pageId}, {"x", rect.x()}, {"y", rect.y()}, {"w", rect.width()}, {"h", rect.height()}, {"rows", rows}, {"cols", cols}};
}

CatalogArea CatalogArea::fromJson(const QJsonObject &o)
{
    CatalogArea a;
    a.pageId = o["page"].toString();
    auto sane = [](double v) { return std::isfinite(v) ? std::clamp(v, -1e6, 1e6) : 0.0; };
    a.rect = QRectF(sane(o["x"].toDouble()), sane(o["y"].toDouble()), sane(o["w"].toDouble()), sane(o["h"].toDouble()));
    a.rows = std::clamp(o["rows"].toInt(2), 1, 50);
    a.cols = std::clamp(o["cols"].toInt(1), 1, 50);
    return a;
}

QJsonObject Document::toJson() const
{
    QJsonObject o;
    o["format"] = "jeffpub";
    o["version"] = 1;
    o["setup"] = setup.toJson();
    QJsonArray ms, ps, ss, st, bz, imgs;
    for (const auto &m : masters) {
        QJsonObject mo = pageBaseJ(*m);
        mo["name"] = m->name;
        mo["abbr"] = m->abbr;
        mo["twoPage"] = m->twoPage;
        mo["grid"] = m->grid.toJson();
        ms.append(mo);
    }
    for (const auto &p : pages) {
        QJsonObject po = pageBaseJ(*p);
        po["master"] = p->masterId;
        if (!p->title.isEmpty()) po["title"] = p->title;
        ps.append(po);
    }
    for (auto it = stories.cbegin(); it != stories.cend(); ++it) {
        QJsonObject so = storyToJson((*it)->doc.get());
        so["id"] = it.key();
        ss.append(so);
    }
    for (const auto &s : styles) st.append(s.toJson());
    for (const auto &b : biz) bz.append(b.toJson());
    for (auto it = images.cbegin(); it != images.cend(); ++it) {
        QJsonObject io{{"id", it.key()}, {"format", it->format}, {"w", it->pixelSize.width()}, {"h", it->pixelSize.height()},
                       {"source", it->sourcePath}, {"linked", it->linked}};
        if (it->linked) {
            io["copy"] = it->keepsCopy;
            if (it->linkPage) io["page"] = it->linkPage;
            io["fileSize"] = double(it->fileSize);
            io["fileTime"] = double(it->fileTime);
            if (!it->fileHash.isEmpty()) io["fileHash"] = QString::fromLatin1(it->fileHash);
        }
        imgs.append(io);
    }
    o["masters"] = ms;
    o["pages"] = ps;
    o["scratch"] = itemsJ(scratch);
    if (!extra.empty()) o["extra"] = itemsJ(extra);
    o["stories"] = ss;
    o["styles"] = st;
    o["business"] = bz;
    o["businessCurrent"] = bizCurrent;
    o["images"] = imgs;
    QJsonArray cs;
    for (int i = 0; i < SlotCount; ++i) cs.append(colorToString(colors.c[i]));
    o["colorScheme"] = QJsonObject{{"name", colors.name}, {"colors", cs}};
    o["fontScheme"] = QJsonObject{{"name", fonts.name}, {"heading", fonts.heading}, {"body", fonts.body}};
    o["merge"] = merge.toJson();
    if (catalog.isActive()) o["catalog"] = catalog.toJson();
    o["props"] = props.toJson();
    o["template"] = templateId;
    o["templateOptions"] = templateOptions;
    o["facing"] = facingPages;
    if (!rulerZero.isNull()) o["rulerZero"] = QJsonArray{rulerZero.x(), rulerZero.y()};
    if (!pubFonts.isEmpty()) o["pubFonts"] = QJsonArray::fromStringList(pubFonts);
    QJsonArray spots;
    for (int i = 0; i < print.spotColors.size(); ++i)
        spots.append(QJsonObject{{"name", print.spotName(i)}, {"color", colorToString(print.spotColors[i])}});
    o["print"] = QJsonObject{{"model", int(print.model)}, {"spots", spots}, {"embedFonts", print.embedFonts},
                            {"overprint", QJsonObject{{"text", print.overprint.text}, {"textBelow", print.overprint.textBelow},
                                                      {"lines", print.overprint.lines}, {"fills", print.overprint.fills},
                                                      {"threshold", print.overprint.threshold}}}};
    return o;
}

void Document::fromJson(const QJsonObject &o)
{
    setup = PageSetup::fromJson(o["setup"].toObject());
    masters.clear();
    pages.clear();
    stories.clear();
    for (const auto &v : o["masters"].toArray()) {
        const auto mo = v.toObject();
        auto m = std::make_shared<MasterPage>();
        pageBaseF(*m, mo);
        m->name = mo["name"].toString();
        m->abbr = mo["abbr"].toString("A");
        m->twoPage = mo["twoPage"].toBool();
        m->grid = GridGuides::fromJson(mo["grid"].toObject());
        masters << m;
    }
    for (const auto &v : o["pages"].toArray()) {
        const auto po = v.toObject();
        auto p = std::make_shared<Page>();
        pageBaseF(*p, po);
        p->masterId = po["master"].toString();
        p->title = po["title"].toString();
        pages << p;
    }
    scratch = itemsF(o["scratch"]);
    extra = itemsF(o["extra"]);
    const auto cso = o["colorScheme"].toObject();
    colors.name = cso["name"].toString();
    const auto cs = cso["colors"].toArray();
    for (int i = 0; i < SlotCount && i < cs.size(); ++i) colors.c[i] = colorFromString(cs[i].toString());
    const auto fso = o["fontScheme"].toObject();
    fonts = {fso["name"].toString(), fso["heading"].toString(), fso["body"].toString()};
    for (const auto &v : o["stories"].toArray()) {
        const auto so = v.toObject();
        auto s = std::make_shared<Story>();
        s->id = so["id"].toString();
        s->doc = std::make_unique<QTextDocument>();
        applyDefaultFont(s->doc.get());
        storyFromJson(s->doc.get(), so);
        stories.insert(s->id, s);
    }
    styles.clear();
    for (const auto &v : o["styles"].toArray()) styles << TextStyle::fromJson(v.toObject());
    if (styles.isEmpty()) styles = defaultStyles();
    biz.clear();
    for (const auto &v : o["business"].toArray()) biz << BusinessInfo::fromJson(v.toObject());
    if (biz.isEmpty()) biz << BusinessInfo();
    bizCurrent = o["businessCurrent"].toInt();
    // Image metadata; bytes are attached by the file loader.
    for (const auto &v : o["images"].toArray()) {
        const auto io = v.toObject();
        ImageData &d = images[io["id"].toString()];
        d.format = io["format"].toString();
        d.pixelSize = QSize(io["w"].toInt(), io["h"].toInt());
        d.sourcePath = io["source"].toString();
        d.linked = io["linked"].toBool();
        d.keepsCopy = io["copy"].toBool(true);
        d.linkPage = io["page"].toInt();
        d.fileSize = qint64(io["fileSize"].toDouble());
        d.fileTime = qint64(io["fileTime"].toDouble());
        d.fileHash = io["fileHash"].toString().toLatin1();
        // Where the file is from the publication wins over where it was.
        // (A relative path that is written as absolute is ignored, and one
        // that leads out of the folder is not looked at.)
        const QString relative = io["relative"].toString();
        if (d.linked && !relative.isEmpty() && !folder.isEmpty() && !looksAbsolute(relative)) {
            const QString found = QDir::cleanPath(QDir(folder).absoluteFilePath(relative));
            if (inFolder(found) && QFileInfo(found).isFile()) d.sourcePath = found;
            else if (!QDir::isAbsolutePath(d.sourcePath)) d.sourcePath = found;   // the full path says nothing
        }
    }
    merge = MergeSource::fromJson(o["merge"].toObject());
    catalog = CatalogArea::fromJson(o["catalog"].toObject());
    props = DocProps::fromJson(o["props"].toObject());
    templateId = o["template"].toString();
    templateOptions = o["templateOptions"].toObject();
    facingPages = o["facing"].toBool();
    const QJsonArray rz = o["rulerZero"].toArray();
    rulerZero = rz.size() == 2 ? QPointF(rz[0].toDouble(), rz[1].toDouble()) : QPointF();
    pubFonts.clear();
    for (const auto &f : o["pubFonts"].toArray()) pubFonts << f.toString();
    const auto pr = o["print"].toObject();
    print.model = PrintInfo::ColorModel(pr["model"].toInt());
    print.spotColors.clear();
    print.spotNames.clear();
    for (const auto &c : pr["spots"].toArray()) {
        if (c.isString()) {   // older files: the color alone
            print.spotColors << QColor(c.toString());
            print.spotNames << QString();
        } else {
            print.spotColors << colorFromString(c.toObject()["color"].toString());
            print.spotNames << c.toObject()["name"].toString();
        }
    }
    print.embedFonts = pr["embedFonts"].toBool(true);
    {
        const QJsonObject op = pr["overprint"].toObject();
        const OverprintSettings def;
        print.overprint.text = op["text"].toBool(def.text);
        print.overprint.textBelow = std::clamp(op["textBelow"].toDouble(def.textBelow), 0.0, 1638.0);
        print.overprint.lines = op["lines"].toBool(def.lines);
        print.overprint.fills = op["fills"].toBool(def.fills);
        print.overprint.threshold = std::clamp(op["threshold"].toInt(def.threshold), 1, 100);
    }
    if (masters.isEmpty()) resetDefaults();
}

} // namespace jp
