#include "templates/changetemplate.h"

#include "text/storyio.h"
#include "text/textprops.h"

#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

namespace jp {

namespace {

// ---------------- what a box is for ----------------

// What the text of a box says about its role.
struct Facts {
    QString plain;
    int chars = 0;            // letters and marks, not spaces or fields
    int running = 0;          // characters set as running text
    int lines = 0;            // paragraphs with something in them
    double biggest = 0;       // the largest type, in points
    QString firstStyle;       // the style of the first paragraph
    QStringList fields;       // every field code in the text
    // The paragraph set largest (the first of them): the box's headline.
    double leadSize = 0;
    QStringList leadFields;
    QString leadText;
};

Facts factsOf(const Document &d, const QTextDocument *sd)
{
    Facts f;
    if (!sd) return f;
    f.plain = sd->toPlainText();
    bool seen = false;
    for (QTextBlock b = sd->begin(); b.isValid(); b = b.next()) {
        const QString style = b.blockFormat().stringProperty(tp::StyleName);
        const double styleSize = style.isEmpty() ? 0 : d.resolvedStyle(style).chr.fontPointSize();
        int len = 0;
        double biggest = 0;
        QStringList fields;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat cf = it.fragment().charFormat();
            const QString field = cf.stringProperty(tp::Field);
            if (!field.isEmpty()) fields << field;
            int n = 0;
            for (const QChar c : it.fragment().text()) n += !c.isSpace() && c != QChar::ObjectReplacementCharacter;
            if (!n && field.isEmpty()) continue;
            double pt = cf.fontPointSize();
            if (pt <= 0) pt = b.charFormat().fontPointSize();
            if (pt <= 0) pt = styleSize;
            if (pt <= 0) pt = 11;
            biggest = std::max(biggest, pt);
            len += n;
        }
        if (!len && fields.isEmpty()) continue;
        if (!seen) f.firstStyle = style;
        seen = true;
        f.chars += len;
        ++f.lines;
        f.fields << fields;
        f.biggest = std::max(f.biggest, biggest);
        if (biggest > f.leadSize) {
            f.leadSize = biggest;
            f.leadFields = fields;
            f.leadText = b.text();
        }
        const bool plainStyle = style.isEmpty() || style == QLatin1String("Normal") || style == QLatin1String("Body Text") || style == QLatin1String("List Bullet");
        if (plainStyle && ((len >= 40 && biggest <= 14.5) || (style == QLatin1String("Body Text") && len >= 8))) f.running += len;
    }
    return f;
}

bool hasField(const QStringList &fields, const char *prefix)
{
    for (const QString &c : fields)
        if (c.startsWith(QLatin1String(prefix))) return true;
    return false;
}

bool looksLikeDate(const QString &text)
{
    static const QRegularExpression date(
        QStringLiteral("\\b(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\\.?\\s+\\d{1,2}\\b"
                       "|\\b(mon|tues|wednes|thurs|fri|satur|sun)day"
                       "|\\b\\d{1,2}[/.-]\\d{1,2}[/.-]\\d{2,4}\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return date.match(text).hasMatch();
}

// The role the text alone settles, or none (the box is display type or
// short text, which its place among the others decides).
QString roleFromText(const Facts &f)
{
    if (hasField(f.fields, "mergeblock:")) return role::Recipient;
    if (f.firstStyle == QLatin1String("Caption") && f.chars < 200) return role::Caption;
    if (f.firstStyle == QLatin1String("Quote") && f.chars < 300) return role::Quote;
    if (f.firstStyle == QLatin1String("Subtitle") && f.chars < 200) return role::Subtitle;
    if (f.running >= 40) return role::Body;
    if (f.lines >= 4 && f.fields.isEmpty()) return role::Body;   // a list, or a run of short paragraphs
    const bool display = f.leadSize >= 18;
    if (display && f.leadFields.isEmpty() && !looksLikeDate(f.leadText)) return QString();
    if (display && hasField(f.leadFields, "biz:person")) return role::Person;
    if (display && (hasField(f.leadFields, "biz:name") || hasField(f.leadFields, "biz:tagline"))) return role::Organization;
    if (hasField(f.fields, "biz:address") || hasField(f.fields, "biz:phone") || hasField(f.fields, "biz:fax") || hasField(f.fields, "biz:email") ||
        hasField(f.fields, "biz:web"))
        return role::Address;
    if (hasField(f.fields, "biz:name") || hasField(f.fields, "biz:tagline")) return role::Organization;
    if (hasField(f.fields, "biz:person") || hasField(f.fields, "biz:title")) return role::Person;
    if (f.chars < 160 && (hasField(f.fields, "date") || looksLikeDate(f.leadText) || looksLikeDate(f.plain))) return role::Date;
    return QString();
}

// Page by page, top to bottom, left to right. Boxes whose tops are within
// a quarter inch of the row's first share the row.
template <typename T>
void inReadingOrder(QVector<T> &v)
{
    std::stable_sort(v.begin(), v.end(), [](const T &a, const T &b) { return a.page != b.page ? a.page < b.page : a.rect.top() < b.rect.top(); });
    for (int i = 0; i < v.size();) {
        int j = i + 1;
        while (j < v.size() && v[j].page == v[i].page && v[j].rect.top() - v[i].rect.top() < 18) ++j;
        std::stable_sort(v.begin() + i, v.begin() + j, [](const T &a, const T &b) { return a.rect.left() < b.rect.left(); });
        i = j;
    }
}

bool isLogo(const Item &it) { return it.name == QLatin1String("Logo"); }

// A picture of a design: its stand-in artwork, which is not your picture.
bool isDesignArt(const Document &d, const PictureItem &p)
{
    const auto img = d.images.constFind(p.imageId);
    return img != d.images.cend() && img->sourcePath.startsWith(QLatin1String("art:"));
}

} // namespace

QHash<QString, QString> contentRoles(const Document &d)
{
    QHash<QString, QString> out;
    QHash<QString, const TextItem *> texts;
    QSet<QString> continued;
    for (const auto &pg : d.pages)
        for (const auto &it : pg->items)
            if (it->type() == ItemType::Text) {
                const auto *t = static_cast<const TextItem *>(it.get());
                texts.insert(t->id, t);
                if (!t->nextId.isEmpty()) continued.insert(t->nextId);
            }
    // Boxes the text alone does not settle wait for the headline to be picked.
    struct Box {
        const Item *item;
        int page;
        QRectF rect;
        double size;      // the largest type; 0 for TextArt
    };
    QVector<Box> waiting, art;
    bool haveTitle = false;
    for (int p = 0; p < d.pages.size(); ++p)
        for (const auto &it : d.pages[p]->items) {
            switch (it->type()) {
            case ItemType::Picture:
                out[it->id] = !it->role.isEmpty() ? it->role : isLogo(*it) ? role::Logo : role::Picture;
                break;
            case ItemType::TextArt:
                if (it->role.isEmpty()) art.push_back({it.get(), p, it->rect, 0});
                else out[it->id] = it->role;
                break;
            case ItemType::Text: {
                if (continued.contains(it->id)) break;
                const auto *t = static_cast<const TextItem *>(it.get());
                const Facts f = factsOf(d, d.storyDoc(t->storyId));
                const QString r = !t->role.isEmpty() ? t->role : f.chars || !f.fields.isEmpty() ? roleFromText(f) : role::Body;
                if (r.isEmpty()) waiting.push_back({t, p, t->rect, f.biggest});
                else out[t->id] = r;
                break;
            }
            default: break;
            }
        }
    for (auto it = out.cbegin(); it != out.cend(); ++it) haveTitle = haveTitle || it.value() == role::Title;
    inReadingOrder(art);
    for (const Box &b : art) {
        out[b.item->id] = haveTitle ? role::Heading : role::Title;
        haveTitle = true;
    }
    // The headline is the largest type on the first page; other display type
    // is a subtitle there and a heading further on; the rest is short text.
    inReadingOrder(waiting);
    int title = -1;
    if (!haveTitle)
        for (int i = 0; i < waiting.size() && waiting[i].page == waiting.first().page; ++i)
            if (waiting[i].size >= 16 && (title < 0 || waiting[i].size > waiting[title].size)) title = i;
    for (int i = 0; i < waiting.size(); ++i) {
        const Box &b = waiting[i];
        out[b.item->id] = i == title ? role::Title : b.size < 16 ? role::Label : b.page == 0 ? role::Subtitle : role::Heading;
    }
    // A box that continues another has its story's role.
    for (auto it = texts.cbegin(); it != texts.cend(); ++it) {
        if (continued.contains(it.key())) continue;
        QSet<QString> seen{it.key()};
        for (QString next = it.value()->nextId; !next.isEmpty() && texts.contains(next) && !seen.contains(next); next = texts[next]->nextId) {
            seen.insert(next);
            out[next] = out.value(it.key());
        }
    }
    return out;
}

void tagRoles(Document &d)
{
    const QHash<QString, QString> roles = contentRoles(d);
    for (const auto &pg : d.pages)
        for (const auto &it : pg->items)
            if (it->role.isEmpty()) it->role = roles.value(it->id);
}

TemplateOptions optionsForChange(const Document &current, TemplateOptions o)
{
    if (o.colorScheme.isEmpty()) o.colors = current.colors;
    if (o.fontScheme.isEmpty()) o.fonts = current.fonts;
    return o;
}

namespace {

// ---------------- moving the content ----------------

// The text of the stories and TextArt the open design's own samples say, so
// that a box you never typed in is not taken for yours.
QSet<QString> sampleTexts(const Document &d)
{
    QSet<QString> out;
    const TemplateInfo *t = findTemplate(d.templateId);
    if (!t) return out;
    TemplateOptions o;
    o.options = d.templateOptions;
    const auto sample = t->build(o);
    for (const auto &pg : sample->pages)
        for (const auto &it : pg->items) {
            if (it->type() == ItemType::Text) out.insert(sample->storyDoc(static_cast<const TextItem *>(it.get())->storyId)->toPlainText());
            if (it->type() == ItemType::TextArt) out.insert(static_cast<const TextArtItem *>(it.get())->text);
        }
    return out;
}

// A story or picture of the publication, or a place for one in the design.
struct Piece {
    int page;
    QRectF rect;
    QString role;
    Item *item;
};

// A story's words on one line, for a TextArt.
QString oneLine(const QString &text)
{
    QString s = text;
    s.remove(QChar::ObjectReplacementCharacter);
    return s.replace(QLatin1Char('\n'), QLatin1Char(' ')).simplified();
}

// `was` set as `model` is, keeping what the words themselves do: a field or
// link, bold or italic, another color, typeface, or size than the rest of
// their paragraph.
QTextCharFormat restyled(const QTextCharFormat &was, const QTextCharFormat &main, const QTextCharFormat &model)
{
    QTextCharFormat f = model;
    for (int prop : {int(tp::Field), int(tp::InlineObject), int(tp::CharStyle), int(tp::Language), int(tp::NoProof), int(QTextFormat::IsAnchor),
                     int(QTextFormat::AnchorHref), int(QTextFormat::AnchorName)})
        if (was.hasProperty(prop)) f.setProperty(prop, was.property(prop));
    if (was.fontWeight() != main.fontWeight()) f.setFontWeight(was.fontWeight());
    if (was.fontItalic() != main.fontItalic()) f.setFontItalic(was.fontItalic());
    if (was.underlineStyle() != main.underlineStyle()) {
        f.setUnderlineStyle(was.underlineStyle());
        if (was.hasProperty(tp::UnderlineKind)) f.setProperty(tp::UnderlineKind, was.property(tp::UnderlineKind));
    }
    if (was.fontStrikeOut() != main.fontStrikeOut()) f.setFontStrikeOut(was.fontStrikeOut());
    if (was.verticalAlignment() != main.verticalAlignment()) f.setVerticalAlignment(was.verticalAlignment());
    if (was.stringProperty(tp::ColorRefP) != main.stringProperty(tp::ColorRefP) || was.foreground() != main.foreground()) {
        f.clearProperty(tp::ColorRefP);
        if (was.hasProperty(tp::ColorRefP)) f.setProperty(tp::ColorRefP, was.property(tp::ColorRefP));
        if (was.hasProperty(QTextFormat::ForegroundBrush)) f.setForeground(was.foreground());
    }
    if (was.stringProperty(tp::HighlightRefP) != main.stringProperty(tp::HighlightRefP) || was.background() != main.background()) {
        f.clearProperty(tp::HighlightRefP);
        if (was.hasProperty(tp::HighlightRefP)) f.setProperty(tp::HighlightRefP, was.property(tp::HighlightRefP));
        if (was.hasProperty(QTextFormat::BackgroundBrush)) f.setBackground(was.background());
    }
    if (was.fontFamilies() != main.fontFamilies() || was.stringProperty(tp::ThemeFont) != main.stringProperty(tp::ThemeFont)) {
        f.clearProperty(QTextFormat::FontFamilies);
        f.clearProperty(tp::ThemeFont);
        if (was.hasProperty(QTextFormat::FontFamilies)) f.setProperty(QTextFormat::FontFamilies, was.property(QTextFormat::FontFamilies));
        if (was.hasProperty(tp::ThemeFont)) f.setProperty(tp::ThemeFont, was.property(tp::ThemeFont));
    }
    if (main.fontPointSize() > 0 && was.fontPointSize() > 0 && model.fontPointSize() > 0 && std::abs(was.fontPointSize() - main.fontPointSize()) > 0.01)
        f.setFontPointSize(model.fontPointSize() * was.fontPointSize() / main.fontPointSize());
    return f;
}

QTextCharFormat mainFormat(const QTextBlock &b)
{
    QTextCharFormat main = b.charFormat();
    int most = -1;
    for (auto it = b.begin(); !it.atEnd(); ++it)
        if (it.fragment().length() > most) {
            most = it.fragment().length();
            main = it.fragment().charFormat();
        }
    return main;
}

// The paragraphs of `doc` set the way `like`, a story of the new design, is:
// its alignment, spacing, and type, so the words look as the design has them.
// Each paragraph follows the paragraph of `like` at the same place when both
// have as many, else one of the same style, else, for running text, the
// longest one (a story's paragraphs are alike), and for anything else the one
// at the same place (the headline, then what goes under it).
void setLike(QTextDocument *doc, const QTextDocument *like, bool running)
{
    struct Model {
        QTextBlockFormat block;
        QTextCharFormat chr;
        QString style;
        int length;
    };
    QVector<Model> models;
    if (!like) return;
    for (QTextBlock b = like->begin(); b.isValid(); b = b.next()) {
        Model m;
        m.block = b.blockFormat();
        m.block.clearProperty(QTextFormat::ObjectIndex);   // a list the sample belonged to is not the words'
        m.style = m.block.stringProperty(tp::StyleName);
        m.length = b.length() - 1;
        m.chr = mainFormat(b);
        for (int prop : {int(tp::Field), int(tp::InlineObject), int(QTextFormat::IsAnchor), int(QTextFormat::AnchorHref), int(QTextFormat::AnchorName)})
            m.chr.clearProperty(prop);
        models << m;
    }
    if (models.isEmpty()) return;
    int longest = 0;
    for (int i = 1; i < models.size(); ++i)
        if (models[i].length > models[longest].length) longest = i;
    const bool pairwise = doc->blockCount() == models.size();
    int n = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next(), ++n) {
        int m = running ? longest : std::min(n, int(models.size()) - 1);
        if (pairwise) m = n;
        else if (const QString style = b.blockFormat().stringProperty(tp::StyleName); !style.isEmpty())
            for (int k = 0; k < models.size(); ++k)
                if (models[k].style == style) {
                    m = k;
                    break;
                }
        const Model &model = models[m];
        const QTextCharFormat main = mainFormat(b);
        struct Run {
            int pos, len;
            QTextCharFormat format;
        };
        QVector<Run> runs;
        for (auto it = b.begin(); !it.atEnd(); ++it) runs.push_back({it.fragment().position(), it.fragment().length(), restyled(it.fragment().charFormat(), main, model.chr)});
        QTextCursor c(b);
        c.mergeBlockFormat(model.block);
        for (const Run &r : runs) {
            QTextCursor rc(doc);
            rc.setPosition(r.pos);
            rc.setPosition(r.pos + r.len, QTextCursor::KeepAnchor);
            rc.setCharFormat(r.format);
        }
        QTextCursor end(b);
        end.setBlockCharFormat(model.chr);
    }
}

// A copy of `from` for Extra Content: alone, with the role it had.
ItemPtr extraCopy(Document &r, const Item &from, const QString &role)
{
    ItemPtr c = r.cloneItem(from);
    c->role = role;
    if (c->type() == ItemType::Text) {
        auto &t = static_cast<TextItem &>(*c);
        t.continuedOn = t.continuedFrom = false;
    }
    return c;
}

// A copy of the story `from` without its first paragraph (the headline), as
// a text box of its own; null when nothing is left.
ItemPtr afterHeadline(Document &r, const TextItem &from)
{
    ItemPtr rest = extraCopy(r, from, role::Subtitle);
    QTextDocument *sd = r.storyDoc(static_cast<TextItem &>(*rest).storyId);
    QTextCursor c(sd);
    auto dropFirst = [&] {
        const QTextBlockFormat next = sd->begin().next().isValid() ? sd->begin().next().blockFormat() : QTextBlockFormat();
        c.setPosition(0);
        c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(0);
        c.setBlockFormat(next);   // the first paragraph's format would stay otherwise
    };
    while (sd->blockCount() > 1 && sd->begin().text().trimmed().isEmpty()) dropFirst();
    if (sd->blockCount() < 2) return nullptr;
    dropFirst();
    return sd->toPlainText().trimmed().isEmpty() ? nullptr : rest;
}

// The new design's box `slot` holds the story or TextArt `from`, in the
// design's own look. `r` has the publication's stories under their own ids.
// A TextArt takes the headline of a story; what follows it is returned.
ItemPtr fillSlot(Document &r, Item &slot, const Item &from, bool running)
{
    const QString words = from.type() == ItemType::TextArt ? static_cast<const TextArtItem &>(from).text
                                                           : r.storyDoc(static_cast<const TextItem &>(from).storyId)->toPlainText();
    if (slot.type() == ItemType::TextArt) {
        QString headline = words;
        for (const QString &line : words.split(QLatin1Char('\n')))
            if (!oneLine(line).isEmpty()) {
                headline = line;
                break;
            }
        static_cast<TextArtItem &>(slot).text = oneLine(headline);
        return from.type() == ItemType::Text ? afterHeadline(r, static_cast<const TextItem &>(from)) : nullptr;
    }
    auto &box = static_cast<TextItem &>(slot);
    const QString story = from.type() == ItemType::TextArt ? r.createStory(oneLine(words)) : r.copyStory(static_cast<const TextItem &>(from).storyId);
    const QString sample = box.storyId;
    setLike(r.storyDoc(story), r.storyDoc(sample), running);
    for (TextItem *frame : r.chainOf(box.id)) frame->storyId = story;
    box.storyId = story;
    box.fitScale = 1.0;
    r.removeStory(sample);
    return nullptr;
}

// Your picture in a design's placeholder: its frame, your picture cropped
// to fill it, with your own adjustments.
void fillPlaceholder(Document &r, PictureItem &holder, const PictureItem &mine)
{
    holder.imageId = mine.imageId;
    holder.fitImage(r.imageSize(mine.imageId), true);
    holder.brightness = mine.brightness;
    holder.contrast = mine.contrast;
    holder.recolor = mine.recolor;
    holder.recolorColor = mine.recolorColor;
    holder.hasTransparentColor = mine.hasTransparentColor;
    holder.transparentColor = mine.transparentColor;
    holder.transparency = mine.transparency;
    holder.altText = mine.altText;
    holder.hyperlink = mine.hyperlink;
}

} // namespace

std::unique_ptr<Document> applyDesign(const Document &current, std::unique_ptr<Document> design, ChangeReport *report)
{
    ChangeReport rep;
    Document &r = *design;
    tagRoles(r);
    // Everything of the publication comes across under its own ids; what ends up unused goes at the end.
    for (auto it = current.stories.cbegin(); it != current.stories.cend(); ++it) {
        auto s = std::make_shared<Story>();
        s->id = it.key();
        s->doc = std::make_unique<QTextDocument>();
        r.applyDefaultFont(s->doc.get());
        storyFromJson(s->doc.get(), storyToJson((*it)->doc.get()));
        r.stories.insert(s->id, s);
    }
    for (auto it = current.images.cbegin(); it != current.images.cend(); ++it)
        if (!r.images.contains(it.key())) r.images.insert(it.key(), it.value());

    // Your stories and pictures, and the design's boxes and placeholders for them.
    const QHash<QString, QString> mine = contentRoles(current);
    const QSet<QString> samples = sampleTexts(current);
    QVector<Piece> stories, pictures;
    {
        QSet<QString> continued;
        for (const auto &pg : current.pages)
            for (const auto &it : pg->items)
                if (it->type() == ItemType::Text) continued.insert(static_cast<const TextItem *>(it.get())->nextId);
        for (int p = 0; p < current.pages.size(); ++p)
            for (const auto &it : current.pages[p]->items) {
                const QString role = mine.value(it->id);
                if (it->type() == ItemType::Text && !continued.contains(it->id)) {
                    const QTextDocument *sd = current.storyDoc(static_cast<const TextItem *>(it.get())->storyId);
                    const QString words = sd ? sd->toPlainText() : QString();
                    if (words.trimmed().isEmpty() || samples.contains(words)) continue;
                    stories.push_back({p, it->rect, role, it.get()});
                } else if (it->type() == ItemType::TextArt) {
                    const QString words = static_cast<const TextArtItem *>(it.get())->text;
                    if (words.trimmed().isEmpty() || samples.contains(words)) continue;
                    stories.push_back({p, it->rect, role, it.get()});
                } else if (it->type() == ItemType::Picture) {
                    const auto *pic = static_cast<const PictureItem *>(it.get());
                    if (pic->imageId.isEmpty() || isLogo(*pic) || isDesignArt(current, *pic)) continue;
                    pictures.push_back({p, it->rect, role, it.get()});
                }
            }
    }
    QVector<Piece> boxes, holders;
    {
        const QHash<QString, QString> theirs = contentRoles(r);
        QSet<QString> continued;
        for (const auto &pg : r.pages)
            for (const auto &it : pg->items)
                if (it->type() == ItemType::Text) continued.insert(static_cast<const TextItem *>(it.get())->nextId);
        for (int p = 0; p < r.pages.size(); ++p)
            for (const auto &it : r.pages[p]->items) {
                if ((it->type() == ItemType::Text && !continued.contains(it->id)) || it->type() == ItemType::TextArt)
                    boxes.push_back({p, it->rect, theirs.value(it->id), it.get()});
                else if (it->type() == ItemType::Picture && !isLogo(*it))
                    holders.push_back({p, it->rect, role::Picture, it.get()});
            }
    }
    inReadingOrder(stories);
    inReadingOrder(pictures);
    inReadingOrder(boxes);
    inReadingOrder(holders);

    // Each story takes the first box of its role that is free; each picture,
    // the next placeholder. What has no place follows the Extra Content already there.
    for (const auto &e : current.extra) r.extra.push_back(r.cloneItem(*e));
    QVector<bool> taken(boxes.size(), false);
    for (const Piece &s : stories) {
        int k = -1;
        for (int i = 0; i < boxes.size() && k < 0; ++i)
            if (!taken[i] && boxes[i].role == s.role) k = i;
        ItemPtr left;
        if (k < 0) {
            left = extraCopy(r, *s.item, s.role);
        } else {
            taken[k] = true;
            ++rep.stories;
            left = fillSlot(r, *boxes[k].item, *s.item, boxes[k].role == role::Body);
        }
        if (left) {
            r.extra.push_back(left);
            ++rep.extraStories;
        }
    }
    for (int i = 0; i < pictures.size(); ++i) {
        if (i < holders.size()) {
            fillPlaceholder(r, *static_cast<PictureItem *>(holders[i].item), *static_cast<const PictureItem *>(pictures[i].item));
            ++rep.pictures;
            continue;
        }
        r.extra.push_back(extraCopy(r, *pictures[i].item, pictures[i].role));
        ++rep.extraPictures;
    }

    // The rest of the publication stays its own.
    for (const auto &it : r.cloneItems(current.scratch)) r.scratch.push_back(it);
    r.props = current.props;
    r.print = current.print;
    r.styles = current.styles;
    r.pubFonts = current.pubFonts;
    r.facingPages = current.facingPages;
    r.setup.firstPageNumber = current.setup.firstPageNumber;
    r.setup.pageNumberFormat = current.setup.pageNumberFormat;
    if (!current.merge.isEmpty()) r.merge = current.merge;
    // Your business information; the set the gallery chose is the current one.
    int chosen = current.bizCurrent;
    if (!r.biz.isEmpty())
        for (int i = 0; i < current.biz.size(); ++i) {
            bool same = current.biz[i].setName == r.biz.first().setName;
            for (const QString &key : BusinessInfo::keys()) same = same && current.biz[i].field(key) == r.biz.first().field(key);
            if (same) {
                chosen = i;
                break;
            }
        }
    r.biz = current.biz;
    r.bizCurrent = chosen;

    const QSet<QString> used = r.storiesInUse();
    for (const QString &id : r.stories.keys())
        if (!used.contains(id)) r.removeStory(id);
    if (report) *report = rep;
    return design;
}

} // namespace jp
