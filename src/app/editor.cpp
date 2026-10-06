#include "app/editor.h"
#include "app/settings.h"
#include "app/appfuncs.h"

#include <algorithm>

#include "core/presets.h"
#include "text/storyio.h"
#include "text/textprops.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QUndoCommand>

namespace jp {

static const char *kItemsMime = "application/x-jeffpub-items";

class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Editor *e, const QString &label, QByteArray before, QByteArray after, QStringList selB, QStringList selA,
                    int pageB, int pageA, QString masterB, QString masterA)
        : QUndoCommand(label), m_e(e), m_before(std::move(before)), m_after(std::move(after)), m_selB(std::move(selB)),
          m_selA(std::move(selA)), m_pageB(pageB), m_pageA(pageA), m_masterB(std::move(masterB)), m_masterA(std::move(masterA))
    {
    }
    void undo() override { m_e->restore(m_before, m_selB, m_pageB, m_masterB); }
    void redo() override
    {
        if (m_first) { m_first = false; return; }
        m_e->restore(m_after, m_selA, m_pageA, m_masterA);
    }

private:
    Editor *m_e;
    QByteArray m_before, m_after;
    QStringList m_selB, m_selA;
    int m_pageB, m_pageA;
    QString m_masterB, m_masterA;
    bool m_first = true;
};

Editor::Editor(QObject *parent) : QObject(parent)
{
    m_doc = Document::blank(QSizeF(612, 792));
    m_typingTimer.setSingleShot(true);
    m_typingTimer.setInterval(1200);
    connect(&m_typingTimer, &QTimer::timeout, this, &Editor::flushTyping);
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) { Q_EMIT modifiedChanged(!clean); });
}

Editor::~Editor() = default;

void Editor::setDocument(std::unique_ptr<Document> d, const QString &path)
{
    m_typingTimer.stop();
    m_typing = false;
    m_changeDepth = 0;
    m_text = TextTarget();
    m_cursor = QTextCursor();
    m_doc = std::move(d);
    m_cache.clear();
    m_path = path;
    m_page = 0;
    m_master.clear();
    m_sel.clear();
    m_mergeRecord = -1;
    m_spread = m_doc->facingPages;
    m_tool = Tool::Select;
    linkSource.clear();
    m_undo.clear();
    m_undo.setClean();
    Q_EMIT documentReplaced();
    Q_EMIT pageChanged();
    Q_EMIT selectionChanged();
    Q_EMIT changed();
}

void Editor::setFilePath(const QString &p)
{
    m_path = p;
    Q_EMIT changed();
}

QString Editor::displayName() const
{
    if (!m_path.isEmpty()) return QFileInfo(m_path).completeBaseName();
    return m_doc->props.title.isEmpty() ? QStringLiteral("Publication1") : m_doc->props.title;
}

// ---------- view ----------
void Editor::setCurrentPage(int i, bool keepTextEdit)
{
    i = std::clamp(i, 0, int(m_doc->pages.size()) - 1);
    if (i == m_page && m_master.isEmpty()) return;
    if (!keepTextEdit) endTextEdit();
    m_page = i;
    m_master.clear();
    if (!keepTextEdit) m_sel.clear();
    Q_EMIT pageChanged();
    Q_EMIT selectionChanged();
}

void Editor::setMasterView(const QString &id)
{
    endTextEdit();
    if (!id.isEmpty() && !m_doc->master(id)) return;
    m_master = id;
    m_sel.clear();
    Q_EMIT pageChanged();
    Q_EMIT selectionChanged();
}

void Editor::setTwoPageSpread(bool on)
{
    m_spread = on;
    Q_EMIT viewChanged();
    Q_EMIT pageChanged();
}

void Editor::setView(const std::function<void(ViewOptions &)> &fn)
{
    fn(view);
    Q_EMIT viewChanged();
}

void Editor::setMergeRecord(int r)
{
    m_mergeRecord = r;
    Q_EMIT viewChanged();
    Q_EMIT changed();
}

QVector<int> Editor::visiblePages() const
{
    if (!m_master.isEmpty()) return {};
    const int n = m_doc->pages.size();
    if (!m_spread || n < 2) return {m_page};
    // Page 1 stands alone on the right; then 2-3, 4-5, ...
    if (m_page == 0) return {0};
    const int left = (m_page % 2 == 1) ? m_page : m_page - 1;
    QVector<int> v{left};
    if (left + 1 < n) v << left + 1;
    return v;
}

// ---------- surface ----------
PageBase *Editor::surface() const
{
    if (!m_master.isEmpty())
        if (MasterPage *m = m_doc->master(m_master)) return m;
    return m_doc->pages[std::clamp(m_page, 0, int(m_doc->pages.size()) - 1)].get();
}

ItemList &Editor::surfaceItems() const { return surface()->items; }

QSizeF Editor::surfaceSize() const
{
    QSizeF s = m_doc->pageSize();
    if (!m_master.isEmpty())
        if (MasterPage *m = m_doc->master(m_master); m && m->twoPage) s.setWidth(s.width() * 2);
    return s;
}

int Editor::surfacePageNumber() const { return m_master.isEmpty() ? m_page + 1 : 1; }

RenderOptions Editor::renderOptions() const
{
    RenderOptions o;
    o.mergeRecord = m_mergeRecord;
    o.shadeFields = view.fields;
    o.showSpecial = view.special;
    o.tableGridlines = view.gridlines;
    if (isEditingText()) {
        o.editStory = m_text.storyId;
        o.selFrom = m_cursor.selectionStart();
        o.selTo = m_cursor.selectionEnd();
    }
    if (view.spelling) {
        for (auto it = m_doc->stories.cbegin(); it != m_doc->stories.cend(); ++it) {
            QVector<QPair<int, int>> r = misspelledIn(it.key());
            // Don't flag the word still being typed.
            if (isEditingText() && it.key() == m_text.storyId && !m_cursor.hasSelection()) {
                const int pos = m_cursor.position();
                r.erase(std::remove_if(r.begin(), r.end(), [pos](const QPair<int, int> &m) { return pos >= m.first && pos <= m.second; }), r.end());
            }
            if (!r.isEmpty()) o.misspelled.insert(it.key(), r);
        }
    }
    return o;
}

QVector<QPair<int, int>> Editor::misspelledIn(const QString &storyId) const
{
    if (!view.spelling) return {};
    const Story *st = m_doc->story(storyId);
    if (!st || !st->doc) return {};
    SpellEntry &e = m_spell[storyId];
    if (e.serial != st->serial || e.revision != st->doc->revision()) {
        e.serial = st->serial;
        e.revision = st->doc->revision();
        e.ranges = misspellings(st->doc.get());
    }
    return e.ranges;
}

// ---------- selection ----------
void Editor::select(const QStringList &ids, bool add)
{
    QStringList next = add ? m_sel : QStringList();
    for (const auto &id : ids)
        if (!next.contains(id) && m_doc->item(id)) next << id;
    if (next == m_sel) return;
    if (isEditingText() && !next.contains(m_text.itemId)) endTextEdit();
    if (!pointsItem.isEmpty() && !next.contains(pointsItem)) pointsItem.clear();
    if (!wrapItem.isEmpty() && !next.contains(wrapItem)) wrapItem.clear();
    m_sel = next;
    Q_EMIT selectionChanged();
}

void Editor::toggleSelect(const QString &id)
{
    QStringList next = m_sel;
    if (next.contains(id)) next.removeAll(id); else next << id;
    if (isEditingText()) endTextEdit();
    m_sel = next;
    Q_EMIT selectionChanged();
}

QVector<Item *> Editor::selectedItems() const
{
    QVector<Item *> out;
    for (const auto &id : m_sel)
        if (Item *it = m_doc->item(id)) out << it;
    return out;
}

Item *Editor::single() const
{
    const auto v = selectedItems();
    return v.size() == 1 ? v.first() : nullptr;
}

QString Editor::selectionKind() const
{
    const auto v = selectedItems();
    if (v.isEmpty()) return {};
    if (v.size() > 1) return QStringLiteral("multi");
    switch (v.first()->type()) {
    case ItemType::Text: return QStringLiteral("text");
    case ItemType::Picture: return QStringLiteral("picture");
    case ItemType::Shape: return QStringLiteral("shape");
    case ItemType::Line: return QStringLiteral("line");
    case ItemType::Table: return QStringLiteral("table");
    case ItemType::TextArt: return QStringLiteral("textart");
    case ItemType::Group: return QStringLiteral("group");
    }
    return {};
}

QRectF Editor::selectionBounds() const
{
    QRectF r;
    for (Item *it : selectedItems()) r = r.isNull() ? it->bounds() : r.united(it->bounds());
    return r;
}

QStringList Editor::topLevelSelection() const
{
    // Selected items that live directly in a list (not inside a selected group).
    QStringList out;
    for (const auto &id : m_sel) {
        const auto loc = m_doc->find(id);
        if (!loc.item) continue;
        bool parentSelected = false;
        if (loc.parent) parentSelected = m_sel.contains(loc.parent->id);
        if (!parentSelected) out << id;
    }
    return out;
}

// ---------- text editing ----------
QTextDocument *Editor::editDoc() const { return m_doc->storyDoc(m_text.storyId); }

void Editor::beginTextEdit(const QString &itemId, int pos, int row, int col)
{
    Item *it = m_doc->item(itemId);
    if (!it) return;
    QString story;
    if (it->type() == ItemType::Text) {
        const auto chain = m_doc->chainOf(itemId);
        story = chain.isEmpty() ? static_cast<TextItem *>(it)->storyId : chain.first()->storyId;
    } else if (it->type() == ItemType::Shape) {
        auto *s = static_cast<ShapeItem *>(it);
        if (s->storyId.isEmpty()) {
            change(QStringLiteral("Add Text"), [&] {
                s->storyId = m_doc->createStory();
                QTextCursor c(m_doc->storyDoc(s->storyId));
                QTextBlockFormat bf;
                bf.setAlignment(Qt::AlignHCenter);
                c.mergeBlockFormat(bf);
                QTextCharFormat cf;
                cf.setProperty(tp::ColorRefP, QStringLiteral("#FFFFFF"));
                c.mergeBlockCharFormat(cf);
                c.mergeCharFormat(cf);
            });
        }
        story = s->storyId;
    } else if (it->type() == ItemType::Table) {
        auto *t = static_cast<TableItem *>(it);
        if (row < 0 || col < 0) { row = 0; col = 0; }
        row = std::clamp(row, 0, t->rows - 1);
        col = std::clamp(col, 0, t->cols - 1);
        while (t->cell(row, col).covered && col > 0) --col;
        while (t->cell(row, col).covered && row > 0) --row;
        story = t->cell(row, col).storyId;
    } else {
        return;
    }
    if (story.isEmpty() || !m_doc->storyDoc(story)) return;
    flushTyping();
    m_text = TextTarget{itemId, story, row, col};
    m_cursor = QTextCursor(m_doc->storyDoc(story));
    if (pos >= 0) m_cursor.setPosition(std::clamp(pos, 0, m_doc->storyDoc(story)->characterCount() - 1));
    else m_cursor.movePosition(QTextCursor::End);
    desiredX = -1;
    if (!m_sel.contains(itemId) || m_sel.size() != 1) {
        m_sel = QStringList{itemId};
        Q_EMIT selectionChanged();
    }
    Q_EMIT textCursorChanged();
    Q_EMIT changed();
}

void Editor::endTextEdit()
{
    if (!isEditingText()) return;
    flushTyping();
    m_text = TextTarget();
    m_cursor = QTextCursor();
    Q_EMIT textCursorChanged();
    Q_EMIT changed();
}

void Editor::setCursor(const QTextCursor &c)
{
    m_cursor = c;
    Q_EMIT textCursorChanged();
}

void Editor::textEdited()
{
    // Called after typing; groups keystrokes into one undo step.
    m_typingTimer.start();
    if (Item *it = m_doc->item(m_text.itemId)) {
        if (it->type() == ItemType::Text) {
            for (TextItem *f : m_doc->chainOf(it->id))
                if (f->autofit == TextItem::GrowBox) autoGrowText(f);
        } else if (it->type() == ItemType::Table) {
            fitTableRows(static_cast<TableItem *>(it));
        }
    }
    Q_EMIT textCursorChanged();
    Q_EMIT changed();
}

void Editor::flushTyping()
{
    m_typingTimer.stop();
    if (m_typing) {
        m_typing = false;
        endChange();
    }
}

// ---------- undo ----------
QByteArray Editor::snapshot() const
{
    return qCompress(QJsonDocument(m_doc->toJson()).toJson(QJsonDocument::Compact), 1);
}

void Editor::restore(const QByteArray &snap, const QStringList &sel, int page, const QString &master)
{
    const QJsonDocument jd = QJsonDocument::fromJson(qUncompress(snap));
    const QString editItem = m_text.itemId;
    m_text = TextTarget();
    m_cursor = QTextCursor();
    m_doc->fromJson(jd.object());
    m_cache.clear();
    m_page = std::clamp(page, 0, int(m_doc->pages.size()) - 1);
    m_master = (!master.isEmpty() && m_doc->master(master)) ? master : QString();
    m_sel.clear();
    for (const auto &id : sel)
        if (m_doc->item(id)) m_sel << id;
    Q_UNUSED(editItem);
    Q_EMIT pageChanged();
    Q_EMIT selectionChanged();
    Q_EMIT textCursorChanged();
    Q_EMIT changed();
}

void Editor::beginChange(const QString &label)
{
    if (m_typing && label != QLatin1String("Typing")) flushTyping();
    if (m_changeDepth++ == 0) {
        m_before = snapshot();
        m_selBefore = m_sel;
        m_pageBefore = m_page;
        m_masterBefore = m_master;
        m_label = label;
    }
}

void Editor::endChange()
{
    if (m_changeDepth == 0) return;
    if (--m_changeDepth > 0) return;
    // Keep the editing cursor valid if the story was replaced.
    QByteArray after = snapshot();
    if (after != m_before) {
        m_undo.push(new SnapshotCommand(this, m_label, m_before, after, m_selBefore, m_sel, m_pageBefore, m_page, m_masterBefore, m_master));
    }
    m_before.clear();
    Q_EMIT changed();
}

void Editor::cancelChange()
{
    if (m_changeDepth == 0) return;
    m_changeDepth = 0;
    restore(m_before, m_selBefore, m_pageBefore, m_masterBefore);
}

void Editor::notifyLive() { Q_EMIT changed(); }

void Editor::undo()
{
    flushTyping();
    if (m_changeDepth) return;
    m_undo.undo();
}

void Editor::redo()
{
    flushTyping();
    if (m_changeDepth) return;
    m_undo.redo();
}

// ---------- tools ----------
void Editor::setTool(Tool t, const QString &shape)
{
    m_tool = t;
    m_toolShape = shape;
    if (t != Tool::Link) linkSource.clear();
    Q_EMIT toolChanged();
}

// ---------- items ----------
QString Editor::addItem(const ItemPtr &it, bool selectIt)
{
    beginChange(QStringLiteral("Insert %1").arg(itemTypeName(it->type())));
    surfaceItems().push_back(it);
    endChange();
    if (selectIt) select(it->id);
    return it->id;
}

ItemPtr Editor::newTextBox(const QRectF &r, const QString &text)
{
    auto t = std::make_shared<TextItem>();
    t->rect = r;
    t->storyId = m_doc->createStory(text);
    // Options > Advanced: hyphenate automatically in new text boxes.
    t->hyphenate = Settings::get().value("edit/hyphenate", true).toBool();
    return t;
}

ItemPtr Editor::newTable(const QRectF &r, int rows, int cols, const QString &format)
{
    auto t = std::make_shared<TableItem>();
    t->rows = std::max(1, rows);
    t->cols = std::max(1, cols);
    t->rect = r;
    t->colW = QVector<double>(t->cols, r.width() / t->cols);
    t->rowH = QVector<double>(t->rows, std::max(18.0, r.height() / t->rows));
    t->cells.resize(t->rows * t->cols);
    for (auto &c : t->cells) c.storyId = m_doc->createStory();
    t->syncRect();
    applyTableFormat(t.get(), format);
    return t;
}

void Editor::removeFromChains(const QStringList &ids)
{
    for (const auto &id : ids) {
        Item *it = m_doc->item(id);
        if (!it || it->type() != ItemType::Text) continue;
        auto *t = static_cast<TextItem *>(it);
        TextItem *prev = m_doc->prevFrame(id);
        if (prev) prev->nextId = t->nextId;
        t->nextId.clear();
    }
}

void Editor::deleteItems(const QStringList &ids)
{
    if (ids.isEmpty()) return;
    endTextEdit();
    beginChange(QStringLiteral("Delete"));
    QStringList all;
    for (const auto &id : ids) {
        all << id;
        if (Item *it = m_doc->item(id); it && it->type() == ItemType::Group)
            walkItems(static_cast<GroupItem *>(it)->children, [&](const ItemPtr &c) { all << c->id; });
    }
    removeFromChains(all);
    for (const auto &id : ids) {
        const auto loc = m_doc->find(id);
        if (loc.list) loc.list->erase(loc.list->begin() + loc.index);
        if (loc.parent) {
            loc.parent->syncRect();
            if (loc.parent->children.size() == 1) {
                // A group of one dissolves.
                const auto ploc = m_doc->find(loc.parent->id);
                if (ploc.list) (*ploc.list)[ploc.index] = loc.parent->children.front();
            }
        }
    }
    // Drop stories no longer referenced.
    QSet<QString> used;
    m_doc->forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Text) used.insert(static_cast<TextItem *>(it)->storyId);
        if (it->type() == ItemType::Shape) used.insert(static_cast<ShapeItem *>(it)->storyId);
        if (it->type() == ItemType::Table)
            for (const auto &c : static_cast<TableItem *>(it)->cells) used.insert(c.storyId);
    });
    for (const auto &sid : m_doc->stories.keys())
        if (!used.contains(sid)) m_doc->removeStory(sid);
    m_sel.clear();
    endChange();
    Q_EMIT selectionChanged();
}

void Editor::deleteSelection() { deleteItems(topLevelSelection()); }

void Editor::duplicateSelection()
{
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    beginChange(QStringLiteral("Duplicate"));
    QStringList made;
    for (const auto &id : ids) {
        Item *it = m_doc->item(id);
        if (!it) continue;
        ItemPtr c = m_doc->cloneItem(*it);
        c->moveBy(12, 12);
        surfaceItems().push_back(c);
        made << c->id;
    }
    m_sel = made;
    endChange();
    Q_EMIT selectionChanged();
}

void Editor::moveSelectionBy(double dx, double dy, const QString &label)
{
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    beginChange(label);
    for (const auto &id : ids)
        if (Item *it = m_doc->item(id); it && !it->locked) {
            it->moveBy(dx, dy);
            if (auto loc = m_doc->find(id); loc.parent) loc.parent->syncRect();
        }
    endChange();
}

void Editor::forEachSelected(const QString &label, const std::function<void(Item *)> &fn)
{
    const auto items = selectedItems();
    if (items.isEmpty()) return;
    beginChange(label);
    for (Item *it : items) fn(it);
    endChange();
}

void Editor::groupSelection()
{
    const QStringList ids = topLevelSelection();
    if (ids.size() < 2) return;
    ItemList &list = surfaceItems();
    beginChange(QStringLiteral("Group"));
    auto g = std::make_shared<GroupItem>();
    int insertAt = -1;
    for (int i = 0; i < int(list.size());) {
        if (ids.contains(list[i]->id)) {
            g->children.push_back(list[i]);
            list.erase(list.begin() + i);
            insertAt = i;
        } else {
            ++i;
        }
    }
    if (g->children.size() < 2) {
        for (auto &c : g->children) list.push_back(c);
        cancelChange();
        return;
    }
    g->syncRect();
    list.insert(list.begin() + std::clamp(insertAt, 0, int(list.size())), g);
    m_sel = QStringList{g->id};
    endChange();
    Q_EMIT selectionChanged();
}

void Editor::ungroupSelection()
{
    const auto items = selectedItems();
    bool any = false;
    for (Item *it : items) any |= it->type() == ItemType::Group;
    if (!any) return;
    beginChange(QStringLiteral("Ungroup"));
    QStringList newSel;
    for (Item *it : items) {
        if (it->type() != ItemType::Group) continue;
        const auto loc = m_doc->find(it->id);
        if (!loc.list) continue;
        ItemPtr keep = (*loc.list)[loc.index];
        auto *g = static_cast<GroupItem *>(keep.get());
        loc.list->erase(loc.list->begin() + loc.index);
        int at = loc.index;
        for (auto &c : g->children) {
            loc.list->insert(loc.list->begin() + at++, c);
            newSel << c->id;
        }
    }
    m_sel = newSel;
    endChange();
    Q_EMIT selectionChanged();
}

void Editor::arrange(Order o)
{
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    beginChange(QStringLiteral("Arrange"));
    for (const auto &id : ids) {
        const auto loc = m_doc->find(id);
        if (!loc.list) continue;
        ItemList &l = *loc.list;
        ItemPtr it = l[loc.index];
        l.erase(l.begin() + loc.index);
        int to = loc.index;
        switch (o) {
        case Order::Forward: to = std::min<int>(loc.index + 1, l.size()); break;
        case Order::Backward: to = std::max(0, loc.index - 1); break;
        case Order::Front: to = l.size(); break;
        case Order::Back: to = 0; break;
        }
        l.insert(l.begin() + to, it);
    }
    endChange();
}

void Editor::align(Align a, bool toMargins)
{
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    QRectF ref;
    if (toMargins || ids.size() == 1) {
        const QSizeF ps = m_doc->pageSize();
        ref = QRectF(QPointF(0, 0), ps);
        if (toMargins) ref = ref.marginsRemoved(m_doc->setup.margins);
    } else {
        for (const auto &id : ids) ref = ref.isNull() ? m_doc->item(id)->bounds() : ref.united(m_doc->item(id)->bounds());
    }
    beginChange(QStringLiteral("Align"));
    for (const auto &id : ids) {
        Item *it = m_doc->item(id);
        if (!it || it->locked) continue;
        const QRectF b = it->bounds();
        double dx = 0, dy = 0;
        switch (a) {
        case Align::Left: dx = ref.left() - b.left(); break;
        case Align::Center: dx = ref.center().x() - b.center().x(); break;
        case Align::Right: dx = ref.right() - b.right(); break;
        case Align::Top: dy = ref.top() - b.top(); break;
        case Align::Middle: dy = ref.center().y() - b.center().y(); break;
        case Align::Bottom: dy = ref.bottom() - b.bottom(); break;
        }
        it->moveBy(dx, dy);
    }
    endChange();
}

void Editor::distribute(bool horizontal, bool toMargins)
{
    QStringList ids = topLevelSelection();
    if (ids.size() < 2 && !toMargins) return;
    QVector<Item *> items;
    for (const auto &id : ids) items << m_doc->item(id);
    std::sort(items.begin(), items.end(), [&](Item *a, Item *b) {
        return horizontal ? a->bounds().center().x() < b->bounds().center().x() : a->bounds().center().y() < b->bounds().center().y();
    });
    double start, end;
    if (toMargins) {
        const QRectF m = QRectF(QPointF(0, 0), m_doc->pageSize()).marginsRemoved(m_doc->setup.margins);
        start = horizontal ? m.left() : m.top();
        end = horizontal ? m.right() : m.bottom();
    } else {
        start = horizontal ? items.first()->bounds().left() : items.first()->bounds().top();
        end = horizontal ? items.last()->bounds().right() : items.last()->bounds().bottom();
    }
    double total = 0;
    for (Item *it : items) total += horizontal ? it->bounds().width() : it->bounds().height();
    const double gap = items.size() > 1 ? (end - start - total) / (items.size() - 1) : 0;
    beginChange(QStringLiteral("Distribute"));
    double pos = start;
    for (Item *it : items) {
        const QRectF b = it->bounds();
        if (horizontal) it->moveBy(pos - b.left(), 0); else it->moveBy(0, pos - b.top());
        pos += (horizontal ? b.width() : b.height()) + gap;
    }
    endChange();
}

void Editor::rotateSelection(double deg)
{
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    beginChange(QStringLiteral("Rotate"));
    for (const auto &id : ids)
        if (Item *it = m_doc->item(id); it && !it->locked) {
            const QPointF c = it->bounds().center();
            it->rotateAround(deg, c);
        }
    endChange();
}

void Editor::flipSelection(bool horizontal)
{
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    beginChange(horizontal ? QStringLiteral("Flip Horizontal") : QStringLiteral("Flip Vertical"));
    for (const auto &id : ids) {
        Item *it = m_doc->item(id);
        if (!it) continue;
        std::function<void(Item *, const QRectF &)> flip = [&](Item *x, const QRectF &box) {
            if (x->type() == ItemType::Group) {
                for (auto &c : static_cast<GroupItem *>(x)->children) flip(c.get(), box);
                static_cast<GroupItem *>(x)->syncRect();
                return;
            }
            if (x->type() == ItemType::Line) {
                auto *l = static_cast<LineItem *>(x);
                if (horizontal) { l->p1.setX(box.left() + box.right() - l->p1.x()); l->p2.setX(box.left() + box.right() - l->p2.x()); }
                else { l->p1.setY(box.top() + box.bottom() - l->p1.y()); l->p2.setY(box.top() + box.bottom() - l->p2.y()); }
                l->syncRect();
                return;
            }
            const QPointF c = x->rect.center();
            const QPointF nc = horizontal ? QPointF(box.left() + box.right() - c.x(), c.y()) : QPointF(c.x(), box.top() + box.bottom() - c.y());
            x->rect.moveCenter(nc);
            if (horizontal) x->flipH = !x->flipH; else x->flipV = !x->flipV;
            x->rotation = std::fmod(360 - x->rotation, 360.0);
        };
        flip(it, it->bounds());
    }
    endChange();
}

// The picture tray: pictures on the scratch area become thumbnails (no
// side over 1.5 inches) in columns to the right of the page, in order.
void Editor::arrangeThumbnails()
{
    const QSizeF ps = m_doc->pageSize();
    const double thumb = 108, gap = 12;
    double x = ps.width() + 36, y = 0, colW = 0;
    beginChange(QStringLiteral("Arrange Thumbnails"));
    for (const ItemPtr &it : m_doc->scratch) {
        auto *pic = dynamic_cast<PictureItem *>(it.get());
        if (!pic) continue;
        QSizeF sz = pic->rect.size();
        if (sz.width() > thumb || sz.height() > thumb) sz.scale(thumb, thumb, Qt::KeepAspectRatio);
        if (y > 0 && y + sz.height() > ps.height()) {
            x += colW + gap;
            y = 0;
            colW = 0;
        }
        pic->scaleInto(pic->rect, QRectF(QPointF(x, y), sz));
        pic->rotation = 0;
        y += sz.height() + gap;
        colW = std::max(colW, sz.width());
    }
    endChange();
}

void Editor::settleScratch(const QStringList &ids)
{
    if (!m_master.isEmpty()) return;
    const QRectF pageRect(QPointF(0, 0), m_doc->pageSize());
    bool moved = false;
    for (const auto &id : ids) {
        const auto loc = m_doc->find(id);
        if (!loc.list || loc.parent) continue;
        ItemPtr it = (*loc.list)[loc.index];
        const bool onPage = it->bounds().intersects(pageRect);
        if (loc.scratch && onPage) {
            loc.list->erase(loc.list->begin() + loc.index);
            surfaceItems().push_back(it);
            moved = true;
        } else if (loc.page >= 0 && !onPage) {
            loc.list->erase(loc.list->begin() + loc.index);
            m_doc->scratch.push_back(it);
            moved = true;
        }
    }
    if (moved) Q_EMIT changed();
}

void Editor::linkFrames(const QString &from, const QString &to)
{
    auto *a = dynamic_cast<TextItem *>(m_doc->item(from));
    auto *b = dynamic_cast<TextItem *>(m_doc->item(to));
    if (!a || !b || a == b || !a->nextId.isEmpty() || m_doc->prevFrame(b->id)) {
        Q_EMIT status(QStringLiteral("You can link only to an empty text box that is not already linked."));
        return;
    }
    QTextDocument *bd = m_doc->storyDoc(b->storyId);
    if (bd && !bd->toPlainText().trimmed().isEmpty()) {
        Q_EMIT status(QStringLiteral("The text box you link to must be empty."));
        return;
    }
    // b must not already be in a's chain.
    for (TextItem *f : m_doc->chainOf(a->id))
        if (f == b) return;
    beginChange(QStringLiteral("Create Text Box Link"));
    const QString head = m_doc->chainOf(a->id).first()->storyId;
    QString old = b->storyId;
    for (TextItem *f : m_doc->chainOf(b->id)) f->storyId = head;
    a->nextId = b->id;
    bool used = false;
    m_doc->forEachItem([&](Item *it, int, const QString &) { if (auto *t = dynamic_cast<TextItem *>(it); t && t->storyId == old) used = true; });
    if (!used) m_doc->removeStory(old);
    endChange();
}

void Editor::breakLink(const QString &from)
{
    auto *a = dynamic_cast<TextItem *>(m_doc->item(from));
    if (!a || a->nextId.isEmpty()) return;
    beginChange(QStringLiteral("Break Forward Link"));
    auto *b = dynamic_cast<TextItem *>(m_doc->item(a->nextId));
    a->nextId.clear();
    if (b) {
        const QString fresh = m_doc->createStory();
        for (TextItem *f : m_doc->chainOf(b->id)) f->storyId = fresh;
    }
    endChange();
}

void Editor::autoGrowText(TextItem *t)
{
    if (!t || t->autofit != TextItem::GrowBox) return;
    FrameSpec spec = Renderer::frameSpec(*m_doc, *t, surfacePageNumber(), renderOptions());
    spec.size.setHeight(1e5);
    spec.valign = VAlign::Top;
    Story *s = m_doc->story(t->storyId);
    if (!s) return;
    StoryLayout lay;
    LayoutEnv env;
    env.colors = m_doc->colors;
    env.fonts = m_doc->fonts;
    lay.build(s->doc.get(), {spec}, env);
    const double need = std::max(18.0, lay.usedHeight(0));
    if (std::abs(need - t->rect.height()) > 0.5) t->rect.setHeight(need);
}

void Editor::fitTableRows(TableItem *t)
{
    if (!t || !t->growToFit) return;
    LayoutEnv env;
    env.colors = m_doc->colors;
    env.fonts = m_doc->fonts;
    bool grew = false;
    for (int r = 0; r < t->rows; ++r) {
        double need = 0;
        for (int c = 0; c < t->cols; ++c) {
            const TableCell &cell = t->cell(r, c);
            if (cell.covered || cell.rowSpan > 1) continue;
            Story *s = m_doc->story(cell.storyId);
            if (!s) continue;
            FrameSpec spec;
            spec.size = QSizeF(t->cellRect(r, c).width(), 1e5);
            spec.insets = cell.margins;
            StoryLayout lay;
            lay.build(s->doc.get(), {spec}, env);
            need = std::max(need, lay.usedHeight(0));
        }
        if (need > t->rowH[r] + 0.5) { t->rowH[r] = need; grew = true; }
    }
    if (grew) t->syncRect();
}

void Editor::applyTableFormat(TableItem *t, const QString &format)
{
    applyTableFormatCells(t, format, [this](TableCell &cell, bool head, bool firstCol) {
        QTextDocument *d = m_doc->storyDoc(cell.storyId);
        if (!d) return;
        QTextCursor cur(d);
        cur.select(QTextCursor::Document);
        QTextCharFormat cf;
        if (head) {
            cf.setProperty(tp::ColorRefP, QStringLiteral("#FFFFFF"));
            cf.setFontWeight(QFont::Bold);
        } else {
            cf.setFontWeight(firstCol ? QFont::Bold : QFont::Normal);
        }
        cur.mergeCharFormat(cf);
        cur.mergeBlockCharFormat(cf);
        if (!head) {
            // Body cells go back to the default text color.
            for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) {
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fr = it.fragment();
                    if (fr.charFormat().stringProperty(tp::ColorRefP) != QLatin1String("#FFFFFF")) continue;
                    QTextCursor fc(d);
                    fc.setPosition(fr.position());
                    fc.setPosition(fr.position() + fr.length(), QTextCursor::KeepAnchor);
                    QTextCharFormat f = fr.charFormat();
                    f.clearProperty(tp::ColorRefP);
                    fc.setCharFormat(f);
                }
                QTextCharFormat bcf = b.charFormat();
                if (bcf.stringProperty(tp::ColorRefP) == QLatin1String("#FFFFFF")) {
                    bcf.clearProperty(tp::ColorRefP);
                    QTextCursor(b).setBlockCharFormat(bcf);
                }
            }
        }
    });
}

// ---------- clipboard ----------
void Editor::copy()
{
    if (isEditingText()) {
        if (!m_cursor.hasSelection()) return;
        auto *md = new QMimeData;
        const QTextDocumentFragment frag = m_cursor.selection();
        md->setText(frag.toPlainText().replace(QChar::ParagraphSeparator, '\n').replace(QChar::LineSeparator, '\n'));
        md->setHtml(frag.toHtml());
        // Lossless copy that keeps JeffPub properties.
        QTextDocument tmp;
        QTextCursor tc(&tmp);
        tc.insertFragment(frag);
        md->setData(QStringLiteral("application/x-jeffpub-text"), QJsonDocument(storyToJson(&tmp)).toJson(QJsonDocument::Compact));
        QApplication::clipboard()->setMimeData(md);
        return;
    }
    const QStringList ids = topLevelSelection();
    if (ids.isEmpty()) return;
    QJsonArray items;
    QJsonObject stories, images;
    std::function<void(const Item &)> collect = [&](const Item &it) {
        auto addStory = [&](const QString &sid) {
            if (QTextDocument *d = m_doc->storyDoc(sid)) stories[sid] = storyToJson(d);
        };
        switch (it.type()) {
        case ItemType::Text: addStory(static_cast<const TextItem &>(it).storyId); break;
        case ItemType::Shape: addStory(static_cast<const ShapeItem &>(it).storyId); break;
        case ItemType::Table: for (const auto &c : static_cast<const TableItem &>(it).cells) addStory(c.storyId); break;
        case ItemType::Picture: {
            const QString iid = static_cast<const PictureItem &>(it).imageId;
            auto im = m_doc->images.find(iid);
            if (im != m_doc->images.end()) images[iid] = QJsonObject{{"format", im->format}, {"data", QString::fromLatin1(im->bytes.toBase64())}};
            break;
        }
        case ItemType::Group: for (const auto &c : static_cast<const GroupItem &>(it).children) collect(*c); break;
        default: break;
        }
        if (!it.fill.imageId.isEmpty()) {
            auto im = m_doc->images.find(it.fill.imageId);
            if (im != m_doc->images.end()) images[it.fill.imageId] = QJsonObject{{"format", im->format}, {"data", QString::fromLatin1(im->bytes.toBase64())}};
        }
    };
    for (const auto &id : ids) {
        Item *it = m_doc->item(id);
        if (!it) continue;
        QJsonObject o = it->toJson();
        if (it->type() == ItemType::Text) o.remove("next");
        items.append(o);
        collect(*it);
    }
    auto *md = new QMimeData;
    md->setData(kItemsMime, QJsonDocument(QJsonObject{{"items", items}, {"stories", stories}, {"images", images}}).toJson(QJsonDocument::Compact));
    // Also offer a picture of the selection to other programs.
    PaintContext ctx;
    ctx.doc = m_doc.get();
    ctx.cache = &m_cache;
    ctx.opt.output = true;
    ItemList list;
    for (const auto &id : ids) list.push_back(m_doc->itemPtr(id));
    md->setImageData(Renderer::renderItemsToImage(ctx, list, 2.0));
    QApplication::clipboard()->setMimeData(md);
}

void Editor::cut()
{
    copy();
    if (isEditingText()) {
        if (!m_cursor.hasSelection()) return;
        beginChange(QStringLiteral("Cut"));
        m_cursor.removeSelectedText();
        endChange();
        textEdited();
        return;
    }
    deleteSelection();
}

bool Editor::canPaste() const
{
    const QMimeData *md = QApplication::clipboard()->mimeData();
    return md && (md->hasFormat(kItemsMime) || md->hasText() || md->hasImage() || md->hasHtml());
}

void Editor::paste(bool textOnly)
{
    const QMimeData *md = QApplication::clipboard()->mimeData();
    if (!md) return;
    if (isEditingText()) {
        beginChange(QStringLiteral("Paste"));
        if (!textOnly && md->hasFormat("application/x-jeffpub-text")) {
            QTextDocument tmp;
            storyFromJson(&tmp, QJsonDocument::fromJson(md->data("application/x-jeffpub-text")).object());
            m_cursor.insertFragment(QTextDocumentFragment(&tmp));
        } else if (!textOnly && md->hasHtml()) {
            m_cursor.insertFragment(QTextDocumentFragment::fromHtml(md->html()));
        } else if (md->hasText()) {
            m_cursor.insertText(md->text());
        }
        endChange();
        textEdited();
        return;
    }
    if (md->hasFormat(kItemsMime) && !textOnly) {
        const QJsonObject o = QJsonDocument::fromJson(md->data(kItemsMime)).object();
        beginChange(QStringLiteral("Paste"));
        QHash<QString, QString> storyMap, imageMap;
        const QJsonObject stories = o["stories"].toObject(), images = o["images"].toObject();
        for (auto it = images.begin(); it != images.end(); ++it) {
            const QJsonObject io = it.value().toObject();
            imageMap[it.key()] = m_doc->addImage(QByteArray::fromBase64(io["data"].toString().toLatin1()), io["format"].toString());
        }
        auto mapStory = [&](QString &sid) {
            if (sid.isEmpty()) return;
            if (!storyMap.contains(sid)) {
                const QString ns = m_doc->createStory();
                if (stories.contains(sid)) storyFromJson(m_doc->storyDoc(ns), stories[sid].toObject());
                storyMap[sid] = ns;
            }
            sid = storyMap[sid];
        };
        std::function<void(Item *)> fix = [&](Item *it) {
            it->id = newId();
            if (!it->fill.imageId.isEmpty()) it->fill.imageId = imageMap.value(it->fill.imageId, it->fill.imageId);
            switch (it->type()) {
            case ItemType::Text: mapStory(static_cast<TextItem *>(it)->storyId); static_cast<TextItem *>(it)->nextId.clear(); break;
            case ItemType::Shape: mapStory(static_cast<ShapeItem *>(it)->storyId); break;
            case ItemType::Table: for (auto &c : static_cast<TableItem *>(it)->cells) mapStory(c.storyId); break;
            case ItemType::Picture: { auto *p = static_cast<PictureItem *>(it); p->imageId = imageMap.value(p->imageId, p->imageId); break; }
            case ItemType::Group: for (auto &c : static_cast<GroupItem *>(it)->children) fix(c.get()); break;
            default: break;
            }
        };
        QStringList made;
        // Paste in place when the originals are gone or on another page; else offset.
        bool offset = false;
        for (const auto &v : o["items"].toArray())
            if (m_doc->item(v.toObject()["id"].toString())) offset = true;
        for (const auto &v : o["items"].toArray()) {
            ItemPtr it = Item::fromJsonAny(v.toObject());
            if (!it) continue;
            fix(it.get());
            if (offset) it->moveBy(12, 12);
            surfaceItems().push_back(it);
            made << it->id;
        }
        m_sel = made;
        endChange();
        Q_EMIT selectionChanged();
        return;
    }
    if (md->hasImage() && !textOnly) {
        const QImage img = qvariant_cast<QImage>(md->imageData());
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
        beginChange(QStringLiteral("Paste Picture"));
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = m_doc->addImage(png, "png");
        QSizeF sz(img.width() * 0.75, img.height() * 0.75);
        const QSizeF ps = m_doc->pageSize();
        sz.scale(std::min(sz.width(), ps.width() * 0.6), std::min(sz.height(), ps.height() * 0.6), Qt::KeepAspectRatio);
        pic->rect = QRectF(QPointF((ps.width() - sz.width()) / 2, (ps.height() - sz.height()) / 2), sz);
        pic->fitImage(img.size(), true);
        surfaceItems().push_back(pic);
        m_sel = QStringList{pic->id};
        endChange();
        Q_EMIT selectionChanged();
        return;
    }
    if (md->hasText() || md->hasHtml()) {
        beginChange(QStringLiteral("Paste Text"));
        const QSizeF ps = m_doc->pageSize();
        auto t = std::static_pointer_cast<TextItem>(newTextBox(QRectF(ps.width() * 0.2, ps.height() * 0.3, ps.width() * 0.6, ps.height() * 0.25)));
        QTextCursor c(m_doc->storyDoc(t->storyId));
        if (!textOnly && md->hasHtml()) c.insertFragment(QTextDocumentFragment::fromHtml(md->html()));
        else c.insertText(md->text());
        surfaceItems().push_back(t);
        m_sel = QStringList{t->id};
        endChange();
        Q_EMIT selectionChanged();
    }
}

// ---------- pages ----------
int Editor::insertPages(int after, int count, bool duplicate, bool oneTextBox, const QString &masterId)
{
    endTextEdit();
    beginChange(count > 1 ? QStringLiteral("Insert Pages") : QStringLiteral("Insert Page"));
    int at = std::clamp(after + 1, 0, int(m_doc->pages.size()));
    const auto src = m_doc->pages.value(std::clamp(after, 0, int(m_doc->pages.size()) - 1));
    for (int i = 0; i < count; ++i) {
        auto p = m_doc->addPage(at + i, masterId.isEmpty() ? (src ? src->masterId : QStringLiteral("A")) : masterId);
        if (duplicate && src) {
            p->background = src->background;
            p->guides = src->guides;
            for (const auto &it : src->items) p->items.push_back(m_doc->cloneItem(*it));
        } else if (oneTextBox) {
            const QRectF r = QRectF(QPointF(0, 0), m_doc->pageSize()).marginsRemoved(m_doc->setup.margins);
            p->items.push_back(newTextBox(r));
        }
    }
    m_page = at;
    m_master.clear();
    m_sel.clear();
    endChange();
    Q_EMIT pageChanged();
    Q_EMIT selectionChanged();
    return at;
}

void Editor::deletePage(int index)
{
    if (m_doc->pages.size() <= 1 || index < 0 || index >= m_doc->pages.size()) {
        Q_EMIT status(QStringLiteral("A publication must have at least one page."));
        return;
    }
    endTextEdit();
    beginChange(QStringLiteral("Delete Page"));
    QStringList ids;
    for (const auto &it : m_doc->pages[index]->items) ids << it->id;
    removeFromChains(ids);
    m_doc->pages.removeAt(index);
    m_page = std::clamp(m_page >= index ? m_page - 1 : m_page, 0, int(m_doc->pages.size()) - 1);
    m_sel.clear();
    endChange();
    Q_EMIT pageChanged();
    Q_EMIT selectionChanged();
}

void Editor::movePage(int from, int to)
{
    const int n = m_doc->pages.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    beginChange(QStringLiteral("Move Page"));
    m_doc->pages.move(from, to);
    m_page = to;
    endChange();
    Q_EMIT pageChanged();
}

void Editor::renamePage(int index, const QString &title)
{
    if (index < 0 || index >= m_doc->pages.size()) return;
    change(QStringLiteral("Rename Page"), [&] { m_doc->pages[index]->title = title; });
}

void Editor::applyMaster(int index, const QString &masterId)
{
    if (index < 0 || index >= m_doc->pages.size()) return;
    change(QStringLiteral("Apply Master Page"), [&] { m_doc->pages[index]->masterId = masterId; });
}

QString Editor::addMaster(bool duplicateCurrent)
{
    beginChange(duplicateCurrent ? QStringLiteral("Duplicate Master Page") : QStringLiteral("New Master Page"));
    auto m = std::make_shared<MasterPage>();
    QString abbr;
    for (char c = 'A'; c <= 'Z'; ++c)
        if (!m_doc->master(QString(QChar(c)))) { abbr = QString(QChar(c)); break; }
    if (abbr.isEmpty()) abbr = newId("M");
    m->id = abbr;
    m->abbr = abbr;
    m->name = QStringLiteral("Master Page %1").arg(abbr);
    if (duplicateCurrent) {
        if (MasterPage *src = m_doc->master(m_master.isEmpty() ? QStringLiteral("A") : m_master)) {
            m->background = src->background;
            m->grid = src->grid;
            m->twoPage = src->twoPage;
            m->guides = src->guides;
            for (const auto &it : src->items) m->items.push_back(m_doc->cloneItem(*it));
        }
    }
    m_doc->masters << m;
    m_master = m->id;
    endChange();
    Q_EMIT pageChanged();
    return m->id;
}

void Editor::deleteMaster(const QString &id)
{
    if (m_doc->masters.size() <= 1) {
        Q_EMIT status(QStringLiteral("A publication needs at least one master page."));
        return;
    }
    beginChange(QStringLiteral("Delete Master Page"));
    for (int i = 0; i < m_doc->masters.size(); ++i)
        if (m_doc->masters[i]->id == id) { m_doc->masters.removeAt(i); break; }
    const QString fallback = m_doc->masters.first()->id;
    for (auto &p : m_doc->pages)
        if (p->masterId == id) p->masterId = fallback;
    if (m_master == id) m_master = fallback;
    endChange();
    Q_EMIT pageChanged();
}

} // namespace jp
