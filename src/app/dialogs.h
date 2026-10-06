#pragma once
// Dialog boxes.

#include "core/document.h"
#include "core/style.h"

#include <QPair>
#include <QString>
#include <QVector>

class QWidget;

namespace jp {

class Editor;
class MainWindow;

void pageSetupDialog(QWidget *p, Editor *ed);
// Symbols: insert one (in a font, or the text's own when empty) at the text
// cursor, remembering it; the recently used ones, newest first ("char\tfont").
bool insertSymbol(QWidget *p, Editor *ed, const QString &ch, const QString &font);
QStringList recentSymbols();
// Layout Guides: Margin Guides (tab 0), Grid Guides (1), Baseline Guides (2).
void gridGuidesDialog(QWidget *p, Editor *ed, int startTab = 1);
// Page sizes the user created (name and whole setup), and their dialogs.
QVector<QPair<QString, PageSetup>> customPageSizes();
// With apply false the new size is only saved (for a new publication).
bool createPageSizeDialog(QWidget *p, Editor *ed, int editIndex = -1, bool apply = true);
void customPageSizesDialog(QWidget *p, Editor *ed);
void applyPageSetup(Editor *ed, const PageSetup &setup, const QString &sizeName, const QString &undoName);
void fontDialog(QWidget *p, Editor *ed);
void paragraphDialog(QWidget *p, Editor *ed, int tab = 0);
void bulletsDialog(QWidget *p, Editor *ed, bool numbering);
void dropCapDialog(QWidget *p, Editor *ed);
void characterSpacingDialog(QWidget *p, Editor *ed);
void textGradientDialog(QWidget *p, Editor *ed);   // Text Fill > Gradient
void formatObjectDialog(QWidget *p, Editor *ed, int tab = 0);
void insertTableDialog(QWidget *p, Editor *ed);
void insertPageDialog(QWidget *p, Editor *ed);
void symbolDialog(QWidget *p, Editor *ed);
void dateTimeDialog(QWidget *p, Editor *ed);
void hyperlinkDialog(QWidget *p, Editor *ed);
void bookmarkDialog(QWidget *p, Editor *ed);
void businessInfoDialog(QWidget *p, Editor *ed);
void colorSchemeDialog(QWidget *p, Editor *ed);
void fontSchemeDialog(QWidget *p, Editor *ed);
bool fillEffectsDialog(QWidget *p, Editor *ed, Fill &fill, const QString &title = QString());
void shadowDialog(QWidget *p, Editor *ed);
void textArtTextDialog(QWidget *p, Editor *ed, const QString &itemId);
void calendarDialog(QWidget *p, Editor *ed);
void styleDialog(QWidget *p, Editor *ed, const QString &styleName = QString());
void wordCountDialog(QWidget *p, Editor *ed);
void optionsDialog(QWidget *p, Editor *ed);
void pageNumberDialog(QWidget *p, Editor *ed);
void documentPropertiesDialog(QWidget *p, Editor *ed, int tab = 0);   // 1 = Commercial Print
void measurementWindow(QWidget *p, Editor *ed);
void tintsDialog(QWidget *p, Editor *ed, const std::function<void(const ColorRef &)> &apply);
void spellingDialog(QWidget *p, Editor *ed);
void thesaurusDialog(QWidget *p, Editor *ed);
void hyphenationDialog(QWidget *p, Editor *ed);
void pasteSpecialDialog(QWidget *p, Editor *ed);
void insertFileDialog(QWidget *p, Editor *ed);
int autoflowText(Editor *ed, const QString &boxId);   // new pages and linked boxes until the story fits; returns pages added
void recipientsDialog(QWidget *p, Editor *ed, bool typeNew);
void mergeFieldDialog(QWidget *p, Editor *ed, int kind);   // 0 address block, 1 greeting line

} // namespace jp
