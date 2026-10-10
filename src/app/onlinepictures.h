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
QUrl searchUrl(Source s, const QString &query);
QList<Picture> parseOpenverse(const QByteArray &json);
QList<Picture> parseCommons(const QByteArray &json);
// The credit line: "Title" by Author, License.
QString credit(const Picture &p);

// Fetches a web address and calls `done` with what came back, or an error
// (replaceable in tests, which never go online). `context` ends the request
// when it goes.
using Done = std::function<void(const QByteArray &bytes, const QString &error)>;
extern std::function<void(const QUrl &url, QObject *context, const Done &done)> fetch;

} // namespace online

class OnlinePicturesPane : public QWidget {
    Q_OBJECT
public:
    explicit OnlinePicturesPane(MainWindow *win);
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
    QTemporaryDir m_downloads;
};

} // namespace jp
