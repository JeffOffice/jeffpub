#pragma once
// EPUB 3 e-books that reflow to any screen: the publication's stories in
// reading order (page by page, top to bottom), chapters starting at each
// Heading 1, pictures and tables in place, a table of contents from the
// Heading 1-3 paragraphs, footnotes that pop up beside the text, endnotes
// at the back, and a cover.

#include <QImage>
#include <QString>

namespace jp {

class Document;

struct EpubOptions {
    // Empty values come from File > Info (title, author), the text's
    // language, and a new identifier.
    QString title, author, language, identifier;
    QImage cover;                   // usually the first page; no cover when null
};

bool exportEpub(const Document &doc, const QString &path, const EpubOptions &opt, QString *error);

} // namespace jp
