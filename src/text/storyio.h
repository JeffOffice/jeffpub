#pragma once
// Lossless JSON serialization of a QTextDocument, including JeffPub's custom
// properties (Qt's HTML export drops those).

#include <QJsonObject>
#include <QTextFormat>

class QTextDocument;

namespace jp {

QJsonObject formatToJson(const QTextFormat &f);
void formatFromJson(QTextFormat &f, const QJsonObject &o);

QJsonObject storyToJson(const QTextDocument *doc);
void storyFromJson(QTextDocument *doc, const QJsonObject &o);

// Plain text with paragraphs separated by '\n'.
void setStoryText(QTextDocument *doc, const QString &text);

} // namespace jp
