#pragma once
// What screen readers see of the page. The page (the Canvas) lists each
// object on it as a child, named by its kind and its text or alt text
// ("Text box: Spring sale", "Picture: a red barn"), with where it is and how
// big. The selected object is the focus, so Tab from object to object is
// read out; while typing, the object's text, cursor and selection are there
// to read, as in any text box.

#include <QAccessibleWidget>
#include <QHash>
#include <QPointer>

namespace jp {

class Canvas;
class Item;

// Registers the page's accessibility (once; Canvas's constructor calls it).
void installCanvasAccessibility();

// One object on the page.
class PageObjectAccessible : public QAccessibleInterface, public QAccessibleTextInterface {
public:
    PageObjectAccessible(Canvas *c, const QString &id) : m_canvas(c), m_id(id) {}
    QString id() const { return m_id; }

    bool isValid() const override;
    QObject *object() const override { return nullptr; }
    QWindow *window() const override;
    QAccessibleInterface *parent() const override;
    QAccessibleInterface *child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    QAccessibleInterface *focusChild() const override { return nullptr; }
    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text, const QString &) override {}
    QRect rect() const override;
    QAccessible::Role role() const override;
    QAccessible::State state() const override;
    void *interface_cast(QAccessible::InterfaceType t) override;

    // Its text: the text being typed in it, or its story.
    void selection(int selectionIndex, int *startOffset, int *endOffset) const override;
    int selectionCount() const override;
    void addSelection(int startOffset, int endOffset) override;
    void removeSelection(int selectionIndex) override;
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    int cursorPosition() const override;
    void setCursorPosition(int position) override;
    QString text(int startOffset, int endOffset) const override;
    int characterCount() const override;
    QRect characterRect(int offset) const override;
    int offsetAtPoint(const QPoint &point) const override;
    void scrollToSubstring(int startIndex, int endIndex) override;
    QString attributes(int offset, int *startOffset, int *endOffset) const override;

private:
    Item *item() const;
    bool editing() const;          // the text cursor is in it
    QString plainText() const;
    QPointer<Canvas> m_canvas;
    QString m_id;
};

// The page: its objects, then its own controls (rulers, scroll bars).
class CanvasAccessible : public QAccessibleWidget {
public:
    explicit CanvasAccessible(Canvas *c);
    ~CanvasAccessible() override;
    int childCount() const override;
    QAccessibleInterface *child(int index) const override;
    int indexOfChild(const QAccessibleInterface *child) const override;
    QAccessibleInterface *childAt(int x, int y) const override;
    QAccessibleInterface *focusChild() const override;
    // The interface for an object (made once, then kept for it).
    PageObjectAccessible *objectInterface(const QString &id) const;

private:
    Canvas *canvas() const;
    QStringList objectIds() const;
    mutable QHash<QString, QAccessible::Id> m_ids;
};

} // namespace jp
