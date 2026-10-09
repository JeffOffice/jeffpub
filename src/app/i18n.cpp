#include "app/i18n.h"

#include "app/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace jp {

QStringList availableUiLanguages()
{
    QStringList out;
    for (const QString &f : QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("jeffpub_*.qm")}, QDir::Files))
        out << f.mid(8, f.size() - 11);   // jeffpub_<language>.qm
    return out;
}

QString installTranslations()
{
    const QStringList have = availableUiLanguages();
    QStringList wanted;
    const QString chosen = Settings::get().value(QStringLiteral("ui/language")).toString();
    if (!chosen.isEmpty()) wanted << chosen;
    else
        for (QString l : QLocale::system().uiLanguages()) wanted << l.replace(QLatin1Char('-'), QLatin1Char('_'));
    QString lang;
    for (const QString &w : wanted) {
        if (w.startsWith(QLatin1String("en"))) break;   // the text is English already
        if (have.contains(w)) { lang = w; break; }
        const QString base = w.section(QLatin1Char('_'), 0, 0);
        if (have.contains(base)) { lang = base; break; }
    }
    if (lang.isEmpty()) return QStringLiteral("en");
    auto *own = new QTranslator(QCoreApplication::instance());
    if (own->load(QStringLiteral(":/i18n/jeffpub_%1.qm").arg(lang))) QCoreApplication::installTranslator(own);
    // Qt's own text (standard buttons such as Cancel, file dialogs): beside
    // the program in packages, else where Qt keeps it.
    auto *qt = new QTranslator(QCoreApplication::instance());
    for (const QString &dir : {QCoreApplication::applicationDirPath() + QStringLiteral("/translations"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)})
        if (qt->load(QStringLiteral("qtbase_%1").arg(lang), dir)) {
            QCoreApplication::installTranslator(qt);
            break;
        }
    return lang;
}

} // namespace jp
