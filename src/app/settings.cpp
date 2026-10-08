#include "app/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

namespace jp {

Settings &Settings::get()
{
    static Settings s;
    return s;
}

void copyMissingSettings(const QSettings &from, QSettings &to)
{
    for (const QString &key : from.allKeys())
        if (!to.contains(key)) to.setValue(key, from.value(key));
}

bool moveDataFolder(const QString &from, const QString &to)
{
    if (!QFileInfo(from).isDir() || QFileInfo::exists(to)) return false;
    QDir().mkpath(QFileInfo(to).path());
    if (QDir().rename(from, to)) return true;
    // Another drive, or a file held open: copy, and leave the old folder.
    QDirIterator it(from, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    bool ok = QDir().mkpath(to);
    while (it.hasNext()) {
        const QFileInfo fi = it.nextFileInfo();
        const QString dest = to + QLatin1Char('/') + QDir(from).relativeFilePath(fi.filePath());
        ok = (fi.isDir() ? QDir().mkpath(dest) : QFile::copy(fi.filePath(), dest)) && ok;
    }
    return ok;
}

void moveFromJeffPub79()
{
    const QString marker = QStringLiteral("migration/jeffpub79");
    QSettings now(QStringLiteral("JeffOffice"), QStringLiteral("JeffPub"));
    if (now.value(marker).toBool()) return;
    {
        const QSettings old(QStringLiteral("JeffPub79"), QStringLiteral("JeffPub79"));
        copyMissingSettings(old, now);
    }
    // The old folder is where QStandardPaths put it under the old names.
    const QString org = QCoreApplication::organizationName(), app = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("JeffPub"));
    QCoreApplication::setApplicationName(QStringLiteral("JeffPub 79"));
    const QString oldData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setOrganizationName(org);
    QCoreApplication::setApplicationName(app);
    const QString newData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!oldData.isEmpty() && !newData.isEmpty() && oldData != newData && moveDataFolder(oldData, newData))
        QDir().rmdir(QFileInfo(oldData).path());   // the old "JeffPub" folder, if now empty
    now.setValue(marker, true);
}

Settings::Settings() : m_s(QStringLiteral("JeffOffice"), QStringLiteral("JeffPub"))
{
    m_unit = Unit(m_s.value("units", 0).toInt());
}

void Settings::setUnit(Unit u)
{
    m_unit = u;
    m_s.setValue("units", int(u));
}

QString Settings::unitSuffix() const
{
    switch (m_unit) {
    case Unit::Inch: return QStringLiteral("\"");
    case Unit::Centimeter: return QStringLiteral(" cm");
    case Unit::Millimeter: return QStringLiteral(" mm");
    case Unit::Point: return QStringLiteral(" pt");
    case Unit::Pica: return QStringLiteral(" pi");
    }
    return {};
}

static double perPoint(Unit u)
{
    switch (u) {
    case Unit::Inch: return 1.0 / 72.0;
    case Unit::Centimeter: return 2.54 / 72.0;
    case Unit::Millimeter: return 25.4 / 72.0;
    case Unit::Point: return 1.0;
    case Unit::Pica: return 1.0 / 12.0;
    }
    return 1.0;
}

double Settings::toUnit(double pt) const { return pt * perPoint(m_unit); }
double Settings::fromUnit(double v) const { return v / perPoint(m_unit); }

QString Settings::format(double pt, int decimals) const
{
    QString n = QString::number(toUnit(pt), 'f', decimals);
    if (n.contains('.')) { while (n.endsWith('0')) n.chop(1); if (n.endsWith('.')) n.chop(1); }
    return n + unitSuffix();
}

bool Settings::parse(const QString &text, double *pt) const
{
    static const QRegularExpression re(QStringLiteral("^\\s*(-?[0-9]*\\.?[0-9]+)\\s*(\"|in|inch|inches|cm|mm|pt|pi|px)?\\s*$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(text);
    if (!m.hasMatch()) return false;
    const double v = m.captured(1).toDouble();
    const QString u = m.captured(2).toLower();
    Unit unit = m_unit;
    if (u == "\"" || u.startsWith("in")) unit = Unit::Inch;
    else if (u == "cm") unit = Unit::Centimeter;
    else if (u == "mm") unit = Unit::Millimeter;
    else if (u == "pt") unit = Unit::Point;
    else if (u == "pi") unit = Unit::Pica;
    else if (u == "px") { *pt = v * 0.75; return true; }
    *pt = v / perPoint(unit);
    return true;
}

QStringList Settings::recentFiles() const { return m_s.value("recent").toStringList(); }

QJsonArray Settings::customPageSizes() const
{
    return QJsonDocument::fromJson(m_s.value("pageSizes/custom").toByteArray()).array();
}

void Settings::setCustomPageSizes(const QJsonArray &sizes)
{
    m_s.setValue("pageSizes/custom", QJsonDocument(sizes).toJson(QJsonDocument::Compact));
}

void Settings::addRecentFile(const QString &path)
{
    QStringList l = recentFiles();
    l.removeAll(path);
    l.prepend(path);
    while (l.size() > m_s.value("recentCount", 25).toInt()) l.removeLast();
    m_s.setValue("recent", l);
}

void Settings::removeRecentFile(const QString &path)
{
    QStringList l = recentFiles();
    l.removeAll(path);
    m_s.setValue("recent", l);
}

QStringList Settings::pinnedFiles() const { return m_s.value("pinned").toStringList(); }

void Settings::setPinned(const QString &path, bool pinned)
{
    QStringList l = pinnedFiles();
    l.removeAll(path);
    if (pinned) l.prepend(path);
    m_s.setValue("pinned", l);
}

QString Settings::userName() const
{
    QString n = m_s.value("user/name").toString();
    if (n.isEmpty()) n = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    return n;
}

} // namespace jp
