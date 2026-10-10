#pragma once
// EPUB 3 e-books that reflow to any screen: the publication's stories in
// reading order (page by page, top to bottom), chapters starting at each
// Heading 1, pictures and tables in place, a table of contents from the
// Heading 1-3 paragraphs, footnotes that pop up beside the text, endnotes
// at the back, and a cover.

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace jp {

class Document;

struct EpubOptions {
    // Empty values come from File > Info (title, author), the text's
    // language, and a new identifier.
    QString title, author, language, identifier;
    QImage cover;                   // usually the first page; no cover when null
};

bool exportEpub(const Document &doc, const QString &path, const EpubOptions &opt, QString *error);

// A fixed-layout (page-for-page) EPUB 3: each page exactly as designed, as
// picture books, magazines and comics are sold. Each page is a drawing
// (SVG, its letters as outlines) with the page's text kept underneath, in
// reading order, for search and screen readers; the contents lists the
// Heading 1-3 paragraphs at the pages they're on.
struct FixedPage {
    QByteArray svg;          // the page drawn
    QString text;            // its words, in reading order
    struct Heading { int level; QString text; };
    QVector<Heading> headings;
};
struct FixedEpubOptions : EpubOptions {
    QSizeF pageSize;         // points
    bool spreads = false;    // facing pages: page 1 alone on the right, then pairs
};
bool exportFixedEpub(const Document &doc, const QString &path, const QVector<FixedPage> &pages, const FixedEpubOptions &opt, QString *error);

} // namespace jp
