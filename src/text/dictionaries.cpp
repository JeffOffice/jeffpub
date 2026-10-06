#include "text/dictionaries.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QMutex>

#include <hunspell/hunspell.hxx>
#include <memory>

namespace jp::dict {

namespace {

struct Entry {
    const char *code, *name;
    const char *spelling;      // .aff/.dic without the extension, under root()
    const char *hyphenation;   // under root()
};

const Entry kEntries[] = {
    {"en-US", "English (United States)", "en/en_US", "en/hyph_en_US.dic"},
    {"en-GB", "English (United Kingdom)", "en/en_GB", "en/hyph_en_GB.dic"},
    {"en-CA", "English (Canada)", "en/en_CA", "en/hyph_en_US.dic"},
    {"en-AU", "English (Australia)", "en/en_AU", "en/hyph_en_GB.dic"},
    {"es-MX", "Spanish (Mexico)", "es/es_MX", "es/hyph_es.dic"},
    {"es-ES", "Spanish (Spain)", "es/es_ES", "es/hyph_es.dic"},
    {"fr-FR", "French", "fr_FR/fr", "fr_FR/hyph_fr.dic"},
    {"de-DE", "German", "de/de_DE_frami", "de/hyph_de_DE.dic"},
    {"it-IT", "Italian", "it_IT/it_IT", "it_IT/hyph_it_IT.dic"},
    {"nl-NL", "Dutch", "nl_NL/nl_NL", "nl_NL/hyph_nl_NL.dic"},
    {"pt-BR", "Portuguese (Brazil)", "pt_BR/pt_BR", "pt_BR/hyph_pt_BR.dic"},
    {"pt-PT", "Portuguese (Portugal)", "pt_PT/pt_PT", "pt_PT/hyph_pt_PT.dic"},
};

const Entry *entry(const QString &code)
{
    for (const Entry &e : kEntries)
        if (code == QLatin1String(e.code)) return &e;
    return nullptr;
}

// A Hunspell checker and the encoding its files use: UTF-8, or ISO 8859-1
// (the German dictionary).
struct Checker {
    std::unique_ptr<Hunspell> h;
    bool utf8 = true;
    std::string encode(QString w) const
    {
        if (utf8) return w.toStdString();
        w.replace(QChar(0x2019), QLatin1Char('\''));
        return w.toLatin1().toStdString();
    }
    QString decode(const std::string &s) const
    {
        return utf8 ? QString::fromStdString(s) : QString::fromLatin1(s.data(), qsizetype(s.size()));
    }
};

QMutex &mutex()
{
    static QMutex m;
    return m;
}

// Loaded on first use: the big dictionaries take a moment.
Checker *checker(const QString &code)
{
    static QHash<QString, std::shared_ptr<Checker>> loaded;
    auto it = loaded.constFind(code);
    if (it != loaded.constEnd()) return it->get();
    std::shared_ptr<Checker> c;
    const Entry *e = entry(code);
    const QString base = e && !root().isEmpty() ? root() + '/' + QLatin1String(e->spelling) : QString();
    if (!base.isEmpty() && QFile::exists(base + ".dic") && QFile::exists(base + ".aff")) {
        c = std::make_shared<Checker>();
        c->h = std::make_unique<Hunspell>(QFile::encodeName(base + ".aff").constData(), QFile::encodeName(base + ".dic").constData());
        c->utf8 = QString::fromStdString(c->h->get_dict_encoding()).compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) == 0;
    }
    loaded.insert(code, c);
    return c.get();
}

} // namespace

const QVector<Language> &languages()
{
    static const QVector<Language> list = [] {
        QVector<Language> l;
        for (const Entry &e : kEntries) l << Language{QLatin1String(e.code), QLatin1String(e.name)};
        return l;
    }();
    return list;
}

QString languageName(const QString &tag)
{
    if (const Entry *e = entry(tag)) return QLatin1String(e->name);
    return tag;
}

QString match(const QString &tag)
{
    if (tag.trimmed().isEmpty()) return QStringLiteral("en-US");
    const QStringList parts = QString(tag).replace('_', '-').split('-', Qt::SkipEmptyParts);
    const QString lang = parts.value(0).toLower();
    const QString region = parts.value(1).toUpper();
    const QString exact = lang + '-' + region;
    if (entry(exact)) return exact;
    if (lang == QLatin1String("en"))
        return QStringList{"IE", "NZ", "ZA", "IN"}.contains(region) ? QStringLiteral("en-GB") : QStringLiteral("en-US");
    if (lang == QLatin1String("pt"))
        return QStringList{"AO", "MZ", "CV", "GW", "ST", "TL"}.contains(region) ? QStringLiteral("pt-PT") : QStringLiteral("pt-BR");
    if (lang == QLatin1String("es")) return QStringLiteral("es-MX");
    for (const Entry &e : kEntries)
        if (QLatin1String(e.code).startsWith(lang + '-')) return QLatin1String(e.code);
    return {};
}

QString root()
{
    static const QString dir = [] {
        const QString app = QCoreApplication::applicationDirPath();
        QStringList dirs{app + "/dict", app + "/../share/jeffpub79/dict", app + "/../dict"};
#ifdef JP_SOURCE_DIR
        dirs << QStringLiteral(JP_SOURCE_DIR) + "/resources/dict";
#endif
        for (const QString &d : dirs)
            if (QFile::exists(d + "/en/en_US.dic")) return d;
        return QString();
    }();
    return dir;
}

bool hasSpelling(const QString &code)
{
    QMutexLocker lock(&mutex());
    return checker(code) != nullptr;
}

bool spell(const QString &code, const QString &word)
{
    QMutexLocker lock(&mutex());
    Checker *c = checker(code);
    if (!c || word.isEmpty()) return true;
    if (c->h->spell(c->encode(word))) return true;
    // Dictionaries list words with one apostrophe or the other.
    const QChar curly(0x2019);
    if (word.contains(curly)) return c->h->spell(c->encode(QString(word).replace(curly, QLatin1Char('\''))));
    if (word.contains(QLatin1Char('\''))) return c->h->spell(c->encode(QString(word).replace(QLatin1Char('\''), curly)));
    return false;
}

QStringList suggest(const QString &code, const QString &word)
{
    QMutexLocker lock(&mutex());
    QStringList out;
    Checker *c = checker(code);
    if (!c) return out;
    for (const std::string &s : c->h->suggest(c->encode(word))) out << c->decode(s);
    return out;
}

QString hyphenationFile(const QString &code)
{
    const Entry *e = entry(code);
    if (!e || root().isEmpty()) return {};
    const QString f = root() + '/' + QLatin1String(e->hyphenation);
    return QFile::exists(f) ? f : QString();
}

} // namespace jp::dict
