#pragma once
// Imports .pub files through libmspub.

#include "core/document.h"

#include <QStringList>

namespace jp {

struct PubImportReport {
    int pages = 0;
    int textBoxes = 0, pictures = 0, shapes = 0, tables = 0;
    int linkedChains = 0;
    int attachedEnds = 0;   // connector ends attached to objects
    int inlineObjects = 0;  // objects set in text
    QStringList fontsUsed;
    QStringList warnings;
};

bool isPublisherFile(const QByteArray &head);
std::unique_ptr<Document> importPublisher(const QByteArray &data, QString *error, PubImportReport *report = nullptr);
std::unique_ptr<Document> importPublisherFile(const QString &path, QString *error, PubImportReport *report = nullptr);
// A Publisher date and time format (Windows' letters) as Qt's QDateTime
// writes it, for a "datetime:" field.
QString pubDateFormat(QString f);

} // namespace jp
