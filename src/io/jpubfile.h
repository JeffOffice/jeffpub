#pragma once
// .jpub publication files: a ZIP holding document.json, the original picture
// files under images/, the small previews of pictures linked to their files
// (and kept nowhere else) under previews/, and a thumbnail of page 1.

#include "core/document.h"

#include <QImage>

namespace jp {

// `embedLinks`: pictures linked to their files are stored whole, for a
// publication that goes to another computer.
bool savePublication(const Document &doc, const QString &path, const QImage &thumbnail, QString *error, bool embedLinks = false);
std::unique_ptr<Document> loadPublication(const QString &path, QString *error);

// `folder` is where the bytes will be saved: linked pictures keep their paths
// relative to it (empty: the publication's own folder).
QByteArray publicationBytes(const Document &doc, const QImage &thumbnail, const QString &folder = QString(), bool embedLinks = false);
// `folder` is where the publication was read from, for finding linked pictures.
std::unique_ptr<Document> publicationFromBytes(const QByteArray &bytes, QString *error, const QString &folder = QString());

// A publication's preview for lists of files: a .jpub's thumbnail, or the
// picture a .pub file keeps in its summary (null when there is none).
QImage publicationThumbnail(const QString &path);

} // namespace jp
