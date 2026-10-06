#pragma once
// Hyphenation by language (Liang patterns from LibreOffice's hyph_*.dic
// files), shared by automatic hyphenation in layout and the Hyphenate command.

#include <QString>
#include <QVector>

namespace jp {

// Positions inside word where a hyphen may go (a break before word[i]), for
// text in `language` (a BCP-47 tag; empty is US English). None for a
// language without patterns.
QVector<int> hyphenationPoints(const QString &word, const QString &language = QString());

// Whether automatic hyphenation may break this word: only words the
// language's dictionary knows, as .pub layouts hyphenate from a dictionary
// (names and coined words stay whole). Without the dictionary, any word.
bool hyphenationKnows(const QString &word, const QString &language = QString());

} // namespace jp
