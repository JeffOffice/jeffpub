#pragma once
// Update check: asks GitHub's public releases list for a newer JeffPub 79 and
// offers to download and run its installer. Sends nothing about the user.

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

class QDialog;
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
    // Whether an installer from the releases list may be downloaded and run:
    // from this project's releases on GitHub, with a usable version tag and
    // the SHA-256 GitHub publishes for it.
    static bool trustedInstaller(const QString &url, const QString &tag, const QString &digest);

    // The part of a release's notes worth showing in the offer: its "New
    // in" sections (headings made bold), without the install steps.
    static QString releaseHighlights(const QString &notes);
    // What the offer's exec() returns besides Rejected (Later).
    enum OfferChoice { Install = 1, Skip, OpenPage };
    // The update offer: the new version, its notes (Markdown) and a choice;
    // canInstall offers Update Now, otherwise the download page.
    static QDialog *offerDialog(QWidget *parent, const QString &version, const QString &notes, bool canInstall, const QString &pageUrl);

private:
    void offer(const QString &tag, const QString &notes, const QString &pageUrl, const QString &setupUrl, const QString &digest);
    void downloadAndRun(const QString &url, const QString &tag, const QString &digest, const QString &pageUrl);
    QNetworkAccessManager m_net;
    QPointer<QWidget> m_win;
    bool m_busy = false;
};

Updater *updater();   // the application's one updater (created by main)
void setUpdater(Updater *u);

} // namespace jp
