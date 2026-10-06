#pragma once
// .jpub publication files: a ZIP holding document.json, the original picture
// files under images/, and a thumbnail of page 1.

#include "core/document.h"

#include <QImage>

namespace jp {

bool savePublication(const Document &doc, const QString &path, const QImage &thumbnail, QString *error);
std::unique_ptr<Document> loadPublication(const QString &path, QString *error);

QByteArray publicationBytes(const Document &doc, const QImage &thumbnail);
std::unique_ptr<Document> publicationFromBytes(const QByteArray &bytes, QString *error);

// A publication's preview for lists of files: a .jpub's thumbnail, or the
// picture a .pub file keeps in its summary (null when there is none).
QImage publicationThumbnail(const QString &path);

} // namespace jp
