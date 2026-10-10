#include "canvas/canvasaccessible.h"

#include "app/settings.h"
#include "canvas/canvas.h"
#include "core/items.h"
#include "render/shapes.h"

#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QWindow>

namespace jp {

namespace {

// The story an object shows, if any (a table: its first cell's).
QString storyOf(const Item *it)
{
    switch (it->type()) {
    case ItemType::Text: return static_cast<const TextItem *>(it)->storyId;
    case ItemType::Shape: return static_cast<const ShapeItem *>(it)->storyId;
    default: return QString();
    }
}

QString shortText(const QString &s)
{
    const QString t = s.simplified();
    return t.size() > 120 ? t.left(117) + QStringLiteral("...") : t;
}

QAccessibleInterface *factory(const QString &className, QObject *object)
{
    if (className == QLatin1String("jp::Canvas") && object && object->isWidgetType()) return new CanvasAccessible(static_cast<Canvas *>(object));
    return nullptr;
}

} // namespace

void installCanvasAccessibility()
{
    static bool done = false;
    if (done) return;
    done = true;
    QAccessible::installFactory(factory);
}

// ---------------- one object ----------------
Item *PageObjectAccessible::item() const { return m_canvas ? m_canvas->editor()->doc()->item(m_id) : nullptr; }

bool PageObjectAccessible::isValid() const { return item() != nullptr; }

bool PageObjectAccessible::editing() const
{
    return m_canvas && m_canvas->editor()->isEditingText() && m_canvas->editor()->textTarget().itemId == m_id;
}

QWindow *PageObjectAccessible::window() const { return m_canvas ? m_canvas->window()->windowHandle() : nullptr; }

QAccessibleInterface *PageObjectAccessible::parent() const { return m_canvas ? QAccessible::queryAccessibleInterface(m_canvas.data()) : nullptr; }

QString PageObjectAccessible::text(QAccessible::Text t) const
{
    const Item *it = item();
    if (!it) return QString();
    const Document *d = m_canvas->editor()->doc();
    if (t == QAccessible::Name) {
        const QString alt = it->altText.simplified();
        switch (it->type()) {
        case ItemType::Text: {
            const QString s = shortText(plainText());
            return s.isEmpty() ? QCoreApplication::translate("Canvas", "Empty text box") : QCoreApplication::translate("Canvas", "Text box: %1").arg(s);
        }
        case ItemType::Picture:
            return alt.isEmpty() ? QCoreApplication::translate("Canvas", "Picture, no alt text") : QCoreApplication::translate("Canvas", "Picture: %1").arg(alt);
        case ItemType::Shape: {
            const ShapeDef *def = shapeDef(static_cast<const ShapeItem *>(it)->shape);
            const QString kind = def && !def->name.isEmpty() ? def->name : QCoreApplication::translate("Canvas", "Shape");   // the library's names are translated
            const QString s = shortText(plainText());
            if (!alt.isEmpty()) return QCoreApplication::translate("Canvas", "%1: %2").arg(kind, alt);
            return s.isEmpty() ? kind : QCoreApplication::translate("Canvas", "%1: %2").arg(kind, s);
        }
        case ItemType::Line: return alt.isEmpty() ? QCoreApplication::translate("Canvas", "Line") : QCoreApplication::translate("Canvas", "Line: %1").arg(alt);
        case ItemType::Table: {
            const auto *tb = static_cast<const TableItem *>(it);
            return QCoreApplication::translate("Canvas", "Table, %1 rows by %2 columns").arg(tb->rows).arg(tb->cols);
        }
        case ItemType::TextArt: return QCoreApplication::translate("Canvas", "Text art: %1").arg(shortText(static_cast<const TextArtItem *>(it)->text));
        case ItemType::Group: {
            // (Counted first: a cast inside the call makes lupdate loop forever.)
            const int n = int(static_cast<const GroupItem *>(it)->children.size());
            return QCoreApplication::translate("Canvas", "Group of %n object(s)", nullptr, n);
        }
        }
        return QString();
    }
    if (t == QAccessible::Description) {
        // Where it is and how big, in the units chosen in Options.
        const Settings &st = Settings::get();
        const QRectF b = it->bounds();
        QString s = QCoreApplication::translate("Canvas", "%1 from the left and %2 from the top, %3 wide and %4 tall.")
                        .arg(st.format(b.left()), st.format(b.top()), st.format(b.width()), st.format(b.height()));
        if (it->rotation != 0) s += QLatin1Char(' ') + QCoreApplication::translate("Canvas", "Turned %1 degrees.").arg(qRound(it->rotation));
        Q_UNUSED(d);
        return s;
    }
    if (t == QAccessible::Value) return storyOf(it).isEmpty() && !editing() ? QString() : plainText();
    if (t == QAccessible::Help)
        return editing() ? QCoreApplication::translate("Canvas", "Typing in it. Escape stops typing and keeps it selected.") : QCoreApplication::translate("Canvas", "Selected. The arrow keys move it, Enter types in it, and Delete removes it.");
    return QString();
}

QRect PageObjectAccessible::rect() const
{
    const Item *it = item();
    return it && m_canvas ? m_canvas->screenRect(it->bounds()) : QRect();
}

QAccessible::Role PageObjectAccessible::role() const
{
    const Item *it = item();
    if (!it) return QAccessible::NoRole;
    switch (it->type()) {
    case ItemType::Text: return QAccessible::EditableText;
    case ItemType::Table: return editing() ? QAccessible::EditableText : QAccessible::Table;
    case ItemType::Group: return QAccessible::Grouping;
    case ItemType::Shape: return editing() ? QAccessible::EditableText : QAccessible::Graphic;
    default: return QAccessible::Graphic;
    }
}

QAccessible::State PageObjectAccessible::state() const
{
    QAccessible::State s;
    const Item *it = item();
    if (!it || !m_canvas) {
        s.invalid = true;
        return s;
    }
    s.focusable = true;
    s.selectable = true;
    const Editor *ed = m_canvas->editor();
    s.selected = ed->selection().contains(m_id) || editing();
    s.focused = m_canvas->hasFocus() && (editing() || (ed->selection().size() == 1 && ed->selection().first() == m_id));
    if (role() == QAccessible::EditableText) {
        s.editable = true;
        s.multiLine = true;
    }
    s.movable = s.sizeable = true;
    return s;
}

void *PageObjectAccessible::interface_cast(QAccessible::InterfaceType t)
{
    const Item *it = item();
    if (t == QAccessible::TextInterface && it && (editing() || !storyOf(it).isEmpty())) return static_cast<QAccessibleTextInterface *>(this);
    return nullptr;
}

QString PageObjectAccessible::plainText() const
{
    if (!m_canvas) return QString();
    Editor *ed = m_canvas->editor();
    if (editing()) return ed->editDoc() ? ed->editDoc()->toPlainText() : QString();
    const Item *it = item();
    const QString sid = it ? storyOf(it) : QString();
    if (sid.isEmpty()) return QString();
    QTextDocument *d = ed->doc()->storyDoc(sid);
    return d ? d->toPlainText() : QString();
}

void PageObjectAccessible::selection(int selectionIndex, int *startOffset, int *endOffset) const
{
    *startOffset = *endOffset = 0;
    if (selectionIndex != 0 || !editing()) return;
    const QTextCursor &c = m_canvas->editor()->cursor();
    *startOffset = c.selectionStart();
    *endOffset = c.selectionEnd();
}

int PageObjectAccessible::selectionCount() const { return editing() && m_canvas->editor()->cursor().hasSelection() ? 1 : 0; }

void PageObjectAccessible::addSelection(int startOffset, int endOffset) { setSelection(0, startOffset, endOffset); }

void PageObjectAccessible::removeSelection(int)
{
    if (!editing()) return;
    QTextCursor c = m_canvas->editor()->cursor();
    c.clearSelection();
    m_canvas->editor()->setCursor(c);
}

void PageObjectAccessible::setSelection(int, int startOffset, int endOffset)
{
    if (!editing()) return;
    const int n = characterCount();
    QTextCursor c = m_canvas->editor()->cursor();
    c.setPosition(std::clamp(startOffset, 0, n));
    c.setPosition(std::clamp(endOffset, 0, n), QTextCursor::KeepAnchor);
    m_canvas->editor()->setCursor(c);
}

int PageObjectAccessible::cursorPosition() const { return editing() ? m_canvas->editor()->cursor().position() : 0; }

void PageObjectAccessible::setCursorPosition(int position)
{
    if (!editing()) return;
    QTextCursor c = m_canvas->editor()->cursor();
    c.setPosition(std::clamp(position, 0, characterCount()));
    m_canvas->editor()->setCursor(c);
}

QString PageObjectAccessible::text(int startOffset, int endOffset) const
{
    const QString t = plainText();
    startOffset = std::clamp(startOffset, 0, int(t.size()));
    endOffset = endOffset < 0 ? int(t.size()) : std::clamp(endOffset, startOffset, int(t.size()));
    return t.mid(startOffset, endOffset - startOffset);
}

int PageObjectAccessible::characterCount() const { return int(plainText().size()); }

QRect PageObjectAccessible::characterRect(int offset) const
{
    if (editing()) return m_canvas->caretScreenRect(offset);
    return rect();
}

int PageObjectAccessible::offsetAtPoint(const QPoint &point) const
{
    if (!m_canvas || !item()) return -1;
    const QPointF view = m_canvas->viewport()->mapFromGlobal(point);
    return m_canvas->textPosAt(m_id, m_canvas->toPage(view));
}

void PageObjectAccessible::scrollToSubstring(int, int)
{
    if (const Item *it = item()) m_canvas->ensureVisible(it->bounds());
}

QString PageObjectAccessible::attributes(int offset, int *startOffset, int *endOffset) const
{
    Q_UNUSED(offset);
    *startOffset = 0;
    *endOffset = characterCount();
    return QString();
}

// ---------------- the page ----------------
CanvasAccessible::CanvasAccessible(Canvas *c) : QAccessibleWidget(c, QAccessible::Client) {}

CanvasAccessible::~CanvasAccessible()
{
    for (QAccessible::Id id : std::as_const(m_ids)) QAccessible::deleteAccessibleInterface(id);
}

Canvas *CanvasAccessible::canvas() const { return static_cast<Canvas *>(widget()); }

QStringList CanvasAccessible::objectIds() const
{
    QStringList ids;
    for (const ItemPtr &it : canvas()->editor()->surfaceItems()) ids << it->id;
    // Objects gone from the page let their interfaces go.
    for (auto it = m_ids.begin(); it != m_ids.end();) {
        if (!canvas()->editor()->doc()->item(it.key())) {
            QAccessible::deleteAccessibleInterface(it.value());
            it = m_ids.erase(it);
        } else ++it;
    }
    return ids;
}

PageObjectAccessible *CanvasAccessible::objectInterface(const QString &id) const
{
    if (id.isEmpty() || !canvas()->editor()->doc()->item(id)) return nullptr;
    auto it = m_ids.constFind(id);
    if (it != m_ids.constEnd())
        if (auto *iface = static_cast<PageObjectAccessible *>(QAccessible::accessibleInterface(*it))) return iface;
    auto *iface = new PageObjectAccessible(canvas(), id);
    m_ids.insert(id, QAccessible::registerAccessibleInterface(iface));
    return iface;
}

int CanvasAccessible::childCount() const { return int(objectIds().size()) + QAccessibleWidget::childCount(); }

QAccessibleInterface *CanvasAccessible::child(int index) const
{
    const QStringList ids = objectIds();
    if (index < 0) return nullptr;
    if (index < ids.size()) return objectInterface(ids[index]);
    return QAccessibleWidget::child(index - int(ids.size()));
}

int CanvasAccessible::indexOfChild(const QAccessibleInterface *child) const
{
    if (!child) return -1;
    const QStringList ids = objectIds();
    for (int i = 0; i < ids.size(); ++i)
        if (m_ids.value(ids[i]) && QAccessible::accessibleInterface(m_ids.value(ids[i])) == child) return i;
    const int w = QAccessibleWidget::indexOfChild(child);
    return w < 0 ? -1 : int(ids.size()) + w;
}

QAccessibleInterface *CanvasAccessible::childAt(int x, int y) const
{
    Canvas *c = canvas();
    const QPoint view = c->viewport()->mapFromGlobal(QPoint(x, y));
    if (c->viewport()->rect().contains(view)) {
        const QString id = c->itemAt(c->toPage(view));
        if (!id.isEmpty()) return objectInterface(id);
    }
    return QAccessibleWidget::childAt(x, y);
}

QAccessibleInterface *CanvasAccessible::focusChild() const
{
    Canvas *c = canvas();
    if (!c->hasFocus()) return nullptr;
    const Editor *ed = c->editor();
    if (ed->isEditingText()) return objectInterface(ed->textTarget().itemId);
    if (ed->selection().size() == 1) return objectInterface(ed->selection().first());
    return nullptr;
}

} // namespace jp
