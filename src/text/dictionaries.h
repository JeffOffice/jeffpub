#pragma once
// The bundled spelling and hyphenation dictionaries, LibreOffice's, one
// folder per language under resources/dict. Text carries its language as a
// BCP-47 tag ("en-US", "pt-BR"); text without one is US English.

#include <QString>
#include <QStringList>
#include <QVector>

namespace jp::dict {

struct Language {
    QString code;   // "es-MX"
    QString name;   // "Spanish (Mexico)"
};

// The languages JeffPub can check and hyphenate, in menu order.
const QVector<Language> &languages();
// The display name of a tag ("pt-BR" -> "Portuguese (Brazil)"), or the tag.
QString languageName(const QString &tag);

// The bundled language for text marked `tag`: the same language and region,
// else that language's usual dictionary ("es-AR" -> "es-MX", "en-NZ" ->
// "en-GB"); empty when the language has none. An empty tag is US English.
QString match(const QString &tag);

// The folder holding the language folders, or empty when it's missing.
QString root();

// Spelling, by a code from match(). A language whose dictionary is missing
// accepts every word; hasSpelling() says whether it's there.
bool hasSpelling(const QString &code);
bool spell(const QString &code, const QString &word);
QStringList suggest(const QString &code, const QString &word);

// The language's hyphenation patterns (a LibreOffice hyph_*.dic), or empty.
QString hyphenationFile(const QString &code);

} // namespace jp::dict
