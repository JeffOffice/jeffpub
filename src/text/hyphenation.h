#pragma once
// English hyphenation (Liang patterns from LibreOffice's hyph_en_US.dic),
// shared by automatic hyphenation in layout and the Hyphenate command.

#include <QString>
#include <QVector>

namespace jp {

// Positions inside word where a hyphen may go (a break before word[i]).
QVector<int> hyphenationPoints(const QString &word);

// Whether automatic hyphenation may break this word: only words the US
// English dictionary knows, as .pub layouts hyphenate from a dictionary
// (names and coined words stay whole). Without the dictionary, any word.
bool hyphenationKnows(const QString &word);

} // namespace jp
