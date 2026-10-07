#include "app/toc.h"

#include "app/editor.h"
#include "core/document.h"
#include "core/items.h"
#include "render/renderer.h"
#include "text/textprops.h"

#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace jp {

namespace {

int headingLevel(const QTextBlock &b, int depth)
{
    const QString style = b.blockFormat().stringProperty(tp::StyleName);
    for (int level = 1; level <= depth; ++level)
        if (style == QStringLiteral("Heading %1").arg(level)) return level;
    return 0;
}

// A heading as one line: no soft hyphens, line breaks or tabs.
QString entryText(const QTextBlock &b)
{
    QString t = b.text();
    t.remove(QChar(0x00AD)).remove(QChar::ObjectReplacementCharacter);
    t.replace(QChar::LineSeparator, QLatin1Char(' ')).replace(QLatin1Char('\t'), QLatin1Char(' '));
    return t.simplified();
}

// The text box a story starts in (the first box of its chain).
const TextItem *boxOf(const Document &doc, const QString &storyId)
{
    const TextItem *found = nullptr;
    for (const auto &pg : doc.pages)
        walkItems(pg->items, [&](const ItemPtr &it) {
            if (found || it->type() != ItemType::Text) return;
            auto *t = static_cast<const TextItem *>(it.get());
            if (t->storyId == storyId) found = doc.chainOf(t->id).value(0, const_cast<TextItem *>(t));
        });
    return found;
}

// The width a right-aligned page number goes to: the box's text width (one
// column of it).
double tabPosition(const Document &doc, const QString &storyId)
{
    const TextItem *t = boxOf(doc, storyId);
    if (!t) return 400;
    const double inner = t->rect.width() - t->insets.left() - t->insets.right();
    const int cols = std::max(1, t->columns);
    return std::max(36.0, (inner - (cols - 1) * t->columnGap) / cols - 1);
}

// Replaces the paragraphs [first, last] (or inserts at `first` when last < 0)
// with a table: its title in the Heading 2 style, then an entry per heading
// in the Normal style, indented by level, with a dot leader to the page
// number at a right tab stop.
void writeTable(const Document &doc, QTextDocument *sd, int firstPos, int lastEnd, const QVector<TocEntry> &entries, double tab)
{
    QTextCursor c(sd);
    c.setPosition(firstPos);
    if (lastEnd > firstPos) c.setPosition(lastEnd, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    auto styled = [&](const QString &name, QTextBlockFormat *bf, QTextCharFormat *cf) {
        if (const TextStyle *st = doc.style(name)) {
            tp::setProperties(*bf, st->blk);
            *cf = st->chr;
            cf->setProperty(tp::CharStyle, QString());
        }
        bf->setProperty(tp::StyleName, name);
    };
    bool first = true;
    auto paragraph = [&](int level, const QString &text, bool bold) {
        QTextBlockFormat bf;
        QTextCharFormat cf;
        styled(level == 0 ? QStringLiteral("Heading 2") : QStringLiteral("Normal"), &bf, &cf);
        bf.setProperty(tp::TocLevel, level);
        if (level > 0) {
            const double indent = (level - 1) * 18.0;
            bf.setLeftMargin(indent);
            bf.setTextIndent(0);
            bf.setTopMargin(level == 1 ? 6 : 0);
            bf.setBottomMargin(2);
            // Tab stops count from the paragraph's indent.
            bf.setTabPositions({QTextOption::Tab(std::max(36.0, tab - indent), QTextOption::RightTab)});
            bf.setProperty(tp::TabLeaders, QStringLiteral("."));
            if (bold) cf.setFontWeight(QFont::Bold);
        }
        if (first) {
            c.setBlockFormat(bf);
            c.setBlockCharFormat(cf);
        } else {
            c.insertBlock(bf, cf);
        }
        first = false;
        c.insertText(text, cf);
    };
    paragraph(0, QStringLiteral("Contents"), false);
    if (entries.isEmpty())
        paragraph(1, QStringLiteral("No headings yet: give headings the Heading 1, Heading 2 or Heading 3 style, then update the table."), false);
    for (const TocEntry &e : entries) paragraph(e.level, e.text + QLatin1Char('\t') + QString::number(e.page), e.level == 1);
}

// The tables in a story: [start of first paragraph, end of last).
QVector<QPair<int, int>> tablesIn(QTextDocument *sd)
{
    QVector<QPair<int, int>> out;
    for (QTextBlock b = sd->begin(); b.isValid(); b = b.next()) {
        if (!b.blockFormat().hasProperty(tp::TocLevel)) continue;
        // A table starts at its title (level 0) or where a run of entries begins.
        const bool start = b.blockFormat().intProperty(tp::TocLevel) == 0 || out.isEmpty() || out.last().second + 1 < b.position();
        if (start) out << qMakePair(b.position(), b.position() + b.length() - 1);
        else out.last().second = b.position() + b.length() - 1;
    }
    return out;
}

bool sameEntries(const QVector<TocEntry> &a, const QVector<TocEntry> &b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].level != b[i].level || a[i].page != b[i].page || a[i].text != b[i].text) return false;
    return true;
}

} // namespace

QVector<TocEntry> tableOfContentsEntries(const Document &doc, int depth)
{
    QVector<TocEntry> out;
    QSet<QString> seen;
    LayoutCache cache;
    RenderOptions opt;
    for (int p = 0; p < doc.pages.size(); ++p)
        walkItems(doc.pages[p]->items, [&](const ItemPtr &it) {
            if (it->type() != ItemType::Text) return;
            auto *t = static_cast<const TextItem *>(it.get());
            QTextDocument *sd = doc.storyDoc(t->storyId);
            const auto fl = cache.textFrame(doc, *t, p + 1, opt);
            if (!sd || !fl.layout) return;
            for (const auto &li : fl.layout->lineInfo(fl.frame)) {
                const QTextBlock b = sd->findBlock(li.docStart);
                if (!b.isValid() || b.blockFormat().hasProperty(tp::TocLevel)) continue;
                const QString key = t->storyId + QLatin1Char('|') + QString::number(b.position());
                if (seen.contains(key)) continue;
                seen.insert(key);
                const int level = headingLevel(b, depth);
                const QString text = entryText(b);
                if (level > 0 && !text.isEmpty()) out << TocEntry{level, text, p + 1};
            }
        });
    return out;
}

namespace {

// Rebuilds every table in the publication. Rewriting a table can move the
// headings after it to other pages: again until the numbers hold (three
// rounds at most).
int rebuildTables(Document *d)
{
    int tables = 0;
    QVector<TocEntry> entries = tableOfContentsEntries(*d);
    for (int round = 0; round < 3; ++round) {
        tables = 0;
        for (auto it = d->stories.cbegin(); it != d->stories.cend(); ++it) {
            QTextDocument *sd = it.value() ? it.value()->doc.get() : nullptr;
            if (!sd) continue;
            const auto found = tablesIn(sd);
            tables += int(found.size());
            // Last first, so earlier positions stay put.
            for (int i = int(found.size()) - 1; i >= 0; --i) writeTable(*d, sd, found[i].first, found[i].second, entries, tabPosition(*d, it.key()));
        }
        const QVector<TocEntry> again = tableOfContentsEntries(*d);
        if (sameEntries(again, entries)) break;
        entries = again;
    }
    return tables;
}

} // namespace

void insertTableOfContents(Editor *ed)
{
    Document *d = ed->doc();
    ed->flushTyping();
    if (ed->isEditingText() && ed->textTarget().row < 0) {
        QTextDocument *sd = d->storyDoc(ed->textTarget().storyId);
        if (!sd) return;
        ed->change(QStringLiteral("Insert Table of Contents"), [&] {
            QTextCursor c = ed->cursor();
            c.removeSelectedText();
            // On paragraphs of its own, before the text after the cursor.
            if (c.positionInBlock() > 0) c.insertBlock();
            if (!c.block().text().isEmpty()) {
                c.insertBlock();
                c.movePosition(QTextCursor::PreviousBlock);
            }
            writeTable(*d, sd, c.position(), c.position(), {}, 400);
            rebuildTables(d);
        });
        return;
    }
    // On the current page when it's empty, else on a new page after it:
    // a text box inside the margins, so nothing on the page has to make room.
    ed->change(QStringLiteral("Insert Table of Contents"), [&] {
        const int page = ed->currentPage();
        TextItem *t = nullptr;
        if (d->pages.value(page) && d->pages[page]->items.empty()) {
            ItemPtr box = ed->newTextBox(QRectF(QPointF(0, 0), d->pageSize()).marginsRemoved(d->setup.margins));
            t = static_cast<TextItem *>(box.get());
            ed->addItem(box);
        } else {
            const int at = ed->insertPages(page, 1, false, true);
            if (!d->pages.value(at) || d->pages[at]->items.empty()) return;
            t = dynamic_cast<TextItem *>(d->pages[at]->items.front().get());
        }
        if (QTextDocument *sd = t ? d->storyDoc(t->storyId) : nullptr) writeTable(*d, sd, 0, sd->characterCount() - 1, {}, 400);
        rebuildTables(d);
    });
}

int updateTablesOfContents(Editor *ed)
{
    Document *d = ed->doc();
    int tables = 0;
    for (auto it = d->stories.cbegin(); it != d->stories.cend(); ++it)
        if (it.value() && it.value()->doc) tables += int(tablesIn(it.value()->doc.get()).size());
    if (tables) ed->change(QStringLiteral("Update Table of Contents"), [&] { rebuildTables(d); });
    return tables;
}

} // namespace jp
