#pragma once
// Imports .pub files through libmspub.

#include "core/document.h"

#include <QStringList>

namespace jp {

struct PubImportReport {
    int pages = 0;
    int textBoxes = 0, pictures = 0, shapes = 0, tables = 0;
    int linkedChains = 0;
    QStringList fontsUsed;
    QStringList warnings;
};

bool isPublisherFile(const QByteArray &head);
std::unique_ptr<Document> importPublisher(const QByteArray &data, QString *error, PubImportReport *report = nullptr);
std::unique_ptr<Document> importPublisherFile(const QString &path, QString *error, PubImportReport *report = nullptr);

} // namespace jp
