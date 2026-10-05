#include "text/hyphenation.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QStringList>
#include <algorithm>

namespace jp {

namespace {

struct Patterns {
    bool loaded = false;
    QHash<QString, QVector<int>> map;
    int maxLen = 1, left = 2, right = 3;
    QHash<QString, QVector<int>> cache;   // word (lower case) -> points
};

QString dictFile()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList dirs{app + "/dict", app + "/../share/jeffpub79/dict", app + "/../dict"};
#ifdef JP_SOURCE_DIR
    dirs << QStringLiteral(JP_SOURCE_DIR) + "/resources/dict";
#endif
    for (const QString &d : dirs)
        if (QFile::exists(d + "/hyph_en_US.dic")) return d + "/hyph_en_US.dic";
    return {};
}

Patterns &patterns()
{
    static Patterns p;
    if (p.loaded) return p;
    p.loaded = true;
    QFile f(dictFile());
    if (!f.open(QIODevice::ReadOnly)) return p;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('%') || line == "UTF-8") continue;
        if (line.startsWith("LEFTHYPHENMIN")) { p.left = line.section(' ', 1).toInt(); continue; }
        if (line.startsWith("RIGHTHYPHENMIN")) { p.right = line.section(' ', 1).toInt(); continue; }
        if (line.contains("HYPHENMIN") || line.contains('/') || line.startsWith("NEXTLEVEL")) continue;
        QString letters;
        QVector<int> v{0};
        for (QChar c : line) {
            if (c.isDigit()) v.last() = c.digitValue();
            else { letters += c; v << 0; }
        }
        p.map.insert(letters, v);
        p.maxLen = std::max(p.maxLen, int(letters.size()));
    }
    return p;
}

} // namespace

QVector<int> hyphenationPoints(const QString &word)
{
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    Patterns &p = patterns();
    if (p.map.isEmpty()) return {};
    const QString lower = word.toLower();
    auto hit = p.cache.constFind(lower);
    if (hit != p.cache.constEnd()) return *hit;
    const QString w = "." + lower + ".";
    QVector<int> levels(w.size() + 1, 0);
    for (int i = 0; i < w.size(); ++i)
        for (int len = 1; len <= std::min(int(w.size()) - i, p.maxLen); ++len) {
            auto it = p.map.constFind(w.mid(i, len));
            if (it == p.map.constEnd()) continue;
            const QVector<int> &v = it.value();
            for (int k = 0; k < v.size(); ++k) levels[i + k] = std::max(levels[i + k], v[k]);
        }
    QVector<int> out;
    for (int i = p.left; i <= word.size() - p.right; ++i)
        if (levels[i + 1] % 2) out << i;
    if (p.cache.size() > 50000) p.cache.clear();
    p.cache.insert(lower, out);
    return out;
}

} // namespace jp
