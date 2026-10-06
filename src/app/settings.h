#pragma once
// User preferences (stored with QSettings) and measurement units.

#include <QJsonArray>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace jp {

enum class Unit { Inch, Centimeter, Millimeter, Point, Pica };

class Settings {
public:
    static Settings &get();

    Unit unit() const { return m_unit; }
    void setUnit(Unit u);
    QString unitSuffix() const;
    double toUnit(double pt) const;
    double fromUnit(double v) const;
    QString format(double pt, int decimals = 3) const;   // "8.125\"", trailing zeros dropped
    bool parse(const QString &text, double *pt) const;   // accepts any unit suffix

    QStringList recentFiles() const;
    void addRecentFile(const QString &path);
    void removeRecentFile(const QString &path);
    QStringList pinnedFiles() const;
    void setPinned(const QString &path, bool pinned);

    // Page sizes the user created: each {"name", "setup" (a page setup)}.
    QJsonArray customPageSizes() const;
    void setCustomPageSizes(const QJsonArray &sizes);

    QVariant value(const QString &key, const QVariant &def = QVariant()) const { return m_s.value(key, def); }
    void setValue(const QString &key, const QVariant &v) { m_s.setValue(key, v); }

    QString userName() const;
    int autoRecoverMinutes() const { return m_s.value("save/autoRecoverMinutes", 10).toInt(); }
    double nudge() const { return m_s.value("edit/nudge", 9.0).toDouble(); }   // points (0.125 in)

private:
    Settings();
    QSettings m_s;
    Unit m_unit = Unit::Inch;
};

} // namespace jp
