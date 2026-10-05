#pragma once
// English hyphenation (Liang patterns from LibreOffice's hyph_en_US.dic),
// shared by automatic hyphenation in layout and the Hyphenate command.

#include <QString>
#include <QVector>

namespace jp {

// Positions inside word where a hyphen may go (a break before word[i]).
QVector<int> hyphenationPoints(const QString &word);

} // namespace jp
