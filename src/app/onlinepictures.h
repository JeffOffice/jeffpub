#pragma once
// Insert > Online Pictures: searches two open libraries of freely licensed
// pictures, Openverse and Wikimedia Commons, shows each one's author and
// license, and inserts the one chosen, with a credit line under it when its
// license asks for one.

#include <QList>
#include <QTemporaryDir>
#include <QUrl>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QPushButton;

namespace jp {

class MainWindow;

namespace online {

struct Picture {
    QString title, creator, license, licenseUrl;
    QUrl page;    // the picture's page in its library
    QUrl full;    // the picture itself
    QUrl thumb;   // a small copy for the results
    bool needsCredit = true;   // the license asks for the author to be named
};

enum Source { Openverse, Commons };
// What a library may send: this many results, at most this many bytes (for a
// picture; for a search answer or a small copy), and from Commons a copy no wider
// than the last width in place of a bigger original upload.
constexpr int kMaxResults = 24;
constexpr qint64 kMaxPictureBytes = 50 * 1024 * 1024;
constexpr qint64 kMaxSmallBytes = 2 * 1024 * 1024;
constexpr int kRenditionWidth = 1920;
QUrl searchUrl(Source s, const QString &query);
QList<Picture> parseOpenverse(const QByteArray &json);
QList<Picture> parseCommons(const QByteArray &json);
// The credit line: "Title" by Author, License.
QString credit(const Picture &p);

// Fetches a secure (https) web address and calls `done` with what came back, or
// an error (replaceable in tests, which never go online). The request ends
// when `context` goes, when more than `maxBytes` come, or when it takes too
// long overall.
using Done = std::function<void(const QByteArray &bytes, const QString &error)>;
extern std::function<void(const QUrl &url, QObject *context, const Done &done, qint64 maxBytes)> fetch;
// The download behind fetch, to any address (tests point it at a local one).
void download(QNetworkAccessManager *net, const QUrl &url, QObject *context, const Done &done, qint64 maxBytes, int totalMs);

} // namespace online

class OnlinePicturesPane : public QWidget {
    Q_OBJECT
public:
    explicit OnlinePicturesPane(MainWindow *win);
    ~OnlinePicturesPane() override;
    void search(const QString &query);   // as if typed and Enter pressed
    QListWidget *results() const { return m_results; }
    QPushButton *insertButton() const { return m_insert; }
    QCheckBox *creditBox() const { return m_credit; }

private:
    void showDetails();
    void insertChosen();
    MainWindow *m_win;
    QComboBox *m_source;
    QLineEdit *m_query;
    QLabel *m_status, *m_details;
    QListWidget *m_results;
    QCheckBox *m_credit;
    QPushButton *m_insert;
    QList<online::Picture> m_found;
    int m_search = 0;   // the latest search, so older answers are dropped
    QObject *m_requests = nullptr;   // the latest search's requests; a new search ends them
    QTemporaryDir m_downloads;
};

} // namespace jp
