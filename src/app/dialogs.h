#pragma once
// Dialog boxes.

#include "core/style.h"

#include <QString>

class QWidget;

namespace jp {

class Editor;
class MainWindow;

void pageSetupDialog(QWidget *p, Editor *ed);
void gridGuidesDialog(QWidget *p, Editor *ed);
void fontDialog(QWidget *p, Editor *ed);
void paragraphDialog(QWidget *p, Editor *ed, int tab = 0);
void bulletsDialog(QWidget *p, Editor *ed, bool numbering);
void dropCapDialog(QWidget *p, Editor *ed);
void characterSpacingDialog(QWidget *p, Editor *ed);
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
void documentPropertiesDialog(QWidget *p, Editor *ed);
void measurementWindow(QWidget *p, Editor *ed);
void tintsDialog(QWidget *p, Editor *ed, const std::function<void(const ColorRef &)> &apply);
void spellingDialog(QWidget *p, Editor *ed);
void thesaurusDialog(QWidget *p, Editor *ed);
void hyphenationDialog(QWidget *p, Editor *ed);
void pasteSpecialDialog(QWidget *p, Editor *ed);
void picturePlaceholderPrompt(QWidget *p, Editor *ed);
void insertFileDialog(QWidget *p, Editor *ed);
void recipientsDialog(QWidget *p, Editor *ed, bool typeNew);
void mergeFieldDialog(QWidget *p, Editor *ed, int kind);   // 0 address block, 1 greeting line

} // namespace jp
