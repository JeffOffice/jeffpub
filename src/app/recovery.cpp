#include "app/recovery.h"

#include "core/document.h"
#include "io/jpubfile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace jp::recovery {

namespace {
// Main thread only, like the windows that keep copies.
QString g_root;
QString g_session;
std::unique_ptr<QLockFile> g_lock;

const char *kLockName = "session.lock";

// Characters some file systems refuse, and leading dots, become "_".
QString safeName(QString s)
{
    for (QChar &c : s)
        if (c < QChar(0x20) || QStringLiteral("<>:\"/\\|?*").contains(c)) c = QLatin1Char('_');
    s = s.trimmed();
    while (s.startsWith(QLatin1Char('.'))) s[0] = QLatin1Char('_');
    if (s.isEmpty()) s = QStringLiteral("Publication");
    if (s.size() > 60) {
        s.truncate(60);
        if (s.back().isHighSurrogate()) s.chop(1);   // not half a character
    }
    return s;
}

QString sidecar(const QString &copy) { return copy.left(copy.size() - 5) + QStringLiteral(".json"); }

Recovered describe(const QString &file)
{
    Recovered r;
    r.file = file;
    const QFileInfo fi(file);
    r.saved = fi.lastModified();
    QFile meta(sidecar(file));
    if (meta.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(meta.readAll()).object();
        r.source = o.value(QStringLiteral("source")).toString();
        r.title = o.value(QStringLiteral("title")).toString();
        const QDateTime t = QDateTime::fromString(o.value(QStringLiteral("saved")).toString(), Qt::ISODate);
        if (t.isValid()) r.saved = t;
    }
    if (r.title.isEmpty()) {
        // "Cover.autorecover.jpub" (older versions), "Cover 1a2b3c4d.jpub".
        QString t = fi.completeBaseName();
        if (t.endsWith(QLatin1String(".autorecover"))) t.chop(12);
        else if (t.size() > 9 && t.at(t.size() - 9) == QLatin1Char(' ')) t.chop(9);
        r.title = t;
    }
    return r;
}
} // namespace

QString root()
{
    if (g_root.isEmpty()) g_root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/AutoRecover");
    return g_root;
}

void setRoot(const QString &dir)
{
    endSession();
    g_root = dir;
}

QString sessionDir()
{
    if (!g_session.isEmpty()) return g_session;
    const QString dir = root() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::Id128).left(12);
    QDir().mkpath(dir);
    auto lock = std::make_unique<QLockFile>(dir + QLatin1Char('/') + QLatin1String(kLockName));
    lock->setStaleLockTime(0);   // held for the whole run; only a dead run's lock is stale
    if (!lock->tryLock(0)) return QString();
    g_lock = std::move(lock);
    g_session = dir;
    return g_session;
}

QString copyPath(const QString &docPath, const QString &displayName, quint64 windowSerial)
{
    const QString dir = sessionDir();
    if (dir.isEmpty()) return QString();
    const QString key = docPath.isEmpty() ? QStringLiteral("window:%1").arg(windowSerial) : QFileInfo(docPath).absoluteFilePath();
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    return dir + QLatin1Char('/') + safeName(displayName) + QLatin1Char(' ') + QString::fromLatin1(h) + QStringLiteral(".jpub");
}

bool write(const Document &doc, const QString &copy, const QString &docPath, const QString &title, QString *error)
{
    if (copy.isEmpty()) return false;
    // Its pictures' links are kept relative to the original's folder, which is
    // where the copy is read as being when it is recovered.
    if (!savePublication(doc, copy, QImage(), error, false, docPath.isEmpty() ? QString() : QFileInfo(docPath).absolutePath())) return false;
    QSaveFile meta(sidecar(copy));
    if (!meta.open(QIODevice::WriteOnly)) return true;   // the copy alone still recovers
    const QJsonObject o{{QStringLiteral("source"), docPath}, {QStringLiteral("title"), title},
                        {QStringLiteral("saved"), QDateTime::currentDateTime().toString(Qt::ISODate)}};
    meta.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    meta.commit();
    return true;
}

void remove(const QString &copy)
{
    if (copy.isEmpty()) return;
    QFile::remove(copy);
    QFile::remove(sidecar(copy));
}

QVector<Recovered> orphans()
{
    QVector<Recovered> out;
    const QDir top(root());
    if (!top.exists()) return out;
    // Copies older versions kept loose in the folder.
    for (const QFileInfo &f : top.entryInfoList({QStringLiteral("*.jpub")}, QDir::Files)) out << describe(f.absoluteFilePath());
    for (const QFileInfo &d : top.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = d.absoluteFilePath();
        if (dir == g_session) continue;
        // A run that's still going holds its lock; a crashed run's lock is
        // stale (its process is gone) and can be taken.
        QLockFile probe(dir + QLatin1Char('/') + QLatin1String(kLockName));
        probe.setStaleLockTime(0);
        if (!probe.tryLock(0)) continue;
        probe.unlock();
        const QFileInfoList copies = QDir(dir).entryInfoList({QStringLiteral("*.jpub")}, QDir::Files);
        if (copies.isEmpty()) QDir(dir).removeRecursively();   // nothing left to offer
        for (const QFileInfo &f : copies) out << describe(f.absoluteFilePath());
    }
    std::sort(out.begin(), out.end(), [](const Recovered &a, const Recovered &b) { return a.saved > b.saved; });
    return out;
}

void discard(const Recovered &r)
{
    remove(r.file);
    const QFileInfo fi(r.file);
    QDir dir = fi.absoluteDir();
    if (QDir::cleanPath(dir.absolutePath()) == QDir::cleanPath(root())) return;
    if (dir.entryInfoList({QStringLiteral("*.jpub")}, QDir::Files).isEmpty()) dir.removeRecursively();
}

void endSession()
{
    if (g_session.isEmpty()) return;
    g_lock.reset();   // unlocks
    QDir(g_session).removeRecursively();
    g_session.clear();
}

} // namespace jp::recovery
