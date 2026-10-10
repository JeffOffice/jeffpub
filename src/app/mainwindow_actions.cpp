#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>
// Every command JeffPub offers, as QActions used by the ribbon, menus and
// keyboard shortcuts.

#include "io/importers.h"
#include "app/appfuncs.h"
#include "app/mainwindow.h"
#include "render/renderer.h"
#include "app/pagespane.h"
#include "app/notes.h"
#include "app/iconpicker.h"
#include "core/svg.h"
#include "app/toc.h"

#include "app/dialogs.h"
#include "app/help.h"
#include "app/ribbon.h"
#include "app/icons.h"
#include "app/settings.h"
#include "app/taskpane.h"
#include "canvas/canvas.h"
#include "render/shapes.h"
#include "render/textart.h"
#include "templates/templates.h"
#include "text/dictionaries.h"
#include "text/textprops.h"

#include <QAction>
#include <QUrlQuery>
#include <QUrl>
#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QTextDocumentFragment>
#include <QMimeData>

namespace jp {

static TableItem *selTable(Editor *ed)
{
    if (ed->isEditingText()) return dynamic_cast<TableItem *>(ed->doc()->item(ed->textTarget().itemId));
    return dynamic_cast<TableItem *>(ed->single());
}

// Table structure edits keep stories, spans and borders consistent.
static void tableInsertRow(Editor *ed, TableItem *t, int at)
{
    at = std::clamp(at, 0, t->rows);
    const int src = std::clamp(at == t->rows ? at - 1 : at, 0, t->rows - 1);
    QVector<TableCell> row;
    for (int c = 0; c < t->cols; ++c) {
        TableCell n;
        const TableCell &s = t->cell(src, c);
        n.fill = s.fill;
        n.border = s.border;
        n.margins = s.margins;
        n.valign = s.valign;
        n.vertical = s.vertical;
        n.hyphenate = s.hyphenate;
        n.hyphenZone = s.hyphenZone;
        n.storyId = ed->doc()->createStory();
        row << n;
    }
    t->cells.insert(at * t->cols, row.size(), TableCell());
    for (int c = 0; c < t->cols; ++c) t->cells[at * t->cols + c] = row[c];
    t->rowH.insert(at, t->rowH.value(src, 18));
    ++t->rows;
    t->syncRect();
}

static void tableInsertCol(Editor *ed, TableItem *t, int at)
{
    at = std::clamp(at, 0, t->cols);
    const int src = std::clamp(at == t->cols ? at - 1 : at, 0, t->cols - 1);
    QVector<TableCell> cells;
    for (int r = 0; r < t->rows; ++r)
        for (int c = 0; c <= t->cols; ++c) {
            if (c == at) {
                TableCell n;
                const TableCell &s = t->cell(r, src);
                n.fill = s.fill;
                n.border = s.border;
                n.margins = s.margins;
                n.vertical = s.vertical;
                n.hyphenate = s.hyphenate;
                n.hyphenZone = s.hyphenZone;
                n.storyId = ed->doc()->createStory();
                cells << n;
            }
            if (c < t->cols) cells << t->cell(r, c);
        }
    const double w = t->colW.value(src, 72);
    t->colW.insert(at, w);
    ++t->cols;
    t->cells = cells;
    // Keep the table the same width.
    double total = 0;
    for (double v : t->colW) total += v;
    const double k = (total - w) / total;
    for (double &v : t->colW) v *= k;
    t->syncRect();
}

static void tableDeleteRow(TableItem *t, int r)
{
    if (t->rows <= 1) return;
    t->cells.remove(r * t->cols, t->cols);
    t->rowH.removeAt(r);
    --t->rows;
    t->syncRect();
}

static void tableDeleteCol(TableItem *t, int c)
{
    if (t->cols <= 1) return;
    QVector<TableCell> cells;
    for (int r = 0; r < t->rows; ++r)
        for (int k = 0; k < t->cols; ++k)
            if (k != c) cells << t->cell(r, k);
    const double w = t->colW[c];
    t->colW.removeAt(c);
    --t->cols;
    t->cells = cells;
    for (double &v : t->colW) v += w / t->cols;
    t->syncRect();
}

void MainWindow::createActions()
{
    Editor *ed = m_ed;
    auto needsSel = [ed] { return !ed->selection().isEmpty(); };
    Q_UNUSED(needsSel);

    // ---------------- File / quick access ----------------
    mk("file.new", tr("New"), "file-plus", QKeySequence::New, [this] { showBackstage("new"); });
    mk("file.blank", tr("New Blank Publication"), "file", QKeySequence(), [this] {
        if (maybeSave()) newPublication(Document::blank(QSizeF(612, 792)));
    });
    mk("file.open", tr("Open"), "folder-open", QKeySequence::Open, [this] {
        if (!maybeSave()) return;
        const QString p = QFileDialog::getOpenFileName(this, tr("Open Publication"), Settings::get().value("dirs/open", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString(),
                                                       tr("Publications (*.jpub *.pub)") + QStringLiteral(";;") + tr("JeffPub Publications (*.jpub)") + QStringLiteral(";;") + tr(".pub Publication Files (*.pub)") + QStringLiteral(";;") + tr("All Files (*)"));
        if (p.isEmpty()) return;
        Settings::get().setValue("dirs/open", QFileInfo(p).absolutePath());
        openFile(p);
    });
    mk("file.save", tr("Save"), "save", QKeySequence::Save, [this] { save(); });
    mk("file.saveAs", tr("Save As"), "save-all", QKeySequence(Qt::Key_F12), [this] { saveAs(); });
    mk("file.saveAsPub", tr("Save as .pub File"), "file-output", QKeySequence(), [this] { saveAs("pub"); });
    mk("file.print", tr("Print"), "printer", QKeySequence::Print, [this] { showBackstage("print"); });
    mk("file.printNow", tr("Print"), "printer", QKeySequence(), [this] { printPublication(); });
    mk("file.exportPdf", tr("Create PDF"), "file-text", QKeySequence(), [this] { exportPdfWithOptions(); });
    mk("file.exportImages", tr("Save as Picture"), "image-down", QKeySequence(), [this] { exportImages(); });
    mk("file.exportHtml", tr("Save as Web Page"), "globe", QKeySequence(), [this] { exportHtml(); });
    mk("file.close", tr("Close"), "x", QKeySequence(Qt::CTRL | Qt::Key_F4), [this] {
        if (maybeSave()) newPublication(Document::blank(QSizeF(612, 792)));
    });
    mk("file.properties", tr("Properties"), "info", QKeySequence(), [this] { documentPropertiesDialog(this, m_ed); });
    mk("file.options", tr("Options"), "settings", QKeySequence(), [this] { optionsDialog(this, m_ed); });
    mk("edit.undo", tr("Undo"), "undo-2", QKeySequence::Undo, [ed] { ed->undo(); });
    mk("edit.redo", tr("Redo"), "redo-2", QKeySequence(Qt::CTRL | Qt::Key_Y), [ed] { ed->redo(); });

    // ---------------- Clipboard ----------------
    mk("edit.cut", tr("Cut"), "scissors", QKeySequence::Cut, [ed] { ed->cut(); });
    mk("edit.copy", tr("Copy"), "copy", QKeySequence::Copy, [ed] { ed->copy(); });
    mk("edit.paste", tr("Paste"), "clipboard-paste", QKeySequence::Paste, [ed] { ed->paste(); });
    mk("edit.pasteText", tr("Keep Text Only"), "clipboard-type", QKeySequence(), [ed] { ed->paste(true); });
    mk("edit.pasteSpecial", tr("Paste Special…"), "clipboard-list", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V), [this] { pasteSpecialDialog(this, m_ed); });
    mk("edit.duplicate", tr("Duplicate"), "copy-plus", QKeySequence(Qt::CTRL | Qt::Key_D), [ed] { ed->duplicateSelection(); });
    mk("edit.delete", tr("Delete Object"), "trash-2", QKeySequence(), [ed] { ed->deleteSelection(); });
    mk("edit.formatPainter", tr("Format Painter"), "paintbrush", QKeySequence(), [ed] {
        if (ed->tool() == Tool::FormatPainter) { ed->setTool(Tool::Select); return; }
        ed->copyFormatting();
        ed->painterLocked = QApplication::keyboardModifiers() & Qt::ShiftModifier;
        ed->setTool(Tool::FormatPainter);
    });
    // As the other program: Ctrl+Shift+C and Ctrl+Shift+V copy and paste
    // formatting, without the painter's pointer.
    mk("edit.copyFormat", tr("Copy Formatting"), "paintbrush", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [this, ed] {
        ed->copyFormatting();
        statusBar()->showMessage(ed->hasCopiedFormatting() ? tr("Formatting copied. Select text or objects and press %1 to apply it.").arg(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V).toString(QKeySequence::NativeText))
                                                           : tr("Select text or an object to copy its formatting."), 5000);
    });
    mk("edit.pasteFormat", tr("Paste Formatting"), "paintbrush", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), [ed] { ed->pasteFormatting(); });
    mk("edit.selectAll", tr("Select All"), "text-select", QKeySequence::SelectAll, [ed] {
        if (ed->isEditingText()) {
            QTextCursor c = ed->cursor();
            c.select(QTextCursor::Document);
            ed->setCursor(c);
            return;
        }
        QStringList ids;
        for (const auto &it : ed->surfaceItems()) ids << it->id;
        ed->select(ids);
    });
    mk("edit.selectText", tr("Select All Text in Text Box"), "text-select", QKeySequence(), [ed] {
        if (Item *it = ed->single(); it && it->hasText()) {
            if (!ed->isEditingText()) ed->beginTextEdit(it->id);
            QTextCursor c = ed->cursor();
            c.select(QTextCursor::Document);
            ed->setCursor(c);
        }
    });
    mk("edit.selectObjects", tr("Select Objects"), "mouse-pointer-2", QKeySequence(), [ed] { ed->setTool(Tool::Select); ed->endTextEdit(); });
    for (const auto &[id, name, type] : {std::tuple{"sel.text", tr("Text Boxes"), ItemType::Text}, {"sel.pictures", tr("Pictures"), ItemType::Picture},
                                          {"sel.shapes", tr("Shapes"), ItemType::Shape}, {"sel.tables", tr("Tables"), ItemType::Table}, {"sel.textart", tr("Text Art"), ItemType::TextArt}}) {
        const ItemType t = type;
        mk(id, tr("Select All %1").arg(name), "", QKeySequence(), [ed, t] {
            QStringList ids;
            for (const auto &it : ed->surfaceItems())
                if (it->type() == t) ids << it->id;
            ed->select(ids);
        });
    }
    mk("edit.find", tr("Find"), "search", QKeySequence::Find, [this] { showTaskPane("find"); });
    mk("edit.replace", tr("Replace"), "replace", QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showTaskPane("replace"); });

    // ---------------- Font ----------------
    mk("fmt.bold", tr("Bold"), "bold", QKeySequence::Bold, [ed] { ed->toggleBold(); }, true);
    mk("fmt.italic", tr("Italic"), "italic", QKeySequence::Italic, [ed] { ed->toggleItalic(); }, true);
    mk("fmt.underline", tr("Underline"), "underline", QKeySequence::Underline, [ed] { ed->toggleUnderline(); }, true);
    mk("fmt.underlineDouble", tr("Double Underline"), "", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), [ed] { ed->toggleUnderline(QTextCharFormat::SingleUnderline, 1); });
    mk("fmt.underlineDotted", tr("Dotted Underline"), "", QKeySequence(), [ed] { ed->toggleUnderline(QTextCharFormat::DotLine); });
    mk("fmt.underlineDash", tr("Dashed Underline"), "", QKeySequence(), [ed] { ed->toggleUnderline(QTextCharFormat::DashUnderline); });
    mk("fmt.underlineWave", tr("Wavy Underline"), "", QKeySequence(), [ed] { ed->toggleUnderline(QTextCharFormat::WaveUnderline); });
    mk("fmt.strike", tr("Strikethrough"), "strikethrough", QKeySequence(), [ed] { ed->toggleStrike(); }, true);
    mk("fmt.sub", tr("Subscript"), "subscript", QKeySequence(Qt::CTRL | Qt::Key_Equal), [ed] { ed->toggleScript(false); }, true);
    mk("fmt.sup", tr("Superscript"), "superscript", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus), [ed] { ed->toggleScript(true); }, true);
    mk("fmt.grow", tr("Grow Font"), "a-arrow-up", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Greater), [ed] { ed->growFont(1); });
    mk("fmt.shrink", tr("Shrink Font"), "a-arrow-down", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Less), [ed] { ed->growFont(-1); });
    mk("fmt.clear", tr("Clear All Formatting"), "remove-formatting", QKeySequence(Qt::CTRL | Qt::Key_Space), [ed] { ed->clearFormatting(); });
    mk("fmt.smallCaps", tr("Small Caps"), "case-upper", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), [ed] {
        QTextCharFormat f;
        f.setFontCapitalization(ed->currentCharFormat().fontCapitalization() == QFont::SmallCaps ? QFont::MixedCase : QFont::SmallCaps);
        ed->mergeCharFormat(f, tr("Small Caps"));
    }, true);
    mk("fmt.allCaps", tr("All Caps"), "case-upper", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [ed] {
        QTextCharFormat f;
        f.setFontCapitalization(ed->currentCharFormat().fontCapitalization() == QFont::AllUppercase ? QFont::MixedCase : QFont::AllUppercase);
        ed->mergeCharFormat(f, tr("All Caps"));
    }, true);
    const QString caseNames[] = {tr("Sentence case."), tr("lowercase"), tr("UPPERCASE"), tr("Capitalize Each Word"), tr("tOGGLE cASE")};
    for (int i = 0; i < 5; ++i) mk(QStringLiteral("case.%1").arg(i), caseNames[i], "", QKeySequence(), [ed, i] { ed->changeCase(i); });
    mk("case.cycle", tr("Change Case"), "case-sensitive", QKeySequence(Qt::SHIFT | Qt::Key_F3), [ed] {
        static int next = 0;
        ed->changeCase(next);
        next = (next + 1) % 3;
    });
    // Tracking presets, as percentages of normal letter spacing.
    struct Spacing { const char *id; QString name; double pct; };
    const Spacing spacing[] = {{"Very Tight", tr("Very Tight"), 75}, {"Tight", tr("Tight"), 87.5}, {"Normal", tr("Normal"), 100}, {"Loose", tr("Loose"), 112.5}, {"Very Loose", tr("Very Loose"), 125}};
    for (const auto &s : spacing) {
        const double v = s.pct;
        mk(QStringLiteral("spacing.%1").arg(QString::fromLatin1(s.id)), s.name, "", QKeySequence(), [ed, v] {
            QTextCharFormat f;
            f.setProperty(tp::Tracking, v);
            ed->mergeCharFormat(f, tr("Character Spacing"));
        });
    }
    mk("fmt.spacingDialog", tr("More Spacing…"), "", QKeySequence(), [this] { characterSpacingDialog(this, m_ed); });
    mk("fmt.fontDialog", tr("Font…"), "type", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] { fontDialog(this, m_ed); });

    // ---------------- Paragraph ----------------
    mk("para.bullets", tr("Bullets"), "list", QKeySequence(), [ed] {
        ed->setList(ed->currentBlockFormat().isValid() && ed->isEditingText() && ed->cursor().block().textList() &&
                            isBulletList(ed->cursor().block().textList()->format().style()) ? 0 : 1);
    }, true);
    mk("para.numbers", tr("Numbering"), "list-ordered", QKeySequence(), [ed] {
        ed->setList(ed->isEditingText() && ed->cursor().block().textList() && !isBulletList(ed->cursor().block().textList()->format().style()) ? 0 : 2, 1);
    }, true);
    for (const auto &[id, ch] : {std::pair{"bullet.disc", "•"}, {"bullet.circle", "◦"}, {"bullet.square", "▪"}, {"bullet.diamond", "❖"},
                                 {"bullet.arrow", "➢"}, {"bullet.check", "✔"}, {"bullet.star", "★"}, {"bullet.dash", "–"}}) {
        const QString c = QString::fromUtf8(ch);
        mk(id, c, "", QKeySequence(), [ed, c] { ed->setList(1, 0, c); });
    }
    const char *numNames[] = {"1. 2. 3.", "a. b. c.", "A. B. C.", "i. ii. iii.", "I. II. III.", "1) 2) 3)", "(1) (2) (3)"};
    const int numFmt[] = {1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 7; ++i) {
        const int f = numFmt[i];
        mk(QStringLiteral("number.%1").arg(i), QString::fromLatin1(numNames[i]), "", QKeySequence(), [ed, f] { ed->setList(2, f); });
    }
    mk("para.listNone", tr("None"), "", QKeySequence(), [ed] { ed->setList(0); });
    mk("para.bulletsDialog", tr("Bullets and Numbering…"), "list-plus", QKeySequence(), [this] { bulletsDialog(this, m_ed, false); });
    // No keys: Ctrl+M is Master Page, and the other program gives these none.
    mk("para.indentDec", tr("Decrease Indent"), "indent-decrease", QKeySequence(), [ed] { ed->changeIndent(-1); });
    mk("para.indentInc", tr("Increase Indent"), "indent-increase", QKeySequence(), [ed] { ed->changeIndent(1); });
    mk("para.ltr", tr("Left-to-Right Text Direction"), "pilcrow-right", QKeySequence(), [ed] { ed->setDirection(Qt::LeftToRight); }, true);
    mk("para.rtl", tr("Right-to-Left Text Direction"), "pilcrow-left", QKeySequence(), [ed] { ed->setDirection(Qt::RightToLeft); }, true);
    mk("para.special", tr("Special Characters"), "pilcrow", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y), [ed] {
        ed->setView([](ViewOptions &v) { v.special = !v.special; });
    }, true);
    mk("para.left", tr("Align Left"), "align-left", QKeySequence(Qt::CTRL | Qt::Key_L), [ed] { ed->setAlignment(Qt::AlignLeft); }, true);
    mk("para.center", tr("Center"), "align-center", QKeySequence(Qt::CTRL | Qt::Key_E), [ed] { ed->setAlignment(Qt::AlignHCenter); }, true);
    mk("para.right", tr("Align Right"), "align-right", QKeySequence(Qt::CTRL | Qt::Key_R), [ed] { ed->setAlignment(Qt::AlignRight); }, true);
    mk("para.justify", tr("Justify"), "align-justify", QKeySequence(Qt::CTRL | Qt::Key_J), [ed] { ed->setAlignment(Qt::AlignJustify); }, true);
    mk("para.distribute", tr("Distribute"), "align-horizontal-space-between", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J), [ed] {
        QTextBlockFormat f;
        f.setAlignment(Qt::AlignJustify);
        f.setProperty(tp::Distribute, true);
        ed->mergeBlockFormat(f, tr("Distribute"));
    }, true);
    const double spacings[] = {1.0, 1.15, 1.5, 2.0, 2.5, 3.0};
    for (double s : spacings)
        mk(QStringLiteral("ls.%1").arg(s), QString::number(s), "", QKeySequence(), [ed, s] { ed->setLineSpacing(QTextBlockFormat::ProportionalHeight, s * 100); });
    mk("para.spaceBefore0", tr("0 pt Before"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(0, -1); });
    mk("para.spaceBefore6", tr("6 pt Before"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(6, -1); });
    mk("para.spaceBefore12", tr("12 pt Before"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(12, -1); });
    mk("para.spaceAfter0", tr("0 pt After"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(-1, 0); });
    mk("para.spaceAfter6", tr("6 pt After"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(-1, 6); });
    mk("para.spaceAfter12", tr("12 pt After"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(-1, 12); });
    mk("para.dialog", tr("Paragraph…"), "pilcrow", QKeySequence(), [this] { paragraphDialog(this, m_ed, 0); });
    mk("para.tabs", tr("Tabs…"), "", QKeySequence(), [this] { paragraphDialog(this, m_ed, 2); });

    // ---------------- Styles ----------------
    mk("style.new", tr("New Style…"), "plus", QKeySequence(), [this] { styleDialog(this, m_ed); });
    mk("style.modify", tr("Modify Style…"), "pencil", QKeySequence(), [this] { styleDialog(this, m_ed, m_ed->currentStyleName().isEmpty() ? QStringLiteral("Normal") : m_ed->currentStyleName()); });
    mk("style.import", tr("Import Styles…"), "import", QKeySequence(), [this] {
        const QString p = QFileDialog::getOpenFileName(this, tr("Import Styles"), QString(), tr("Publications (*.jpub *.pub)"));
        if (p.isEmpty()) return;
        QString err;
        auto src = loadAnyPublication(p, &err);
        if (!src) { QMessageBox::warning(this, tr("Import Styles"), err); return; }
        m_ed->change(tr("Import Styles"), [&] {
            for (const auto &s : src->styles) {
                bool replaced = false;
                for (auto &mine : m_ed->doc()->styles)
                    if (mine.name == s.name) { mine = s; replaced = true; }
                if (!replaced) m_ed->doc()->styles << s;
            }
        });
    });
    mk("style.byExample", tr("New Style by Example"), "", QKeySequence(), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Style"), tr("Name the style based on the selected text:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        m_ed->change(tr("New Style"), [&] {
            TextStyle s;
            s.name = name.trimmed();
            s.chr = m_ed->currentCharFormat();
            s.blk = m_ed->currentBlockFormat();
            s.blk.setProperty(tp::StyleName, s.name);
            s.basedOn = QStringLiteral("Normal");
            s.next = s.name;
            m_ed->doc()->styles << s;
        });
        m_ed->applyStyle(name.trimmed());
    });

    // ---------------- Objects / Insert ----------------
    mk("ins.textbox", tr("Draw Text Box"), "text-cursor-input", QKeySequence(), [ed] { ed->setTool(Tool::Text); });
    mk("ins.tableDialog", tr("Insert Table…"), "table", QKeySequence(), [this] { insertTableDialog(this, m_ed); });
    mk("ins.drawTable", tr("Draw Table"), "pencil-ruler", QKeySequence(), [ed] { ed->setTool(Tool::Table); });
    mk("ins.picture", tr("Pictures"), "image", QKeySequence(), [this] { insertPictureFromFile(); });
    mk("ins.onlinePicture", tr("Online Pictures"), "globe", QKeySequence(), [this] { showTaskPane("online"); });
    mk("ins.placeholder", tr("Picture Placeholder"), "image-plus", QKeySequence(), [ed] {
        auto pic = std::make_shared<PictureItem>();
        const QSizeF ps = ed->doc()->pageSize();
        pic->rect = QRectF(ps.width() / 2 - 108, ps.height() / 2 - 72, 216, 144);
        pic->imgRect = QRectF(0, 0, 216, 144);
        ed->addItem(pic);
    });
    mk("ins.textart", tr("Text Art"), "type", QKeySequence(), [ed] { ed->setTool(Tool::TextArt); });
    mk("ins.bizInfo", tr("Edit Business Information…"), "contact", QKeySequence(), [this] { businessInfoDialog(this, m_ed); });
    mk("ins.file", tr("Insert File"), "file-input", QKeySequence(), [this] { insertFileDialog(this, m_ed); });
    mk("ins.symbol", tr("Symbol"), "omega", QKeySequence(), [this] { symbolDialog(this, m_ed); });
    mk("ins.barcode", tr("Barcode"), "barcode", QKeySequence(), [this] { barcodeDialog(this, m_ed); });
    mk("ins.icons", tr("Icons"), "smile-plus", QKeySequence(), [this] { insertIcons(m_ed, pickIcons(this)); });
    mk("ins.toc", tr("Table of Contents"), "table-of-contents", QKeySequence(), [this] { insertTableOfContents(m_ed); });
    mk("ins.footnote", tr("Footnote"), "superscript", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F), [this] {
        if (m_ed->isEditingText()) noteDialog(this, m_ed, false);
        else statusBar()->showMessage(tr("Click in text where the footnote's number goes, then choose Footnote."), 6000);
    });
    mk("ins.endnote", tr("Endnote"), "notebook-text", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_D), [this] {
        if (m_ed->isEditingText()) noteDialog(this, m_ed, true);
        else statusBar()->showMessage(tr("Click in text where the endnote's number goes, then choose Endnote."), 6000);
    });
    mk("ins.updateToc", tr("Update Table"), "refresh-cw", QKeySequence(), [this] {
        if (!updateTablesOfContents(m_ed))
            statusBar()->showMessage(tr("This publication has no table of contents yet: Insert > References > Table of Contents adds one."), 6000);
    });
    mk("ins.datetime", tr("Date & Time"), "calendar-clock", QKeySequence(), [this] { dateTimeDialog(this, m_ed); });
    mk("ins.object", tr("Object"), "paperclip", QKeySequence(), [this] {
        // Embedded objects become pictures of their content where JeffPub can render it.
        insertPictureFromFile();
    });
    mk("ins.link", tr("Link"), "link", QKeySequence(Qt::CTRL | Qt::Key_K), [this] { hyperlinkDialog(this, m_ed); });
    mk("ins.bookmark", tr("Bookmark"), "bookmark", QKeySequence(), [this] { bookmarkDialog(this, m_ed); });
    mk("ins.header", tr("Header"), "panel-top", QKeySequence(), [this] {
        const Page *pg = m_ed->doc()->pages[m_ed->currentPage()].get();
        m_ed->setMasterView(pg->masterId.isEmpty() ? m_ed->doc()->masters.first()->id : pg->masterId);
        // Put the caret in (or create) a header box at the top margin.
        const QRectF content = QRectF(QPointF(0, 0), m_ed->doc()->pageSize()).marginsRemoved(m_ed->doc()->setup.margins);
        for (const auto &it : m_ed->surfaceItems())
            if (it->type() == ItemType::Text && it->rect.top() <= content.top() + 2 && it->rect.bottom() < content.top() + 60) { m_ed->beginTextEdit(it->id); return; }
        auto t = m_ed->newTextBox(QRectF(content.left(), std::max(9.0, content.top() - 30), content.width(), 24));
        t->name = QStringLiteral("Header");
        m_ed->addItem(t);
        m_ed->beginTextEdit(t->id);
    });
    mk("ins.footer", tr("Footer"), "panel-bottom", QKeySequence(), [this] {
        const Page *pg = m_ed->doc()->pages[m_ed->currentPage()].get();
        m_ed->setMasterView(pg->masterId.isEmpty() ? m_ed->doc()->masters.first()->id : pg->masterId);
        const QSizeF ps = m_ed->doc()->pageSize();
        const QRectF content = QRectF(QPointF(0, 0), ps).marginsRemoved(m_ed->doc()->setup.margins);
        for (const auto &it : m_ed->surfaceItems())
            if (it->type() == ItemType::Text && it->rect.top() >= content.bottom() - 2) { m_ed->beginTextEdit(it->id); return; }
        auto t = m_ed->newTextBox(QRectF(content.left(), std::min(ps.height() - 33, content.bottom() + 6), content.width(), 24));
        t->name = QStringLiteral("Footer");
        m_ed->addItem(t);
        m_ed->beginTextEdit(t->id);
    });
    // Today's date and the time as fields that stay current, as the other
    // program's Insert Date and Insert Time (Alt+Shift+D and Alt+Shift+T).
    mk("ins.date", tr("Insert Date"), "calendar", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_D), [this] {
        if (m_ed->isEditingText()) m_ed->insertField(QStringLiteral("date"));
        else statusBar()->showMessage(tr("Click in text where the date goes, then choose Insert Date."), 6000);
    });
    mk("ins.time", tr("Insert Time"), "clock", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_T), [this] {
        if (m_ed->isEditingText()) m_ed->insertField(QStringLiteral("time"));
        else statusBar()->showMessage(tr("Click in text where the time goes, then choose Insert Time."), 6000);
    });
    // The master page's header, then its footer, then the header again.
    mk("mp.showHeaderFooter", tr("Show Header/Footer"), "rows-3", QKeySequence(), [this] {
        const Item *in = m_ed->isEditingText() && !m_ed->masterView().isEmpty() ? m_ed->doc()->item(m_ed->textTarget().itemId) : nullptr;
        const QRectF content = QRectF(QPointF(0, 0), m_ed->doc()->pageSize()).marginsRemoved(m_ed->doc()->setup.margins);
        const bool inHeader = in && (in->name == QLatin1String("Header") || in->rect.bottom() < content.top() + 60);
        act(inHeader ? QStringLiteral("ins.footer") : QStringLiteral("ins.header"))->trigger();
    });
    mk("ins.pageNumber", tr("Insert Page Number"), "hash", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_P), [this] {
        if (m_ed->isEditingText()) m_ed->insertField(QStringLiteral("page"));
        else pageNumberDialog(this, m_ed);
    });
    mk("ins.pageCount", tr("Insert Page Count"), "", QKeySequence(), [ed] { ed->insertField(QStringLiteral("pages")); });
    mk("ins.pageNumberFormat", tr("Format Page Numbers…"), "", QKeySequence(), [this] { pageNumberFormatDialog(this, m_ed); });
    for (const QString &k : BusinessInfo::keys()) {
        mk("biz." + k, BusinessInfo::label(k), "", QKeySequence(), [this, k] {
            if (m_ed->isEditingText()) { m_ed->insertField("biz:" + k); return; }
            const QSizeF ps = m_ed->doc()->pageSize();
            auto t = std::static_pointer_cast<TextItem>(m_ed->newTextBox(QRectF(ps.width() / 2 - 108, ps.height() / 2 - 18, 216, k == "address" ? 54 : 30)));
            QTextCursor c(m_ed->doc()->storyDoc(t->storyId));
            QTextCharFormat f;
            f.setProperty(tp::Field, "biz:" + k);
            c.insertText(QString(QChar::ObjectReplacementCharacter), f);
            t->autofit = TextItem::GrowBox;
            m_ed->addItem(t);
        });
    }
    mk("biz.logo", tr("Logo"), "", QKeySequence(), [this] {
        const QString id = m_ed->doc()->business().logoImageId;
        if (id.isEmpty()) { businessInfoDialog(this, m_ed); return; }
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = id;
        pic->rect = QRectF(72, 72, 108, 108);
        pic->fitImage(m_ed->doc()->imageSize(id), false);
        m_ed->addItem(pic);
    });

    // ---------------- Pages ----------------
    mk("page.insert", tr("Insert Blank Page"), "file-plus-2", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [ed] { ed->insertPages(ed->currentPage(), 1, false, false); });
    mk("page.insertDup", tr("Insert Duplicate Page"), "copy", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U), [ed] { ed->insertPages(ed->currentPage(), 1, true, false); });
    mk("page.insertDialog", tr("Insert Page…"), "files", QKeySequence(), [this] { insertPageDialog(this, m_ed); });
    mk("page.delete", tr("Delete Page"), "file-minus", QKeySequence(), [this] {
        if (QMessageBox::question(this, tr("Delete Page"), tr("Delete page %1?").arg(m_ed->currentPage() + 1)) == QMessageBox::Yes)
            m_ed->deletePage(m_ed->currentPage());
    });
    mk("page.moveUp", tr("Move Page Up"), "arrow-up", QKeySequence(), [ed] { ed->movePage(ed->currentPage(), ed->currentPage() - 1); });
    mk("page.moveDown", tr("Move Page Down"), "arrow-down", QKeySequence(), [ed] { ed->movePage(ed->currentPage(), ed->currentPage() + 1); });
    mk("page.move", tr("Move Page…"), "arrow-up-down", QKeySequence(), [this] {
        bool ok = false;
        const int to = QInputDialog::getInt(this, tr("Move Page"), tr("Move this page to position:"), m_ed->currentPage() + 1, 1,
                                            m_ed->doc()->pages.size(), 1, &ok);
        if (ok) m_ed->movePage(m_ed->currentPage(), to - 1);
    });
    mk("page.rename", tr("Rename Page…"), "pencil-line", QKeySequence(), [this] {
        bool ok = false;
        const QString t = QInputDialog::getText(this, tr("Rename Page"), tr("Page title:"), QLineEdit::Normal,
                                                m_ed->doc()->pages[m_ed->currentPage()]->title, &ok);
        if (ok) m_ed->renamePage(m_ed->currentPage(), t);
    });
    mk("page.next", tr("Next Page"), "chevron-right", QKeySequence(Qt::CTRL | Qt::Key_PageDown), [ed] { ed->setCurrentPage(ed->currentPage() + 1); });
    mk("page.prev", tr("Previous Page"), "chevron-left", QKeySequence(Qt::CTRL | Qt::Key_PageUp), [ed] { ed->setCurrentPage(ed->currentPage() - 1); });
    mk("page.goto", tr("Go to Page…"), "", QKeySequence(Qt::Key_F5), [this] {
        bool ok = false;
        const int p = QInputDialog::getInt(this, tr("Go to Page"), tr("Page:"), m_ed->currentPage() + 1, 1, m_ed->doc()->pages.size(), 1, &ok);
        if (ok) m_ed->setCurrentPage(p - 1);
    });

    // ---------------- Arrange ----------------
    mk("arr.front", tr("Bring to Front"), "bring-to-front", QKeySequence(Qt::ALT | Qt::Key_F6), [ed] { ed->arrange(Editor::Order::Front); });
    mk("arr.forward", tr("Bring Forward"), "arrow-up-from-line", QKeySequence(), [ed] { ed->arrange(Editor::Order::Forward); });
    mk("arr.backward", tr("Send Backward"), "arrow-down-to-line", QKeySequence(), [ed] { ed->arrange(Editor::Order::Backward); });
    mk("arr.back", tr("Send to Back"), "send-to-back", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F6), [ed] { ed->arrange(Editor::Order::Back); });
    mk("arr.group", tr("Group"), "group", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), [ed] {
        if (ed->selectionKind() == "group") ed->ungroupSelection(); else ed->groupSelection();
    });
    mk("arr.ungroup", tr("Ungroup"), "ungroup", QKeySequence(), [ed] { ed->ungroupSelection(); });
    mk("arr.regroup", tr("Regroup"), "group", QKeySequence(), [ed] { ed->regroup(); });
    mk("arr.relMargins", tr("Relative to Margin Guides"), "", QKeySequence(), [] {}, true);
    auto toMargins = [this] { return act("arr.relMargins")->isChecked(); };
    mk("arr.alignLeft", tr("Align Left"), "align-start-vertical", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Left, toMargins()); });
    mk("arr.alignCenter", tr("Align Center"), "align-center-vertical", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Center, toMargins()); });
    mk("arr.alignRight", tr("Align Right"), "align-end-vertical", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Right, toMargins()); });
    mk("arr.alignTop", tr("Align Top"), "align-start-horizontal", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Top, toMargins()); });
    mk("arr.alignMiddle", tr("Align Middle"), "align-center-horizontal", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Middle, toMargins()); });
    mk("arr.alignBottom", tr("Align Bottom"), "align-end-horizontal", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Bottom, toMargins()); });
    mk("arr.distH", tr("Distribute Horizontally"), "align-horizontal-distribute-center", QKeySequence(), [ed, toMargins] { ed->distribute(true, toMargins()); });
    mk("arr.distV", tr("Distribute Vertically"), "align-vertical-distribute-center", QKeySequence(), [ed, toMargins] { ed->distribute(false, toMargins()); });
    mk("arr.rotR", tr("Rotate Right 90°"), "rotate-cw", QKeySequence(), [ed] { ed->rotateSelection(90); });
    mk("arr.rotL", tr("Rotate Left 90°"), "rotate-ccw", QKeySequence(), [ed] { ed->rotateSelection(-90); });
    mk("arr.flipH", tr("Flip Horizontal"), "flip-horizontal-2", QKeySequence(), [ed] { ed->flipSelection(true); });
    mk("arr.flipV", tr("Flip Vertical"), "flip-vertical-2", QKeySequence(), [ed] { ed->flipSelection(false); });
    mk("arr.freeRotate", tr("More Rotation Options…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 1); });
    // An object set in text that the text selection holds comes back onto the page.
    auto setWrap = [ed](Wrap::Mode m) {
        if (ed->moveOutOfText(m)) return;
        ed->forEachSelected(tr("Wrap Text"), [m](Item *it) { it->wrap.mode = m; });
    };
    mk("wrap.none", tr("None"), "", QKeySequence(), [setWrap] { setWrap(Wrap::None); }, true);
    mk("wrap.square", tr("Square"), "", QKeySequence(), [setWrap] { setWrap(Wrap::Square); }, true);
    mk("wrap.tight", tr("Tight"), "", QKeySequence(), [setWrap] { setWrap(Wrap::Tight); }, true);
    mk("wrap.through", tr("Through"), "", QKeySequence(), [setWrap] { setWrap(Wrap::Through); }, true);
    mk("wrap.topBottom", tr("Top and Bottom"), "", QKeySequence(), [setWrap] { setWrap(Wrap::TopBottom); }, true);
    mk("wrap.inline", tr("In Line with Text"), "", QKeySequence(), [this, ed] {
        if (ed->selectionIsInlineObject()) return;
        if (!ed->moveIntoText())
            statusBar()->showMessage(tr("Move the object over a text box first; it goes into the text where its top left corner is."), 6000);
    }, true);
    mk("wrap.edit", tr("Edit Wrap Points"), "wrap-points", QKeySequence(), [this] {
        Item *it = m_ed->single();
        if (!it) return;
        if (m_ed->wrapItem == it->id) { m_ed->setWrapItem(QString()); return; }
        // Text wraps tightly around the points; start from the object's outline.
        m_ed->change(tr("Edit Wrap Points"), [&] {
            if (it->wrap.mode != Wrap::Tight && it->wrap.mode != Wrap::Through) it->wrap.mode = Wrap::Tight;
            if (it->wrap.points.size() < 3) it->wrap.points = Renderer::defaultWrapPolygon(*m_ed->doc(), *it);
        });
        m_ed->setWrapItem(it->id);
        statusBar()->showMessage(tr("Drag a point to change how text wraps. Drag an edge to add a point; Ctrl+click a point to delete it. Press Esc when done."), 8000);
    });
    mk("wrap.more", tr("More Layout Options…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 2); });
    mk("obj.lock", tr("Lock Position and Size"), "lock", QKeySequence(), [ed] {
        const bool lock = ed->single() ? !ed->single()->locked : true;
        ed->forEachSelected(tr("Lock"), [lock](Item *it) { it->locked = lock; });
    }, true);
    mk("obj.format", tr("Format Object…"), "settings-2", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 0); });
    mk("obj.sizePos", tr("Size and Position…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 1); });
    mk("obj.altText", tr("Alt Text…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 5); });
    mk("obj.saveBlock", tr("Save as Building Block…"), "package-plus", QKeySequence(), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save as Building Block"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        m_ed->copy();
        const QByteArray data = QApplication::clipboard()->mimeData()->data("application/x-jeffpub-items");
        const QString dir = userBlocksDir();
        QDir().mkpath(dir);
        // The name becomes a file name: characters a file name can't hold
        // (or that would leave the folder) become dashes.
        QString file = name.trimmed();
        file.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]|^\\.+")), QStringLiteral("-"));
        QSaveFile f(QDir(dir).filePath(file + ".json"));
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
            QMessageBox::warning(this, tr("Save as Building Block"), tr("JeffPub couldn't save the building block: %1").arg(f.errorString()));
            return;
        }
        statusBar()->showMessage(tr("Saved \"%1\" to My Building Blocks.").arg(name.trimmed()), 4000);
    });

    // ---------------- Page Design ----------------
    mk("pd.changeTemplate", tr("Change Template"), "layout-template", QKeySequence(), [this] { showBackstage("new"); });
    mk("pd.pageSetup", tr("Page Setup…"), "file-cog", QKeySequence(), [this] { pageSetupDialog(this, m_ed); });
    mk("pd.guidesDialog", tr("Grid and Baseline Guides…"), "grid-3x3", QKeySequence(), [this] { gridGuidesDialog(this, m_ed); });
    // A new guide goes to the middle of the page, or half an inch on from
    // the last free place, so adding several doesn't stack them.
    mk("pd.addH", tr("Add Horizontal Ruler Guide"), "", QKeySequence(), [ed] {
        ed->change(tr("Add Guide"), [ed] {
            const double h = ed->doc()->pageSize().height();
            auto &g = ed->surface()->guides.h;
            g << RulerGuides::freeSpot(g, h / 2, 36, h);
        });
    });
    mk("pd.addV", tr("Add Vertical Ruler Guide"), "", QKeySequence(), [ed] {
        ed->change(tr("Add Guide"), [ed] {
            const double w = ed->doc()->pageSize().width();
            auto &g = ed->surface()->guides.v;
            g << RulerGuides::freeSpot(g, w / 2, 36, w);
        });
    });
    mk("pd.rulerGuides", tr("Ruler Guides…"), "", QKeySequence(), [this] { rulerGuidesDialog(this, m_ed); });
    mk("pd.clearGuides", tr("Clear All Ruler Guides"), "", QKeySequence(), [ed] {
        ed->change(tr("Clear Guides"), [ed] { ed->surface()->guides = RulerGuides(); });
    });
    mk("pd.alignGuides", tr("Align to Guides"), "magnet", QKeySequence(), [ed] { ed->setView([](ViewOptions &v) { v.snapGuides = !v.snapGuides; }); }, true);
    mk("pd.alignObjects", tr("Align to Objects"), "magnet", QKeySequence(), [ed] { ed->setView([](ViewOptions &v) { v.snapObjects = !v.snapObjects; }); }, true);
    mk("pd.portrait", tr("Portrait"), "rectangle-vertical", QKeySequence(), [ed] {
        QSizeF s = ed->doc()->setup.size;
        if (s.width() <= s.height()) return;
        ed->change(tr("Orientation"), [ed, s] { ed->doc()->setup.size = s.transposed(); ed->doc()->setup.sheet = ed->doc()->setup.sheet.transposed(); });
    });
    mk("pd.landscape", tr("Landscape"), "rectangle-horizontal", QKeySequence(), [ed] {
        QSizeF s = ed->doc()->setup.size;
        if (s.width() >= s.height()) return;
        ed->change(tr("Orientation"), [ed, s] { ed->doc()->setup.size = s.transposed(); ed->doc()->setup.sheet = ed->doc()->setup.sheet.transposed(); });
    });
    struct MarginPreset { const char *id; QString name; double pts; };
    const MarginPreset margins[] = {{"None", tr("None"), 0}, {"Narrow", tr("Narrow"), 18}, {"Moderate", tr("Moderate"), 36}, {"Wide", tr("Wide"), 54}, {"Extra Wide", tr("Extra Wide"), 72}};
    for (const auto &m : margins) {
        const double v = m.pts;
        mk(QStringLiteral("margins.%1").arg(QString::fromLatin1(m.id)), tr("%1 (%2)").arg(m.name, Settings::get().format(v)), "",
           QKeySequence(), [ed, v] { ed->change(tr("Margins"), [ed, v] { ed->doc()->setup.margins = QMarginsF(v, v, v, v); }); });
    }
    // Margins on the Page Design tab are the margin guides (Publisher's word for them).
    mk("pd.customMargins", tr("Custom Margins…"), "", QKeySequence(), [this] { gridGuidesDialog(this, m_ed, 0); });
    mk("pd.newPageSize", tr("Create New Page Size…"), "file-plus", QKeySequence(), [this] { createPageSizeDialog(this, m_ed); });
    mk("pd.customSizes", tr("Edit Custom Page Sizes…"), "", QKeySequence(), [this] { customPageSizesDialog(this, m_ed); });
    mk("pd.newColorScheme", tr("Create New Color Scheme…"), "palette", QKeySequence(), [this] { colorSchemeDialog(this, m_ed); });
    mk("pd.newFontScheme", tr("Create New Font Scheme…"), "type", QKeySequence(), [this] { fontSchemeDialog(this, m_ed); });
    mk("pd.updateFonts", tr("Update Font Scheme"), "refresh-cw", QKeySequence(), [ed] {
        // Re-applies scheme fonts to text whose fonts were set by styles.
        ed->change(tr("Update Font Scheme"), [ed] {
            for (auto &st : ed->doc()->styles) st.chr.clearProperty(QTextFormat::FontFamilies);
        });
    });
    mk("pd.bgNone", tr("No Background"), "ban", QKeySequence(), [ed] {
        ed->change(tr("Background"), [ed] { ed->surface()->background = Fill(); });
    });
    mk("pd.bgMore", tr("More Backgrounds…"), "image", QKeySequence(), [this] {
        Fill f = m_ed->surface()->background;
        if (fillEffectsDialog(this, m_ed, f, tr("Format Background")))
            m_ed->change(tr("Background"), [&] { m_ed->surface()->background = f; });
    });
    mk("pd.bgAllPages", tr("Apply Background to All Pages"), "", QKeySequence(), [ed] {
        const Fill f = ed->surface()->background;
        ed->change(tr("Background"), [ed, f] { for (auto &p : ed->doc()->pages) p->background = f; });
    });
    mk("pd.bgImage", tr("Apply Image as Background"), "image", QKeySequence(), [this] {
        Item *it = m_ed->single();
        auto *pic = dynamic_cast<PictureItem *>(it);
        if (!pic || pic->imageId.isEmpty()) {
            QMessageBox::information(this, tr("Apply Image as Background"), tr("Select a picture first."));
            return;
        }
        m_ed->change(tr("Background"), [&] {
            Fill f;
            f.type = Fill::Picture;
            f.imageId = pic->imageId;
            m_ed->surface()->background = f;
        });
        m_ed->deleteSelection();
    });

    // ---------------- Master pages ----------------
    mk("view.master", tr("Master Page"), "layout-panel-top", QKeySequence(Qt::CTRL | Qt::Key_M), [ed] {
        if (!ed->masterView().isEmpty()) { ed->setMasterView(QString()); return; }
        const Page *pg = ed->doc()->pages[ed->currentPage()].get();
        ed->setMasterView(pg->masterId.isEmpty() ? ed->doc()->masters.first()->id : pg->masterId);
    }, true);
    mk("view.normal", tr("Normal"), "file", QKeySequence(), [ed] { ed->setMasterView(QString()); }, true);
    mk("mp.add", tr("Add Master Page"), "file-plus", QKeySequence(), [ed] { ed->addMaster(false); });
    mk("mp.dup", tr("Duplicate"), "copy", QKeySequence(), [ed] { ed->addMaster(true); });
    mk("mp.rename", tr("Rename"), "pencil-line", QKeySequence(), [this] {
        MasterPage *m = m_ed->doc()->master(m_ed->masterView());
        if (!m) return;
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Rename Master Page"), tr("Description:"), QLineEdit::Normal, m->name, &ok);
        if (ok && !n.trimmed().isEmpty()) m_ed->change(tr("Rename Master Page"), [&] { m->name = n.trimmed(); });
    });
    mk("mp.delete", tr("Delete"), "trash-2", QKeySequence(), [ed] { ed->deleteMaster(ed->masterView()); });
    mk("mp.twoPage", tr("Two-Page Master"), "book-open", QKeySequence(), [ed] {
        MasterPage *m = ed->doc()->master(ed->masterView());
        if (m) ed->change(tr("Two-Page Master"), [m] { m->twoPage = !m->twoPage; });
        ed->notifyLive();
    }, true);
    mk("mp.applyAll", tr("Apply to All Pages"), "", QKeySequence(), [ed] {
        const QString id = ed->masterView().isEmpty() ? ed->doc()->masters.first()->id : ed->masterView();
        ed->change(tr("Apply Master Page"), [ed, id] { for (auto &p : ed->doc()->pages) p->masterId = id; });
    });
    mk("mp.applyCurrent", tr("Apply to Current Page"), "", QKeySequence(), [ed] {
        const QString id = ed->masterView().isEmpty() ? ed->doc()->masters.first()->id : ed->masterView();
        ed->applyMaster(ed->currentPage(), id);
    });
    mk("mp.none", tr("None"), "", QKeySequence(), [ed] { ed->applyMaster(ed->currentPage(), QString()); });
    mk("mp.close", tr("Close Master Page"), "x", QKeySequence(), [ed] { ed->setMasterView(QString()); });

    // ---------------- Mailings ----------------
    mk("mm.wizard", tr("Step-by-Step Mail Merge Wizard"), "wand-sparkles", QKeySequence(), [this] { showTaskPane("mailmerge"); });
    mk("mm.typeNew", tr("Type a New List…"), "user-plus", QKeySequence(), [this] { recipientsDialog(this, m_ed, true); });
    auto useList = [this](const QString &title, const QString &filter) {
        const QString p = QFileDialog::getOpenFileName(this, title, QString(), filter + QStringLiteral(";;") + tr("All Files (*)"));
        if (p.isEmpty()) return;
        MergeSource src;
        QString err;
        if (!loadMergeSource(p, &src, &err)) { QMessageBox::warning(this, title, err); return; }
        m_ed->change(tr("Select Recipients"), [&] { m_ed->doc()->merge = src; });
        recipientsDialog(this, m_ed, false);
    };
    mk("mm.existing", tr("Use an Existing List…"), "file-spreadsheet", QKeySequence(), [useList] {
        useList(tr("Select Data Source"), tr("Data Sources (*.csv *.txt *.tsv *.xlsx *.vcf)"));
    });
    // The other program reads one mail program's contacts; JeffPub reads the
    // contacts any address book can save, as vCard files.
    mk("mm.contacts", tr("Select from Contacts…"), "contact", QKeySequence(), [useList] {
        useList(tr("Select from Contacts"), tr("Contacts (*.vcf *.vcard)"));
    });
    mk("mm.editList", tr("Edit Recipient List"), "user-pen", QKeySequence(), [this] { recipientsDialog(this, m_ed, false); });
    mk("mm.addressBlock", tr("Address Block"), "mail-open", QKeySequence(), [this] { mergeFieldDialog(this, m_ed, 0); });
    mk("mm.greeting", tr("Greeting Line"), "hand", QKeySequence(), [this] { mergeFieldDialog(this, m_ed, 1); });
    mk("mm.pictureField", tr("Picture Field"), "image", QKeySequence(), [this] {
        const QStringList fields = m_ed->doc()->merge.fields;
        if (fields.isEmpty()) { QMessageBox::information(this, tr("Picture Field"), tr("Select a recipient list first.")); return; }
        bool ok = false;
        const QString f = QInputDialog::getItem(this, tr("Picture Field"), tr("Field that holds picture file names:"), fields, 0, false, &ok);
        if (!ok) return;
        m_ed->change(tr("Picture Field"), [&] { m_ed->doc()->merge.pictureField = f; });
        auto pic = std::make_shared<PictureItem>();
        pic->name = "merge:" + f;
        pic->rect = QRectF(72, 72, 144, 144);
        pic->imgRect = QRectF(0, 0, 144, 144);
        m_ed->addItem(pic);
    });
    mk("mm.preview", tr("Preview Results"), "eye", QKeySequence(), [ed] {
        if (ed->mergeRecord() >= 0) ed->setMergeRecord(-1);
        else if (!ed->doc()->merge.isEmpty()) ed->setMergeRecord(ed->doc()->merge.includedRows().value(0, 0));
    }, true);
    auto stepRecord = [ed](int how) {
        const QVector<int> rows = ed->doc()->merge.includedRows();
        if (rows.isEmpty()) return;
        int i = std::max(0, int(rows.indexOf(ed->mergeRecord())));
        // A catalog page shows a pageful of records, so it steps by a page.
        const int step = ed->doc()->catalog.isActive() ? ed->doc()->catalog.perPage() : 1;
        if (how == 0) i = 0;
        else if (how == 3) i = int(rows.size() - 1) / step * step;
        else i = std::clamp(i + (how == 1 ? -step : step), 0, int(rows.size()) - 1);
        ed->setMergeRecord(rows[i]);
    };
    mk("mm.first", tr("First Record"), "chevrons-left", QKeySequence(), [stepRecord] { stepRecord(0); });
    mk("mm.prev", tr("Previous Record"), "chevron-left", QKeySequence(), [stepRecord] { stepRecord(1); });
    mk("mm.next", tr("Next Record"), "chevron-right", QKeySequence(), [stepRecord] { stepRecord(2); });
    mk("mm.last", tr("Last Record"), "chevrons-right", QKeySequence(), [stepRecord] { stepRecord(3); });
    mk("mm.findRecipient", tr("Find Recipient"), "search", QKeySequence(), [this] {
        bool ok = false;
        const QString q = QInputDialog::getText(this, tr("Find Recipient"), tr("Find:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || q.isEmpty()) return;
        const MergeSource &m = m_ed->doc()->merge;
        for (int r : m.includedRows())
            for (const QString &v : m.rows[r])
                if (v.contains(q, Qt::CaseInsensitive)) { m_ed->setMergeRecord(r); return; }
        statusBar()->showMessage(tr("No recipient matches \"%1\".").arg(q), 4000);
    });
    mk("mm.mergeNew", tr("Merge to New Publication"), "files", QKeySequence(), [this] {
        if (m_ed->doc()->merge.isEmpty()) { QMessageBox::information(this, tr("Merge"), tr("Select a recipient list first.")); return; }
        if (!maybeSave()) return;
        newPublication(mergeToNewPublication(*m_ed->doc()));
    });
    mk("mm.mergePrint", tr("Merge to Printer…"), "printer", QKeySequence(), [this] {
        if (m_ed->doc()->merge.isEmpty()) { QMessageBox::information(this, tr("Merge"), tr("Select a recipient list first.")); return; }
        showBackstage("print");
    });
    mk("mm.mergePdf", tr("Merge to PDF…"), "file-text", QKeySequence(), [this] {
        if (m_ed->doc()->merge.isEmpty()) { QMessageBox::information(this, tr("Merge"), tr("Select a recipient list first.")); return; }
        exportPdf(QString(), true);
    });
    mk("mm.mergeEmail", tr("Merge to Email…"), "mail", QKeySequence(), [this] {
        mergeToEmailFiles(this, m_ed);
    });
    mk("mm.exportList", tr("Export Recipient List…"), "file-down", QKeySequence(), [this] {
        const QString p = askSavePath(this, tr("Export Recipient List"), QString(), tr("CSV (*.csv)"));
        if (p.isEmpty()) return;
        saveMergeCsv(m_ed->doc()->merge, p);
    });

    // ---------------- Review ----------------
    mk("rev.spelling", tr("Spelling"), "spell-check", QKeySequence(Qt::Key_F7), [this] { spellingDialog(this, m_ed); });
    mk("rev.checkAsType", tr("Check Spelling as You Type"), "spell-check-2", QKeySequence(), [ed] { ed->setView([](ViewOptions &v) { v.spelling = !v.spelling; }); }, true);
    mk("rev.thesaurus", tr("Thesaurus"), "book-a", QKeySequence(Qt::SHIFT | Qt::Key_F7), [this] { thesaurusDialog(this, m_ed); });
    mk("rev.research", tr("Research"), "book-open-text", QKeySequence(Qt::ALT | Qt::Key_F7), [this] { showTaskPane("research"); });
    mk("rev.translate", tr("Translate"), "languages", QKeySequence(), [this] {
        // The selected text, else the story being edited, else the selected box's text.
        QString text;
        if (m_ed->isEditingText()) {
            const QTextCursor c = m_ed->cursor();
            text = c.hasSelection() ? c.selectedText() : (m_ed->editDoc() ? m_ed->editDoc()->toPlainText() : QString());
        } else if (Item *it = m_ed->single()) {
            QString sid;
            if (auto *t = dynamic_cast<TextItem *>(it)) sid = t->storyId;
            else if (auto *sh = dynamic_cast<ShapeItem *>(it)) sid = sh->storyId;
            if (QTextDocument *d = m_ed->doc()->storyDoc(sid)) text = d->toPlainText();
        }
        text.replace(QChar::ParagraphSeparator, '\n').replace(QChar::LineSeparator, '\n').remove(QChar(0x00AD)).remove(QChar::ObjectReplacementCharacter);
        text = text.trimmed();
        if (text.isEmpty()) {
            QMessageBox::information(this, tr("Translate"), tr("Select the text you want to translate, then choose Translate."));
            return;
        }
        // The saved setting keeps the English name, so it survives a change of language.
        struct TargetLanguage { QString english, shown, code; };
        const QList<TargetLanguage> langs = {
            {"English", tr("English"), "en"}, {"Spanish", tr("Spanish"), "es"}, {"French", tr("French"), "fr"}, {"German", tr("German"), "de"},
            {"Italian", tr("Italian"), "it"}, {"Portuguese", tr("Portuguese"), "pt"}, {"Dutch", tr("Dutch"), "nl"}, {"Polish", tr("Polish"), "pl"},
            {"Russian", tr("Russian"), "ru"}, {"Ukrainian", tr("Ukrainian"), "uk"}, {"Arabic", tr("Arabic"), "ar"}, {"Hindi", tr("Hindi"), "hi"},
            {"Chinese (Simplified)", tr("Chinese (Simplified)"), "zh-Hans"}, {"Japanese", tr("Japanese"), "ja"}, {"Korean", tr("Korean"), "ko"},
            {"Vietnamese", tr("Vietnamese"), "vi"}, {"Turkish", tr("Turkish"), "tr"}, {"Greek", tr("Greek"), "el"}};
        QStringList names;
        int lastIndex = 0;
        const QString last = Settings::get().value(QStringLiteral("translate/target"), QStringLiteral("Spanish")).toString();
        for (const auto &l : langs) {
            if (l.english == last) lastIndex = int(names.size());
            names << l.shown;
        }
        bool ok = false;
        const QString choice = QInputDialog::getItem(this, tr("Translate"),
                                                     tr("Translate the selected text into:\n\n"
                                                        "Your web browser will open LibreTranslate, a free, open-source translation service, "
                                                        "with this text. Nothing is sent until you click OK."),
                                                     names, lastIndex, false, &ok);
        if (!ok) return;
        QString code = QStringLiteral("en");
        for (const auto &l : langs)
            if (l.shown == choice) {
                code = l.code;
                Settings::get().setValue(QStringLiteral("translate/target"), l.english);
            }
        if (text.size() > 5000) text = text.left(5000);   // the service's limit for one request
        QUrl url(QStringLiteral("https://libretranslate.com/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("source"), QStringLiteral("auto"));
        q.addQueryItem(QStringLiteral("target"), code);
        q.addQueryItem(QStringLiteral("q"), text);
        url.setQuery(q);
        QDesktopServices::openUrl(url);
    });
    mk("rev.language", tr("Set Proofing Language…"), "globe", QKeySequence(), [this] {
        // The languages with dictionaries, by name, starting at the text's own.
        const QVector<dict::Language> &langs = dict::languages();
        QStringList names;
        for (const dict::Language &l : langs) names << l.name;
        names << tr("Do not check spelling");
        const QTextCharFormat now = m_ed->currentCharFormat();
        int current = 0;
        for (int i = 0; i < langs.size(); ++i)
            if (langs[i].code == dict::match(now.stringProperty(tp::Language))) current = i;
        if (now.boolProperty(tp::NoProof)) current = int(names.size()) - 1;
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, tr("Language"), tr("Mark selected text as:"), names, current, false, &ok);
        if (!ok) return;
        const int i = int(names.indexOf(pick));
        QTextCharFormat f;
        if (i >= langs.size()) f.setProperty(tp::NoProof, true);
        else {
            f.setProperty(tp::Language, langs[i].code);
            f.setProperty(tp::NoProof, false);
        }
        m_ed->mergeCharFormat(f, tr("Language"));
    });
    mk("rev.designChecker", tr("Run Design Checker"), "shield-check", QKeySequence(), [this] { showTaskPane("designchecker"); });
    mk("rev.wordCount", tr("Word Count"), "whole-word", QKeySequence(), [this] { wordCountDialog(this, m_ed); });
    mk("rev.hyphenation", tr("Hyphenation…"), "hyphenation", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), [this] { hyphenationDialog(this, m_ed); });

    // ---------------- View ----------------
    mk("view.single", tr("Single Page"), "file", QKeySequence(), [ed] { ed->setTwoPageSpread(false); }, true);
    mk("view.spread", tr("Two-Page Spread"), "book-open", QKeySequence(), [ed] { ed->setTwoPageSpread(true); }, true);
    auto toggle = [ed](bool ViewOptions::*m) { ed->setView([m](ViewOptions &v) { v.*m = !(v.*m); }); };
    mk("view.boundaries", tr("Boundaries"), "square-dashed", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [toggle] { toggle(&ViewOptions::boundaries); }, true);
    mk("view.guides", tr("Guides"), "layout-grid", QKeySequence(), [toggle] { toggle(&ViewOptions::guides); }, true);
    mk("view.fields", tr("Fields"), "braces", QKeySequence(), [toggle] { toggle(&ViewOptions::fields); }, true);
    mk("view.rulers", tr("Rulers"), "ruler", QKeySequence(), [this, toggle] {
        toggle(&ViewOptions::rulers);
        m_canvas->setRulersVisible(m_ed->view.rulers);
    }, true);
    mk("view.pageNav", tr("Page Navigation"), "panel-left", QKeySequence(), [this, toggle] {
        toggle(&ViewOptions::pageNav);
        m_pages->setVisible(m_ed->view.pageNav);
    }, true);
    mk("view.scratch", tr("Scratch Area"), "layout-dashboard", QKeySequence(), [toggle] { toggle(&ViewOptions::scratch); }, true);
    mk("view.baselines", tr("Baselines"), "align-vertical-space-between", QKeySequence(Qt::CTRL | Qt::Key_F7), [toggle] { toggle(&ViewOptions::baselines); }, true);
    mk("view.gridlines", tr("View Gridlines"), "grid-2x2", QKeySequence(), [toggle] { toggle(&ViewOptions::gridlines); }, true);
    mk("view.graphics", tr("Graphics Manager"), "images", QKeySequence(), [this] {
        if (currentTaskPane() == "graphics") hideTaskPane(); else showTaskPane("graphics");
    }, true);
    mk("view.measurement", tr("Measurement"), "ruler-dimension-line", QKeySequence(), [this] { measurementWindow(this, m_ed); });
    mk("zoom.100", QStringLiteral("100%"), "scan", QKeySequence(Qt::Key_F9), [this] {
        if (std::abs(m_canvas->zoom() - 1.0) < 0.01) m_canvas->zoomToFit(Canvas::Fit::WholePage);
        else { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(1.0); }
    });
    mk("zoom.page", tr("Whole Page"), "maximize", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L), [this] { m_canvas->zoomToFit(Canvas::Fit::WholePage); });
    mk("zoom.width", tr("Page Width"), "move-horizontal", QKeySequence(), [this] { m_canvas->zoomToFit(Canvas::Fit::PageWidth); });
    mk("zoom.selection", tr("Selected Objects"), "scan-search", QKeySequence(), [this] { m_canvas->zoomToFit(Canvas::Fit::Selection); });
    mk("zoom.in", tr("Zoom In"), "zoom-in", QKeySequence::ZoomIn, [this] { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(m_canvas->zoom() * 1.2); });
    mk("zoom.out", tr("Zoom Out"), "zoom-out", QKeySequence::ZoomOut, [this] { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(m_canvas->zoom() / 1.2); });
    mk("win.new", tr("New Window"), "app-window", QKeySequence(), [this] {
        auto *w = new MainWindow();
        w->setAttribute(Qt::WA_DeleteOnClose);
        if (!m_ed->filePath().isEmpty()) w->openFile(m_ed->filePath());
        w->show();
    });
    mk("win.arrange", tr("Arrange All"), "layout-panel-left", QKeySequence(), [] {
        QList<QWidget *> wins;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<MainWindow *>(w) && w->isVisible()) wins << w;
        if (wins.isEmpty()) return;
        const QRect area = wins.first()->screen()->availableGeometry();
        const int wdt = area.width() / wins.size();
        for (int i = 0; i < wins.size(); ++i) wins[i]->setGeometry(area.left() + i * wdt, area.top(), wdt, area.height());
    });
    mk("win.cascade", tr("Cascade"), "layers-2", QKeySequence(), [] {
        int k = 0;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<MainWindow *>(w) && w->isVisible()) { w->move(40 + k * 30, 40 + k * 30); w->raise(); ++k; }
    });

    // ---------------- Shape / drawing tools ----------------
    mk("shape.addText", tr("Add Text"), "text-cursor", QKeySequence(), [ed] { if (Item *it = ed->single()) ed->beginTextEdit(it->id); });
    mk("shape.editPoints", tr("Edit Points"), "spline", QKeySequence(), [this] {
        auto *s = dynamic_cast<ShapeItem *>(m_ed->single());
        if (!s) return;
        if (m_ed->pointsItem == s->id) { m_ed->setPointsItem(QString()); return; }
        // Convert to a freeform path so its points can be dragged; Ctrl+click
        // a point deletes it, Ctrl+click an edge adds one, Esc finishes.
        if (s->customPath.isEmpty())
            m_ed->change(tr("Edit Points"), [&] { s->customPath = shapePath(s->shape, s->rect.size(), s->adj); });
        m_ed->setPointsItem(s->id);
        statusBar()->showMessage(tr("Drag a point to reshape. Ctrl+click a point to delete it, or an edge to add one. Press Esc when done."), 8000);
    });
    mk("line.draw", tr("Line"), "minus", QKeySequence(), [ed] { ed->setTool(Tool::Line); });
    mk("line.arrow", tr("Arrow"), "move-right", QKeySequence(), [ed] { ed->setTool(Tool::Arrow); });
    mk("line.double", tr("Double Arrow"), "move-horizontal", QKeySequence(), [ed] { ed->setTool(Tool::DoubleArrow); });
    for (double w : {0.25, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0, 4.5, 6.0}) {
        mk(QStringLiteral("weight.%1").arg(w), tr("%1 pt").arg(w), "", QKeySequence(), [ed, w] {
            ed->forEachSelected(tr("Line Weight"), [w](Item *it) {
                if (it->stroke.isNone()) it->stroke = Stroke::line(ColorRef::scheme(Main), w);
                it->stroke.width = w;
            });
        });
    }
    for (int d = 0; d < 8; ++d) {
        mk(QStringLiteral("dash.%1").arg(d), dashName(Stroke::Dash(d)), "", QKeySequence(), [ed, d] {
            ed->forEachSelected(tr("Dashes"), [d](Item *it) { it->stroke.dash = Stroke::Dash(d); });
        });
    }
    const QString arrowNames[] = {tr("No Arrow"), tr("Arrow"), tr("Open Arrow"), tr("Stealth Arrow"), tr("Diamond Arrow"), tr("Oval Arrow")};
    for (int a = 0; a < 6; ++a) {
        mk(QStringLiteral("arrowEnd.%1").arg(a), tr("End: %1").arg(arrowNames[a]), "", QKeySequence(), [ed, a] {
            ed->forEachSelected(tr("Arrows"), [a](Item *it) { it->stroke.endArrow = Arrow(a); });
        });
        mk(QStringLiteral("arrowStart.%1").arg(a), tr("Start: %1").arg(arrowNames[a]), "", QKeySequence(), [ed, a] {
            ed->forEachSelected(tr("Arrows"), [a](Item *it) { it->stroke.startArrow = Arrow(a); });
        });
    }
    mk("fill.effects", tr("Fill Effects…"), "paint-bucket", QKeySequence(), [this] {
        Item *it = m_ed->single();
        if (!it) return;
        Fill f = it->fill;
        if (fillEffectsDialog(this, m_ed, f)) m_ed->forEachSelected(tr("Fill"), [f](Item *x) { x->fill = f; });
    });
    mk("fill.picture", tr("Picture Fill…"), "image", QKeySequence(), [this] {
        const QString p = QFileDialog::getOpenFileName(this, tr("Picture Fill"), QString(), tr("Pictures (*.png *.jpg *.jpeg *.gif *.bmp *.tif *.webp *.svg)"));
        if (p.isEmpty()) return;
        QFile file(p);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QString id = m_ed->doc()->addImage(file.readAll(), QFileInfo(p).suffix().toLower(), p);
        m_ed->forEachSelected(tr("Picture Fill"), [id](Item *x) { x->fill.type = Fill::Picture; x->fill.imageId = id; x->fill.tile = false; });
    });
    mk("fill.none", tr("No Fill"), "ban", QKeySequence(), [ed] { ed->forEachSelected(tr("Fill"), [](Item *x) { x->fill = Fill(); }); });
    mk("line.none", tr("No Outline"), "ban", QKeySequence(), [ed] { ed->forEachSelected(tr("Outline"), [](Item *x) { x->stroke.color = ColorRef::none(); }); });
    mk("line.more", tr("More Lines…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 0); });
    // Effects
    auto shadowPreset = [ed](int k) {
        ed->forEachSelected(tr("Shadow"), [k](Item *it) {
            it->fx.shadow.on = k > 0;
            it->fx.shadow.blur = k == 1 ? 4 : k == 2 ? 0 : 8;
            it->fx.shadow.distance = k == 3 ? 0 : 3;
            it->fx.shadow.angle = k == 4 ? 135 : 45;
            it->fx.shadow.transparency = k == 3 ? 0.5 : 0.6;
        });
    };
    const QString shadowNames[] = {tr("No Shadow"), tr("Outer Shadow"), tr("Hard Shadow"), tr("Soft Glow Shadow"), tr("Shadow Down-Left")};
    for (int k = 0; k < 5; ++k) mk(QStringLiteral("shadow.%1").arg(k), shadowNames[k], "", QKeySequence(), [shadowPreset, k] { shadowPreset(k); });
    mk("shadow.options", tr("Shadow Options…"), "", QKeySequence(), [this] { shadowDialog(this, m_ed); });
    for (int g : {0, 5, 8, 11, 18}) {
        mk(QStringLiteral("glow.%1").arg(g), g ? tr("%1 pt Glow").arg(g) : tr("No Glow"), "", QKeySequence(), [ed, g] {
            ed->forEachSelected(tr("Glow"), [g](Item *it) { it->fx.glow.on = g > 0; it->fx.glow.size = g; });
        });
    }
    for (int s : {0, 2, 5, 10, 25}) {
        mk(QStringLiteral("soft.%1").arg(s), s ? tr("%1 pt").arg(s) : tr("No Soft Edges"), "", QKeySequence(), [ed, s] {
            ed->forEachSelected(tr("Soft Edges"), [s](Item *it) { it->fx.softEdge = s; });
        });
    }
    const QString reflNames[] = {tr("No Reflection"), tr("Tight Reflection"), tr("Half Reflection"), tr("Full Reflection")};
    for (int r = 0; r < 4; ++r) {
        mk(QStringLiteral("refl.%1").arg(r), reflNames[r], "", QKeySequence(), [ed, r] {
            ed->forEachSelected(tr("Reflection"), [r](Item *it) {
                it->fx.reflection.on = r > 0;
                it->fx.reflection.size = r == 1 ? 0.3 : r == 2 ? 0.5 : 1.0;
                it->fx.reflection.distance = 2;
            });
        });
    }
    const QString bevelNames[] = {tr("No Bevel"), tr("Circle"), tr("Relaxed Inset"), tr("Cool Slant"), tr("Angle"), tr("Soft Round"), tr("Convex")};
    for (int b = 0; b < 7; ++b) {
        mk(QStringLiteral("bevel.%1").arg(b), bevelNames[b], "", QKeySequence(), [ed, b] {
            ed->forEachSelected(tr("Bevel"), [b](Item *it) { it->fx.bevel.type = b; it->fx.bevel.width = 4 + b; });
        });
    }
    struct Rot3d { const char *id; QString name; QPointF angle; };
    const Rot3d rot3d[] = {{"No Rotation", tr("No Rotation"), {0, 0}}, {"Perspective Left", tr("Perspective Left"), {0, 30}}, {"Perspective Right", tr("Perspective Right"), {0, -30}},
                           {"Perspective Above", tr("Perspective Above"), {30, 0}}, {"Perspective Below", tr("Perspective Below"), {-30, 0}}, {"Off Axis", tr("Off Axis"), {15, 25}}};
    for (const auto &r : rot3d) {
        const QPointF a = r.angle;
        mk(QStringLiteral("rot3d.%1").arg(QString::fromLatin1(r.id)), r.name, "", QKeySequence(), [ed, a] {
            ed->forEachSelected(tr("3-D Rotation"), [a](Item *it) { it->fx.rot3d.x = a.x(); it->fx.rot3d.y = a.y(); it->fx.rot3d.perspective = 30; });
        });
    }

    // ---------------- Text box tools ----------------
    auto setAutofit = [ed](TextItem::Autofit f) {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        auto *t = dynamic_cast<TextItem *>(it);
        if (!t) return;
        ed->change(tr("Text Fit"), [ed, t, f] {
            t->autofit = f;
            t->fitAsStored = false;   // fitted here from now on
            if (f == TextItem::GrowBox) ed->autoGrowText(t);
        });
    };
    mk("fit.best", tr("Best Fit"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::BestFit); }, true);
    mk("fit.shrink", tr("Shrink Text On Overflow"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::ShrinkOnOverflow); }, true);
    mk("fit.grow", tr("Grow Text Box to Fit"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::GrowBox); }, true);
    mk("fit.none", tr("Do Not Autofit"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::NoAutofit); }, true);
    mk("tb.textFit", tr("Text Fit"), "fold-vertical", QKeySequence(), [] {});
    mk("tb.direction", tr("Text Direction"), "arrow-down-wide-narrow", QKeySequence(), [ed] {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(tr("Text Direction"), [t] { t->vertical = !t->vertical; });
        else if (auto *tb = dynamic_cast<TableItem *>(it)) {
            // The cell the text cursor is in, or with the table selected, every cell
            // (all turned, unless they all are already).
            QVector<TableCell *> cells;
            for (int rr = 0; rr < tb->rows; ++rr)
                for (int cc = 0; cc < tb->cols; ++cc)
                    if (!ed->isEditingText() || (rr == ed->textTarget().row && cc == ed->textTarget().col)) cells << &tb->cell(rr, cc);
            const bool all = std::all_of(cells.begin(), cells.end(), [](const TableCell *x) { return x->vertical; });
            ed->change(tr("Text Direction"), [&] {
                for (TableCell *x : cells) x->vertical = !all;
                ed->fitTableRows(tb);   // turned back, the text may need more room
            });
        }
    }, true);
    const QPair<QString, int> valigns[] = {{tr("Align Top"), 0}, {tr("Align Middle"), 1}, {tr("Align Bottom"), 2}};
    for (const auto &v : valigns) {
        const int k = v.second;
        mk(QStringLiteral("valign.%1").arg(k), v.first, k == 0 ? "align-vertical-justify-start" : k == 1 ? "align-vertical-justify-center" : "align-vertical-justify-end",
           QKeySequence(), [ed, k] {
               Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
               if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(tr("Vertical Alignment"), [t, k] { t->valign = VAlign(k); });
               else if (auto *s = dynamic_cast<ShapeItem *>(it)) ed->change(tr("Vertical Alignment"), [s, k] { s->valign = VAlign(k); });
               else if (auto *tb = dynamic_cast<TableItem *>(it); tb && ed->isEditingText()) {
                   const auto &tt = ed->textTarget();
                   ed->change(tr("Cell Alignment"), [tb, tt, k] { tb->cell(tt.row, tt.col).valign = VAlign(k); });
               }
           }, true);
    }
    for (int c = 1; c <= 4; ++c) {
        mk(QStringLiteral("cols.%1").arg(c), c == 1 ? tr("One Column") : tr("%1 Columns").arg(c), c == 1 ? "square" : c == 2 ? "columns-2" : "columns-3",
           QKeySequence(), [ed, c] {
               Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
               if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(tr("Columns"), [t, c] { t->columns = c; });
           });
    }
    mk("cols.more", tr("More Columns…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 3); });
    mk("tb.customMargins", tr("Custom Margins…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 3); });   // the Text Box tab: margins and columns
    struct BoxMargin { const char *id; QString name; double pts; };
    const BoxMargin tbMargins[] = {{"None", tr("None"), 0}, {"Narrow", tr("Narrow"), 2.88}, {"Moderate", tr("Moderate"), 7.2}, {"Wide", tr("Wide"), 14.4}};
    for (const auto &m : tbMargins) {
        const double v = m.pts;
        mk(QStringLiteral("tbmargin.%1").arg(QString::fromLatin1(m.id)), m.name, "", QKeySequence(), [ed, v] {
            Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
            if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(tr("Margins"), [t, v] { t->insets = QMarginsF(v, v, v, v); });
            else if (auto *s = dynamic_cast<ShapeItem *>(it)) ed->change(tr("Margins"), [s, v] { s->insets = QMarginsF(v, v, v, v); });
            else if (auto *tb = dynamic_cast<TableItem *>(it)) {
                // The cell the text cursor is in, or with the table selected, every cell.
                const int r = ed->isEditingText() ? ed->textTarget().row : -1, c = ed->isEditingText() ? ed->textTarget().col : -1;
                ed->change(tr("Cell Margins"), [tb, v, r, c] {
                    for (int rr = 0; rr < tb->rows; ++rr)
                        for (int cc = 0; cc < tb->cols; ++cc)
                            if (r < 0 || (rr == r && cc == c)) tb->cell(rr, cc).margins = QMarginsF(v, v, v, v);
                });
            }
        });
    }
    mk("tb.link", tr("Create Link"), "link-2", QKeySequence(), [ed] {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        if (!dynamic_cast<TextItem *>(it)) return;
        ed->endTextEdit();
        ed->linkSource = it->id;
        ed->setTool(Tool::Link);
        ed->linkSource = it->id;
    });
    mk("tb.break", tr("Break"), "unlink-2", QKeySequence(), [ed] {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        if (it) ed->breakLink(it->id);
    });
    mk("tb.prev", tr("Previous"), "arrow-left", QKeySequence(), [ed] {
        Item *it = ed->single();
        if (!it) return;
        if (TextItem *p = ed->doc()->prevFrame(it->id)) {
            const auto loc = ed->doc()->find(p->id);
            if (loc.page >= 0) ed->setCurrentPage(loc.page);
            ed->select(p->id);
        }
    });
    mk("tb.next", tr("Next"), "arrow-right", QKeySequence(), [ed] {
        auto *t = dynamic_cast<TextItem *>(ed->single());
        if (!t || t->nextId.isEmpty()) return;
        const auto loc = ed->doc()->find(t->nextId);
        if (loc.page >= 0) ed->setCurrentPage(loc.page);
        ed->select(t->nextId);
    });
    mk("tb.contOn", tr("Include \"Continued on page…\""), "", QKeySequence(), [ed] {
        if (auto *t = dynamic_cast<TextItem *>(ed->single())) ed->change(tr("Continued Notice"), [t] { t->continuedOn = !t->continuedOn; });
    }, true);
    mk("tb.contFrom", tr("Include \"Continued from page…\""), "", QKeySequence(), [ed] {
        if (auto *t = dynamic_cast<TextItem *>(ed->single())) ed->change(tr("Continued Notice"), [t] { t->continuedFrom = !t->continuedFrom; });
    }, true);
    for (int lines : {0, 2, 3, 4, 5}) {
        mk(QStringLiteral("dropcap.%1").arg(lines), lines ? tr("Drop Cap %1 Lines").arg(lines) : tr("No Drop Cap"), "", QKeySequence(),
           [ed, lines] { ed->setDropCap(lines); });
    }
    mk("dropcap.custom", tr("Custom Drop Cap…"), "", QKeySequence(), [this] { dropCapDialog(this, m_ed); });
    auto boolProp = [ed](int prop, const QString &label) {
        const bool on = !ed->currentCharFormat().boolProperty(prop);
        ed->setCharProperty(prop, on, label);
    };
    mk("tb.shadow", tr("Shadow"), "", QKeySequence(), [boolProp] { boolProp(tp::Shadow, tr("Text Shadow")); }, true);
    mk("tb.outline", tr("Outline"), "", QKeySequence(), [ed] {
        if (!ed->currentCharFormat().stringProperty(tp::OutlineRef).isEmpty()) ed->clearCharProperty(tp::OutlineRef, tr("Text Outline"));
        else ed->setCharProperty(tp::OutlineRef, ColorRef::scheme(Main).toString(), tr("Text Outline"));
    }, true);
    mk("tb.emboss", tr("Emboss"), "", QKeySequence(), [boolProp] { boolProp(tp::Emboss, tr("Emboss")); }, true);
    mk("tb.engrave", tr("Engrave"), "", QKeySequence(), [boolProp] { boolProp(tp::Engrave, tr("Engrave")); }, true);
    mk("tb.trueSmallCaps", tr("True Small Caps"), "", QKeySequence(), [boolProp] { boolProp(tp::TrueSmallCaps, tr("Small Caps")); }, true);
    mk("tb.swash", tr("Swash"), "", QKeySequence(), [boolProp] { boolProp(tp::Swash, tr("Swash")); }, true);
    mk("tb.alternates", tr("Stylistic Alternates"), "", QKeySequence(), [boolProp] { boolProp(tp::Alternates, tr("Stylistic Alternates")); }, true);
    const QString numStyles[] = {tr("Default"), tr("Lining"), tr("Old-style")};
    for (int i = 0; i < 3; ++i) mk(QStringLiteral("numstyle.%1").arg(i), numStyles[i], "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::NumberStyle, i, tr("Number Style")); });
    const QString numSpacing[] = {tr("Default Spacing"), tr("Proportional"), tr("Tabular")};
    for (int i = 0; i < 3; ++i) mk(QStringLiteral("numspacing.%1").arg(i), numSpacing[i], "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::NumberSpacing, i, tr("Number Spacing")); });
    const QString ligNames[] = {tr("Standard Ligatures"), tr("No Ligatures"), tr("All Ligatures")};
    for (int i = 0; i < 3; ++i) mk(QStringLiteral("lig.%1").arg(i), ligNames[i], "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::Ligatures, i, tr("Ligatures")); });
    for (int i = 0; i <= 20; ++i) mk(QStringLiteral("ss.%1").arg(i), i ? tr("Stylistic Set %1").arg(i) : tr("Default Set"), "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::StylisticSet, i, tr("Stylistic Set")); });

    // ---------------- Pictures ----------------
    mk("pic.change", tr("Change Picture"), "image-up", QKeySequence(), [this] {
        if (Item *it = m_ed->single(); it && it->type() == ItemType::Picture) insertPictureFromFile(it->id);
    });
    mk("pic.remove", tr("Remove Picture"), "image-off", QKeySequence(), [ed] {
        if (auto *p = dynamic_cast<PictureItem *>(ed->single())) ed->change(tr("Remove Picture"), [p] { p->imageId.clear(); });
    });
    mk("pic.arrangeThumbs", tr("Arrange Thumbnails"), "layout-grid", QKeySequence(), [ed] { ed->arrangeThumbnails(); });
    mk("pic.swap", tr("Swap"), "arrow-left-right", QKeySequence(), [ed] {
        const auto sel = ed->selectedItems();
        if (sel.size() != 2) { Q_EMIT ed->status(tr("Select two pictures to swap.")); return; }
        auto *a = dynamic_cast<PictureItem *>(sel[0]);
        auto *b = dynamic_cast<PictureItem *>(sel[1]);
        if (!a || !b) return;
        ed->change(tr("Swap Pictures"), [&] {
            std::swap(a->imageId, b->imageId);
            a->fitImage(ed->doc()->imageSize(a->imageId), true);
            b->fitImage(ed->doc()->imageSize(b->imageId), true);
        });
    });
    for (int b : {-40, -20, 0, 20, 40}) {
        for (int c : {-40, -20, 0, 20, 40}) {
            mk(QStringLiteral("corr.%1.%2").arg(b).arg(c), tr("Brightness %1%, Contrast %2%").arg(b > 0 ? "+" + QString::number(b) : QString::number(b)).arg(c > 0 ? "+" + QString::number(c) : QString::number(c)),
               "", QKeySequence(), [ed, b, c] {
                   ed->forEachSelected(tr("Corrections"), [b, c](Item *it) {
                       if (auto *p = dynamic_cast<PictureItem *>(it)) { p->brightness = b; p->contrast = c; }
                   });
               });
        }
    }
    const QString recolorNames[] = {tr("No Recolor"), tr("Grayscale"), tr("Sepia"), tr("Washout"), tr("Black and White")};
    for (int r = 0; r < 5; ++r) {
        mk(QStringLiteral("recolor.%1").arg(r), recolorNames[r], "", QKeySequence(), [ed, r] {
            ed->forEachSelected(tr("Recolor"), [r](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->recolor = PictureItem::Recolor(r); });
        });
    }
    for (int s = 1; s <= 5; ++s) {
        const ColorRef c = ColorRef::scheme(s);
        mk(QStringLiteral("recolor.slot%1").arg(s), tr("%1 Tint").arg(slotName(s)), "", QKeySequence(), [ed, c] {
            ed->forEachSelected(tr("Recolor"), [c](Item *it) {
                if (auto *p = dynamic_cast<PictureItem *>(it)) { p->recolor = PictureItem::ColorTint; p->recolorColor = c; }
            });
        });
    }
    mk("pic.transparent", tr("Set Transparent Color"), "pipette", QKeySequence(), [this] {
        auto *p = dynamic_cast<PictureItem *>(m_ed->single());
        if (!p) return;
        const QImage img = m_ed->doc()->image(p->imageId);
        if (img.isNull()) return;
        // Use the corner pixel, which is the background in most clip art and logos.
        const QColor c = img.pixelColor(0, 0);
        m_ed->change(tr("Set Transparent Color"), [p, c] { p->hasTransparentColor = true; p->transparentColor = c; });
    });
    mk("pic.compress", tr("Compress Pictures…"), "minimize-2", QKeySequence(), [this] {
        compressPicturesDialog(this, m_ed);
    });
    mk("pic.reset", tr("Reset Picture"), "rotate-ccw", QKeySequence(), [ed] {
        ed->forEachSelected(tr("Reset Picture"), [ed](Item *it) {
            if (auto *p = dynamic_cast<PictureItem *>(it)) {
                p->brightness = p->contrast = 0;
                p->recolor = PictureItem::NoRecolor;
                p->hasTransparentColor = false;
                p->transparency = 0;
                p->fx = Effects();
                p->fitImage(ed->doc()->imageSize(p->imageId), true);
            }
        });
    });
    mk("pic.toShapes", tr("Convert to Shapes"), "shapes", QKeySequence(), [this, ed] {
        // An SVG picture becomes editable artwork shapes in its place.
        auto *pic = dynamic_cast<PictureItem *>(ed->single());
        if (!pic) return;
        const ImageData data = ed->doc()->images.value(pic->imageId);
        if (data.format != QLatin1String("svg")) return;
        ItemList &items = ed->surfaceItems();
        const auto at = std::find_if(items.begin(), items.end(), [pic](const ItemPtr &x) { return x.get() == pic; });
        if (at == items.end()) return;
        bool partial = false;
        const ItemPtr made = svg::pictureShapes(data.bytes, *pic, &partial);
        if (!made) {
            QMessageBox::information(this, tr("Convert to Shapes"), tr("This picture has no lines or areas to turn into shapes."));
            return;
        }
        const qsizetype index = at - items.begin();
        ed->change(tr("Convert to Shapes"), [&] { items[index] = made; });
        ed->select(made->id);
        if (partial)
            QMessageBox::information(this, tr("Convert to Shapes"),
                                     tr("Text and pictures inside this drawing can't become shapes, so they were left out. Undo brings the picture back."));
    });
    mk("pic.crop", tr("Crop"), "crop", QKeySequence(), [ed] {
        Item *it = ed->single();
        if (!it || it->type() != ItemType::Picture) return;
        ed->setCropItem(ed->cropItem == it->id ? QString() : it->id);
    }, true);
    mk("pic.fit", tr("Fit"), "shrink", QKeySequence(), [ed] {
        ed->forEachSelected(tr("Fit"), [ed](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->fitImage(ed->doc()->imageSize(p->imageId), false); });
    });
    mk("pic.fill", tr("Fill"), "expand", QKeySequence(), [ed] {
        ed->forEachSelected(tr("Fill"), [ed](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->fitImage(ed->doc()->imageSize(p->imageId), true); });
    });
    mk("pic.clearCrop", tr("Clear Crop"), "eraser", QKeySequence(), [ed] {
        ed->forEachSelected(tr("Clear Crop"), [ed](Item *it) {
            auto *p = dynamic_cast<PictureItem *>(it);
            if (!p) return;
            // Grow the frame to show the whole picture.
            const QPointF tl = p->transform().map(p->imgRect.topLeft());
            const QPointF br = p->transform().map(p->imgRect.bottomRight());
            const QPointF c = (tl + br) / 2;
            p->rect = QRectF(c.x() - p->imgRect.width() / 2, c.y() - p->imgRect.height() / 2, p->imgRect.width(), p->imgRect.height());
            p->imgRect = QRectF(QPointF(0, 0), p->rect.size());
        });
    });
    mk("pic.saveAs", tr("Save as Picture…"), "image-down", QKeySequence(), [this] {
        Item *it = m_ed->single();
        if (!it) return;
        const QString p = askSavePath(this, tr("Save as Picture"), QString(), tr("PNG (*.png)") + QStringLiteral(";;") + tr("JPEG (*.jpg)"));
        if (p.isEmpty()) return;
        PaintContext ctx;
        ctx.doc = m_ed->doc();
        ctx.cache = &m_ed->cache();
        ctx.opt.output = true;
        Renderer::renderItemsToImage(ctx, {m_ed->doc()->itemPtr(it->id)}, 300.0 / 72).save(p);
    });
    mk("pic.caption", tr("Caption"), "captions", QKeySequence(), [ed] {
        auto *p = dynamic_cast<PictureItem *>(ed->single());
        if (!p) return;
        ed->beginChange(tr("Caption"));
        auto t = std::static_pointer_cast<TextItem>(ed->newTextBox(QRectF(p->rect.left(), p->rect.bottom() + 4, p->rect.width(), 22), QStringLiteral("Caption describing picture or graphic.")));
        ed->doc()->storyDoc(t->storyId);
        QTextCursor c(ed->doc()->storyDoc(t->storyId));
        c.select(QTextCursor::Document);
        if (const TextStyle *s = ed->doc()->style("Caption")) { c.mergeCharFormat(s->chr); c.mergeBlockFormat(s->blk); }
        ed->surfaceItems().push_back(t);
        p->caption = t->id;
        ed->endChange();
        ed->select(t->id);
    });
    mk("pic.shapeRect", tr("Rectangle"), "", QKeySequence(), [ed] { ed->forEachSelected(tr("Picture Shape"), [](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->maskShape = "rect"; }); });
    mk("obj.transparency", tr("Transparency…"), "blend", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 4); });

    // ---------------- Tables ----------------
    // A row and column command: on the cell the text cursor is in, or with the
    // whole table selected, -1 (callers choose: the ends to insert at, and
    // nothing to delete).
    auto tableEdit = [this](const QString &label, const std::function<void(TableItem *, int, int)> &fn) {
        TableItem *t = selTable(m_ed);
        if (!t) return;
        const int r = m_ed->isEditingText() ? m_ed->textTarget().row : -1;
        const int c = m_ed->isEditingText() ? m_ed->textTarget().col : -1;
        m_ed->endTextEdit();
        m_ed->change(label, [&] { fn(t, r, c); });
        m_ed->select(t->id);
    };
    mk("tbl.insAbove", tr("Insert Above"), "between-horizontal-end", QKeySequence(), [this, tableEdit] { tableEdit(tr("Insert Row"), [this](TableItem *t, int r, int) { tableInsertRow(m_ed, t, std::max(0, r)); }); });
    mk("tbl.insBelow", tr("Insert Below"), "between-horizontal-start", QKeySequence(), [this, tableEdit] { tableEdit(tr("Insert Row"), [this](TableItem *t, int r, int) { tableInsertRow(m_ed, t, r < 0 ? t->rows : r + 1); }); });
    mk("tbl.insLeft", tr("Insert Left"), "between-vertical-end", QKeySequence(), [this, tableEdit] { tableEdit(tr("Insert Column"), [this](TableItem *t, int, int c) { tableInsertCol(m_ed, t, std::max(0, c)); }); });
    mk("tbl.insRight", tr("Insert Right"), "between-vertical-start", QKeySequence(), [this, tableEdit] { tableEdit(tr("Insert Column"), [this](TableItem *t, int, int c) { tableInsertCol(m_ed, t, c < 0 ? t->cols : c + 1); }); });
    auto needCell = [this](const QString &what) {
        if (selTable(m_ed) && !m_ed->isEditingText()) { Q_EMIT m_ed->status(what); return true; }
        return false;
    };
    mk("tbl.delRow", tr("Delete Rows"), "", QKeySequence(), [tableEdit, needCell, this] {
        if (needCell(tr("Click in the row to delete, then choose Delete Rows. Delete Table removes the whole table."))) return;
        tableEdit(tr("Delete Row"), [](TableItem *t, int r, int) { tableDeleteRow(t, r); });
    });
    mk("tbl.delCol", tr("Delete Columns"), "", QKeySequence(), [tableEdit, needCell, this] {
        if (needCell(tr("Click in the column to delete, then choose Delete Columns. Delete Table removes the whole table."))) return;
        tableEdit(tr("Delete Column"), [](TableItem *t, int, int c) { tableDeleteCol(t, c); });
    });
    mk("tbl.delTable", tr("Delete Table"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) { m_ed->endTextEdit(); m_ed->deleteItems({t->id}); }
    });
    mk("tbl.merge", tr("Merge Cells"), "table-cells-merge", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) { Q_EMIT m_ed->status(tr("Click in a cell, then choose Merge Cells to merge it with the cell to its right.")); return; }
        const int r = m_ed->textTarget().row, c = m_ed->textTarget().col;
        TableCell &cell = t->cell(r, c);
        const int next = c + cell.colSpan;
        if (next >= t->cols) return;
        m_ed->endTextEdit();
        m_ed->change(tr("Merge Cells"), [&] {
            TableCell &right = t->cell(r, next);
            // Move the right cell's text into this one.
            QTextDocument *dst = m_ed->doc()->storyDoc(cell.storyId), *src = m_ed->doc()->storyDoc(right.storyId);
            if (dst && src && !src->toPlainText().isEmpty()) {
                QTextCursor dc(dst);
                dc.movePosition(QTextCursor::End);
                dc.insertBlock();
                dc.insertFragment(QTextDocumentFragment(src));
            }
            cell.colSpan += right.colSpan;
            for (int k = next; k < c + cell.colSpan; ++k) t->cell(r, k).covered = true;
        });
        m_ed->select(t->id);
    });
    mk("tbl.split", tr("Split Cells"), "table-cells-split", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const int r = m_ed->textTarget().row, c = m_ed->textTarget().col;
        m_ed->endTextEdit();
        m_ed->change(tr("Split Cells"), [&] {
            TableCell &cell = t->cell(r, c);
            for (int rr = r; rr < r + cell.rowSpan; ++rr)
                for (int cc = c; cc < c + cell.colSpan; ++cc)
                    if (rr != r || cc != c) t->cell(rr, cc).covered = false;
            cell.rowSpan = cell.colSpan = 1;
        });
        m_ed->select(t->id);
    });
    mk("tbl.diagDown", tr("Divide Down"), "", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const auto tt = m_ed->textTarget();
        m_ed->change(tr("Diagonals"), [&] { t->cell(tt.row, tt.col).diagonal = 1; });
    });
    mk("tbl.diagUp", tr("Divide Up"), "", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const auto tt = m_ed->textTarget();
        m_ed->change(tr("Diagonals"), [&] { t->cell(tt.row, tt.col).diagonal = 2; });
    });
    mk("tbl.diagNone", tr("No Division"), "", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const auto tt = m_ed->textTarget();
        m_ed->change(tr("Diagonals"), [&] { t->cell(tt.row, tt.col).diagonal = 0; });
    });
    mk("tbl.selectTable", tr("Select Table"), "", QKeySequence(), [this] { if (TableItem *t = selTable(m_ed)) { m_ed->endTextEdit(); m_ed->select(t->id); } });
    mk("tbl.selectCell", tr("Select Cell"), "", QKeySequence(), [this] {
        if (!m_ed->isEditingText()) return;
        QTextCursor c = m_ed->cursor();
        c.select(QTextCursor::Document);
        m_ed->setCursor(c);
    });
    mk("tbl.grow", tr("Grow to Fit Text"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) m_ed->change(tr("Grow to Fit Text"), [this, t] { t->growToFit = !t->growToFit; m_ed->fitTableRows(t); });
    }, true);
    mk("tbl.distributeRows", tr("Distribute Rows"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) m_ed->change(tr("Distribute Rows"), [t] {
            double total = 0;
            for (double h : t->rowH) total += h;
            for (double &h : t->rowH) h = total / t->rows;
        });
    });
    mk("tbl.distributeCols", tr("Distribute Columns"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) m_ed->change(tr("Distribute Columns"), [t] {
            double total = 0;
            for (double w : t->colW) total += w;
            for (double &w : t->colW) w = total / t->cols;
        });
    });
    for (const auto &[id, label, which] : {std::tuple{"border.all", tr("All Borders"), 0}, {"border.outside", tr("Outside Borders"), 1}, {"border.inside", tr("Inside Borders"), 2},
                                            {"border.none", tr("No Border"), 3}, {"border.top", tr("Top Border"), 4}, {"border.bottom", tr("Bottom Border"), 5},
                                            {"border.left", tr("Left Border"), 6}, {"border.right", tr("Right Border"), 7}}) {
        const int w = which;
        mk(id, label, "", QKeySequence(), [this, w] {
            TableItem *t = selTable(m_ed);
            if (!t) return;
            const Stroke s = w == 3 ? Stroke::none() : currentBorderStroke();
            const bool inCell = m_ed->isEditingText();
            const int r0 = inCell ? m_ed->textTarget().row : 0, c0 = inCell ? m_ed->textTarget().col : 0;
            const int r1 = inCell ? r0 : t->rows - 1, c1 = inCell ? c0 : t->cols - 1;
            m_ed->change(tr("Borders"), [&] {
                for (int r = r0; r <= r1; ++r)
                    for (int c = c0; c <= c1; ++c) {
                        CellBorder &b = t->cell(r, c).border;
                        const bool top = r == r0, bottom = r == r1, left = c == c0, right = c == c1;
                        switch (w) {
                        case 0: case 3: b.top = b.bottom = b.left = b.right = s; break;
                        case 1: if (top) b.top = s; if (bottom) b.bottom = s; if (left) b.left = s; if (right) b.right = s; break;
                        case 2: if (!top) b.top = s; if (!bottom) b.bottom = s; if (!left) b.left = s; if (!right) b.right = s; break;
                        case 4: if (top) b.top = s; break;
                        case 5: if (bottom) b.bottom = s; break;
                        case 6: if (left) b.left = s; break;
                        case 7: if (right) b.right = s; break;
                        }
                    }
            });
        });
    }

    // ---------------- TextArt ----------------
    mk("wa.edit", tr("Edit Text"), "pencil", QKeySequence(), [this] { if (Item *it = m_ed->single(); it && it->type() == ItemType::TextArt) editTextArt(it->id); });
    for (const auto &[id, label, v] : {std::tuple{"waspace.vt", tr("Very Tight"), 0.8}, {"waspace.t", tr("Tight"), 0.9}, {"waspace.n", tr("Normal"), 1.0}, {"waspace.l", tr("Loose"), 1.2}, {"waspace.vl", tr("Very Loose"), 1.5}}) {
        const double s = v;
        mk(id, label, "", QKeySequence(), [ed, s] {
            ed->forEachSelected(tr("Spacing"), [s](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->spacing = s; });
        });
    }
    mk("wa.even", tr("Even Height"), "move-vertical", QKeySequence(), [ed] {
        ed->forEachSelected(tr("Even Height"), [](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->evenHeight = !w->evenHeight; });
    }, true);
    mk("wa.vertical", tr("Text Art Vertical Text"), "arrow-down-narrow-wide", QKeySequence(), [ed] {
        ed->forEachSelected(tr("Vertical Text"), [](Item *it) {
            if (auto *w = dynamic_cast<TextArtItem *>(it)) { w->vertical = !w->vertical; w->rect.setSize(w->rect.size().transposed()); }
        });
    }, true);
    const QString waAlign[] = {tr("Left Align"), tr("Center"), tr("Right Align"), tr("Word Justify"), tr("Letter Justify"), tr("Stretch Justify")};
    for (int i = 0; i < 6; ++i) {
        mk(QStringLiteral("waalign.%1").arg(i), waAlign[i], "", QKeySequence(), [ed, i] {
            ed->forEachSelected(tr("Align Text"), [i](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->align = i; });
        });
    }
    // ---------------- Help ----------------
    auto described = [](QAction *a, const QString &tip) { a->setToolTip(tip); return a; };
    described(mk("help.show", tr("Help"), "circle-help", QKeySequence::HelpContents, [this] { showHelp(helpContext()); }),
              tr("Help (F1): how to do things in JeffPub, and a search that finds commands too."));
    described(mk("help.support", tr("Contact Support"), "headset", QKeySequence(), [] { help::openUrl(help::supportUrl()); }),
              tr("Contact Support: report a problem on JeffPub's GitHub page (a free GitHub account is needed)."));
    described(mk("help.feedback", tr("Feedback"), "smile", QKeySequence(), [] { help::openUrl(help::feedbackUrl()); }),
              tr("Feedback: suggest an idea for JeffPub on its GitHub page (a free GitHub account is needed)."));
    described(mk("help.shortcuts", tr("Keyboard Shortcuts"), "keyboard", QKeySequence(), [this] { showHelp(QStringLiteral("keyboard")); }),
              tr("Keyboard Shortcuts: every command's keys, and how to use JeffPub without a mouse."));
    described(mk("help.whatsNew", tr("What's New"), "sparkles", QKeySequence(), [] { help::openUrl(help::whatsNewUrl()); }),
              tr("What's New: this version's release notes, on JeffPub's GitHub page."));
    described(mk("view.collapseRibbon", tr("Collapse the Ribbon"), "chevron-up", QKeySequence(Qt::CTRL | Qt::Key_F1), [this] { m_ribbon->setMinimized(!m_ribbon->isMinimized()); }),
              tr("Collapse the Ribbon (Ctrl+F1): only the tab names show, leaving more room for the page. Again to show it."));
}

} // namespace jp
