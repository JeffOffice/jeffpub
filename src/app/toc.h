#pragma once
// Tables of contents, built from the paragraphs in the Heading 1, Heading 2
// and Heading 3 styles, in reading order, with the page each one starts on.
// A table's paragraphs carry tp::TocLevel, so Update finds and rebuilds it.

#include <QString>
#include <QVector>

namespace jp {

class Document;
class Editor;

struct TocEntry {
    int level = 1;   // 1-3, from Heading 1-3
    QString text;
    int page = 1;    // where the heading starts, numbered as page fields are
};

QVector<TocEntry> tableOfContentsEntries(const Document &doc, int depth = 3);
// At the cursor while editing text; otherwise in a text box inside the
// margins of the current page if it is empty, or of a new page after it.
void insertTableOfContents(Editor *ed);
// Rebuilds every table in the publication; returns how many there were.
int updateTablesOfContents(Editor *ed);

} // namespace jp
