#include "app/telemetry.h"

#include "app/settings.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace jp::telemetry {

namespace {

const char *kUrl = "https://telemetry-production-9964.up.railway.app/v1/ping";
const char *kAbout = "https://telemetry-production-9964.up.railway.app/";
const QString kEnabled = QStringLiteral("telemetry/enabled");
const QString kInstall = QStringLiteral("telemetry/install");
const QString kLastSent = QStringLiteral("telemetry/lastSent");
const QString kLastVersion = QStringLiteral("telemetry/lastVersion");
const QString kLaunches = QStringLiteral("telemetry/launches");
const QString kCounts = QStringLiteral("telemetry/counts");
constexpr int kMaxFeatures = 400;   // the collector takes no more

QString installId()
{
    QString id = Settings::get().value(kInstall).toString();
    if (QUuid::fromString(id).isNull()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Settings::get().setValue(kInstall, id);
    }
    return id;
}

// Only the characters the collector takes, cut to its length.
QString clean(QString s, const QString &bad, int max)
{
    s.replace(QRegularExpression(bad), QStringLiteral("-"));
    return s.left(max);
}

void send(QNetworkAccessManager *net)
{
    if (!due()) return;
    const QDate today = QDate::currentDate();
    QNetworkRequest req{QUrl(QString::fromLatin1(kUrl))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JeffPub/%1").arg(QStringLiteral(JP_VERSION)));
    req.setTransferTimeout(15000);
    const QJsonObject sent = payload();
    QNetworkReply *r = net->post(req, QJsonDocument(sent).toJson(QJsonDocument::Compact));
    QObject::connect(r, &QNetworkReply::finished, r, [r, sent, today] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError || status < 200 || status >= 300) return;   // kept for the next try
        // What was sent is counted; anything used meanwhile waits for tomorrow.
        QVariantMap counts = Settings::get().value(kCounts).toMap();
        const QJsonObject done = sent.value(QStringLiteral("counts")).toObject();
        for (auto it = done.begin(); it != done.end(); ++it) {
            const int left = counts.value(it.key()).toInt() - it.value().toInt();
            if (left > 0) counts[it.key()] = left;
            else counts.remove(it.key());
        }
        Settings::get().setValue(kCounts, counts);
        Settings::get().setValue(kLaunches, std::max(0, Settings::get().value(kLaunches).toInt() - sent.value(QStringLiteral("launches")).toInt()));
        Settings::get().setValue(kLastSent, today);
        Settings::get().setValue(kLastVersion, QStringLiteral(JP_VERSION));
    });
}

// Asked once, when JeffPub first starts (unless setup already asked).
void ask(QWidget *window)
{
    QDialog d(window);
    d.setWindowTitle(QCoreApplication::translate("Telemetry", "Help improve JeffPub"));
    auto *v = new QVBoxLayout(&d);
    auto *text = new QLabel(QCoreApplication::translate("Telemetry",
        "JeffPub can send anonymous usage statistics once a day (and when it's updated): its version, your operating system and language, "
        "and how often each of its commands is used. That shows how many people use it and which parts matter most.<br><br>"
        "It never sends your files, their names, or anything in them. <a href=\"%1\">What's sent</a><br><br>"
        "You can change this anytime in File &gt; Options.").arg(QString::fromLatin1(kAbout)), &d);
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    text->setMinimumWidth(420);
    auto *box = new QCheckBox(QCoreApplication::translate("Telemetry", "Send anonymous usage statistics"), &d);
    box->setChecked(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &d);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    v->addWidget(text);
    v->addSpacing(6);
    v->addWidget(box);
    v->addWidget(buttons);
    d.exec();
    setEnabled(box->isChecked());
}

} // namespace

bool decided() { return Settings::get().value(kEnabled).isValid(); }

bool enabled() { return Settings::get().value(kEnabled, false).toBool(); }

void setEnabled(bool on)
{
    Settings::get().setValue(kEnabled, on);
    if (on) return;
    // Turned off: nothing kept to send, and a new id if it's turned on again.
    for (const QString &k : {kInstall, kLaunches, kCounts, kLastSent, kLastVersion}) Settings::get().setValue(k, QVariant());
}

bool due()
{
    if (!enabled() || qEnvironmentVariableIsSet("JP_NO_TELEMETRY")) return false;
    // A new version reports at once, even on a day that already had a ping
    // (the collector adds it to the day's), so an update shows up the day
    // it's installed.
    return Settings::get().value(kLastSent).toDate() != QDate::currentDate() ||
           Settings::get().value(kLastVersion).toString() != QStringLiteral(JP_VERSION);
}

void count(const QString &feature)
{
    if (!enabled()) return;
    const QString f = clean(feature.toLower(), QStringLiteral("[^a-z0-9._-]"), 48);
    if (f.isEmpty() || !f[0].isLetterOrNumber()) return;
    QVariantMap counts = Settings::get().value(kCounts).toMap();
    if (!counts.contains(f) && counts.size() >= kMaxFeatures) return;
    counts[f] = counts.value(f).toInt() + 1;
    Settings::get().setValue(kCounts, counts);
}

QJsonObject payload()
{
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("windows"), osVersion = QSysInfo::kernelVersion();
#elif defined(Q_OS_MACOS)
    const QString os = QStringLiteral("macos"), osVersion = QSysInfo::productVersion();
#else
    const QString os = QStringLiteral("linux"), osVersion = QSysInfo::productType() + QLatin1Char(' ') + QSysInfo::productVersion();
#endif
    QJsonObject counts;
    const QVariantMap stored = Settings::get().value(kCounts).toMap();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        if (it.value().toInt() > 0) counts.insert(it.key(), std::min(it.value().toInt(), 100000));
    return QJsonObject{{QStringLiteral("install"), installId()},
                       {QStringLiteral("version"), clean(QStringLiteral(JP_VERSION), QStringLiteral("[^0-9A-Za-z.+-]"), 20)},
                       {QStringLiteral("os"), os},
                       {QStringLiteral("osVersion"), clean(osVersion, QStringLiteral("[^0-9A-Za-z .()_+-]"), 40)},
                       {QStringLiteral("lang"), clean(QLocale::system().name(), QStringLiteral("[^A-Za-z_-]"), 20)},
                       {QStringLiteral("launches"), std::clamp(Settings::get().value(kLaunches).toInt(), 0, 1000)},
                       {QStringLiteral("counts"), counts}};
}

void start(QWidget *window)
{
    if (qEnvironmentVariableIsSet("JP_NO_TELEMETRY")) return;
    // Setup (on Windows) writes the choice where it's kept; turned off there,
    // nothing counted before is kept either.
    if (decided() && !enabled()) setEnabled(false);
    auto *net = new QNetworkAccessManager(window);
    QPointer<QWidget> w(window);
    QTimer::singleShot(1500, window, [w, net] {
        if (!w) return;
        if (!decided()) ask(w);
        if (enabled()) Settings::get().setValue(kLaunches, Settings::get().value(kLaunches).toInt() + 1);
        QTimer::singleShot(20000, net, [net] { send(net); });
        // A day that turns over while JeffPub is open gets its ping too.
        auto *hourly = new QTimer(net);
        QObject::connect(hourly, &QTimer::timeout, net, [net] { send(net); });
        hourly->start(60 * 60 * 1000);
    });
}

} // namespace jp::telemetry
