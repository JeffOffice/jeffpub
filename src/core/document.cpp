#include "core/document.h"

#include <cmath>

#include "render/metafile.h"
#include "text/storyio.h"
#include "text/textprops.h"

#include <QBuffer>
#include <atomic>
#include <QFont>
#include <QImageReader>
#include <QJsonArray>
#include <QSvgRenderer>
#include <QPainter>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>

namespace jp {

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
    if (format == QLatin1String("svg")) {
        QSvgRenderer r(bytes);
        QSize s = r.defaultSize().isValid() ? r.defaultSize() : QSize(512, 512);
        s.scale(std::max(s.width(), 1200), std::max(s.height(), 1200), Qt::KeepAspectRatio);
        QImage img(s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        r.render(&p);
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
    return {{"size", sizeJ(size)}, {"margins", margJ(margins)}, {"sizeName", sizeName}, {"layout", int(layout)}, {"fold", int(fold)},
            {"sheet", sizeJ(sheet)}, {"gridRows", gridRows}, {"gridCols", gridCols}, {"gapH", gapH}, {"gapV", gapV},
            {"sideMargin", sideMargin}, {"topMargin", topMargin}};
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
    static const QMap<QString, QString> m{{"name", "Organization Name"}, {"tagline", "Tagline or Motto"}, {"person", "Individual Name"},
                                          {"title", "Job Position or Title"}, {"address", "Address"}, {"phone", "Phone"},
                                          {"fax", "Fax"}, {"email", "Email"}, {"web", "Website"}};
    return m.value(k, k);
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

QString Document::addImage(const QByteArray &bytes, const QString &format, const QString &sourcePath)
{
    for (auto it = images.cbegin(); it != images.cend(); ++it)
        if (it->bytes == bytes) return it.key();
    ImageData d;
    d.bytes = bytes;
    d.format = format.toLower();
    d.sourcePath = sourcePath;
    const QImage img = d.image();
    d.pixelSize = img.size();
    const QString id = newId("img");
    images.insert(id, d);
    return id;
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

ItemPtr Document::cloneItem(const Item &src)
{
    ItemPtr c = src.clone();
    std::function<void(Item *)> fix = [&](Item *it) {
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
        case ItemType::Group:
            for (auto &ch : static_cast<GroupItem *>(it)->children) {
                ch->id = newId();
                fix(ch.get());
            }
            break;
        default: break;
        }
    };
    fix(c.get());
    return c;
}

void Document::forEachItem(const std::function<void(Item *, int, const QString &)> &fn) const
{
    for (int p = 0; p < pages.size(); ++p) walkItems(pages[p]->items, [&](const ItemPtr &it) { fn(it.get(), p, QString()); });
    for (const auto &m : masters) walkItems(m->items, [&](const ItemPtr &it) { fn(it.get(), -1, m->id); });
    walkItems(scratch, [&](const ItemPtr &it) { fn(it.get(), -1, QString()); });
}

const TextStyle *Document::style(const QString &name) const
{
    for (const auto &s : styles)
        if (s.name == name) return &s;
    return nullptr;
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
    for (auto it = images.cbegin(); it != images.cend(); ++it)
        imgs.append(QJsonObject{{"id", it.key()}, {"format", it->format}, {"w", it->pixelSize.width()}, {"h", it->pixelSize.height()},
                                {"source", it->sourcePath}, {"linked", it->linked}});
    o["masters"] = ms;
    o["pages"] = ps;
    o["scratch"] = itemsJ(scratch);
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
    QJsonArray spots;
    for (int i = 0; i < print.spotColors.size(); ++i)
        spots.append(QJsonObject{{"name", print.spotName(i)}, {"color", colorToString(print.spotColors[i])}});
    o["print"] = QJsonObject{{"model", int(print.model)}, {"spots", spots}, {"embedFonts", print.embedFonts}};
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
    }
    merge = MergeSource::fromJson(o["merge"].toObject());
    catalog = CatalogArea::fromJson(o["catalog"].toObject());
    props = DocProps::fromJson(o["props"].toObject());
    templateId = o["template"].toString();
    templateOptions = o["templateOptions"].toObject();
    facingPages = o["facing"].toBool();
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
    if (masters.isEmpty()) resetDefaults();
}

} // namespace jp
