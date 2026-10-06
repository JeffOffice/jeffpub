#include "app/updater.h"

#include "app/icons.h"
#include "app/settings.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QDate>
#include <QDesktopServices>
#include <QVBoxLayout>
#include <QTextBrowser>
#include <QTextDocument>
#include <QLabel>
#include <QHBoxLayout>
#include <QDialog>
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

#include <algorithm>
#include <memory>

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

bool Updater::trustedInstaller(const QString &url, const QString &tag, const QString &digest)
{
    static const QRegularExpression tagChars(QStringLiteral("^[A-Za-z0-9._-]{1,40}$"));
    static const QRegularExpression sha(QStringLiteral("^sha256:[0-9a-fA-F]{64}$"));
    return url.startsWith(QLatin1String("https://github.com/jeffsteinport/jeffpub79/releases/download/")) && tagChars.match(tag).hasMatch() &&
           sha.match(digest).hasMatch();
}

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
        // The newest release is offered, with the notes of every release
        // since the one running (newest first).
        QJsonObject newest;
        QStringList notes;
        for (const QJsonValue &v : releases) {
            const QJsonObject rel = v.toObject();
            if (rel.value("draft").toBool()) continue;
            const QString tag = rel.value("tag_name").toString();
            if (!isNewer(tag, QStringLiteral(JP_VERSION))) break;   // newest first: nothing newer
            if (newest.isEmpty()) {
                if (!interactive && Settings::get().value(QStringLiteral("updates/skip")).toString() == tag) return;
                newest = rel;
            }
            if (const QString h = releaseHighlights(rel.value("body").toString()); !h.isEmpty()) notes << h;
        }
        if (!newest.isEmpty()) {
            const QString tag = newest.value("tag_name").toString();
            QString setup, digest;
            for (const QJsonValue &a : newest.value("assets").toArray())
                if (a.toObject().value("name").toString().endsWith(QLatin1String("Setup.exe"), Qt::CaseInsensitive)) {
                    setup = a.toObject().value("browser_download_url").toString();
                    digest = a.toObject().value("digest").toString();   // "sha256:..."
                }
            // Only an installer that can be checked is offered to run; the
            // release page is offered either way.
            if (!trustedInstaller(setup, tag, digest)) setup.clear();
            offer(tag, notes.join(QStringLiteral("\n\n")), newest.value("html_url").toString(), setup, digest);
            return;
        }
        if (interactive)
            QMessageBox::information(m_win, QStringLiteral("Check for Updates"),
                                     QStringLiteral("You have the latest version of JeffPub 79 (%1).").arg(QStringLiteral(JP_VERSION)));
    });
}

QString Updater::releaseHighlights(const QString &notes)
{
    static const QRegularExpression heading(QStringLiteral("^#{1,6}\\s+(.*)$"));
    QStringList news, others;   // "New in" sections; the rest, if there are none
    QStringList *into = nullptr;
    for (const QString &line : QString(notes).replace(QLatin1String("\r\n"), QLatin1String("\n")).split(QLatin1Char('\n'))) {
        if (const auto m = heading.match(line); m.hasMatch()) {
            const QString h = m.captured(1).remove(QLatin1Char('*')).trimmed();
            into = h.startsWith(QLatin1String("New in"), Qt::CaseInsensitive) ? &news
                 : h.startsWith(QLatin1String("Install"), Qt::CaseInsensitive) ? nullptr
                                                                                : &others;
            if (into) *into << QStringLiteral("**%1**").arg(h) << QString();
        } else if (into) {
            *into << line;   // text before the first heading is the preview notice
        }
    }
    return (news.isEmpty() ? others : news).join(QLatin1Char('\n')).trimmed();
}

QDialog *Updater::offerDialog(QWidget *parent, const QString &version, const QString &notes, bool canInstall, const QString &pageUrl)
{
    auto *d = new QDialog(parent);
    d->setWindowTitle(QStringLiteral("Update Available"));
    d->resize(560, 0);
    auto *icon = new QLabel(d);
    icon->setPixmap(QApplication::windowIcon().pixmap(QSize(56, 56)));
    icon->setAlignment(Qt::AlignTop);
    icon->setVisible(!icon->pixmap().isNull());
    auto *title = new QLabel(QStringLiteral("JeffPub 79 %1 is available").arg(version.toHtmlEscaped()), d);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.4);
    tf.setBold(true);
    title->setFont(tf);
    auto *sub = new QLabel(canInstall ? QStringLiteral("You have version %1. Update Now downloads the installer and asks you to save your work first.").arg(QStringLiteral(JP_VERSION))
                                      : QStringLiteral("You have version %1. The new version can be downloaded from its release page.").arg(QStringLiteral(JP_VERSION)),
                           d);
    sub->setWordWrap(true);
    QPalette dim = sub->palette();
    dim.setColor(QPalette::WindowText, dim.color(QPalette::PlaceholderText));
    sub->setPalette(dim);
    auto *head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(sub);
    auto *top = new QHBoxLayout;
    top->setSpacing(16);
    top->addWidget(icon, 0, Qt::AlignTop);
    top->addLayout(head, 1);

    auto *news = new QTextBrowser(d);
    news->setOpenExternalLinks(true);
    news->setMarkdown(notes.isEmpty() ? QStringLiteral("This version has no notes.") : notes);
    news->document()->setDocumentMargin(12);
    news->document()->setIndentWidth(18);
    // As tall as the notes need, within reason (longer notes scroll). The
    // browser lays its own document out at its current width, so measure a
    // copy at the width it will have.
    std::unique_ptr<QTextDocument> probe(news->document()->clone());
    probe->setDefaultFont(news->font());
    probe->setTextWidth(560 - 2 * 24 - 2);
    news->setFixedHeight(std::clamp(int(probe->size().height()) + 4, 110, 300));
    news->setStyleSheet(uiDark() ? QStringLiteral("QTextBrowser{background:#22262d; border:1px solid #2f343d; border-radius:10px;}")
                                 : QStringLiteral("QTextBrowser{background:#ffffff; border:1px solid #dcdfe4; border-radius:10px;}"));
    auto *page = new QLabel(QStringLiteral("<a href=\"%1\">Release page</a>").arg(pageUrl.toHtmlEscaped()), d);
    page->setOpenExternalLinks(true);

    auto *skip = new QPushButton(QStringLiteral("Skip This Version"), d);
    auto *later = new QPushButton(QStringLiteral("Later"), d);
    auto *go = new QPushButton(canInstall ? QStringLiteral("Update Now") : QStringLiteral("Open Download Page"), d);
    go->setProperty("primary", true);
    go->setDefault(true);
    QObject::connect(skip, &QPushButton::clicked, d, [d] { d->done(Skip); });
    QObject::connect(later, &QPushButton::clicked, d, &QDialog::reject);
    QObject::connect(go, &QPushButton::clicked, d, [d, canInstall] { d->done(canInstall ? Install : OpenPage); });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(page);
    buttons->addStretch(1);
    buttons->addWidget(skip);
    buttons->addWidget(later);
    buttons->addWidget(go);

    auto *v = new QVBoxLayout(d);
    v->setContentsMargins(24, 22, 24, 18);
    v->setSpacing(14);
    v->addLayout(top);
    v->addWidget(news, 1);
    v->addLayout(buttons);
    return d;
}

void Updater::offer(const QString &tag, const QString &notes, const QString &pageUrl, const QString &setupUrl, const QString &digest)
{
    QString version = tag;
    if (version.startsWith('v')) version.remove(0, 1);
#ifdef Q_OS_WIN
    const bool canInstall = !setupUrl.isEmpty();
#else
    const bool canInstall = false;   // Linux packages are installed by the user
#endif
    std::unique_ptr<QDialog> d(offerDialog(m_win, version, notes, canInstall, pageUrl));
    const int choice = d->exec();
    d.reset();
    if (choice == OpenPage) QDesktopServices::openUrl(QUrl(pageUrl));
    else if (choice == Skip) Settings::get().setValue(QStringLiteral("updates/skip"), tag);
    else if (choice == Install) downloadAndRun(setupUrl, tag, digest, pageUrl);
}

void Updater::downloadAndRun(const QString &url, const QString &tag, const QString &digest, const QString &pageUrl)
{
    if (!trustedInstaller(url, tag, digest)) return;
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
    connect(r, &QNetworkReply::finished, this, [this, r, progress, dest, digest, pageUrl] {
        r->deleteLater();
        progress->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            if (r->error() != QNetworkReply::OperationCanceledError)
                QMessageBox::warning(m_win, QStringLiteral("Update"), QStringLiteral("The download didn't finish: %1").arg(r->errorString()));
            return;
        }
        const QByteArray bytes = r->readAll();
        // Run only the installer GitHub lists for this release, byte for byte.
        const QByteArray got = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        if (bytes.isEmpty() || got != digest.mid(7).toLatin1().toLower()) {
            QMessageBox::warning(m_win, QStringLiteral("Update"),
                                 QStringLiteral("The downloaded installer didn't match the one published for this release, so it wasn't run. "
                                                "You can download it from the release page instead."));
            QDesktopServices::openUrl(QUrl(pageUrl));
            return;
        }
        QSaveFile f(dest);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
            QMessageBox::warning(m_win, QStringLiteral("Update"), QStringLiteral("JeffPub 79 couldn't save the installer to %1.").arg(QDir::toNativeSeparators(dest)));
            return;
        }
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
