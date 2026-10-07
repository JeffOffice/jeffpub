#pragma once
// Footnotes and endnotes: a numbered reference at the cursor (the field
// "footnote:<story>" or "endnote:<story>") whose note text is a story of its
// own; the text engine numbers and places them (StoryLayout::notes()).

#include <QString>

class QWidget;

namespace jp {

class Editor;

// The note story of the reference at or just before the cursor, or empty.
QString noteAtCursor(Editor *ed, bool *endnote = nullptr);
// A new note at the cursor with this text; returns its story id. One undo
// step.
QString addNote(Editor *ed, bool endnote, const QString &text);
// Replaces a note's text, keeping its formatting. One undo step.
void setNoteText(Editor *ed, const QString &storyId, const QString &text);
// Insert > Footnote / Endnote: asks for the text of a new note, or, on a
// reference, edits that note.
void noteDialog(QWidget *parent, Editor *ed, bool endnote);

} // namespace jp
