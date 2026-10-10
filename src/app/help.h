#pragma once
// Help: topics written in Markdown (resources/help/<language>/<id>.md), shown
// in the Help pane or, over a dialog, in a small window of their own. F1 picks
// the topic for what the person is doing (resources/help/context.json), and
// the search box finds topics and the program's commands, which it can run.
//
// A topic file starts with "# Title", then "<!-- keywords: a, b, c -->" (words
// people may search for instead of the title's), then the text. Links to other
// topics are relative: [Link text boxes](linked-text). The "index" topic is the
// table of contents. "<!-- shortcuts -->" in a topic becomes a table of every
// command's keyboard shortcut, made from the commands themselves.

#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <functional>

class QAction;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTextBrowser;
class QToolButton;

namespace jp {

class MainWindow;

namespace help {

struct Topic {
    QString id, title, keywords, markdown;   // markdown: the whole file
    bool isNull() const { return title.isEmpty(); }
};

// Every topic id, from the English topics (a translation may leave some out).
QStringList topicIds();
// A topic in the display language when it has been translated, else in English.
Topic topic(const QString &id);
// The topic for a context ("tab:Home", "pane:find", "item:picture", "file:print",
// "dialog:Page Setup"), or empty when it has none.
QString contextTopic(const QString &key);
// Topics matching every word of `query`, best first.
QStringList searchTopics(const QString &query);
// The Markdown shown for a topic: comments out, the shortcut table in.
QString displayMarkdown(const Topic &t, MainWindow *win);

// Web pages Help opens: support, feedback and this version's notes.
QUrl supportUrl();
QUrl feedbackUrl();
QUrl whatsNewUrl();
// Opens a web page in the browser (replaceable for tests).
extern std::function<bool(const QUrl &)> openUrl;

} // namespace help

// The Help pane's contents: a search box, the results (topics and commands),
// and the topic, with Back, Forward and Contents.
class HelpView : public QWidget {
    Q_OBJECT
public:
    // `commands`: the search finds commands too (not while a dialog holds the window).
    explicit HelpView(MainWindow *win, QWidget *parent = nullptr, bool commands = true);
    void showTopic(const QString &id);       // "index" is the contents
    QString currentTopic() const { return m_current; }
    void search(const QString &text);        // as if typed in the search box
    QLineEdit *searchBox() const { return m_search; }
    QListWidget *results() const { return m_results; }
    QTextBrowser *browser() const { return m_browser; }
    void focusTopic();                       // the keyboard goes to the topic's text

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void showResults(const QString &text);
    void activate(QListWidgetItem *it);
    void go(const QString &id, bool record);
    void updateButtons();
    MainWindow *m_win;
    bool m_commands;
    QLineEdit *m_search;
    QListWidget *m_results;
    QTextBrowser *m_browser;
    QToolButton *m_back, *m_forward, *m_home;
    QString m_current;
    QStringList m_history;   // topics before the current one
    QStringList m_ahead;     // topics Back left
};

// Help over a dialog, which blocks the main window and so its Help pane.
class HelpWindow : public QDialog {
    Q_OBJECT
public:
    HelpWindow(MainWindow *win, QWidget *owner);
    HelpView *view() const { return m_view; }

private:
    HelpView *m_view;
};

} // namespace jp
