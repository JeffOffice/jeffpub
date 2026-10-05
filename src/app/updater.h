#pragma once
// Update check: asks GitHub's public releases list for a newer JeffPub 79 and
// offers to download and run its installer. Sends nothing about the user.

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

class QWidget;

namespace jp {

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QWidget *window);
    // interactive: also report "up to date" and errors (Check for Updates button).
    void check(bool interactive);
    // Check at most once a day, if automatic checks are on.
    void checkOnStartup();

    // Version strings like "0.1.6" or tags like "v0.1.0-preview5", compared numerically.
    static bool isNewer(const QString &candidate, const QString &current);

private:
    void offer(const QString &tag, const QString &notes, const QString &pageUrl, const QString &setupUrl);
    void downloadAndRun(const QString &url, const QString &tag);
    QNetworkAccessManager m_net;
    QPointer<QWidget> m_win;
    bool m_busy = false;
};

Updater *updater();   // the application's one updater (created by main)
void setUpdater(Updater *u);

} // namespace jp
