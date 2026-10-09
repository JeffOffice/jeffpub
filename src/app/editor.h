#pragma once
// The controller between the document and the UI: selection, the current
// page or master page, text editing state, tools, and undo/redo. Every change
// to the document goes through beginChange()/endChange(), which records a
// before/after snapshot as one undo step.

#include "core/document.h"
#include "render/renderer.h"

#include <QObject>
#include <QTextCursor>
#include <QTimer>
#include <QUndoStack>
#include <functional>

namespace jp {

struct ViewOptions {
    bool boundaries = true;
    bool guides = true;
    bool fields = false;
    bool rulers = true;
    bool pageNav = true;
    bool scratch = true;
    bool baselines = false;
    bool special = false;
    bool snapGuides = true;
    bool snapObjects = true;
    bool snapRuler = false;
    bool spelling = true;
    bool gridlines = true;
};

enum class Tool { Select, Text, Table, Picture, Shape, Line, Arrow, DoubleArrow, Freeform, TextArt, Link, FormatPainter };

class Editor : public QObject {
    Q_OBJECT
public:
    explicit Editor(QObject *parent = nullptr);
    ~Editor() override;

    Document *doc() const { return m_doc.get(); }
    LayoutCache &cache() { return m_cache; }
    QUndoStack *undoStack() { return &m_undo; }

    void setDocument(std::unique_ptr<Document> d, const QString &path = QString());
    QString filePath() const { return m_path; }
    void setFilePath(const QString &p);
    QString displayName() const;
    // What a document with no file of its own is called, in place of its
    // title (recovered work keeps its file's name until it's saved again).
    void setUntitledName(const QString &name) { m_untitledName = name; }
    bool isModified() const { return !m_undo.isClean(); }
    void markSaved() { m_undo.setClean(); }
    void markUnsaved() { m_undo.resetClean(); }   // a recovered copy: unsaved until saved

    // ---- view ----
    int currentPage() const { return m_page; }
    void setCurrentPage(int i, bool keepTextEdit = false);
    QString masterView() const { return m_master; }
    void setMasterView(const QString &id);
    bool twoPageSpread() const { return m_spread; }
    void setTwoPageSpread(bool on);
    ViewOptions view;
    void setView(const std::function<void(ViewOptions &)> &fn);
    int mergeRecord() const { return m_mergeRecord; }
    void setMergeRecord(int r);        // -1 = show field codes
    QVector<int> visiblePages() const; // pages shown on the canvas (1 or 2)

    // ---- surface (page or master being edited) ----
    PageBase *surface() const;
    ItemList &surfaceItems() const;
    QSizeF surfaceSize() const;
    int surfacePageNumber() const;     // for fields on master pages
    RenderOptions renderOptions() const;
    // Misspelled [start, end) ranges in a story (empty when checking is off).
    QVector<QPair<int, int>> misspelledIn(const QString &storyId) const;
    void invalidateSpelling() { m_spell.clear(); notifyLive(); }   // after the dictionary changes

    // ---- selection ----
    const QStringList &selection() const { return m_sel; }
    void select(const QStringList &ids, bool add = false);
    void select(const QString &id) { select(QStringList{id}); }
    void clearSelection() { select(QStringList()); }
    void toggleSelect(const QString &id);
    QVector<Item *> selectedItems() const;
    Item *single() const;
    QString selectionKind() const;     // "", "text", "picture", "shape", "line", "table", "textart", "group", "multi"
    QRectF selectionBounds() const;

    // ---- text editing ----
    struct TextTarget {
        QString itemId, storyId;
        int row = -1, col = -1;        // table cell
    };
    bool isEditingText() const { return !m_text.storyId.isEmpty(); }
    const TextTarget &textTarget() const { return m_text; }
    QTextCursor &cursor() { return m_cursor; }
    QTextDocument *editDoc() const;
    void beginTextEdit(const QString &itemId, int pos = -1, int row = -1, int col = -1);
    void endTextEdit();
    void setCursor(const QTextCursor &c);
    void textEdited();                 // call after any cursor-based edit
    double desiredX = -1;              // for up/down movement

    // ---- changes and undo ----
    void beginChange(const QString &label);
    void endChange();
    void cancelChange();
    void change(const QString &label, const std::function<void()> &fn) { beginChange(label); fn(); endChange(); }
    void notifyLive();                 // repaint during a drag without recording
    void flushTyping();
    void undo();
    void redo();

    // ---- tools ----
    Tool tool() const { return m_tool; }
    QString toolShape() const { return m_toolShape; }
    void setTool(Tool t, const QString &shape = QString());
    QString linkSource;                // text box waiting for Create Link
    QString cropItem;                  // picture in crop mode
    QString pointsItem;                // shape whose points are being edited
    QString wrapItem;                  // object whose wrap points are being edited
    QJsonObject painterItem;           // Format Painter: copied object formatting
    QTextCharFormat painterText;       // Format Painter: copied text formatting
    bool painterHasText = false;
    bool painterLocked = false;
    void setCropItem(const QString &id) { cropItem = id; Q_EMIT viewChanged(); }
    void setPointsItem(const QString &id) { pointsItem = id; Q_EMIT viewChanged(); }
    void setWrapItem(const QString &id) { wrapItem = id; Q_EMIT viewChanged(); }

    // ---- items ----
    QString addItem(const ItemPtr &it, bool selectIt = true);
    ItemPtr newTextBox(const QRectF &r, const QString &text = QString());
    ItemPtr newTable(const QRectF &r, int rows, int cols, const QString &format = QStringLiteral("Table Style 1"));
    void deleteSelection();
    void deleteItems(const QStringList &ids);
    void duplicateSelection();
    void moveSelectionBy(double dx, double dy, const QString &label = QStringLiteral("Move"));
    void forEachSelected(const QString &label, const std::function<void(Item *)> &fn);
    void groupSelection();
    // Regroup: the objects of the group last ungrouped, grouped again.
    bool canRegroup() const;
    void regroup();
    // Objects set in text (Wrap Text > In Line with Text). The selected object
    // goes into the text box under it, at the character nearest its top left;
    // an object in text that the text selection holds goes back onto the page
    // where it's drawn, wrapped as `mode`.
    bool canMoveIntoText() const;
    bool moveIntoText();
    bool selectionIsInlineObject() const;
    bool moveOutOfText(Wrap::Mode mode);
    void ungroupSelection();
    enum class Order { Forward, Backward, Front, Back };
    void arrange(Order o);
    enum class Align { Left, Center, Right, Top, Middle, Bottom };
    void align(Align a, bool toMargins);
    void distribute(bool horizontal, bool toMargins);
    void rotateSelection(double deg);
    void flipSelection(bool horizontal);
    void settleScratch(const QStringList &ids);   // move items between page and scratch area
    void arrangeThumbnails();                       // tidy the pictures on the scratch area into a tray
    void linkFrames(const QString &from, const QString &to);
    void breakLink(const QString &from);
    void applyTableFormat(TableItem *t, const QString &format);
    void fitTableRows(TableItem *t);
    void autoGrowText(TextItem *t);

    // ---- clipboard ----
    void copy();
    void cut();
    void paste(bool textOnly = false);
    bool canPaste() const;

    // ---- pages ----
    int insertPages(int after, int count, bool duplicate, bool oneTextBox, const QString &masterId = QString());
    void deletePage(int index);
    void movePage(int from, int to);
    void renamePage(int index, const QString &title);
    void applyMaster(int index, const QString &masterId);
    QString addMaster(bool duplicateCurrent);
    void deleteMaster(const QString &id);

    // ---- text formatting (editor_text.cpp) ----
    QTextCharFormat currentCharFormat() const;
    QTextBlockFormat currentBlockFormat() const;
    void mergeCharFormat(const QTextCharFormat &f, const QString &label);
    void mergeBlockFormat(const QTextBlockFormat &f, const QString &label);
    void setCharProperty(int prop, const QVariant &v, const QString &label);
    void clearCharProperty(int prop, const QString &label);
    void toggleBold();
    void toggleItalic();
    void toggleUnderline(QTextCharFormat::UnderlineStyle style = QTextCharFormat::SingleUnderline);
    void toggleStrike();
    void toggleScript(bool super);
    void setFontFamily(const QString &f);
    void setFontSize(double pt);
    void growFont(int dir);
    void clearFormatting();
    void setTextColor(const ColorRef &c);
    void setHighlight(const ColorRef &c);
    void changeCase(int mode);       // 0 sentence, 1 lower, 2 upper, 3 capitalize, 4 toggle
    void setAlignment(Qt::Alignment a);
    // Reading direction; a paragraph aligned to its start moves to the new start.
    void setDirection(Qt::LayoutDirection d);
    void setLineSpacing(int type, double value);
    void setParagraphSpacing(double before, double after);
    void changeIndent(int dir);
    void setList(int kind, int format = 0, const QString &bullet = QString(), int start = 1);   // kind 0 none, 1 bullets, 2 numbers
    void applyStyle(const QString &name);
    void setDropCap(int lines, int chars = 1, const QString &font = QString());
    void insertText(const QString &t);
    // Typing: smart quotes, and AutoCorrect when a word ends (both as set
    // in Options > Proofing).
    void typeText(const QString &t);
    void autoCorrectWord();   // the word just before the cursor
    void autoFormatWord();    // dashes, fractions and ordinals just before the cursor
    void editTextAs(const QString &label, const std::function<void(QTextCursor &)> &fn);   // one undo step of its own
    void editText(const std::function<void(QTextCursor &)> &fn);   // keystroke-level edit, grouped as Typing
    void retargetText(const QString &itemId);                       // caret moved into another linked box
    void insertField(const QString &code);
    void insertTextBlock(const QString &text, const QString &label);
    QString currentStyleName() const;
    QVector<QTextCursor> formatTargets() const;   // cursor(s) a formatting command applies to

Q_SIGNALS:
    void documentReplaced();
    void changed();
    void selectionChanged();
    void pageChanged();
    void viewChanged();
    void textCursorChanged();
    void toolChanged();
    void status(const QString &message);
    void modifiedChanged(bool);

private:
    TextItem *textBoxUnder(const Item &obj) const;
    friend class SnapshotCommand;
    QByteArray snapshot() const;
    void restore(const QByteArray &snap, const QStringList &sel, int page, const QString &master);
    void removeFromChains(const QStringList &ids);
    QStringList topLevelSelection() const;

    std::unique_ptr<Document> m_doc;
    LayoutCache m_cache;
    // Spelling results per story, recomputed only when the story's text changes.
    struct SpellEntry { quint64 serial = 0; int revision = -1; QVector<QPair<int, int>> ranges; };
    mutable QHash<QString, SpellEntry> m_spell;
    QUndoStack m_undo;
    QString m_path;
    QString m_untitledName;
    int m_page = 0;
    QString m_master;
    bool m_spread = false;
    int m_mergeRecord = -1;
    QStringList m_sel;
    TextTarget m_text;
    QTextCursor m_cursor;
    Tool m_tool = Tool::Select;
    QStringList m_regroup;   // the objects of the group last ungrouped
    QString m_toolShape;

    int m_changeDepth = 0;
    QByteArray m_before;
    QStringList m_selBefore;
    int m_pageBefore = 0;
    QString m_masterBefore;
    QString m_label;
    bool m_typing = false;
    QTimer m_typingTimer;
};

} // namespace jp
