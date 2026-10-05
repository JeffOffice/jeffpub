#include "app/updater.h"

#include "app/settings.h"

#include <QApplication>
#include <QDate>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVector>

namespace jp {

namespace {
const char *kReleases = "https://api.github.com/repos/jeffsteinport/jeffpub79/releases?per_page=10";
Updater *g_updater = nullptr;

// (major, minor, patch, preview); a final release sorts after its previews.
QVector<int> versionKey(QString v)
{
    v = v.trimmed();
    if (v.startsWith('v') || v.startsWith('V')) v.remove(0, 1);
    const QString main = v.section('-', 0, 0), rest = v.section('-', 1);
    QVector<int> k;
    for (const QString &part : main.split('.')) k << part.toInt();
    while (k.size() < 3) k << 0;
    const auto m = QRegularExpression(QStringLiteral("(\\d+)")).match(rest);
    k << (rest.isEmpty() ? 1000000 : (m.hasMatch() ? m.captured(1).toInt() : 0));
    return k;
}
} // namespace

Updater *updater() { return g_updater; }
void setUpdater(Updater *u) { g_updater = u; }

Updater::Updater(QWidget *window) : QObject(qApp), m_win(window) {}

bool Updater::isNewer(const QString &candidate, const QString &current) { return versionKey(candidate) > versionKey(current); }

void Updater::checkOnStartup()
{
    Settings &st = Settings::get();
    if (!st.value(QStringLiteral("updates/auto"), true).toBool()) return;
    const QDate last = st.value(QStringLiteral("updates/lastCheck")).toDate();
    if (last.isValid() && last >= QDate::currentDate()) return;
    QTimer::singleShot(4000, this, [this] { check(false); });
}

void Updater::check(bool interactive)
{
    if (m_busy) return;
    m_busy = true;
    QNetworkRequest req{QUrl(QString::fromLatin1(kReleases))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JeffPub79/%1").arg(QStringLiteral(JP_VERSION)));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(15000);
    QNetworkReply *r = m_net.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, interactive] {
        r->deleteLater();
        m_busy = false;
        if (r->error() != QNetworkReply::NoError) {
            if (interactive)
                QMessageBox::warning(m_win, QStringLiteral("Check for Updates"),
                                     QStringLiteral("JeffPub 79 couldn't reach the update server. Check your internet connection and try again.\n\n%1").arg(r->errorString()));
            return;
        }
        Settings::get().setValue(QStringLiteral("updates/lastCheck"), QDate::currentDate());
        const QJsonArray releases = QJsonDocument::fromJson(r->readAll()).array();
        for (const QJsonValue &v : releases) {
            const QJsonObject rel = v.toObject();
            if (rel.value("draft").toBool()) continue;
            const QString tag = rel.value("tag_name").toString();
            if (!isNewer(tag, QStringLiteral(JP_VERSION))) break;   // newest first: nothing newer
            if (!interactive && Settings::get().value(QStringLiteral("updates/skip")).toString() == tag) return;
            QString setup;
            for (const QJsonValue &a : rel.value("assets").toArray())
                if (a.toObject().value("name").toString().endsWith(QLatin1String("Setup.exe"), Qt::CaseInsensitive))
                    setup = a.toObject().value("browser_download_url").toString();
            offer(tag, rel.value("body").toString(), rel.value("html_url").toString(), setup);
            return;
        }
        if (interactive)
            QMessageBox::information(m_win, QStringLiteral("Check for Updates"),
                                     QStringLiteral("You have the latest version of JeffPub 79 (%1).").arg(QStringLiteral(JP_VERSION)));
    });
}

void Updater::offer(const QString &tag, const QString &notes, const QString &pageUrl, const QString &setupUrl)
{
    QString version = tag;
    if (version.startsWith('v')) version.remove(0, 1);
    QMessageBox box(m_win);
    box.setWindowTitle(QStringLiteral("Update Available"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("<b>JeffPub 79 %1 is available.</b><br>You have version %2.").arg(version.toHtmlEscaped(), QStringLiteral(JP_VERSION)));
    QString summary = notes;
    summary.remove(QRegularExpression(QStringLiteral("[*#`]")));
    if (summary.size() > 600) summary = summary.left(600) + QStringLiteral("…");
    box.setInformativeText(summary.trimmed());
#ifdef Q_OS_WIN
    QPushButton *now = setupUrl.isEmpty() ? nullptr : box.addButton(QStringLiteral("Update Now"), QMessageBox::AcceptRole);
#else
    QPushButton *now = nullptr;
    Q_UNUSED(setupUrl);
#endif
    QPushButton *page = box.addButton(QStringLiteral("What's New"), QMessageBox::HelpRole);
    QPushButton *later = box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    QPushButton *skip = box.addButton(QStringLiteral("Skip This Version"), QMessageBox::DestructiveRole);
    box.setDefaultButton(now ? now : later);
    box.exec();
    if (box.clickedButton() == page) QDesktopServices::openUrl(QUrl(pageUrl));
    else if (box.clickedButton() == skip) Settings::get().setValue(QStringLiteral("updates/skip"), tag);
    else if (now && box.clickedButton() == now) downloadAndRun(setupUrl, tag);
}

void Updater::downloadAndRun(const QString &url, const QString &tag)
{
    const QString dest = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("JeffPub79-Setup-%1.exe").arg(tag));
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JeffPub79/%1").arg(QStringLiteral(JP_VERSION)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = m_net.get(req);
    auto *progress = new QProgressDialog(QStringLiteral("Downloading JeffPub 79 %1…").arg(tag), QStringLiteral("Cancel"), 0, 100, m_win);
    progress->setWindowTitle(QStringLiteral("Update"));
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    connect(progress, &QProgressDialog::canceled, r, &QNetworkReply::abort);
    connect(r, &QNetworkReply::downloadProgress, progress, [progress](qint64 got, qint64 total) {
        if (total > 0) progress->setValue(int(got * 100 / total));
    });
    connect(r, &QNetworkReply::finished, this, [this, r, progress, dest] {
        r->deleteLater();
        progress->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            if (r->error() != QNetworkReply::OperationCanceledError)
                QMessageBox::warning(m_win, QStringLiteral("Update"), QStringLiteral("The download didn't finish: %1").arg(r->errorString()));
            return;
        }
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly) || f.write(r->readAll()) <= 0) {
            QMessageBox::warning(m_win, QStringLiteral("Update"), QStringLiteral("JeffPub 79 couldn't save the installer to %1.").arg(QDir::toNativeSeparators(dest)));
            return;
        }
        f.close();
        // Give every window a chance to save, then hand over to the installer.
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->inherits("jp::MainWindow") && w->isVisible() && !w->close()) return;
        if (!QProcess::startDetached(dest, {})) {
            QMessageBox::warning(m_win, QStringLiteral("Update"), QStringLiteral("JeffPub 79 couldn't start the installer. You can run it yourself from %1.").arg(QDir::toNativeSeparators(dest)));
            return;
        }
        QApplication::quit();
    });
}

} // namespace jp
