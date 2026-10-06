#include "text/hyphenation.h"

#include "text/dictionaries.h"

#include <QFile>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <memory>

namespace jp {

namespace {

// One level of Liang patterns: letters -> the values between them.
struct Level {
    QHash<QString, QVector<int>> map;
    QSet<QString> prefixes;   // every start of every pattern: libhyphen's states
    int maxLen = 1;

    void add(const QString &letters, const QVector<int> &values)
    {
        map.insert(letters, values);
        for (int n = 1; n <= letters.size(); ++n) prefixes.insert(letters.left(n));
        maxLen = std::max(maxLen, int(letters.size()));
    }

    // The value at each gap of ".word." (index i is before w[i]), matched as
    // libhyphen, LibreOffice's hyphenator, matches: after each letter only
    // the longest pattern start ending there counts, and its values apply
    // when it is a whole pattern. LibreOffice's files are written for that
    // (the German ones, applied pattern by pattern, would give "isa-r-vor").
    QVector<int> apply(const QString &lower) const
    {
        const QString w = "." + lower + ".";
        QVector<int> levels(w.size() + 1, 0);
        for (int end = 1; end <= w.size(); ++end)
            for (int len = std::min(end, maxLen); len >= 1; --len) {
                const QString s = w.mid(end - len, len);
                if (!prefixes.contains(s)) continue;
                auto it = map.constFind(s);
                if (it != map.constEnd()) {
                    const QVector<int> &v = it.value();
                    for (int k = 0; k < v.size(); ++k) levels[end - len + k] = std::max(levels[end - len + k], v[k]);
                }
                break;
            }
        return levels;
    }
};

// A hyph_*.dic file as libhyphen loads it. With NEXTLEVEL, the patterns
// before it are the first level, which finds where compounds join (German
// "Abend|brot"), and the rest hyphenate the parts. Without, the first level
// only splits at hyphens and apostrophes and the file is the second.
struct Patterns {
    Level first, second;
    bool hasSecond = false;
    int left = 0, right = 0, compoundLeft = 0, compoundRight = 0;   // the first level's minimums
    QStringList noHyphen;   // no hyphen right before or after these
    QHash<QString, QVector<int>> cache;   // word (lower case) -> points
};

QVector<int> parse(const QString &line, QString *letters)
{
    QVector<int> v{0};
    for (QChar c : line) {
        if (c.isDigit()) v.last() = c.digitValue();
        else { *letters += c; v << 0; }
    }
    return v;
}

void load(Patterns &p, const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QList<QByteArray> lines = f.readAll().split('\n');
    if (lines.isEmpty()) return;
    // The first line names the encoding: UTF-8 or ISO8859-1.
    const bool utf8 = lines[0].trimmed() == "UTF-8";
    const bool twoLevels = std::any_of(lines.begin(), lines.end(), [](const QByteArray &l) { return l.trimmed() == "NEXTLEVEL"; });
    struct Mins { int left = 0, right = 0, compoundLeft = 0, compoundRight = 0; } mins[2];
    QStringList noHyphen[2];
    int level = twoLevels ? 0 : 1;
    for (int n = 1; n < lines.size(); ++n) {
        const QString line = (utf8 ? QString::fromUtf8(lines[n]) : QString::fromLatin1(lines[n])).trimmed();
        if (line.isEmpty() || line.startsWith('%') || line.startsWith('#')) continue;
        if (line == QLatin1String("NEXTLEVEL")) { level = 1; continue; }
        const int value = line.section(' ', 1).toInt();
        Mins &m = mins[level];
        if (line.startsWith(QLatin1String("LEFTHYPHENMIN"))) { m.left = value; continue; }
        if (line.startsWith(QLatin1String("RIGHTHYPHENMIN"))) { m.right = value; continue; }
        if (line.startsWith(QLatin1String("COMPOUNDLEFTHYPHENMIN"))) { m.compoundLeft = value; continue; }
        if (line.startsWith(QLatin1String("COMPOUNDRIGHTHYPHENMIN"))) { m.compoundRight = value; continue; }
        if (line.startsWith(QLatin1String("NOHYPHEN"))) { noHyphen[level] = line.section(' ', 1).split(',', Qt::SkipEmptyParts); continue; }
        if (line.contains('/')) continue;   // non-standard hyphenation (old German "ck" -> "k-k")
        QString letters;
        const QVector<int> v = parse(line.section(' ', 0, 0), &letters);
        (level == 0 ? p.first : p.second).add(letters, v);
    }
    p.hasSecond = true;
    if (twoLevels) {
        p.left = mins[0].left, p.right = mins[0].right;
        p.compoundLeft = mins[0].compoundLeft, p.compoundRight = mins[0].compoundRight;
        p.noHyphen = noHyphen[0];
    } else {
        // libhyphen's default first level: split at hyphens and apostrophes.
        const QStringList marks = utf8 ? QStringList{"'", QString(QChar(0x2013)), QString(QChar(0x2019)), "-"} : QStringList{"'", "-"};
        for (const QString &mark : marks) {
            QString letters;
            p.first.add(mark, parse("1" + mark + "1", &letters));
        }
        p.noHyphen = marks;
        p.left = mins[1].left, p.right = mins[1].right;
        p.compoundLeft = mins[1].compoundLeft ? mins[1].compoundLeft : mins[1].left ? mins[1].left : 3;
        p.compoundRight = mins[1].compoundRight ? mins[1].compoundRight : mins[1].right ? mins[1].right : 3;
    }
}

// No break in the first `min - 1` gaps, or the last `min` (counting the one
// after the last letter), as libhyphen's lhmin and rhmin.
void leftMin(QVector<int> &gaps, int min)
{
    for (int i = 0; i < min - 1 && i < gaps.size(); ++i) gaps[i] = 0;
}
void rightMin(QVector<int> &gaps, int min)
{
    for (int i = int(gaps.size()) - 1, n = 0; i > 0 && n < min; --i, ++n) gaps[i] = 0;
}

// libhyphen's hnj_hyphen_hyph_: the value of the gap after each letter. At
// the first level, each part between its breaks goes through the first level
// again, on its own, and a word with no break there through the second.
// lend/rend: the word starts or ends the whole word (else the compound
// minimums apply).
QVector<int> hyph(const Patterns &p, bool first, const QString &word, int cl, int cr, bool lend, bool rend)
{
    const int n = int(word.size());
    const QVector<int> levels = (first ? p.first : p.second).apply(word);
    QVector<int> gaps(n, 0);
    for (int i = 0; i + 1 < n; ++i) gaps[i] = levels[i + 2];
    if (!first) return gaps;
    int begin = 0;
    for (int i = 0; i < n; ++i) {
        if (!(gaps[i] % 2) && !(begin > 0 && i + 1 == n)) continue;
        if (i - begin > 0) {
            const QVector<int> part = hyph(p, true, word.mid(begin, i - begin + 1), cl, cr, begin > 0 ? false : lend, gaps[i] % 2 ? false : rend);
            for (int j = 0; j < i - begin; ++j) gaps[begin + j] = part[j];
        }
        begin = i + 1;
    }
    if (begin == 0) {
        gaps = hyph(p, false, word, cl, cr, lend, rend);
        if (!lend) leftMin(gaps, cl);
        if (!rend) rightMin(gaps, cr);
    }
    return gaps;
}

QMutex &mutex()
{
    static QMutex m;
    return m;
}

Patterns *patternsFor(const QString &code)
{
    static QHash<QString, std::shared_ptr<Patterns>> loaded;   // by file: languages share some
    const QString file = dict::hyphenationFile(code);
    if (file.isEmpty()) return nullptr;
    auto it = loaded.constFind(file);
    if (it != loaded.constEnd()) return it->get();
    auto p = std::make_shared<Patterns>();
    load(*p, file);
    loaded.insert(file, p);
    return p.get();
}

} // namespace

QVector<int> hyphenationPoints(const QString &word, const QString &language)
{
    QMutexLocker lock(&mutex());
    Patterns *p = patternsFor(dict::match(language));
    if (!p || p->second.map.isEmpty()) return {};
    const QString lower = word.toLower();
    if (lower.size() != word.size() || lower.isEmpty()) return {};   // positions wouldn't map back
    auto hit = p->cache.constFind(lower);
    if (hit != p->cache.constEnd()) return *hit;
    // libhyphen's hnj_hyphen_hyphenate3, with LibreOffice's defaults: at
    // least two letters before and after a hyphen, more if the file says so.
    const int left = std::max(2, p->left), right = std::max(2, p->right);
    const int cl = std::max(2, p->compoundLeft), cr = std::max(2, p->compoundRight);
    QVector<int> gaps = hyph(*p, true, lower, cl, cr, true, true);
    leftMin(gaps, left);
    rightMin(gaps, right);
    for (const QString &mark : p->noHyphen)
        for (qsizetype at = lower.indexOf(mark); at >= 0; at = lower.indexOf(mark, at + 1)) {
            gaps[at + mark.size() - 1] = 0;
            if (at > 0) gaps[at - 1] = 0;
        }
    QVector<int> out;
    for (int i = 0; i + 1 < gaps.size(); ++i)
        if (gaps[i] % 2) out << i + 1;
    if (p->cache.size() > 50000) p->cache.clear();
    p->cache.insert(lower, out);
    return out;
}

bool hyphenationKnows(const QString &word, const QString &language)
{
    static QMutex cacheMutex;
    static QHash<QString, bool> cache;   // "code|word"
    const QString code = dict::match(language);
    if (code.isEmpty()) return true;
    const QString key = code + '|' + word;
    {
        QMutexLocker lock(&cacheMutex);
        auto it = cache.constFind(key);
        if (it != cache.constEnd()) return *it;
    }
    const bool known = dict::spell(code, word);
    QMutexLocker lock(&cacheMutex);
    if (cache.size() > 200000) cache.clear();
    cache.insert(key, known);
    return known;
}

} // namespace jp
