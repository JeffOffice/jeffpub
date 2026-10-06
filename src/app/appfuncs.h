#pragma once
// Commands implemented in their own files (sharing.cpp, proofing.cpp,
// mainwindow_ribbon.cpp) and used from the window, backstage and task panes.
// Declared here at namespace scope: block-scope declarations inside lambdas
// resolve to the global namespace on some compilers and fail to link.

#include "core/style.h"

#include <QString>
#include <QPair>
#include <QStringList>
#include <QVector>

class QImage;
class QTextDocument;
class QWidget;

namespace jp {

class Document;
class Editor;
class MainWindow;

void emailCurrentPage(QWidget *parent, Editor *ed);
void emailAsAttachment(QWidget *parent, Editor *ed, const QString &format);
// A mail header value from a name: one line, non-ASCII encoded (RFC 2047).
QString emlHeaderText(const QString &s);
void packAndGo(QWidget *parent, MainWindow *win, bool forPrinter);
void saveForPhotoPrinter(QWidget *parent, MainWindow *win);
void saveAsTemplate(QWidget *parent, MainWindow *win);
QStringList thesaurusLookup(const QString &word);
Stroke currentBorderStroke();
QImage proceduralTexture(const QString &name, int size);   // built-in texture fills
int hyphenateStory(QTextDocument *doc);                   // returns hyphens inserted
QVector<QPair<int, int>> misspellings(QTextDocument *doc); // [start, end) document ranges
QStringList pressProblems(const Document &d);              // what a printer would object to (before PDF/X)
QStringList spellingSuggestions(const QString &word, const QString &language = QString());   // language: BCP-47, "" US English
void spellingAdd(const QString &word);                    // add to the user dictionary
void spellingIgnore(const QString &word);                 // ignore for this session
// The save dialog every Save As and Export command uses (see filedialogs.cpp).
// Extra advice after a failed save (Windows: folders protected from unknown apps).
QString saveFailureHint(const QString &error);
QString askSavePath(QWidget *parent, const QString &caption, const QString &suggested,
                    const QString &filter = QString(), QString *selectedFilter = nullptr);   // the border chosen in the table Borders gallery

} // namespace jp
